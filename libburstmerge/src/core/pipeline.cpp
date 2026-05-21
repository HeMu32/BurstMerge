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
#include "burstmerge/internal/merge/frequency.h"
#include "burstmerge/internal/merge/spatial.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <vector>

namespace burstmerge
{
namespace
{

using ProgressFn = PipelineOrchestrator::ProgressFn;

struct DebugRegion
{
    int mosaic_x;
    int mosaic_y;
    int mosaic_w;
    int mosaic_h;
};

void Report(const ProgressFn& progress, float percent, const std::string& stage)
{
    if (progress) progress(percent, stage);
}

void WriteLe16(std::FILE* f, uint16_t v)
{
    const uint8_t b[2] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu)
    };
    std::fwrite(b, 1, 2, f);
}

void WriteLe32(std::FILE* f, uint32_t v)
{
    const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu)
    };
    std::fwrite(b, 1, 4, f);
}

void SaveGrayBmp(const char* path, const FloatImage& gray, float white_level)
{
    if (gray.channels != 1 || gray.width == 0 || gray.height == 0) return;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;

    const uint32_t width = gray.width;
    const uint32_t height = gray.height;
    const uint32_t row_bytes = width * 4u;
    const uint32_t pixel_bytes = row_bytes * height;
    const uint32_t dib_size = 124u;
    const uint32_t file_size = 14u + dib_size + pixel_bytes;
    const uint32_t pixel_offset = 14u + dib_size;

    std::fwrite("BM", 1, 2, f);
    WriteLe32(f, file_size);
    WriteLe16(f, 0);
    WriteLe16(f, 0);
    WriteLe32(f, pixel_offset);
    WriteLe32(f, dib_size);
    WriteLe32(f, width);
    WriteLe32(f, height);
    WriteLe16(f, 1);
    WriteLe16(f, 32);
    WriteLe32(f, 3u);
    WriteLe32(f, pixel_bytes);
    WriteLe32(f, 2835u);
    WriteLe32(f, 2835u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0x00FF0000u);
    WriteLe32(f, 0x0000FF00u);
    WriteLe32(f, 0x000000FFu);
    WriteLe32(f, 0xFF000000u);
    WriteLe32(f, 0x57696E20u);
    for (int i = 0; i < 9; ++i) WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);

    const float denom = std::max(1.0f, white_level);
    std::vector<uint8_t> row(static_cast<size_t>(row_bytes), 0);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            float v = gray.At(x, static_cast<uint32_t>(y), 0) / denom;
            v = std::max(0.0f, std::min(1.0f, v));
            const uint8_t g = static_cast<uint8_t>(std::lround(v * 255.0f));
            const size_t base = static_cast<size_t>(x) * 4u;
            row[base + 0] = g;
            row[base + 1] = g;
            row[base + 2] = g;
            row[base + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[DIAG] Saved stage BMP: %s (%ux%u)\n", path, width, height);
}

void DumpStageImage(const char* stage, const FloatImage& img, float white_level)
{
    char path[160];
    if (img.channels == 1)
    {
        std::snprintf(path, sizeof(path), "R:\\%s.bmp", stage);
        SaveGrayBmp(path, img, white_level);
        return;
    }

    if (img.channels == 4)
    {
        FloatImage avg;
        avg.width = img.width;
        avg.height = img.height;
        avg.channels = 1;
        avg.data.assign(static_cast<size_t>(img.width) * img.height, 0.0f);
        for (uint32_t y = 0; y < img.height; ++y)
        {
            for (uint32_t x = 0; x < img.width; ++x)
            {
                float s = 0.0f;
                for (uint32_t c = 0; c < 4; ++c) s += img.At(x, y, c);
                avg.At(x, y, 0) = s * 0.25f;
            }
        }
        std::snprintf(path, sizeof(path), "R:\\%s_avg.bmp", stage);
        SaveGrayBmp(path, avg, white_level);

        for (uint32_t c = 0; c < 4; ++c)
        {
            FloatImage plane;
            plane.width = img.width;
            plane.height = img.height;
            plane.channels = 1;
            plane.data.assign(static_cast<size_t>(img.width) * img.height, 0.0f);
            for (uint32_t y = 0; y < img.height; ++y)
            {
                for (uint32_t x = 0; x < img.width; ++x)
                {
                    plane.At(x, y, 0) = img.At(x, y, c);
                }
            }
            std::snprintf(path, sizeof(path), "R:\\%s_ch%u.bmp", stage, c);
            SaveGrayBmp(path, plane, white_level);
        }
    }
}

void DumpDiffImage(const char* stage, const FloatImage& ref, const FloatImage& img, float white_level)
{
    if (ref.width != img.width || ref.height != img.height || ref.channels != img.channels) return;

    if (ref.channels == 1)
    {
        FloatImage diff;
        diff.width = ref.width;
        diff.height = ref.height;
        diff.channels = 1;
        diff.data.assign(static_cast<size_t>(ref.width) * ref.height, 0.0f);
        for (uint32_t y = 0; y < ref.height; ++y)
        {
            for (uint32_t x = 0; x < ref.width; ++x)
            {
                diff.At(x, y, 0) = std::abs(img.At(x, y, 0) - ref.At(x, y, 0));
            }
        }
        char path[160];
        std::snprintf(path, sizeof(path), "R:\\%s.bmp", stage);
        SaveGrayBmp(path, diff, std::max(1.0f, white_level * 0.25f));
        return;
    }

    if (ref.channels == 4)
    {
        FloatImage avg;
        avg.width = ref.width;
        avg.height = ref.height;
        avg.channels = 1;
        avg.data.assign(static_cast<size_t>(ref.width) * ref.height, 0.0f);
        for (uint32_t y = 0; y < ref.height; ++y)
        {
            for (uint32_t x = 0; x < ref.width; ++x)
            {
                float s = 0.0f;
                for (uint32_t c = 0; c < 4; ++c)
                {
                    s += std::abs(img.At(x, y, c) - ref.At(x, y, c));
                }
                avg.At(x, y, 0) = s * 0.25f;
            }
        }
        char path[160];
        std::snprintf(path, sizeof(path), "R:\\%s_avg.bmp", stage);
        SaveGrayBmp(path, avg, std::max(1.0f, white_level * 0.25f));

        for (uint32_t c = 0; c < 4; ++c)
        {
            FloatImage plane;
            plane.width = ref.width;
            plane.height = ref.height;
            plane.channels = 1;
            plane.data.assign(static_cast<size_t>(ref.width) * ref.height, 0.0f);
            for (uint32_t y = 0; y < ref.height; ++y)
            {
                for (uint32_t x = 0; x < ref.width; ++x)
                {
                    plane.At(x, y, 0) = std::abs(img.At(x, y, c) - ref.At(x, y, c));
                }
            }
            std::snprintf(path, sizeof(path), "R:\\%s_ch%u.bmp", stage, c);
            SaveGrayBmp(path, plane, std::max(1.0f, white_level * 0.25f));
        }
    }
}

void PrintRegionPlaneBias(const char* label,
                          const FloatImage& ref,
                          const FloatImage& img)
{
    if (ref.channels != 4 || img.channels != 4 || ref.width != img.width || ref.height != img.height)
    {
        return;
    }

    const DebugRegion regions[] = {
        {1384, 2126, 4, 32},
        {1337, 1968, 5, 6},
        {549,  2161, 6, 10},
        {506,  2469, 7, 7},
    };

    for (size_t ri = 0; ri < sizeof(regions) / sizeof(regions[0]); ++ri)
    {
        const int px = regions[ri].mosaic_x / 2;
        const int py = regions[ri].mosaic_y / 2;
        const int pw = std::max(1, regions[ri].mosaic_w / 2);
        const int ph = std::max(1, regions[ri].mosaic_h / 2);
        const int x0 = std::max(0, px - 2);
        const int y0 = std::max(0, py - 2);
        const int x1 = std::min<int>(static_cast<int>(ref.width), px + pw + 2);
        const int y1 = std::min<int>(static_cast<int>(ref.height), py + ph + 2);

        double sum_d[4] = {};
        double sum_abs[4] = {};
        uint64_t n = 0;
        for (int y = y0; y < y1; ++y)
        {
            for (int x = x0; x < x1; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    double d = static_cast<double>(img.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(c))) -
                               static_cast<double>(ref.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(c)));
                    sum_d[c] += d;
                    sum_abs[c] += std::abs(d);
                }
                ++n;
            }
        }
        if (n == 0) continue;

        const double mean_r = sum_d[0] / static_cast<double>(n);
        const double mean_g1 = sum_d[1] / static_cast<double>(n);
        const double mean_g2 = sum_d[2] / static_cast<double>(n);
        const double mean_b = sum_d[3] / static_cast<double>(n);
        const double mean_g = 0.5 * (mean_g1 + mean_g2);
        const double rb_minus_g = 0.5 * (mean_r + mean_b) - mean_g;

        std::fprintf(stderr,
            "[DIAG] %s region%zu plane(%d,%d %dx%d): dMean=[R %.3f G1 %.3f G2 %.3f B %.3f] avgAbs=[R %.3f G1 %.3f G2 %.3f B %.3f] rb_minus_g=%.3f\n",
            label, ri + 1, px, py, pw, ph,
            mean_r, mean_g1, mean_g2, mean_b,
            sum_abs[0] / static_cast<double>(n),
            sum_abs[1] / static_cast<double>(n),
            sum_abs[2] / static_cast<double>(n),
            sum_abs[3] / static_cast<double>(n),
            rb_minus_g);
    }
}

void PrintRegionConfidenceStats(const char* label, const FloatImage& conf)
{
    if (conf.channels != 1) return;

    const DebugRegion regions[] = {
        {1384, 2126, 4, 32},
        {1337, 1968, 5, 6},
        {549,  2161, 6, 10},
        {506,  2469, 7, 7},
    };

    for (size_t ri = 0; ri < sizeof(regions) / sizeof(regions[0]); ++ri)
    {
        const int px = regions[ri].mosaic_x / 2;
        const int py = regions[ri].mosaic_y / 2;
        const int pw = std::max(1, regions[ri].mosaic_w / 2);
        const int ph = std::max(1, regions[ri].mosaic_h / 2);
        const int x0 = std::max(0, px - 2);
        const int y0 = std::max(0, py - 2);
        const int x1 = std::min<int>(static_cast<int>(conf.width), px + pw + 2);
        const int y1 = std::min<int>(static_cast<int>(conf.height), py + ph + 2);
        double sum = 0.0;
        uint64_t n = 0;
        uint64_t low = 0;
        for (int y = y0; y < y1; ++y)
        {
            for (int x = x0; x < x1; ++x)
            {
                const float v = conf.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0);
                sum += v;
                if (v < 0.5f) ++low;
                ++n;
            }
        }
        if (!n) continue;
        std::fprintf(stderr,
            "[DIAG] %s region%zu conf_mean=%.3f low_frac=%.3f\n",
            label, ri + 1,
            sum / static_cast<double>(n),
            static_cast<double>(low) / static_cast<double>(n));
    }
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
        const bool stage_debug = (dng_paths.size() == 1 || dng_paths.size() == 3);
        const size_t debug_stage_idx = dng_paths.size() == 1 ? 0u : 1u;
        if (stage_debug && images.size() > debug_stage_idx)
        {
            FloatImage raw0 = HostBufferToFloatImage(images[debug_stage_idx].pixels);
            DumpStageImage("stage1_read_raw", raw0, static_cast<float>(images[debug_stage_idx].metadata.white_level));
        }

        Report(progress, PipelineConstants::kProgressRefFrame, "Selecting reference frame");
        size_t ref_idx = SelectExposureRefIndex(images);
        Report(progress, PipelineConstants::kProgressRefSelected, "Reference frame selected: " + std::to_string(ref_idx + 1) + "/" + std::to_string(images.size()));

        Report(progress, PipelineConstants::kProgressHotpixel, "Repairing hot pixels");
        std::vector<FloatImage> float_images = BuildFloatImages(images);
        if (stage_debug && float_images.size() > debug_stage_idx)
        {
            DumpStageImage("stage2_mosaic_to_plane", float_images[debug_stage_idx], static_cast<float>(images[debug_stage_idx].metadata.white_level));
        }
        uint32_t hotpixel_period = (float_images.empty() || float_images[0].channels <= 1)
            ? images[0].metadata.mosaic_pattern_width
            : 1u;
        RepairHotPixels(float_images,
                        static_cast<float>(images[0].metadata.white_level),
                        images[0].metadata.black_level,
                        hotpixel_period);
        if (stage_debug && float_images.size() > debug_stage_idx)
        {
            DumpStageImage("stage3_after_hotpixel", float_images[debug_stage_idx], static_cast<float>(images[debug_stage_idx].metadata.white_level));
        }

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
        if (stage_debug && float_images.size() > debug_stage_idx)
        {
            DumpStageImage("stage4_after_normalize", float_images[debug_stage_idx], static_cast<float>(images[debug_stage_idx].metadata.white_level));
        }

        for (size_t i = 1; i < images.size(); ++i)
        {
            if (!IsCompatibleForAverage(images[0], images[i]))
            {
                throw std::runtime_error("Input images differ in dimensions, format, or stride");
            }
        }

        uint32_t cfa_period = images[ref_idx].metadata.mosaic_pattern_width;
        std::vector<FloatImage> confidence_maps;
        std::vector<FloatImage> aligned = BuildAlignedComparisons(float_images, images, ref_idx, settings_, cfa_period, progress,
                                                                  &confidence_maps);
        if (float_images[ref_idx].channels == 4)
        {
            for (size_t i = 0; i < aligned.size(); ++i)
            {
                char tag[128];
                std::snprintf(tag, sizeof(tag), "premarge_diff_cmp%zu_vs_ref", i);
                DumpDiffImage(tag, float_images[ref_idx], aligned[i], static_cast<float>(images[ref_idx].metadata.white_level));
            }
        }
        if (ref_idx < float_images.size() && float_images[ref_idx].channels == 4)
        {
            for (size_t i = 0; i < aligned.size(); ++i)
            {
                char tag[64];
                std::snprintf(tag, sizeof(tag), "aligned_cmp%zu_vs_ref", i);
                PrintRegionPlaneBias(tag, float_images[ref_idx], aligned[i]);
            }
        }
        for (size_t i = 0; i < confidence_maps.size(); ++i)
        {
            char tag[64];
            std::snprintf(tag, sizeof(tag), "conf_cmp%zu", i);
            PrintRegionConfidenceStats(tag, confidence_maps[i]);
        }

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
            params.num_confidence_maps = static_cast<uint32_t>(confidence_maps.size());
            params.confidence_maps = confidence_maps.empty() ? nullptr : confidence_maps.data();
            merged = SpatialMerge(float_images[ref_idx], aligned, params);
        }

// Compute bit-depth rescaling factor (must happen in black-subtracted space)
        float ref_bl = MeanBlackLevel(images[ref_idx].metadata);
        uint32_t sensor_white = images[ref_idx].metadata.white_level;
        uint32_t target_white = sensor_white;
        switch (settings_.dng_bit_depth)
        {
            case 12: target_white = 4095;  break;
            case 14: target_white = sensor_white; break;
            case 16: target_white = 65535; break;
            default: target_white = sensor_white; break;
        }
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

        if (ref_idx < float_images.size() && float_images[ref_idx].channels == 4 && merged.channels == 4)
        {
            PrintRegionPlaneBias("merged_vs_ref", float_images[ref_idx], merged);
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
