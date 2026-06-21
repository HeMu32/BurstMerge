#include "burstmerge/internal/core/pipeline.h"

#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/gpu_pipeline.h"
#include "burstmerge/internal/core/pipeline_align.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/pipeline_io.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"
#include "burstmerge/internal/denoise/temporal.h"
#include "burstmerge/internal/exposure/exposure.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/io/image_decoder.h"
#include "burstmerge/internal/io/image_writer.h"
#include "burstmerge/internal/merge/frequency.h"
#include "burstmerge/internal/merge/spatial.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
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

uint32_t ResolveTargetWhiteLevel(int bit_depth, uint32_t sensor_white)
{
    if (bit_depth <= 0)
    {
        std::fprintf(stderr, "[WARN] ResolveTargetWhiteLevel: requested bit_depth=%d <= 0; falling back to sensor white level %u\n", bit_depth, sensor_white);
        return sensor_white;
    }

    if (bit_depth > 16)
    {   // illegal for current DNG container - keep sensor white level unchanged
        std::fprintf(stderr, "[WARN] ResolveTargetWhiteLevel: requested bit_depth=%d out of range [1..16]; falling back to sensor white level %u\n", bit_depth, sensor_white);
        return sensor_white;
    }

    // Supported bit depths: compute target white level directly.
    return (1u << bit_depth) - 1u;
}

} // unnamed namespace

// RAW extensions known to the Adobe DNG Converter
bool IsRawExtension(const std::string& ext)
{
    static const char* raw_exts[] =
    {
        ".dng", ".arw", ".cr2", ".cr3", ".nef", ".nrw",
        ".orf", ".raf", ".rw2", ".pef", ".srw", ".x3f",
        ".sr2", ".srf", ".kdc", ".dcr", ".k25", ".mdc",
        ".mef", ".mrw", ".iiq", ".eip", ".bay", ".3fr",
        ".fff", ".mos"
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

namespace {
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
        else throw std::runtime_error("Unsupported input file format: " + p);
    }
    if (has_raw && has_rgb) return InputClass::Mixed;
    if (has_rgb) return InputClass::Rgb;
    return InputClass::RAW;
}

// Format-aware output path resolution, shared by all three pipeline paths.
//   - If path has an extension → treat as explicit file path.
//     If the extension does not match the selected format, it is silently
//     corrected to the canonical extension for that format.
//   - If no extension → treat as directory, create it, and append
//     "burstmerge_output.<fmt_ext>".
static const char* CanonicalExt(OutputFormat fmt)
{
    switch (fmt)
    {
        case OutputFormat::PNG:  return ".png";
        case OutputFormat::JPEG: return ".jpg";
        case OutputFormat::BMP:  return ".bmp";
        case OutputFormat::TIFF: return ".tif";
        case OutputFormat::DNG:  return ".dng";
        default:
        case OutputFormat::Auto: return ".png";
    }
}

static std::string ResolveImageOutputPath(const std::string& output_path_or_dir,
                                           OutputFormat fmt)
{
    const char* def_ext = CanonicalExt(fmt);

    std::filesystem::path out(output_path_or_dir.empty() ? "." : output_path_or_dir);

    std::string ext = out.extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (!ext.empty())
    {
        // Correct mismatched extension when format is explicitly known (not Auto).
        if (fmt != OutputFormat::Auto)
        {
            bool match = false;
            switch (fmt)
            {
                case OutputFormat::JPEG:
                    match = (ext == ".jpg" || ext == ".jpeg");
                    break;
                case OutputFormat::TIFF:
                    match = (ext == ".tif" || ext == ".tiff");
                    break;
                default:
                    match = (ext == std::string(def_ext));
                    break;
            }
            if (!match)
                out.replace_extension(def_ext);
        }

        std::error_code ec;
        if (out.has_parent_path())
            std::filesystem::create_directories(out.parent_path(), ec);
        return out.string();
    }

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    return (out / ("burstmerge_output" + std::string(def_ext))).string();
}

static std::vector<uint8_t> ReadFileToMemory(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Failed to open file: " + path);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buf.data()), buf.size()))
        throw std::runtime_error("Failed to read file: " + path);
    return buf;
}

} // namespace

PipelineOrchestrator::PipelineOrchestrator(BackendType backend, Settings settings)
    : backend_(backend), settings_(settings)
{
    if (settings_.tile_size < PipelineConstants::kMinTileSize)
        settings_.tile_size = PipelineConstants::kMinTileSize;
}

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
        if (backend_ != BackendType::CPU && backend_ != BackendType::Vulkan)
        {
            return
            {false, "", "Unsupported backend"};
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
                // Keep RAW files + unknown extension files (they will fail
                // in PrepareDngInputs with a clear error, rather than being
                // silently dropped - see audit A.3/C.4).
                if (IsRawExtension(ext) || ext == ".dng" || !IsImageExtension(ext))
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
                ri.metadata.ev_value = d.info.ev_value;
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
            } else if (settings_.merge_algo == MergeAlgorithm::TemporalMedian)
            {
                Report(progress, PipelineConstants::kProgressMerge, "Merging frames (temporal median)");
                TemporalDenoiseParams params;
                params.strength = settings_.noise_reduction;
                params.white_level = white_level;
                params.black_level = 0.0f;
                params.num_scales = static_cast<uint32_t>(aligned.size());
                params.exposure_scales = nullptr;
                merged = TemporalMedian(float_images[ref_idx], aligned, params);
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

            // Resolve output format (never Auto at this point).
            OutputFormat eff_fmt;
            if (settings_.output_format != OutputFormat::Auto)
            {
                eff_fmt = settings_.output_format;
            } else
            {
                OutputFormat fallback = io::InferOutputFormat(settings_, false);
                eff_fmt = io::InferFormatFromExtension(output_path_or_dir, fallback);
                if (eff_fmt != fallback)
                {
                    Report(progress, PipelineConstants::kProgressMerge,
                        "Warning: output format not specified - inferred "
                        + std::string(io::OutputFormatToString(eff_fmt))
                        + " from filename extension");
                } else
                {
                    Report(progress, PipelineConstants::kProgressMerge,
                        "Warning: output format not specified - defaulting to "
                        + std::string(io::OutputFormatToString(eff_fmt)));
                }
            }
            std::string output_path = ResolveImageOutputPath(output_path_or_dir, eff_fmt);
            io::WriteImage(output_path, merged, decoded, settings_);

            {
                std::fprintf(stderr, "\nOutput: %s\n", output_path.c_str());
                std::fprintf(stderr, "  Format:   %s\n", io::OutputFormatToString(eff_fmt));
                std::fprintf(stderr, "  Bit depth: %u\n", settings_.bit_depth);
                if (!decoded.empty())
                {
                    const auto& tags = decoded[0].info.tags;
                    for (const auto& [key, val] : tags)
                    {
                        std::fprintf(stderr, "  %s: %s\n", key.c_str(), val.c_str());
                    }
                }
            }

            Report(progress, PipelineConstants::kProgressDone, "Done");
            if (ProfileEnabled())
            {
                std::fprintf(stderr, "%s", BuildProfileReport().c_str());
                std::fflush(stderr);
            }
            return {true, output_path, ""}; //////////////////////////////////////////////
        }

        // ---- RAW pipeline ----
        // Use output_path_or_dir as conversion context only;
        // final output path is resolved after format decision below.
        std::string convert_ctx = output_path_or_dir.empty() ? "." : output_path_or_dir;
        Report(progress, 0.02f, "Preparing inputs");
        convert_dir_.clear();
        std::vector<std::string> dng_paths;
        { ProfileScope _ps("time.pipeline.prepare_dng_inputs");
        dng_paths = PrepareDngInputs(input_paths, convert_ctx, progress, convert_dir_);
        }
        if (dng_paths.empty()) throw std::runtime_error("No readable DNG inputs");

// DNG read Phase 1: read all DNG files into memory (sequential I/O)
        constexpr float kReadFraction = 0.3f;
        const float kReadEnd = PipelineConstants::kProgressDecodeStart +
                               PipelineConstants::kProgressDecodeRange * kReadFraction;
        Report(progress, PipelineConstants::kProgressDecodeStart, "Reading DNG files");
        std::vector<std::vector<uint8_t>> file_buffers(dng_paths.size());
        for (size_t i = 0; i < dng_paths.size(); ++i)
        {
            file_buffers[i] = ReadFileToMemory(dng_paths[i]);
            float p = PipelineConstants::kProgressDecodeStart +
                      (kReadEnd - PipelineConstants::kProgressDecodeStart) *
                          static_cast<float>(i + 1) / static_cast<float>(dng_paths.size());
            Report(progress, p, "Read file " + std::to_string(i + 1) + "/" + std::to_string(dng_paths.size()));
        }

// DNG read Phase 2: decode all DNGs from memory in parallel (CPU-bound)
        const float kDecodeStart = kReadEnd;
        Report(progress, kDecodeStart, "Decoding DNG files");
        std::vector<RawImage> images(dng_paths.size());
        {
        ProfileScope _ps("time.pipeline.decode_dng");
            std::atomic<int> decoded_count{0};
            std::mutex pm;
            ParallelFor(dng_paths.size(), 1, [&](size_t begin, size_t end)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    images[i] = ReadDngFromBuffer(file_buffers[i].data(),
                                                   static_cast<uint32_t>(file_buffers[i].size()));
                    int done = decoded_count.fetch_add(1) + 1;
                    {
                        std::lock_guard<std::mutex> lock(pm);
                        float p = kDecodeStart +
                                  (PipelineConstants::kProgressDecodeStart +
                                   PipelineConstants::kProgressDecodeRange - kDecodeStart) *
                                      static_cast<float>(done) / static_cast<float>(dng_paths.size());
                            Report(progress, p,
                                   "Decoded image " + std::to_string(done) + "/" + std::to_string(dng_paths.size()));
                    }
                }
            }, "decode_dng" /* named tag for profiler */);
        }
        Report(progress, PipelineConstants::kProgressRefFrame, "Selecting reference frame");
        size_t ref_idx = SelectExposureRefIndex(images);
        Report(progress, PipelineConstants::kProgressRefSelected, "Reference frame selected: " + std::to_string(ref_idx + 1) + "/" + std::to_string(images.size()));
        FloatImage merged;
        if (backend_ == BackendType::Vulkan)
        {
            // GPU-native pipeline: prepare / align / merge all on Vulkan compute.
            // Falls through to the shared CPU tail (bit-depth / exposure / DNG).
            merged = GpuRunBurstPipeline(images, ref_idx, settings_, progress);
        }
        else
        {
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
        float ref_ev = images[ref_idx].metadata.ev_value;
        float ref_bias = images[ref_idx].metadata.exposure_bias;
        std::vector<float> exp_scales;
        exp_scales.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            if (i == ref_idx) continue;
            float comp_ev = images[i].metadata.ev_value;
            if (ref_ev > 0.0f && comp_ev > 0.0f)
            {
                exp_scales.push_back((ref_ev / comp_ev) *
                    std::pow(2.0f, ref_bias - images[i].metadata.exposure_bias));
            } else
            {
                exp_scales.push_back(1.0f);
            }
        }

        //
        // Merge algorithm selection: three mutually exclusive paths.
        // Exposure scales (for clipped-pixel detection / temporal weighting)
        // are computed unconditionally above.
        //
        if (settings_.merge_algo == MergeAlgorithm::TemporalAverage)
        {
            // TemporalAverage: simple exposure-weighted frame average.
            // noise_reduction is ignored - averaging is averaging.
            Report(progress, PipelineConstants::kProgressMerge, "Merging frames with temporal average");
            TemporalDenoiseParams params;
            params.strength = settings_.noise_reduction;   // stored but unused by TemporalAverage
            params.white_level = static_cast<float>(images[ref_idx].metadata.white_level);
            params.black_level = MeanBlackLevel(images[ref_idx].metadata);
            params.num_scales = static_cast<uint32_t>(exp_scales.size());
            params.exposure_scales = exp_scales.data();
            merged = TemporalAverage(float_images[ref_idx], aligned, params);
        } else if (settings_.merge_algo == MergeAlgorithm::TemporalMedian)
        {
            // TemporalMedian: per-pixel median across all frames.
            // Robust to outliers; noise_reduction / exposure_scales unused.
            Report(progress, PipelineConstants::kProgressMerge, "Merging frames with temporal median");
            TemporalDenoiseParams params;
            params.strength = settings_.noise_reduction;
            params.white_level = static_cast<float>(images[ref_idx].metadata.white_level);
            params.black_level = MeanBlackLevel(images[ref_idx].metadata);
            params.num_scales = static_cast<uint32_t>(exp_scales.size());
            params.exposure_scales = exp_scales.data();
            merged = TemporalMedian(float_images[ref_idx], aligned, params);
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
            std::fprintf(stderr, "[DBG] CPU spatial noise_floor est=%.4f formula=%.4f -> %.4f\n", estimated_noise, formula_noise, params.noise_floor);
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
        }

// Compute bit-depth rescaling factor (must happen in black-subtracted space)
        float ref_bl = MeanBlackLevel(images[ref_idx].metadata);
        uint32_t sensor_white = images[ref_idx].metadata.white_level;
        uint32_t target_white = ResolveTargetWhiteLevel(settings_.bit_depth,
                                                        sensor_white);
        float bit_scale = (target_white != sensor_white && sensor_white > 0)
            ? static_cast<float>(target_white) / static_cast<float>(sensor_white)
            : 1.0f;

        // -----------------------------------------------------------------
        // Bit-depth rescaling and black-level handling.
        //
        // There are two output formats depending on requested bit depth:
        //
        // 1. LOW BIT DEPTH (<=10): Black-subtracted pixel data + BlackLevel=0.
        //    ACR/Lightroom has a rendering bug when WhiteLevel is low (<=1023)
        //    and BlackLevel is non-zero: shadows are flooded with red because
        //    the black subtraction is applied incorrectly.  By baking the black
        //    offset into the pixel data and writing BlackLevel=0 we avoid the
        //    bug.  WhiteLevel is reduced to (sensor_white - ref_bl)*bit_scale.
        //
        // 2. HIGH BIT DEPTH (>10): Original black-restored format.
        //    Pixel data = raw * bit_scale + ref_bl*bit_scale.
        //    BlackLevel = ref_bl * bit_scale (non-zero).
        //    WhiteLevel = sensor_white * bit_scale.
        //    This is the mathematically correct DNG representation and works
        //    fine with ACR because the quantization step is fine enough that
        //    the bug does not manifest.
        //
        // NOTE on per-channel delta code:
        // After exposure correction we inject per-channel black-level deltas
        // when merged.channels==4.  BuildFloatImages() converts Bayer mosaic
        // into a 4-plane (R,Gr,Gb,B) image, so merged.channels is always 4
        // at this point (before ConvertPlaneImageToMosaic below).  The delta
        // is skipped for low bit depth because the zero-black path bakes the
        // offset uniformly into pixel data.
        // -----------------------------------------------------------------
        bool use_zero_black = (settings_.bit_depth <= 10);

        if (use_zero_black)
        {
            // Pixel data is already black-subtracted from NormalizeFrames().
            // Just apply the bit-depth scaling; do NOT restore the black offset.
            if (bit_scale != 1.0f)
            {
                for (float& v : merged.data) v *= bit_scale;
            }
        }
        else
        {
            // Original path (bit_depth > 10): restore the mean black level so
            // that the DNG pixel values include the black offset.  The decoder
            // will subtract it later using the BlackLevel metadata tag.
            if (ref_bl > 1.0f)
            {
                float scaled_bl = ref_bl * bit_scale;
                if (bit_scale != 1.0f)
                {
                    for (float& v : merged.data)
                    {
                        v = v * bit_scale + scaled_bl;
                    }
                }
                else
                {
                    for (float& v : merged.data) v += ref_bl;
                }
            }
            else
            {
                if (bit_scale != 1.0f)
                {
                    for (float& v : merged.data) v *= bit_scale;
                }
            }
        }

        // Exposure correction ------------------------------------------------
        if (settings_.exposure_mode != ExposureMode::Off || settings_.exposure_stops != 0.0f)
        {
            Report(progress, PipelineConstants::kProgressExposure, "Exposure correction");
            ExposureParams params;
            params.mode = settings_.exposure_mode;
            params.curve_mode = settings_.exposure_curve_mode;
            params.stops = settings_.exposure_stops;
            params.mosaic_pattern_width = images[ref_idx].metadata.mosaic_pattern_width;
            if (use_zero_black)
            {
                // Black is already subtracted; tell ApplyExposure there is no
                // black offset.  white_level must match the rescaled range.
                for (int i = 0; i < 4; ++i) params.black_level[i] = 0.0f;
                uint32_t exposure_white = static_cast<uint32_t>(std::lround(
                    (static_cast<float>(sensor_white) - ref_bl) * bit_scale));
                ApplyExposure(merged, exposure_white, params);
            }
            else
            {
                // Original path: black level is present in pixel data.
                for (int i = 0; i < 4; ++i) params.black_level[i] = images[ref_idx].metadata.black_level[i] * bit_scale;
                ApplyExposure(merged, target_white, params);
            }
        }

        // Per-channel black-level delta for 4-plane images (Bayer demosaiced).
        // Only needed when the image is still in plane layout and per-channel
        // black levels differ from the mean.  Skipped for low bit depth because
        // the zero-black path bakes the offset into pixel data uniformly.
        if (!use_zero_black && merged.channels == 4 && ref_bl > 1.0f)
        {
            float bl_ch[4] = {};
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
                for (int i = 0; i < 4; ++i) delta[i] = (bl_ch[i] - ref_bl) * bit_scale;
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

        // Determine effective output format (never Auto at this point).
        OutputFormat eff_fmt;
        if (settings_.output_format != OutputFormat::Auto)
        {
            eff_fmt = settings_.output_format;
        } else
        {
            OutputFormat fallback = io::InferOutputFormat(settings_, true);
            eff_fmt = io::InferFormatFromExtension(output_path_or_dir, fallback);
            if (eff_fmt != fallback)
            {
                Report(progress, PipelineConstants::kProgressMerge,
                    "Warning: output format not specified - inferred "
                    + std::string(io::OutputFormatToString(eff_fmt))
                    + " from filename extension");
            } else
            {
                Report(progress, PipelineConstants::kProgressMerge,
                    "Warning: output format not specified - defaulting to "
                    + std::string(io::OutputFormatToString(eff_fmt)));
            }
        }
        bool want_dng = (eff_fmt == OutputFormat::DNG);

        // Resolve output path after format decision (see audit A.1)
        std::string output_path = ResolveImageOutputPath(output_path_or_dir, eff_fmt);

        // -----------------------------------------------------------------
        // DNG output.
        // -----------------------------------------------------------------
        if (want_dng)
        {
            Report(progress, PipelineConstants::kProgressQuantize, "Quantizing float image to UInt16");
            HostBuffer averaged = FloatImageToUint16HostBuffer(merged, target_white);

            Report(progress, PipelineConstants::kProgressContainer, "Preparing output DNG container");
            RawImage output;
            output.metadata = std::move(images[ref_idx].metadata);
            output.metadata.white_level = target_white;

            if (use_zero_black)
            {
                // Low bit depth: write BlackLevel=0 and adjust WhiteLevel to
                // the rescaled dynamic range (sensor_white - ref_bl)*bit_scale.
                // The pixel data already has the black offset baked in via
                // NormalizeFrames(), so the decoder sees BlackLevel=0 and does
                // not attempt to subtract anything further.
                for (int i = 0; i < 4; ++i) output.metadata.black_level[i] = 0.0f;
                if (ref_bl > 1.0f && bit_scale != 1.0f)
                {
                    output.metadata.white_level = static_cast<uint32_t>(std::lround(
                        (static_cast<float>(sensor_white) - ref_bl) * bit_scale));
                }
            }
            else
            {
                // High bit depth: original behaviour.  Scale the metadata
                // BlackLevel values by bit_scale so they match the rescaled
                // pixel data.  The decoder subtracts these values to recover
                // the linear light signal.
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
            }
            output.pixels = std::move(averaged);

            Report(progress, PipelineConstants::kProgressWrite, "Writing output DNG file");
            io::SetDngWhiteLevel(output.metadata.dng_negative, output.metadata.white_level);
            if (use_zero_black)
            {
                // Force BlackLevel to 0 in the DNG SDK negative so the
                // resulting DNG tag is exactly [0,0,0,0].
                float zero_bl[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                io::SetDngBlackLevel(output.metadata.dng_negative, zero_bl);
            }
            else if (bit_scale != 1.0f && ref_bl > 1.0f)
            {
                float scaled_bl[4];
                for (int i = 0; i < 4; ++i)
                {
                    scaled_bl[i] = output.metadata.black_level[i];
                }
                io::SetDngBlackLevel(output.metadata.dng_negative, scaled_bl);
            }
            DngWriter writer(output.metadata.dng_negative);
            writer.Write(output_path.c_str(), output);
        }
        else
        {
            // RAW → non-DNG: write Bayer mosaic grayscale with white-point scaling
            Report(progress, PipelineConstants::kProgressQuantize, "Writing RAW as non-DNG (Bayer mosaic)");

            io::DecodedImage raw_decoded;
            raw_decoded.info.width     = images[ref_idx].metadata.width;
            raw_decoded.info.height    = images[ref_idx].metadata.height;
            raw_decoded.info.pix_fmt   = io::kPixelGray;
            raw_decoded.info.bit_depth = settings_.bit_depth;
            raw_decoded.info.is_raw    = true;
            raw_decoded.info.white_level = static_cast<float>(target_white);
            raw_decoded.info.ev_value = images[ref_idx].metadata.ev_value;
            raw_decoded.info.exposure_bias     = images[ref_idx].metadata.exposure_bias;
            raw_decoded.pixels = merged.data;

            // Build single-element input vector so WriteImage can infer format
            std::vector<io::DecodedImage> raw_inputs;
            raw_inputs.push_back(raw_decoded);

            io::WriteImage(output_path, merged, raw_inputs, settings_);
        }

        {
            std::fprintf(stderr, "\nOutput: %s\n", output_path.c_str());
            std::fprintf(stderr, "  Format:   %s\n", io::OutputFormatToString(eff_fmt));
            std::fprintf(stderr, "  Bit depth: %u\n", settings_.bit_depth);
        }

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
