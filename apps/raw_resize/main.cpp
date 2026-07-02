#include "burstmerge/api.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/dng_io.h"
#include "cxxopts.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool ParseInterpolation(const std::string& value, burstmerge::InterpolationMethod& out)
{
    std::string v = value;
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "bilinear") { out = burstmerge::InterpolationMethod::Bilinear; return true; }
    if (v == "bicubic")  { out = burstmerge::InterpolationMethod::Bicubic; return true; }
    return false;
}

const char* InterpName(burstmerge::InterpolationMethod m)
{
    switch (m)
    {
        case burstmerge::InterpolationMethod::Bilinear: return "bilinear";
        case burstmerge::InterpolationMethod::Bicubic:  return "bicubic";
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
        ("interp", "Interpolation method: bilinear, bicubic (default)", cxxopts::value<std::string>()->default_value("bicubic"))
        ("bit-depth", "Output bit depth: 8, 10, 12, 14, 16 (default 16)", cxxopts::value<std::string>()->default_value("16"))
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
            if (!burstmerge::RunAdobeDngConverter(single_input, convert_dir, converted) || converted.empty())
            {
                throw std::runtime_error("Adobe DNG Converter failed or timed out");
            }
            dng_path = converted[0];
            std::cout << "  Converted: " << dng_path << std::endl;
#else
            throw std::runtime_error("Non-DNG RAW input requires pre-conversion on this platform");
#endif
        }

        // ---- Read DNG ----
        std::cout << "Reading DNG..." << std::endl;
        burstmerge::DngReader reader(dng_path.c_str());
        burstmerge::RawImage raw = reader.Read();
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

        uint32_t target_white = static_cast<uint32_t>(
            std::lround(static_cast<double>(meta.white_level) *
                        static_cast<double>((1u << bit_depth) - 1) / 65535.0));
        if (target_white > 65535) target_white = 65535;
        if (target_white < 1) target_white = 1;

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

        // ---- Convert to FloatImage ----
        burstmerge::FloatImage fin = burstmerge::HostBufferToFloatImage(raw.pixels);

        // ---- Process based on CFA type ----
        burstmerge::FloatImage result;
        if (is_linear_rgb)
        {
            std::cout << "Resizing (LinearRaw, 3ch)..." << std::endl;
            result = burstmerge::ResizeImage(fin, dst_mosaic_w, dst_mosaic_h, interp);
        }
        else
        {
            std::cout << "Converting mosaic to plane image..." << std::endl;
            burstmerge::FloatImage plane = burstmerge::ConvertMosaicToPlaneImage(fin, period);

            std::cout << "Resizing plane image (" << InterpName(interp) << ")..." << std::endl;
            burstmerge::FloatImage resized = burstmerge::ResizeImage(plane, dst_plane_w, dst_plane_h, interp);

            std::cout << "Converting plane back to mosaic..." << std::endl;
            result = burstmerge::ConvertPlaneImageToMosaic(resized, dst_mosaic_w, dst_mosaic_h, period);
        }

        // ---- Convert back to uint16 ----
        std::cout << "Quantizing to uint16..." << std::endl;
        burstmerge::HostBuffer averaged = burstmerge::FloatImageToUint16HostBuffer(result, target_white);

        // ---- Prepare output RawImage ----
        burstmerge::RawImage output;
        output.metadata = std::move(raw.metadata);
        output.metadata.width = dst_mosaic_w;
        output.metadata.height = dst_mosaic_h;
        output.metadata.white_level = target_white;
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
        burstmerge::DngWriter writer(output.metadata.dng_negative);
        writer.Write(output_path.c_str(), output);

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
