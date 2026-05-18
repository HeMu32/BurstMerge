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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace burstmerge
{
namespace
{

using ProgressFn = PipelineOrchestrator::ProgressFn;

void Report(const ProgressFn& progress, float percent, const std::string& stage)
{
    if (progress) progress(percent, stage);
}

std::string LowerExt(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool IsDngPath(const std::string& path)
{
    return LowerExt(std::filesystem::path(path)) == ".dng";
}

bool LooksLikeDngOutputPath(const std::string& path)
{
    return IsDngPath(path);
}

std::string ResolveOutputPath(const std::string& output_path_or_dir)
{
    std::filesystem::path out(output_path_or_dir.empty() ? "." : output_path_or_dir);
    if (LooksLikeDngOutputPath(out.string()))
    {
        if (out.has_parent_path())
        {
            std::filesystem::create_directories(out.parent_path());
        }
        return out.string();
    }

    std::filesystem::create_directories(out);
    return (out / "burstmerge_output.dng").string();
}

std::string GenerateRunId()
{
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "run_%lu_%llx", pid, tick);
    return buf;
#else
    auto pid = static_cast<unsigned long>(::getpid());
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "run_" + std::to_string(pid) + "_" + std::to_string(now);
#endif
}

void OrphanSweep(const std::filesystem::path& parent)
{
    auto max_age = PipelineConstants::kOrphanMaxAge;
    if (!std::filesystem::exists(parent)) return;
    auto now = std::filesystem::file_time_type::clock::now();
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(parent, ec))
    {
        if (ec)
        { ec.clear(); continue; }
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind("run_", 0) != 0) continue;
        auto ft = entry.last_write_time(ec);
        if (ec)
        { ec.clear(); continue; }
        auto age = now - ft;
        if (age > max_age)
        {
            std::filesystem::remove_all(entry.path(), ec);
            ec.clear();
        }
    }
}

std::string MakeTempConvertDir(const std::string& output_path)
{
    std::filesystem::path base(output_path);
    std::filesystem::path parent = base.has_parent_path() ? base.parent_path() : std::filesystem::current_path();
    std::filesystem::path dir = parent / "burstmerge_converted";
    std::filesystem::create_directories(dir);
    // Sweep orphan directories from crashed runs (older than 24 hours)
    OrphanSweep(dir);
    // Create a per-process run directory so parallel CLI processes don't collide
    std::filesystem::path run_dir = dir / GenerateRunId();
    std::filesystem::create_directories(run_dir);
    return run_dir.string();
}

std::vector<std::string> PrepareDngInputs(const std::vector<std::string>& input_paths,
                                          const std::string& output_path,
                                          const ProgressFn& progress,
                                          std::string& out_convert_dir)
{
    std::vector<std::string> dng_paths;
    std::vector<std::string> raw_paths;
    dng_paths.reserve(input_paths.size());
    out_convert_dir.clear();

    Report(progress, PipelineConstants::kProgressValidate, "Validating input files");
    for (const auto& path : input_paths)
    {
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error("Input does not exist: " + path);
        }
        if (IsDngPath(path)) dng_paths.push_back(path);
        else raw_paths.push_back(path);
    }

    if (raw_paths.empty()) return dng_paths;

#ifdef _WIN32
    Report(progress, PipelineConstants::kProgressConvertStart, "Preparing RAW to DNG conversion");
    out_convert_dir = MakeTempConvertDir(output_path);
    std::vector<std::string> converted;
    Report(progress, PipelineConstants::kProgressConvertStart + 0.02f, "Converting " + std::to_string(raw_paths.size()) + " RAW file(s) to DNG");
    if (!RunAdobeDngConverter(raw_paths, out_convert_dir, converted))
    {
        out_convert_dir.clear();
        throw std::runtime_error("Adobe DNG Converter failed or timed out");
    }
    Report(progress, PipelineConstants::kProgressConvertEnd, "RAW to DNG conversion completed");
    dng_paths.insert(dng_paths.end(), converted.begin(), converted.end());
    return dng_paths;
#else
    throw std::runtime_error("Non-DNG RAW input requires pre-conversion on this platform");
#endif
}

bool IsCompatibleForAverage(const RawImage& a, const RawImage& b)
{
    return a.pixels.width == b.pixels.width &&
           a.pixels.height == b.pixels.height &&
           a.pixels.format == b.pixels.format &&
           a.pixels.row_stride == b.pixels.row_stride;
}

float ComputeRobustness(float noise_reduction)
{
    return std::max(PipelineConstants::kRobustnessMin, noise_reduction / PipelineConstants::kRobustnessDiv);
}

float EstimateNoiseFloor(const FloatImage& image, uint32_t guide_block_size)
{
    if (image.data.empty()) return 8.0f;

    const int blur_radius = 2;
    const FloatImage blurred = BoxBlur(image, blur_radius);
    const uint32_t step = std::max<uint32_t>(1, guide_block_size);

    double sum_sq = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 0; y < image.height; y += step)
    {
        for (uint32_t x = 0; x < image.width; x += step)
        {
            size_t idx = (static_cast<size_t>(y) * image.width + x) * image.channels;
            for (uint32_t c = 0; c < image.channels; ++c)
            {
                float d = image.data[idx + c] - blurred.data[idx + c];
                sum_sq += static_cast<double>(d) * static_cast<double>(d);
                ++count;
            }
        }
    }

    if (count == 0) return 8.0f;
    float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
    // Floor at 8.0 DN: prevents degenerate behavior in the weight formula
    // when the reference frame is unusually flat. RMS for any real sensor
    // frame (even dark) exceeds this minimum. The estimate runs before
    // bit-depth rescaling, so this value is in sensor-native DN and valid
    // for 12/14/16-bit sources.
    return std::max(PipelineConstants::kNoiseFloorMin, rms);
}

float MeanBlackLevel(const RawMetadata& meta)
{
    float sum = 0.0f;
    int n = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (meta.black_level[i] > 0.0f)
        { sum += meta.black_level[i]; ++n; }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

void NormalizeFrames(std::vector<FloatImage>& float_images,
                     const std::vector<RawImage>& raw_images,
                     size_t ref_idx)
{
    float ref_iso = raw_images[ref_idx].metadata.iso_exposure_time;

    for (size_t i = 0; i < float_images.size(); ++i)
    {
        const auto& meta = raw_images[i].metadata;
        FloatImage& img = float_images[i];

        // Remove each frame's own black level (per-frame ADC offset)
        float bl = MeanBlackLevel(meta);
        if (bl > 1.0f)
        {
            for (float& v : img.data) v -= bl;
        }

        if (i == ref_idx) continue;

        // Exposure-normalize comparison frames to the reference's exposure
        float comp_iso = meta.iso_exposure_time;
        if (ref_iso > 0.0f && comp_iso > 0.0f)
        {
            float scale = (ref_iso / comp_iso) *
                          std::pow(2.0f, raw_images[ref_idx].metadata.exposure_bias - meta.exposure_bias);
            if (std::abs(scale - 1.0f) > 0.001f)
            {
                for (float& v : img.data) v *= scale;
            }
        }
    }
}

std::vector<FloatImage> BuildFloatImages(const std::vector<RawImage>& images)
{
    std::vector<FloatImage> out;
    out.reserve(images.size());
    for (const auto& img : images)
    {
        FloatImage fi = HostBufferToFloatImage(img.pixels);
        if (img.metadata.mosaic_pattern_width > 1 && fi.channels == 1)
        {
            fi = ConvertMosaicToPlaneImage(fi, img.metadata.mosaic_pattern_width);
        }
        out.push_back(std::move(fi));
    }
    return out;
}

std::vector<FloatImage> BuildAlignedComparisons(const std::vector<FloatImage>& float_images,
                                                const std::vector<RawImage>& raw_images,
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
    params.mode = settings.alignment_mode;
    // On plane images (channels > 1) the CFA phase is already separated per channel,
    // so the alignment does not need period snapping. Fall back to cfa_period=1.
    params.cfa_period = (float_images[0].channels > 1) ? 1u : std::max<uint32_t>(1, cfa_period);

    // NOTE: Advanced dense alignment in this pipeline is still under development.
    // Current behavior is experimental and does not yet guarantee robust results
    // across all bracketed-exposure scenes.
    if (settings.alignment_mode == AlignmentMode::DenseTile)
    {
        std::fprintf(stderr,
            "[WARN] Advanced dense alignment is experimental and not fully implemented yet. Results may be unstable.\n");
    }

    auto align_and_warp = [&](const FloatImage& guide_ref,
                              const FloatImage& source,
                              size_t progress_idx,
                              size_t total_count) -> FloatImage
                              {
        // Align on grayscale (average of all colour planes), then warp the
        // full colour-plane image using the result.  This reduces alignment
        // computation to 1/4 of the per-plane approach with negligible loss
        // of alignment accuracy (the HDR+ paper uses the same strategy).
        const FloatImage gray_ref = ConvertPlanesToGrayscale(guide_ref);
        const FloatImage gray_src = ConvertPlanesToGrayscale(source);

        Report(progress,
               PipelineConstants::kProgressAlignStart + PipelineConstants::kProgressAlignRange * static_cast<float>(progress_idx) / static_cast<float>(std::max<size_t>(1, total_count)),
               "Aligning frame " + std::to_string(progress_idx + 1) + "/" + std::to_string(total_count));
        AlignmentResult ar = EstimateTranslation(gray_ref, gray_src, params);

        Report(progress,
               PipelineConstants::kProgressWarpStart + PipelineConstants::kProgressWarpRange * static_cast<float>(progress_idx) / static_cast<float>(std::max<size_t>(1, total_count)),
               "Warping frame " + std::to_string(progress_idx + 1) + "/" + std::to_string(total_count));
        return WarpAligned(source, ar);
    };

    bool has_exposure = false;
    float min_exp = std::numeric_limits<float>::max();
    float max_exp = 0.0f;
    std::vector<std::pair<float, size_t>> exposure_order;
    exposure_order.reserve(raw_images.size());
    for (size_t i = 0; i < raw_images.size(); ++i)
    {
        float v = raw_images[i].metadata.iso_exposure_time;
        if (v > 0.0f)
        {
            has_exposure = true;
            min_exp = std::min(min_exp, v);
            max_exp = std::max(max_exp, v);
            exposure_order.push_back(
            {v, i});
        }
    }

    const bool use_transmission = settings.alignment_mode == AlignmentMode::DenseTile &&
                                  has_exposure &&
                                  !exposure_order.empty() &&
                                  max_exp > min_exp * std::pow(2.0f, PipelineConstants::kBracketTransmissionFallbackEv);

    if (!use_transmission)
    {
        const FloatImage& ref = float_images[ref_idx];
        size_t processed = 0;
        const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
        for (size_t i = 0; i < float_images.size(); ++i)
        {
            if (i == ref_idx) continue;
            aligned.push_back(align_and_warp(ref, float_images[i], processed, total));
            ++processed;
        }
        return aligned;
    }

    std::sort(exposure_order.begin(), exposure_order.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });

    const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
    size_t root_pos = 0;
    for (size_t pos = 0; pos < exposure_order.size(); ++pos)
    {
        if (exposure_order[pos].second == ref_idx)
        {
            root_pos = pos;
            break;
        }
    }
    const size_t root_idx = exposure_order[root_pos].second;
    std::vector<FloatImage> aligned_to_root(float_images.size());
    std::vector<uint8_t> has_aligned(float_images.size(), 0);
    aligned_to_root[root_idx] = float_images[root_idx];
    has_aligned[root_idx] = 1;

    size_t processed = 0;

    for (size_t pos = root_pos; pos > 0; --pos)
    {
        size_t parent_idx = exposure_order[pos].second;
        size_t child_idx = exposure_order[pos - 1].second;
        const FloatImage& parent_ref = has_aligned[parent_idx] ? aligned_to_root[parent_idx] : float_images[parent_idx];
        FloatImage child_aligned = align_and_warp(parent_ref, float_images[child_idx], processed, total);
        aligned_to_root[child_idx] = std::move(child_aligned);
        has_aligned[child_idx] = 1;
        ++processed;
    }

    for (size_t pos = root_pos + 1; pos < exposure_order.size(); ++pos)
    {
        size_t parent_idx = exposure_order[pos - 1].second;
        size_t child_idx = exposure_order[pos].second;
        const FloatImage& parent_ref = has_aligned[parent_idx] ? aligned_to_root[parent_idx] : float_images[parent_idx];
        FloatImage child_aligned = align_and_warp(parent_ref, float_images[child_idx], processed, total);
        aligned_to_root[child_idx] = std::move(child_aligned);
        has_aligned[child_idx] = 1;
        ++processed;
    }

    for (size_t i = 0; i < float_images.size(); ++i)
    {
        if (i == ref_idx) continue;
        if (has_aligned[i]) aligned.push_back(std::move(aligned_to_root[i]));
        else aligned.push_back(align_and_warp(float_images[ref_idx], float_images[i], processed, total));
    }
    return aligned;
}

size_t SelectExposureRefIndex(const std::vector<RawImage>& images)
{
    if (images.empty()) return 0;

    bool has_exposure = false;
    float min_exp = std::numeric_limits<float>::max();
    float max_exp = 0.0f;
    for (const auto& img : images)
    {
        float v = img.metadata.iso_exposure_time;
        if (v > 0.0f)
        {
            has_exposure = true;
            min_exp = std::min(min_exp, v);
            max_exp = std::max(max_exp, v);
        }
    }

    if (has_exposure && max_exp > min_exp * 1.25f)
    {
        std::vector<std::pair<float, size_t>> exposure_order;
        exposure_order.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            float v = images[i].metadata.iso_exposure_time;
            if (v > 0.0f) exposure_order.push_back(
            {v, i});
        }
        if (!exposure_order.empty())
        {
            std::sort(exposure_order.begin(), exposure_order.end(),
                      [](const auto& a, const auto& b)
                      { return a.first < b.first; });
            // Use the darkest frame as the exposure/output anchor for bracketed sets.
            // This keeps highlight headroom and avoids the brighter mid-exposure
            // reference making the final DNG look globally over-lifted.
            return exposure_order.front().second;
        }
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
    Result result =
    {false, "", ""};
    try
    {
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

        Report(progress, PipelineConstants::kProgressRefFrame, "Selecting reference frame");
        size_t ref_idx = SelectExposureRefIndex(images);
        Report(progress, PipelineConstants::kProgressRefSelected, "Reference frame selected: " + std::to_string(ref_idx + 1) + "/" + std::to_string(images.size()));

        Report(progress, PipelineConstants::kProgressHotpixel, "Repairing hot pixels");
        std::vector<FloatImage> float_images = BuildFloatImages(images);
        uint32_t hotpixel_period = (float_images.empty() || float_images[0].channels <= 1)
            ? images[0].metadata.mosaic_pattern_width
            : 1u;
        RepairHotPixels(float_images, static_cast<float>(images[0].metadata.white_level), hotpixel_period);

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
    if (convert_dir_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(convert_dir_, ec);
    // Also remove the parent burstmerge_converted/ if it became empty
    std::filesystem::path parent = std::filesystem::path(convert_dir_).parent_path();
    convert_dir_.clear();
    if (std::filesystem::exists(parent) &&
        std::filesystem::is_empty(parent, ec))
        {
        std::filesystem::remove(parent, ec);
    }
}

} // namespace burstmerge
