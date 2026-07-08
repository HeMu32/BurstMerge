#include "burstmerge/api.h"
#include "burstmerge/internal/core/chroma_effects.h"
#include "burstmerge/internal/core/dither.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/image_resize.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/io/dng_sdk_bridge_resize.h"
#include "cxxopts.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// ---- Diagnostic stage-timer -------------------------------------------------
// Same env-var and debug/release semantics as the project-wide profiler (see
// profiler.cpp::ProfileEnabled) and the chroma-effects timing (see
// chroma_effects.cpp `ChromaTimingEnabled`):
//
//   * In a Release build (NDEBUG defined) the entire StageTimer struct and
//     the macro STAGE_TIMER expand to nothing, so the resize utility has
//     zero overhead and stays bit-identical to a build that never had them.
//   * In a Debug build, the tier is selected at RUNTIME by the
//     BURSTMERGE_PROFILE environment variable. When it is unset or set to
//     "0", the timers are constructed but report nothing. When set to any
//     other value, each StageTimer prints a stage-tag and wall-clock
//     millisecond count on scope exit to stderr (so stdout progress
//     messages stay clean).
//
// This unifies the resize tool's diagnostics with the rest of the project:
// one env var controls both StageTimer in main.cpp and ChromaTimer inside
// chroma_effects.cpp.
#ifndef NDEBUG

inline bool StageTimingEnabled()
{
    static int enabled = []()
    {
        const char* env = std::getenv("BURSTMERGE_PROFILE");
        return (env && env[0] && env[0] != '0') ? 1 : 0;
    }();
    return enabled != 0;
}

struct StageTimer
{
    const char* tag;
    std::chrono::steady_clock::time_point t0;
    explicit StageTimer(const char* t) : tag(t), t0(std::chrono::steady_clock::now()) {}
    ~StageTimer()
    {
        if (!StageTimingEnabled()) return;
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr, "[stage-timing] %-36s %8lld ms\n", tag, static_cast<long long>(ms));
    }
};
#define STAGE_TIMER(tag) StageTimer _stage_t(tag)

#else  // NDEBUG

#define STAGE_TIMER(tag) ((void)0)

#endif  // NDEBUG

bool ParseInterpolation(const std::string& value, burstmerge::InterpolationMethod& out)
{
    std::string v = value;
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "bilinear")   { out = burstmerge::InterpolationMethod::Bilinear; return true; }
    if (v == "bicubic")     { out = burstmerge::InterpolationMethod::Bicubic; return true; }
    if (v == "area" || v == "area-average" || v == "area_average")
        { out = burstmerge::InterpolationMethod::AreaAverage; return true; }
    if (v == "gauss" || v == "gaussian" || v == "gaussian-area" || v == "gauss-area")
        { out = burstmerge::InterpolationMethod::GaussianArea; return true; }
    if (v == "50percent" || v == "half" || v == "half-sample" || v == "half_sample")
        { out = burstmerge::InterpolationMethod::HalfSample; return true; }
    return false;
}

const char* InterpName(burstmerge::InterpolationMethod m)
{
    switch (m)
    {
        case burstmerge::InterpolationMethod::Bilinear:      return "bilinear";
        case burstmerge::InterpolationMethod::Bicubic:       return "bicubic";
        case burstmerge::InterpolationMethod::AreaAverage:   return "area-average";
        case burstmerge::InterpolationMethod::GaussianArea:  return "gaussian-area";
        case burstmerge::InterpolationMethod::HalfSample:    return "50percent";
    }
    return "unknown";
}

bool ParseBitDepth(const std::string& value, int& bit_depth)
{
    try { bit_depth = std::stoi(value); } catch (...) { return false; }
    return (bit_depth == 8 || bit_depth == 10 || bit_depth == 12 || bit_depth == 14 || bit_depth == 16);
}

uint32_t RoundDownToMultiple(uint32_t val, uint32_t multiple)
{
    if (multiple <= 1) return val;
    return (val / multiple) * multiple;
}

std::string LowerExt(const std::string& path)
{
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool IsDngExt(const std::string& ext)
{
    return ext == ".dng";
}

bool IsRawExt(const std::string& ext)
{
    static const char* raw_exts[] =
    {
        ".arw", ".cr2", ".cr3", ".nef", ".nrw",
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

#ifdef _WIN32
std::string MakeTempDir(const std::string& base)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        std::string dir = base + "\\raw_resize_tmp_" + std::to_string(dist(gen));
        if (std::filesystem::create_directories(dir))
            return dir;
    }
    throw std::runtime_error("Failed to create temp directory");
}
#endif

} // namespace

int main(int argc, char* argv[])
{
    cxxopts::Options opts("burstmerge_raw_resize", "Single-frame RAW (DNG) resizer");
    opts.add_options()
        ("i,input", "Input RAW/DNG file path", cxxopts::value<std::string>())
        ("o,output", "Output DNG file path", cxxopts::value<std::string>()->default_value("./out.dng"))
        ("W,width", "Output mosaic width in pixels (must be multiple of CFA period, typically 2)", cxxopts::value<uint32_t>())
        ("H,height", "Output mosaic height in pixels (must be multiple of CFA period, typically 2)", cxxopts::value<uint32_t>())
        ("scale", "Uniform scale factor (alternative to --width/--height)", cxxopts::value<double>())
        ("interp", "Interpolation method: bilinear, bicubic (default), area (area-average; recommended for heavy downscale), gaussian-area >=3x downscale only), 50percent (half-sample; >=2x downscale only)", cxxopts::value<std::string>()->default_value("bicubic"))
        ("bit-depth", "Output bit depth: 8, 10, 12, 14, 16 (default 16)", cxxopts::value<std::string>()->default_value("16"))
        ("pseudo-olpf", "Pseudo optical low-pass filter strength (gaussian pre-blur before downscale to suppress moire; sigma = strength * log2(downscale)). Default 0 (disabled).", cxxopts::value<double>()->default_value("0"))
        ("dither", "TPDF dither amplitude in output LSB applied before uint16 quantisation to decorrelate quantisation noise. Default 0 (disabled). 1.0 = full ±1 LSB TPDF.", cxxopts::value<double>()->default_value("0"))
        ("h,help", "Print help");

    cxxopts::ParseResult args;
    try
    {
        args = opts.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Argument error: " << e.what() << std::endl;
        std::cerr << opts.help() << std::endl;
        return 2;
    }

    if (args.count("help"))
    {
        std::cout << opts.help() << std::endl;
        return 0;
    }

    if (!args.count("input"))
    {
        std::cerr << "No input file. Use -i path/to/file.dng" << std::endl;
        return 2;
    }

    bool has_wh = (args.count("width") && args.count("height"));
    bool has_scale = args.count("scale");
    if (!has_wh && !has_scale)
    {
        std::cerr << "Specify either --width and --height, or --scale" << std::endl;
        return 2;
    }
    if (has_wh && has_scale)
    {
        std::cerr << "Specify either --width/--height or --scale, not both" << std::endl;
        return 2;
    }

    burstmerge::InterpolationMethod interp = burstmerge::InterpolationMethod::Bicubic;
    if (!ParseInterpolation(args["interp"].as<std::string>(), interp))
    {
        std::cerr << "Invalid interpolation method (use bilinear or bicubic)" << std::endl;
        return 2;
    }

    int bit_depth = 16;
    if (!ParseBitDepth(args["bit-depth"].as<std::string>(), bit_depth))
    {
        std::cerr << "Invalid bit depth (use 8, 10, 12, 14, or 16)" << std::endl;
        return 2;
    }

    std::string input_path = args["input"].as<std::string>();
    std::string output_path = args["output"].as<std::string>();

    if (!std::filesystem::exists(input_path))
    {
        std::cerr << "Input file does not exist: " << input_path << std::endl;
        return 1;
    }

    std::string ext = LowerExt(input_path);
    if (!IsDngExt(ext) && !IsRawExt(ext))
    {
        std::cerr << "Unsupported file format: " << ext << std::endl;
        return 2;
    }

    std::cout << "raw_resize" << std::endl;
    std::cout << "  Input:  " << input_path << std::endl;
    std::cout << "  Output: " << output_path << std::endl;
    std::cout << "  Interpolation: " << InterpName(interp) << std::endl;
    std::cout << "  Bit depth: " << bit_depth << std::endl;

    std::string convert_dir;
    std::string dng_path = input_path;

    try
    {
        // ---- Convert to DNG if needed ----
        if (!IsDngExt(ext))
        {
#ifdef _WIN32
            std::cout << "Converting RAW to DNG via Adobe DNG Converter..." << std::endl;
            convert_dir = MakeTempDir(std::filesystem::path(output_path).parent_path().string());
            std::cout << "  Temp dir: " << convert_dir << std::endl;
            std::vector<std::string> single_input = { input_path };
            std::vector<std::string> converted;
            {
                STAGE_TIMER("AdobeDngConverter (ARW -> DNG)");
                if (!burstmerge::RunAdobeDngConverter(single_input, convert_dir, converted) || converted.empty())
                {
                    throw std::runtime_error("Adobe DNG Converter failed or timed out");
                }
            }
            dng_path = converted[0];
            std::cout << "  Converted: " << dng_path << std::endl;
#else
            throw std::runtime_error("Non-DNG RAW input requires pre-conversion on this platform");
#endif
        }

        // ---- Read DNG ----
        // Wrap DngReader construction + Read in a small IIFE block scope so
        // the StageTimer's destructor fires (and prints to stderr) BEFORE the
        // following std::cout metadata log dump — the resize tool's stdout
        // progress (`Source: ...`, `Target mosaic: ...`) arrives right after
        // DNG parsing completes, but the stage timing should land just on its
        // heels rather than after the metadata logging.
        std::cout << "Reading DNG..." << std::endl;
        burstmerge::RawImage raw = [&]() -> burstmerge::RawImage
        {
            STAGE_TIMER("DngReader::Read (parse+decode)");
            burstmerge::DngReader reader(dng_path.c_str());
            return reader.Read();
        }();
        burstmerge::RawMetadata& meta = raw.metadata;

        uint32_t src_mosaic_w = meta.width;
        uint32_t src_mosaic_h = meta.height;
        uint32_t period = meta.mosaic_pattern_width;
        bool is_linear_rgb = (period <= 1);

        std::cout << "  Source: " << src_mosaic_w << "x" << src_mosaic_h;
        if (is_linear_rgb)
            std::cout << " (LinearRaw, " << (raw.pixels.format == burstmerge::PixelFormat::R16_Uint_RGB ? "3ch" : "?") << ")";
        else
            std::cout << " (Bayer mosaic, period=" << period << ")";
        std::cout << std::endl;
        std::cout << "  White level: " << meta.white_level << std::endl;

        // ---- Compute target dimensions ----
        uint32_t dst_mosaic_w, dst_mosaic_h;
        if (has_scale)
        {
            double sf = args["scale"].as<double>();
            if (sf <= 0.0)
            {
                std::cerr << "Scale factor must be positive" << std::endl;
                return 2;
            }
            dst_mosaic_w = static_cast<uint32_t>(std::lround(static_cast<double>(src_mosaic_w) * sf));
            dst_mosaic_h = static_cast<uint32_t>(std::lround(static_cast<double>(src_mosaic_h) * sf));
        }
        else
        {
            dst_mosaic_w = args["width"].as<uint32_t>();
            dst_mosaic_h = args["height"].as<uint32_t>();
        }

        if (dst_mosaic_w == 0 || dst_mosaic_h == 0)
        {
            std::cerr << "Output dimensions must be non-zero" << std::endl;
            return 2;
        }

        if (!is_linear_rgb)
        {
            uint32_t rw = RoundDownToMultiple(dst_mosaic_w, period);
            uint32_t rh = RoundDownToMultiple(dst_mosaic_h, period);
            if (rw != dst_mosaic_w || rh != dst_mosaic_h)
            {
                std::cout << "  Warning: output dimensions rounded down to "
                          << rw << "x" << rh << " (must be multiples of " << period << " for CFA period)"
                          << std::endl;
                dst_mosaic_w = rw;
                dst_mosaic_h = rh;
                if (dst_mosaic_w == 0 || dst_mosaic_h == 0)
                {
                    std::cerr << "Output dimensions too small after rounding" << std::endl;
                    return 2;
                }
            }
        }

        // Output white level fills the entire target bit-depth container, so
        // bit_scale = target_white / sensor_white re-maps sensor LSB into
        // output LSB and the dither amplitude is naturally expressed in
        // output LSB. This matches ResolveTargetWhiteLevel in the main
        // pipeline (libburstmerge/src/core/pipeline.cpp).
        uint32_t target_white = (1u << bit_depth) - 1u;
        if (target_white > 65535) target_white = 65535;
        if (target_white < 1) target_white = 1;

        // Bit-depth scaling factor: rebase float-image values from the sensor
        // (input) LSB scale to the output LSB scale before quantisation. The
        // main pipeline applies this in pipeline.cpp; without it, downscaled
        // bit_depth below input bit_depth would clamp most pixels to the
        // (tiny) output range and dither amplitudes would be expressed in
        // input LSB rather than output LSB.
        const uint32_t sensor_white = meta.white_level;
        const float bit_scale = (sensor_white > 0 && target_white != sensor_white)
            ? static_cast<float>(target_white) / static_cast<float>(sensor_white)
            : 1.0f;


        uint32_t src_plane_w = src_mosaic_w;
        uint32_t src_plane_h = src_mosaic_h;
        uint32_t dst_plane_w = dst_mosaic_w;
        uint32_t dst_plane_h = dst_mosaic_h;
        if (!is_linear_rgb)
        {
            src_plane_w = (src_mosaic_w + period - 1) / period;
            src_plane_h = (src_mosaic_h + period - 1) / period;
            dst_plane_w = dst_mosaic_w / period;
            dst_plane_h = dst_mosaic_h / period;
        }

        std::cout << "  Source plane: " << src_plane_w << "x" << src_plane_h
                  << " x " << (period * period) << "ch" << std::endl;
        std::cout << "  Target mosaic: " << dst_mosaic_w << "x" << dst_mosaic_h;
        if (!is_linear_rgb)
            std::cout << " (plane: " << dst_plane_w << "x" << dst_plane_h
                      << " x " << (period * period) << "ch)";
        std::cout << std::endl;
        std::cout << "  Target white level: " << target_white << std::endl;

        // Gaussian-area algorithm requires a downscale factor of at least 3x
        // (on either axis) for its advantage over plain area-average to
        // materialise; below that, fall back to area-average to avoid the
        // wasted expf() cost and degraded anti-alias approximation.
        burstmerge::InterpolationMethod effective_interp = interp;
        if (effective_interp == burstmerge::InterpolationMethod::GaussianArea)
        {
            const float src_w_eff = is_linear_rgb ? static_cast<float>(src_mosaic_w) : static_cast<float>(src_plane_w);
            const float dst_w_eff = is_linear_rgb ? static_cast<float>(dst_mosaic_w) : static_cast<float>(dst_plane_w);
            const float src_h_eff = is_linear_rgb ? static_cast<float>(src_mosaic_h) : static_cast<float>(src_plane_h);
            const float dst_h_eff = is_linear_rgb ? static_cast<float>(dst_mosaic_h) : static_cast<float>(dst_plane_h);
            const float downscale_x = (dst_w_eff > 0) ? src_w_eff / dst_w_eff : 1.0f;
            const float downscale_y = (dst_h_eff > 0) ? src_h_eff / dst_h_eff : 1.0f;
            const float min_downscale = std::min(downscale_x, downscale_y);
            if (min_downscale < 3.0f)
            {
                std::cout << "  Warning: gaussian-area requires downscale >=3x, "
                          << "current min downscale is "
                          << static_cast<int>(std::lround(min_downscale * 100.0f)) / 100.0f
                          << "x; falling back to area-average" << std::endl;
                effective_interp = burstmerge::InterpolationMethod::AreaAverage;
            }
        }

        // 50-percent algorithm samples only the top-left half of each downscale
        // block; if the min downscale is below 2x the half-block collapses to
        // 0 and the algorithm is degenerate. Fall back to area-average.
        if (effective_interp == burstmerge::InterpolationMethod::HalfSample)
        {
            const float src_w_eff = is_linear_rgb ? static_cast<float>(src_mosaic_w) : static_cast<float>(src_plane_w);
            const float dst_w_eff = is_linear_rgb ? static_cast<float>(dst_mosaic_w) : static_cast<float>(dst_plane_w);
            const float src_h_eff = is_linear_rgb ? static_cast<float>(src_mosaic_h) : static_cast<float>(src_plane_h);
            const float dst_h_eff = is_linear_rgb ? static_cast<float>(dst_mosaic_h) : static_cast<float>(dst_plane_h);
            const float downscale_x = (dst_w_eff > 0) ? src_w_eff / dst_w_eff : 1.0f;
            const float downscale_y = (dst_h_eff > 0) ? src_h_eff / dst_h_eff : 1.0f;
            const float min_downscale = std::min(downscale_x, downscale_y);
            if (min_downscale < 2.0f)
            {
                std::cout << "  Warning: 50percent requires downscale >=2x, "
                          << "current min downscale is "
                          << static_cast<int>(std::lround(min_downscale * 100.0f)) / 100.0f
                          << "x; falling back to area-average" << std::endl;
                effective_interp = burstmerge::InterpolationMethod::AreaAverage;
            }
        }

        // ---- Convert to FloatImage ----
        burstmerge::FloatImage fin = burstmerge::HostBufferToFloatImage(raw.pixels);

        // ---- Optional chromatic-aberration Lo-Fi effect -------------------
        // Driven completely by compile-time macros (see
        // burstmerge/internal/core/chroma_effects.h). All geometry is measured
        // in the input image's own pixel units, so this MUST run before the
        // resize step. The function is a no-op when EFFECT_CA_Enabled is 0.
        {
            STAGE_TIMER("ApplyChromaticEffects (chroma, full-res)");
            burstmerge::ApplyChromaticEffects(fin,
                                               meta.mosaic_pattern_width,
                                               meta.mosaic_pattern,
                                               static_cast<float>(meta.white_level));
        }

        // ---- Process based on CFA type ----
        const float olpf_strength = static_cast<float>(args["pseudo-olpf"].as<double>());
        float olpf_sigma = 0.0f;
        if (olpf_strength > 0.0f)
        {
            const float src_w_eff = is_linear_rgb ? static_cast<float>(src_mosaic_w) : static_cast<float>(src_plane_w);
            const float dst_w_eff = is_linear_rgb ? static_cast<float>(dst_mosaic_w) : static_cast<float>(dst_plane_w);
            const float src_h_eff = is_linear_rgb ? static_cast<float>(src_mosaic_h) : static_cast<float>(src_plane_h);
            const float dst_h_eff = is_linear_rgb ? static_cast<float>(dst_mosaic_h) : static_cast<float>(dst_plane_h);
            const float down_x = (dst_w_eff > 0) ? src_w_eff / dst_w_eff : 1.0f;
            const float down_y = (dst_h_eff > 0) ? src_h_eff / dst_h_eff : 1.0f;
            const float down_avg = 0.5f * (down_x + down_y);
            if (down_avg > 1.5f)
            {
                olpf_sigma = olpf_strength * std::log2(down_avg);
                std::cout << "  Pseudo-OLPF: sigma=" << olpf_sigma
                          << " (strength=" << olpf_strength
                          << ", downscale avg=" << static_cast<int>(std::lround(down_avg * 100.0f)) / 100.0f
                          << "x)" << std::endl;
            }
            else
            {
                std::cout << "  Pseudo-OLPF: skipped (downscale <= 1.5x)" << std::endl;
            }
        }

        burstmerge::FloatImage result;
        if (is_linear_rgb)
        {
            burstmerge::FloatImage src_for_resize = fin;
            if (olpf_sigma > 0.0f)
            {
                std::cout << "Applying pseudo-OLPF (LinearRaw)..." << std::endl;
                STAGE_TIMER("GaussianBlur (OLPF, LinearRaw)");
                src_for_resize = burstmerge::GaussianBlur(fin, olpf_sigma);
            }
            std::cout << "Resizing (LinearRaw, 3ch)..." << std::endl;
            {
                STAGE_TIMER("ResizeImage (LinearRaw)");
                result = burstmerge::ResizeImage(src_for_resize, dst_mosaic_w, dst_mosaic_h, effective_interp);
            }
        }
        else
        {
            std::cout << "Converting mosaic to plane image..." << std::endl;
            burstmerge::FloatImage plane;
            {
                STAGE_TIMER("ConvertMosaicToPlaneImage (pre-resize)");
                plane = burstmerge::ConvertMosaicToPlaneImage(fin, period);
            }

            if (olpf_sigma > 0.0f)
            {
                std::cout << "Applying pseudo-OLPF (plane)..." << std::endl;
                STAGE_TIMER("GaussianBlur (OLPF, plane)");
                plane = burstmerge::GaussianBlur(plane, olpf_sigma);
            }

            std::cout << "Resizing plane image (" << InterpName(effective_interp) << ")..." << std::endl;
            burstmerge::FloatImage resized;
            {
                STAGE_TIMER("ResizeImage (plane, Bayer)");
                resized = burstmerge::ResizeImage(plane, dst_plane_w, dst_plane_h, effective_interp);
            }

            std::cout << "Converting plane back to mosaic..." << std::endl;
            {
                STAGE_TIMER("ConvertPlaneImageToMosaic");
                result = burstmerge::ConvertPlaneImageToMosaic(resized, dst_mosaic_w, dst_mosaic_h, period);
            }
        }

        // ---- Bit-depth scale (must run before dither and quantisation so
        //      pixel values and dither amplitude are both in output LSB) ----
        if (bit_scale != 1.0f)
        {
            std::cout << "Applying bit-depth scale (x" << bit_scale
                      << ", sensor_white=" << sensor_white
                      << " -> target_white=" << target_white << ")..." << std::endl;
            for (float& v : result.data) v *= bit_scale;
        }

        // ---- Optional dither (operates on output-LSB-scaled float image,
        //      before uint16 quantisation) ----
        const float dither_amp = static_cast<float>(args["dither"].as<double>());
        if (dither_amp > 0.0f)
        {
            std::cout << "Applying dither (amplitude=" << dither_amp << " LSB)..." << std::endl;
            STAGE_TIMER("ApplyQuantizationDither");
            burstmerge::ApplyQuantizationDither(result, dither_amp);
        }

        // ---- Convert back to uint16 ----
        std::cout << "Quantizing to uint16..." << std::endl;
        burstmerge::HostBuffer averaged;
        {
            STAGE_TIMER("FloatImageToUint16HostBuffer (quantize)");
            averaged = burstmerge::FloatImageToUint16HostBuffer(result, target_white);
        }

        // ---- Prepare output RawImage ----
        burstmerge::RawImage output;
        output.metadata = std::move(raw.metadata);
        output.metadata.width = dst_mosaic_w;
        output.metadata.height = dst_mosaic_h;
        output.metadata.white_level = target_white;
        if (bit_scale != 1.0f)
        {
            for (int i = 0; i < 4; ++i)
                output.metadata.black_level[i] = meta.black_level[i] * bit_scale;
        }
        output.pixels = std::move(averaged);

        if (is_linear_rgb || result.channels == 3)
        {
            burstmerge::io::ClearDngMosaicInfo(output.metadata.dng_negative);
        }

        // ---- Update geometry metadata for resized dimensions ----
        burstmerge::io::SetDngDimensions(output.metadata.dng_negative, dst_mosaic_w, dst_mosaic_h);
        burstmerge::io::ClearDngOriginalSizes(output.metadata.dng_negative);

        // ---- Write DNG ----
        std::cout << "Writing DNG..." << std::endl;
        burstmerge::io::SetDngWhiteLevel(output.metadata.dng_negative, output.metadata.white_level);
        if (bit_scale != 1.0f)
        {
            burstmerge::io::SetDngBlackLevel(output.metadata.dng_negative, output.metadata.black_level);
        }
        burstmerge::DngWriter writer(output.metadata.dng_negative);
        {
            STAGE_TIMER("DngWriter::Write (encode + disk)");
            writer.Write(output_path.c_str(), output);
        }

        std::cout << "Done: " << output_path << std::endl;
        std::cout << "  " << dst_mosaic_w << "x" << dst_mosaic_h
                  << " @" << bit_depth << "bit (white=" << target_white << ")"
                  << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        if (!convert_dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(convert_dir, ec);
        }
        return 1;
    }
    catch (...)
    {
        std::cerr << "Unknown error" << std::endl;
        if (!convert_dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(convert_dir, ec);
        }
        return 1;
    }

    if (!convert_dir.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(convert_dir, ec);
    }

    return 0;
}
