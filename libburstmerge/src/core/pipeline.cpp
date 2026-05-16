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

float EstimateNoiseFloor(const FloatImage& image, uint32_t guide_block_size) {
    if (image.data.empty()) return 8.0f;

    const int blur_radius = 2;
    const FloatImage blurred = BoxBlur(image, blur_radius);
    const uint32_t step = std::max<uint32_t>(1, guide_block_size);

    double sum_sq = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 0; y < image.height; y += step) {
        for (uint32_t x = 0; x < image.width; x += step) {
            size_t idx = (static_cast<size_t>(y) * image.width + x) * image.channels;
            for (uint32_t c = 0; c < image.channels; ++c) {
                float d = image.data[idx + c] - blurred.data[idx + c];
                sum_sq += static_cast<double>(d) * static_cast<double>(d);
                ++count;
            }
        }
    }

    if (count == 0) return 8.0f;
    float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
    return std::max(8.0f, rms);
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
    float ref_iso = raw_images[ref_idx].metadata.iso_exposure_time;

    for (size_t i = 0; i < float_images.size(); ++i) {
        const auto& meta = raw_images[i].metadata;
        FloatImage& img = float_images[i];

        // Black level removal using mean black level
        float bl = MeanBlackLevel(meta);
        if (bl > 1.0f) {
            for (float& v : img.data) v -= bl;
        }

        // Exposure normalization using both physical exposure and exposure_bias
        if (i == ref_idx) continue;
        float comp_iso = meta.iso_exposure_time;
        if (ref_iso > 0.0f && comp_iso > 0.0f) {
            float scale = (ref_iso / comp_iso) *
                          std::pow(2.0f, raw_images[ref_idx].metadata.exposure_bias - meta.exposure_bias);
            if (std::abs(scale - 1.0f) > 0.001f) {
                for (float& v : img.data) v *= scale;
            }
        }
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
    params.cfa_period = std::max<uint32_t>(1, cfa_period);

    const FloatImage& ref = float_images[ref_idx];
    size_t processed = 0;
    const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
    for (size_t i = 0; i < float_images.size(); ++i) {
        if (i == ref_idx) continue;

        Report(progress,
               0.56f + 0.06f * static_cast<float>(processed) / static_cast<float>(std::max<size_t>(1, total)),
               "Aligning frame " + std::to_string(processed + 1) + "/" + std::to_string(total));
        AlignmentResult ar = EstimateTranslation(ref, float_images[i], params);

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
            float estimated_noise = EstimateNoiseFloor(float_images[ref_idx], std::max<uint32_t>(1, cfa_period));
            float formula_noise = std::max(8.0f, settings_.noise_reduction * 4.0f);
            // Assertion: auto-estimate must not exceed formula value.
            // Dark reference frames (Bkt) can produce inflated noise floor,
            // which disables the robust weight formula and causes blur.
            params.noise_floor = std::min(estimated_noise, formula_noise);
            float avg_bl = MeanBlackLevel(images[ref_idx].metadata);
            params.highlight_threshold = (static_cast<float>(images[ref_idx].metadata.white_level) - avg_bl) * 0.92f;
            params.clip_threshold = (static_cast<float>(images[ref_idx].metadata.white_level) - avg_bl) * 0.98f;
            params.guide_block_size = images[ref_idx].metadata.mosaic_pattern_width >= 2
                ? images[ref_idx].metadata.mosaic_pattern_width
                : 2;
            // Collect per-comparison-frame exposure scale factors
            float ref_iso = images[ref_idx].metadata.iso_exposure_time;
            float ref_bias = images[ref_idx].metadata.exposure_bias;
            std::vector<float> exp_scales;
            exp_scales.reserve(images.size());
            for (size_t i = 0; i < images.size(); ++i) {
                if (i == ref_idx) continue;
                float comp_iso = images[i].metadata.iso_exposure_time;
                if (ref_iso > 0.0f && comp_iso > 0.0f) {
                    exp_scales.push_back((ref_iso / comp_iso) *
                        std::pow(2.0f, ref_bias - images[i].metadata.exposure_bias));
                } else {
                    exp_scales.push_back(1.0f);
                }
            }
            params.num_scales = static_cast<uint32_t>(exp_scales.size());
            params.exposure_scales = exp_scales.data();
            merged = SpatialMerge(float_images[ref_idx], aligned, params);
        }

        // Add back reference black level so DNG writer's metadata matches pixel values.
        // Both subtraction (NormalizeFrames) and restoration use mean black level to avoid
        // per-channel DC offset. The DNG metadata retains original per-channel black_level[],
        // causing Lightroom to subtract slightly wrong values per channel, but the error is
        // < 4 DN for typical sensors and visually negligible.
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
