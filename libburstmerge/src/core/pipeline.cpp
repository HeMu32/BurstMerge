#include "burstmerge/internal/core/pipeline.h"

#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/denoise/temporal.h"
#include "burstmerge/internal/exposure/exposure.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/merge/frequency.h"
#include "burstmerge/internal/merge/spatial.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace burstmerge {
namespace {

using ProgressFn = PipelineOrchestrator::ProgressFn;

void Report(const ProgressFn& progress, float percent, const std::string& stage) {
    if (progress) progress(percent, stage);
}

std::string LowerExt(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool IsDngPath(const std::string& path) {
    return LowerExt(std::filesystem::path(path)) == ".dng";
}

bool LooksLikeDngOutputPath(const std::string& path) {
    return IsDngPath(path);
}

std::string ResolveOutputPath(const std::string& output_path_or_dir) {
    std::filesystem::path out(output_path_or_dir.empty() ? "." : output_path_or_dir);
    if (LooksLikeDngOutputPath(out.string())) {
        if (out.has_parent_path()) {
            std::filesystem::create_directories(out.parent_path());
        }
        return out.string();
    }

    std::filesystem::create_directories(out);
    return (out / "burstmerge_output.dng").string();
}

std::filesystem::path MakeTempConvertDir(const std::string& output_path) {
    std::filesystem::path base(output_path);
    std::filesystem::path parent = base.has_parent_path() ? base.parent_path() : std::filesystem::current_path();
    std::filesystem::path dir = parent / "burstmerge_converted";
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::string> PrepareDngInputs(const std::vector<std::string>& input_paths,
                                          const std::string& output_path,
                                          const ProgressFn& progress)
{
    std::vector<std::string> dng_paths;
    std::vector<std::string> raw_paths;
    dng_paths.reserve(input_paths.size());

    Report(progress, 0.03f, "Validating input files");
    for (const auto& path : input_paths) {
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("Input does not exist: " + path);
        }
        if (IsDngPath(path)) dng_paths.push_back(path);
        else raw_paths.push_back(path);
    }

    if (raw_paths.empty()) return dng_paths;

#ifdef _WIN32
    Report(progress, 0.08f, "Preparing RAW to DNG conversion");
    std::vector<std::string> converted;
    std::filesystem::path convert_dir = MakeTempConvertDir(output_path);
    Report(progress, 0.10f, "Converting " + std::to_string(raw_paths.size()) + " RAW file(s) to DNG");
    if (!RunAdobeDngConverter(raw_paths, convert_dir.string(), converted)) {
        throw std::runtime_error("Adobe DNG Converter failed or timed out");
    }
    Report(progress, 0.16f, "RAW to DNG conversion completed");
    dng_paths.insert(dng_paths.end(), converted.begin(), converted.end());
    return dng_paths;
#else
    throw std::runtime_error("Non-DNG RAW input requires pre-conversion on this platform");
#endif
}

bool IsCompatibleForAverage(const RawImage& a, const RawImage& b) {
    return a.pixels.width == b.pixels.width &&
           a.pixels.height == b.pixels.height &&
           a.pixels.format == b.pixels.format &&
           a.pixels.row_stride == b.pixels.row_stride;
}

float ComputeRobustness(float noise_reduction) {
    return std::max(0.2f, noise_reduction / 13.0f);
}

float MeanBlackLevel(const RawMetadata& meta) {
    float sum = 0.0f;
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        if (meta.black_level[i] > 0.0f) { sum += meta.black_level[i]; ++n; }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

void NormalizeFrames(std::vector<FloatImage>& float_images,
                     const std::vector<RawImage>& raw_images,
                     size_t ref_idx)
{
    // Subtract black level for correct zero-point, then normalize exposure
    // so that all comparison frames match the reference frame's exposure.
    float ref_bl = MeanBlackLevel(raw_images[ref_idx].metadata);
    float ref_iso = raw_images[ref_idx].metadata.iso_exposure_time;
    float ref_bias = raw_images[ref_idx].metadata.exposure_bias;

    for (size_t i = 0; i < float_images.size(); ++i) {
        float bl = MeanBlackLevel(raw_images[i].metadata);

        // Black level removal
        if (bl > 1.0f) {
            for (float& v : float_images[i].data) v -= bl;
        } else if (ref_bl > 1.0f) {
            // Reference has black but this frame doesn't (unlikely). Pad.
            // Since we're normalizing to ref, add ref's black for consistency.
            for (float& v : float_images[i].data) v += ref_bl;
        }

        // Exposure normalization (comparison frames only)
        if (i == ref_idx) continue;
        float comp_iso = raw_images[i].metadata.iso_exposure_time;
        if (ref_iso <= 0.0f || comp_iso <= 0.0f) continue;
        float comp_bias = raw_images[i].metadata.exposure_bias;
        float scale = (ref_iso / comp_iso) * std::pow(2.0f, ref_bias - comp_bias);
        if (std::abs(scale - 1.0f) < 0.001f) continue;
        for (float& v : float_images[i].data) v *= scale;
    }
}

std::vector<FloatImage> BuildFloatImages(const std::vector<RawImage>& images) {
    std::vector<FloatImage> out;
    out.reserve(images.size());
    for (const auto& img : images) {
        out.push_back(HostBufferToFloatImage(img.pixels));
    }
    return out;
}

std::vector<FloatImage> BuildAlignedComparisons(const std::vector<FloatImage>& float_images,
                                                size_t ref_idx,
                                                const Settings& settings,
                                                uint32_t cfa_period,
                                                const ProgressFn& progress)
{
    std::vector<FloatImage> aligned;
    aligned.reserve(float_images.size() > 0 ? float_images.size() - 1 : 0);

    AlignParams params;
    params.tile_size = settings.tile_size;
    params.search_distance = settings.search_distance;
    params.pyramid_levels = 3;

    const FloatImage& ref = float_images[ref_idx];
    size_t processed = 0;
    const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
    for (size_t i = 0; i < float_images.size(); ++i) {
        if (i == ref_idx) continue;

        Report(progress,
               0.56f + 0.06f * static_cast<float>(processed) / static_cast<float>(std::max<size_t>(1, total)),
               "Aligning frame " + std::to_string(processed + 1) + "/" + std::to_string(total));
        AlignmentResult ar = EstimateTranslation(ref, float_images[i], params);

        // Snap alignment to CFA period boundaries to preserve Bayer/X-Trans phase,
        // preventing false color at edges during demosaic.
        if (cfa_period > 1) {
            auto snap = [](int v, int p) -> int {
                if (v >= 0) return (v / static_cast<int>(p)) * static_cast<int>(p);
                else return -((-v) / static_cast<int>(p)) * static_cast<int>(p);
            };
            ar.shift_x = snap(ar.shift_x, static_cast<int>(cfa_period));
            ar.shift_y = snap(ar.shift_y, static_cast<int>(cfa_period));
        }

        Report(progress,
               0.62f + 0.06f * static_cast<float>(processed) / static_cast<float>(std::max<size_t>(1, total)),
               "Warping frame " + std::to_string(processed + 1) + "/" + std::to_string(total));
        aligned.push_back(WarpAligned(float_images[i], ar));
        ++processed;
    }
    return aligned;
}

size_t SelectReferenceIndex(const std::vector<RawImage>& images) {
    if (images.empty()) return 0;

    bool has_exposure = false;
    float min_exp = std::numeric_limits<float>::max();
    float max_exp = 0.0f;
    for (const auto& img : images) {
        float v = img.metadata.iso_exposure_time;
        if (v > 0.0f) {
            has_exposure = true;
            min_exp = std::min(min_exp, v);
            max_exp = std::max(max_exp, v);
        }
    }

    if (has_exposure && max_exp > min_exp * 1.25f) {
        size_t idx = 0;
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i < images.size(); ++i) {
            float v = images[i].metadata.iso_exposure_time;
            if (v > 0.0f && v < best) {
                best = v;
                idx = i;
            }
        }
        return idx;
    }

    return images.size() / 2;
}

} // namespace

PipelineOrchestrator::PipelineOrchestrator(BackendType backend, Settings settings)
    : backend_(backend), settings_(settings)
{}

Result PipelineOrchestrator::Process(const std::vector<std::string>& input_paths,
                                     const std::string& output_path_or_dir,
                                     ProgressFn progress)
{
    try {
        Report(progress, 0.0f, "Starting");
        if (backend_ != BackendType::CPU) {
            return {false, "", "Stage 1 currently supports CPU backend only"};
        }
        if (input_paths.empty()) {
            return {false, "", "No input images"};
        }

        std::string output_path = ResolveOutputPath(output_path_or_dir);
        Report(progress, 0.02f, "Preparing inputs");
        std::vector<std::string> dng_paths = PrepareDngInputs(input_paths, output_path, progress);
        if (dng_paths.empty()) return {false, "", "No readable DNG inputs"};

        Report(progress, 0.18f, "Reading and decoding DNG files");
        std::vector<RawImage> images;
        images.reserve(dng_paths.size());
        for (size_t i = 0; i < dng_paths.size(); ++i) {
            DngReader reader(dng_paths[i].c_str());
            images.push_back(reader.Read());
            float p = 0.18f + 0.30f * static_cast<float>(i + 1) / static_cast<float>(dng_paths.size());
            Report(progress, p, "Decoded image " + std::to_string(i + 1) + "/" + std::to_string(dng_paths.size()));
        }

        Report(progress, 0.50f, "Selecting reference frame");
        size_t ref_idx = SelectReferenceIndex(images);
        Report(progress, 0.52f, "Reference frame selected: " + std::to_string(ref_idx + 1) + "/" + std::to_string(images.size()));

        Report(progress, 0.54f, "Repairing hot pixels");
        std::vector<FloatImage> float_images = BuildFloatImages(images);
        RepairHotPixels(float_images, static_cast<float>(images[0].metadata.white_level), images[0].metadata.mosaic_pattern_width);

        Report(progress, 0.545f, "Normalizing frames (black level & exposure)");
        NormalizeFrames(float_images, images, ref_idx);

        for (size_t i = 1; i < images.size(); ++i) {
            if (!IsCompatibleForAverage(images[0], images[i])) {
                return {false, "", "Input images differ in dimensions, format, or stride"};
            }
        }

        uint32_t cfa_period = images[ref_idx].metadata.mosaic_pattern_width;
        std::vector<FloatImage> aligned = BuildAlignedComparisons(float_images, ref_idx, settings_, cfa_period, progress);

        FloatImage merged;
        if (settings_.noise_reduction >= 22.5f) {
            Report(progress, 0.70f, "Merging frames with temporal average");
            TemporalDenoiseParams params;
            params.strength = settings_.noise_reduction;
            merged = TemporalAverage(float_images[ref_idx], aligned, params);
        } else if (settings_.merge_algo == MergeAlgorithm::Frequency) {
            Report(progress, 0.70f, "Merging frames with frequency path");
            FrequencyMergeParams params;
            params.noise_reduction = settings_.noise_reduction;
            params.tile_size = settings_.tile_size;
            merged = FrequencyMerge(float_images[ref_idx], aligned, params);
        } else {
            Report(progress, 0.70f, "Merging frames with spatial path");
            SpatialMergeParams params;
            params.noise_reduction = settings_.noise_reduction;
            params.robustness = ComputeRobustness(settings_.noise_reduction);
            params.highlight_threshold =
                (static_cast<float>(images[ref_idx].metadata.white_level) -
                 MeanBlackLevel(images[ref_idx].metadata)) * 0.92f;
            params.guide_block_size = images[ref_idx].metadata.mosaic_pattern_width >= 2
                ? images[ref_idx].metadata.mosaic_pattern_width
                : 2;
            merged = SpatialMerge(float_images[ref_idx], aligned, params);
        }

        // Add back reference black level so DNG writer's metadata matches pixel values
        {
            float ref_bl = MeanBlackLevel(images[ref_idx].metadata);
            if (ref_bl > 1.0f) {
                for (float& v : merged.data) v += ref_bl;
            }
        }

        if (settings_.exposure_mode != ExposureMode::Off || settings_.exposure_stops != 0.0f) {
            Report(progress, 0.78f, "Exposure correction");
            ExposureParams params;
            params.mode = settings_.exposure_mode;
            params.stops = settings_.exposure_stops;
            ApplyExposure(merged, images[ref_idx].metadata.white_level, params);
        }

        Report(progress, 0.80f, "Quantizing float image to UInt16");
        HostBuffer averaged = FloatImageToUint16HostBuffer(merged, images[ref_idx].metadata.white_level);

        Report(progress, 0.82f, "Preparing output DNG container");
        RawImage output;
        output.metadata = std::move(images[ref_idx].metadata);
        output.pixels = std::move(averaged);

        Report(progress, 0.90f, "Writing output DNG file");
        DngWriter writer(output.metadata.dng_negative);
        writer.Write(output_path.c_str(), output);

        Report(progress, 1.0f, "Done");
        return {true, output_path, ""};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    } catch (...) {
        return {false, "", "Unknown processing error"};
    }
}

} // namespace burstmerge
