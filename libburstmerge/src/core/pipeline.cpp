#include "burstmerge/internal/core/pipeline.h"

#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/pipeline_align.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/pipeline_io.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/denoise/temporal.h"
#include "burstmerge/internal/exposure/exposure.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/io/image_decoder.h"
#include "burstmerge/internal/io/image_writer.h"
#include "burstmerge/internal/merge/frequency.h"
#include "burstmerge/internal/merge/spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <vector>

namespace burstmerge
{
namespace
{

using ProgressFn = PipelineOrchestrator::ProgressFn;

void Report(const ProgressFn& progress, float percent, const std::string& stage)
{
    if (progress) progress(percent, stage);
}

uint32_t ResolveTargetWhiteLevel(int dng_bit_depth, uint32_t sensor_white)
{
    switch (dng_bit_depth)
    {
        case 12: return 4095;
        case 14: return 16383;
        case 16: return 65535;
        default: return sensor_white;
    }
}
// RAW extensions known to the Adobe DNG Converter
bool IsRawExtension(const std::string& ext)
{
    static const char* raw_exts[] =
    {
        ".dng", ".arw", ".cr2", ".cr3", ".nef", ".nrw",
        ".orf", ".raf", ".rw2", ".pef", ".srw", ".x3f",
        ".sr2", ".srf", ".kdc", ".dcr", ".k25", ".mdc",
        ".mef", ".mrw", ".iiq", ".eip", ".bay", ".3fr",
        ".fff", ".mos", ".tif", ".tiff"  // TIFF may be camera RAW
    };
    for (const char* re : raw_exts)
    {
        if (ext == re) return true;
    }
    return false;
}

bool IsImageExtension(const std::string& ext)
{
    return ext == ".jpg" || ext == ".jpeg" ||
           ext == ".png" ||
           ext == ".bmp" ||
           ext == ".tif" || ext == ".tiff";
}

enum class InputClass { RAW, Rgb, Mixed };

static InputClass ClassifyInputs(const std::vector<std::string>& paths)
{
    bool has_raw = false;
    bool has_rgb = false;
    for (const auto& p : paths)
    {
        std::filesystem::path fp(p);
        std::string ext = fp.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (IsRawExtension(ext)) has_raw = true;
        else if (IsImageExtension(ext)) has_rgb = true;
        // Unknown extension: treat as RAW (will error later if unsupported)
        else has_raw = true;
    }
    if (has_raw && has_rgb) return InputClass::Mixed;
    if (has_rgb) return InputClass::Rgb;
    return InputClass::RAW;
}

} // namespace

PipelineOrchestrator::PipelineOrchestrator(BackendType backend, Settings settings)
    : backend_(backend), settings_(settings)
{}

Result PipelineOrchestrator::Process(const std::vector<std::string>& input_paths,
                                     const std::string& output_path_or_dir,
                                     ProgressFn progress)
{
    Result result =
    {false, "", ""};
    try
    {
        ResetProfiler();
        Report(progress, 0.0f, "Starting");
        if (backend_ != BackendType::CPU)
        {
            return
            {false, "", "Stage 1 currently supports CPU backend only"};
        }
        if (input_paths.empty())
        {
            return
            {false, "", "No input images"};
        }

        InputClass input_class = ClassifyInputs(input_paths);

        if (input_class == InputClass::Mixed)
        {
            Report(progress, 0.02f, "Mixed input: filtering non-RAW files");
            std::vector<std::string> filtered;
            for (const auto& p : input_paths)
            {
                std::filesystem::path fp(p);
                std::string ext = fp.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".dng" || IsRawExtension(ext))
                {
                    filtered.push_back(p);
                } else
                {
                    Report(progress, 0.02f, "Skipping non-RAW file: " + p);
                }
            }
            if (filtered.empty())
            {
                throw std::runtime_error("No RAW files remaining after filtering mixed input");
            }
            input_class = InputClass::RAW;
            return Process(filtered, output_path_or_dir, progress);
        }

        if (input_class == InputClass::Rgb)
        {
            Report(progress, 0.02f, "RGB input pipeline");
            std::vector<io::DecodedImage> decoded;
            decoded.reserve(input_paths.size());
            for (size_t i = 0; i < input_paths.size(); ++i)
            {
                Report(progress, PipelineConstants::kProgressDecodeStart + PipelineConstants::kProgressDecodeRange * static_cast<float>(i) / static_cast<float>(input_paths.size()),
                    "Decoding " + std::to_string(i + 1) + "/" + std::to_string(input_paths.size()));
                decoded.push_back(io::ReadImage(input_paths[i]));
            }

            size_t ref_idx = decoded.size() / 2;
            std::vector<FloatImage> float_images = BuildRgbImages(decoded);

            float white_level = decoded[ref_idx].info.white_level;
            uint32_t cfa_period = 1;

            // Build minimal RawImage vector just for metadata access in alignment
            std::vector<RawImage> raw_wrappers;
            raw_wrappers.reserve(decoded.size());
            for (const auto& d : decoded)
            {
                RawImage ri;
                ri.metadata.white_level = static_cast<uint32_t>(d.info.white_level);
                ri.metadata.iso_exposure_time = d.info.iso_exposure_time;
                ri.metadata.exposure_bias = d.info.exposure_bias;
                std::memcpy(ri.metadata.black_level, d.info.black_level, sizeof(float) * 4);
                ri.metadata.mosaic_pattern_width = 0;
                raw_wrappers.push_back(std::move(ri));
            }

            Report(progress, PipelineConstants::kProgressAlignStart, "Aligning frames (RGB)");
            std::vector<FloatImage> aligned = BuildAlignedComparisons(float_images, raw_wrappers, ref_idx,
                settings_, cfa_period, progress);

            FloatImage merged;
            if (settings_.merge_algo == MergeAlgorithm::TemporalAverage)
            {
                Report(progress, PipelineConstants::kProgressMerge, "Merging frames (temporal average)");
                TemporalDenoiseParams params;
                params.strength = settings_.noise_reduction;
                params.white_level = white_level;
                params.black_level = 0.0f;
                params.num_scales = static_cast<uint32_t>(aligned.size());
                params.exposure_scales = nullptr;
                merged = TemporalAverage(float_images[ref_idx], aligned, params);
            } else if (settings_.merge_algo == MergeAlgorithm::Frequency)
            {
                Report(progress, PipelineConstants::kProgressMerge, "Merging frames (frequency)");
                FrequencyMergeParams params;
                params.mode = settings_.frequency_mode;
                params.noise_reduction = settings_.noise_reduction;
                params.tile_size = settings_.tile_size;
                params.white_level = white_level;
                params.black_level = 0.0f;
                params.num_scales = static_cast<uint32_t>(aligned.size());
                params.exposure_scales = nullptr;
                merged = FrequencyMerge(float_images[ref_idx], aligned, params);
            } else
            {
                Report(progress, PipelineConstants::kProgressMerge, "Merging frames (spatial)");
                SpatialMergeParams params;
                params.mode = settings_.spatial_mode;
                params.noise_reduction = settings_.noise_reduction;
                params.robustness = ComputeRobustness(settings_.noise_reduction);
                float estimated_noise = EstimateNoiseFloor(float_images[ref_idx], 1);
                float formula_noise = std::max(PipelineConstants::kNoiseFloorMin,
                    settings_.noise_reduction * PipelineConstants::kNoiseFormulaMul);
                params.noise_floor = std::min(estimated_noise, formula_noise);
                params.highlight_threshold = white_level * PipelineConstants::kHighlightFactor;
                params.clip_threshold = white_level * PipelineConstants::kClipFactor;
                params.guide_block_size = 2;
                params.num_scales = static_cast<uint32_t>(aligned.size());
                params.exposure_scales = nullptr;
                merged = SpatialMerge(float_images[ref_idx], aligned, params);
            }

            std::string output_path = output_path_or_dir;
            if (output_path.empty())
            {
                output_path = "burstmerge_output.png";
            }
            else
            {
                // Determine if path looks like a file (has extension) or directory
                std::string ext = std::filesystem::path(output_path).extension().string();
                bool has_ext = !ext.empty();
                if (!has_ext)
                {
                    // Treat as directory: create and append default filename
                    std::error_code ec;
                    std::filesystem::create_directories(output_path, ec);
                    output_path = (std::filesystem::path(output_path) / "burstmerge_output.png").string();
                }
                else
                {
                    // Treat as file path: ensure parent dir exists
                    std::error_code ec;
                    auto parent = std::filesystem::path(output_path).parent_path();
                    if (!parent.empty())
                    {
                        std::filesystem::create_directories(parent, ec);
                    }
                }
            }

            io::WriteImage(output_path, merged, decoded, settings_);

            Report(progress, PipelineConstants::kProgressDone, "Done");
            if (ProfileEnabled())
            {
                std::fprintf(stderr, "%s", BuildProfileReport().c_str());
                std::fflush(stderr);
            }
            return {true, output_path, ""};
        }

        // ---- RAW pipeline ----
        std::string output_path = ResolveOutputPath(output_path_or_dir);
        Report(progress, 0.02f, "Preparing inputs");
        convert_dir_.clear();
        std::vector<std::string> dng_paths = PrepareDngInputs(input_paths, output_path, progress, convert_dir_);
        if (dng_paths.empty()) throw std::runtime_error("No readable DNG inputs");

        Report(progress, PipelineConstants::kProgressDecodeStart, "Reading and decoding DNG files");
        std::vector<RawImage> images;
        images.reserve(dng_paths.size());
        for (size_t i = 0; i < dng_paths.size(); ++i)
        {
            DngReader reader(dng_paths[i].c_str());
            images.push_back(reader.Read());
            float p = PipelineConstants::kProgressDecodeStart + PipelineConstants::kProgressDecodeRange * static_cast<float>(i + 1) / static_cast<float>(dng_paths.size());
            Report(progress, p, "Decoded image " + std::to_string(i + 1) + "/" + std::to_string(dng_paths.size()));
        }
        Report(progress, PipelineConstants::kProgressRefFrame, "Selecting reference frame");
        size_t ref_idx = SelectExposureRefIndex(images);
        Report(progress, PipelineConstants::kProgressRefSelected, "Reference frame selected: " + std::to_string(ref_idx + 1) + "/" + std::to_string(images.size()));

        Report(progress, PipelineConstants::kProgressHotpixel, "Repairing hot pixels");
        std::vector<FloatImage> float_images = BuildFloatImages(images);
        uint32_t hotpixel_period = (float_images.empty() || float_images[0].channels <= 1)
            ? images[0].metadata.mosaic_pattern_width
            : 1u;
        RepairHotPixels(float_images,
                        static_cast<float>(images[0].metadata.white_level),
                        images[0].metadata.black_level,
                        hotpixel_period);
        // Log the CFA pattern so we can verify channel ordering
        if (images[ref_idx].metadata.mosaic_pattern_width > 0)
        {
            uint32_t pw = images[ref_idx].metadata.mosaic_pattern_width;
            char buf[128];
            int n = std::snprintf(buf, sizeof(buf), "CFA pattern (%ux%u):", pw, pw);
            for (uint32_t i = 0; i < pw * pw; ++i)
            {
                n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %u",
                    static_cast<unsigned>(images[ref_idx].metadata.mosaic_pattern[i]));
            }
            Report(progress, PipelineConstants::kProgressCfaLog, std::string(buf));
        }

        Report(progress, PipelineConstants::kProgressNormalize, "Normalizing frames (black level & exposure)");
        NormalizeFrames(float_images, images, ref_idx);
        for (size_t i = 1; i < images.size(); ++i)
        {
            if (!IsCompatibleForAverage(images[0], images[i]))
            {
                throw std::runtime_error("Input images differ in dimensions, format, or stride");
            }
        }

        uint32_t cfa_period = images[ref_idx].metadata.mosaic_pattern_width;
        std::vector<FloatImage> aligned = BuildAlignedComparisons(float_images, images, ref_idx, settings_, cfa_period, progress);
        float ref_iso = images[ref_idx].metadata.iso_exposure_time;
        float ref_bias = images[ref_idx].metadata.exposure_bias;
        std::vector<float> exp_scales;
        exp_scales.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            if (i == ref_idx) continue;
            float comp_iso = images[i].metadata.iso_exposure_time;
            if (ref_iso > 0.0f && comp_iso > 0.0f)
            {
                exp_scales.push_back((ref_iso / comp_iso) *
                    std::pow(2.0f, ref_bias - images[i].metadata.exposure_bias));
            } else
            {
                exp_scales.push_back(1.0f);
            }
        }

        FloatImage merged;
        //
        // Merge algorithm selection: three mutually exclusive paths.
        // Exposure scales (for clipped-pixel detection / temporal weighting)
        // are computed unconditionally above.
        //
        if (settings_.merge_algo == MergeAlgorithm::TemporalAverage)
        {
            // TemporalAverage: simple exposure-weighted frame average.
            // noise_reduction is ignored — averaging is averaging.
            Report(progress, PipelineConstants::kProgressMerge, "Merging frames with temporal average");
            TemporalDenoiseParams params;
            params.strength = settings_.noise_reduction;   // stored but unused by TemporalAverage
            params.white_level = static_cast<float>(images[ref_idx].metadata.white_level);
            params.black_level = MeanBlackLevel(images[ref_idx].metadata);
            params.num_scales = static_cast<uint32_t>(exp_scales.size());
            params.exposure_scales = exp_scales.data();
            merged = TemporalAverage(float_images[ref_idx], aligned, params);
        } else if (settings_.merge_algo == MergeAlgorithm::Frequency)
        {
            Report(progress, PipelineConstants::kProgressMerge, "Merging frames with frequency path");
            FrequencyMergeParams params;
            params.mode = settings_.frequency_mode;
            params.noise_reduction = settings_.noise_reduction;
            params.tile_size = settings_.tile_size;
            params.white_level = static_cast<float>(images[ref_idx].metadata.white_level);
            params.black_level = MeanBlackLevel(images[ref_idx].metadata);
            params.num_scales = static_cast<uint32_t>(exp_scales.size());
            params.exposure_scales = exp_scales.data();
            merged = FrequencyMerge(float_images[ref_idx], aligned, params);
        } else
        {
            Report(progress, PipelineConstants::kProgressMerge, "Merging frames with spatial path");
            SpatialMergeParams params;
            params.mode = settings_.spatial_mode;
            params.noise_reduction = settings_.noise_reduction;
            params.robustness = ComputeRobustness(settings_.noise_reduction);
            float estimated_noise = EstimateNoiseFloor(float_images[ref_idx], std::max<uint32_t>(1, cfa_period));
            float formula_noise = std::max(PipelineConstants::kNoiseFloorMin, settings_.noise_reduction * PipelineConstants::kNoiseFormulaMul);
            // Assertion: auto-estimate must not exceed formula value.
            // Dark reference frames (Bkt) can produce inflated noise floor,
            // which disables the robust weight formula and causes blur.
            params.noise_floor = std::min(estimated_noise, formula_noise);
            float avg_bl = MeanBlackLevel(images[ref_idx].metadata);
            params.highlight_threshold = (static_cast<float>(images[ref_idx].metadata.white_level) - avg_bl) * PipelineConstants::kHighlightFactor;
            params.clip_threshold = (static_cast<float>(images[ref_idx].metadata.white_level) - avg_bl) * PipelineConstants::kClipFactor;
            params.guide_block_size = images[ref_idx].metadata.mosaic_pattern_width >= 2
                ? images[ref_idx].metadata.mosaic_pattern_width
                : 2;
            params.num_scales = static_cast<uint32_t>(exp_scales.size());
            params.exposure_scales = exp_scales.data();
            merged = SpatialMerge(float_images[ref_idx], aligned, params);
        }

// Compute bit-depth rescaling factor (must happen in black-subtracted space)
        float ref_bl = MeanBlackLevel(images[ref_idx].metadata);
        uint32_t sensor_white = images[ref_idx].metadata.white_level;
        uint32_t target_white = ResolveTargetWhiteLevel(settings_.dng_bit_depth,
                                                        sensor_white);
        float bit_scale = (target_white != sensor_white && sensor_white > 0)
            ? static_cast<float>(target_white) / static_cast<float>(sensor_white)
            : 1.0f;

        // Black level restore: exposure correction (LocalReinhard) uses a single
        // mean black level for multi-channel images, so we must first add back
        // ref_bl (mean), let exposure do its work, then inject per-channel delta
        // afterward.  This way the DNG pixel data matches per-channel BlackLevel
        // metadata without confusing the tone mapper.
        if (ref_bl > 1.0f)
        {
            float scaled_bl = ref_bl * bit_scale;
            if (bit_scale != 1.0f)
            {
                for (float& v : merged.data)
                {
                    v = v * bit_scale + scaled_bl;
                }
            } else
            {
                for (float& v : merged.data) v += ref_bl;
            }
        } else
        {
            if (bit_scale != 1.0f)
            {
                for (float& v : merged.data) v *= bit_scale;
            }
        }
        if (settings_.exposure_mode != ExposureMode::Off || settings_.exposure_stops != 0.0f)
        {
            Report(progress, PipelineConstants::kProgressExposure, "Exposure correction");
            ExposureParams params;
            params.mode = settings_.exposure_mode;
            params.curve_mode = settings_.exposure_curve_mode;
            params.stops = settings_.exposure_stops;
            params.mosaic_pattern_width = images[ref_idx].metadata.mosaic_pattern_width;
            for (int i = 0; i < 4; ++i) params.black_level[i] = images[ref_idx].metadata.black_level[i] * bit_scale;
            // Exposure correction operates on data that already has black restored,
            // so the white_level passed must be the final (rescaled) one.
            ApplyExposure(merged, target_white, params);
        }

        // After exposure (which uses mean black level for multi-channel images),
        // inject per-channel delta so final pixel data matches per-channel
        // BlackLevel metadata.  Only needed when the image is still in plane
        // layout (4 channels) and per-channel black levels differ from the mean.
        if (merged.channels == 4 && ref_bl > 1.0f)
        {
            float bl_ch[4] =
            {};
            bool has_per_channel = false;
            for (int i = 0; i < 4 && i < static_cast<int>(merged.channels); ++i)
            {
                float v = images[ref_idx].metadata.black_level[i];
                if (v > 0.0f)
                { bl_ch[i] = v; has_per_channel = true; }
                else
                { bl_ch[i] = ref_bl; }
            }
            if (has_per_channel)
            {
                float delta[4];
                for (int i = 0; i < 4; ++i) delta[i] = bl_ch[i] - ref_bl;
                for (uint32_t y = 0; y < merged.height; ++y)
                {
                    for (uint32_t x = 0; x < merged.width; ++x)
                    {
                        size_t base = (static_cast<size_t>(y) * merged.width + x) * 4;
                        for (uint32_t c = 0; c < 4; ++c)
                        {
                            merged.data[base + c] += delta[c];
                        }
                    }
                }
            }
        }

        if (images[ref_idx].metadata.mosaic_pattern_width > 1 &&
            merged.channels == images[ref_idx].metadata.mosaic_pattern_width * images[ref_idx].metadata.mosaic_pattern_width)
            {
            merged = ConvertPlaneImageToMosaic(merged,
                                               images[ref_idx].metadata.width,
                                               images[ref_idx].metadata.height,
                                               images[ref_idx].metadata.mosaic_pattern_width);
        }

        Report(progress, PipelineConstants::kProgressQuantize, "Quantizing float image to UInt16");
        HostBuffer averaged = FloatImageToUint16HostBuffer(merged, target_white);

        Report(progress, PipelineConstants::kProgressContainer, "Preparing output DNG container");
        RawImage output;
        output.metadata = std::move(images[ref_idx].metadata);
        output.metadata.white_level = target_white;
        if (bit_scale != 1.0f && ref_bl > 1.0f)
        {
            for (int i = 0; i < 4; ++i)
            {
                if (output.metadata.black_level[i] > 0.0f)
                {
                    output.metadata.black_level[i] *= bit_scale;
                }
            }
        }
        output.pixels = std::move(averaged);

        Report(progress, PipelineConstants::kProgressWrite, "Writing output DNG file");
        io::SetDngWhiteLevel(output.metadata.dng_negative, target_white);
        if (bit_scale != 1.0f && ref_bl > 1.0f)
        {
            float scaled_bl[4];
            // Use the (already-moved) output metadata's black_level (which was scaled above)
            // to avoid reading from the moved-from images[ref_idx].
            for (int i = 0; i < 4; ++i)
            {
                scaled_bl[i] = output.metadata.black_level[i];
            }
            io::SetDngBlackLevel(output.metadata.dng_negative, scaled_bl);
        }
        DngWriter writer(output.metadata.dng_negative);
        writer.Write(output_path.c_str(), output);

        Report(progress, PipelineConstants::kProgressDone, "Done");
        result =
        {true, output_path, ""};
    } catch (const std::exception& e)
    {
        result =
        {false, "", e.what()};
    } catch (...)
    {
        result =
        {false, "", "Unknown processing error"};
    }
    CleanupConvertDir();
    return result;
}

void PipelineOrchestrator::CleanupConvertDir()
{
    if (!convert_dir_.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(convert_dir_, ec);
        std::filesystem::path parent = std::filesystem::path(convert_dir_).parent_path();
        convert_dir_.clear();
        if (std::filesystem::exists(parent) &&
            std::filesystem::is_empty(parent, ec))
            {
            std::filesystem::remove(parent, ec);
        }
    }

    if (ProfileEnabled())
    {
        std::fprintf(stderr, "%s", BuildProfileReport().c_str());
        std::fflush(stderr);
    }
}

} // namespace burstmerge
