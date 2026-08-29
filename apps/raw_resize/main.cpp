#include "burstmerge/api.h"
#include "burstmerge/internal/core/chroma_effects.h"
#include "burstmerge/internal/core/dither.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/image_resize.h"
#include "burstmerge/internal/io/dng_io.h"
#include "cxxopts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

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

bool ParseLaCAColor(const std::string& value, burstmerge::LaCAColor& out)
{
    std::string v = value;
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "red" || v == "r")         { out = burstmerge::LaCAColor_Red; return true; }
    if (v == "green" || v == "g")       { out = burstmerge::LaCAColor_Green; return true; }
    if (v == "blue" || v == "b")        { out = burstmerge::LaCAColor_Blue; return true; }
    if (v == "cyan" || v == "c")        { out = burstmerge::LaCAColor_Cyan; return true; }
    if (v == "magenta" || v == "m")     { out = burstmerge::LaCAColor_Magenta; return true; }
    if (v == "yellow" || v == "y")      { out = burstmerge::LaCAColor_Yellow; return true; }
    return false;
}

bool ParseLoCAColor(const std::string& value, burstmerge::LoCAColor& out)
{
    std::string v = value;
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "red" || v == "r")         { out = burstmerge::LoCAColor_Red; return true; }
    if (v == "green" || v == "g")       { out = burstmerge::LoCAColor_Green; return true; }
    if (v == "blue" || v == "b")        { out = burstmerge::LoCAColor_Blue; return true; }
    if (v == "cyan" || v == "c")        { out = burstmerge::LoCAColor_Cyan; return true; }
    if (v == "magenta" || v == "m")     { out = burstmerge::LoCAColor_Magenta; return true; }
    if (v == "yellow" || v == "y")      { out = burstmerge::LoCAColor_Yellow; return true; }
    return false;
}

bool ParseBitDepth(const std::string& value, int& bit_depth)
{
    try { bit_depth = std::stoi(value); } catch (...) { return false; }
    return (bit_depth == 8 || bit_depth == 10 || bit_depth == 12 || bit_depth == 14 || bit_depth == 16);
}

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
        ("interp", "Interpolation method: bilinear, bicubic (default), area (area-average; recommended for heavy downscale), gaussian-area (>=3x downscale only), 50percent (half-sample; >=2x downscale only)", cxxopts::value<std::string>()->default_value("bicubic"))
        ("bit-depth", "Output bit depth: 8, 10, 12, 14, 16 (default 16)", cxxopts::value<std::string>()->default_value("16"))
        ("pseudo-olpf", "Pseudo optical low-pass filter strength (gaussian pre-blur before downscale to suppress moire; sigma = strength * log2(downscale)). Default 0 (disabled).", cxxopts::value<double>()->default_value("0"))
        ("dither", "TPDF dither amplitude in output LSB applied before uint16 quantisation to decorrelate quantisation noise. Default 0 (disabled). 1.0 = full +-1 LSB TPDF.", cxxopts::value<double>()->default_value("0"))
        ("clear-camera-hints", "Clear camera metadata hints (anti-alias strength, noise profile, sharpness hints)", cxxopts::value<bool>()->default_value("false"))

        // Chromatic aberration (Lo-Fi effects)
        ("chroma", "Enable chromatic aberration effects (LaCA / LoCA)", cxxopts::value<bool>()->default_value("false"))
        ("laca-color", "Lateral CA color preset (red, green, blue, cyan, magenta, yellow)", cxxopts::value<std::string>()->default_value("red"))
        ("laca-width", "Lateral CA corner displacement in %% of diagonal", cxxopts::value<double>()->default_value("0.0"))
        ("loca-color", "Longitudinal CA color preset (red, green, blue, cyan, magenta, yellow)", cxxopts::value<std::string>()->default_value("magenta"))
        ("loca-strength", "Longitudinal CA edge sensitivity multiplier (1.0 = saturate)", cxxopts::value<double>()->default_value("0.2"))
        ("loca-width", "Longitudinal CA fringing band width in %% of diagonal", cxxopts::value<double>()->default_value("0.05"))
        ("loca-min-sensi", "Longitudinal CA edge detection floor threshold", cxxopts::value<double>()->default_value("0.08"))

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

    burstmerge::RawResizeOptions resize_opts;

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

    if (has_scale)
    {
        resize_opts.scale = args["scale"].as<double>();
        if (resize_opts.scale <= 0.0)
        {
            std::cerr << "Scale factor must be positive" << std::endl;
            return 2;
        }
    }
    else
    {
        resize_opts.width = args["width"].as<uint32_t>();
        resize_opts.height = args["height"].as<uint32_t>();
    }

    if (!ParseInterpolation(args["interp"].as<std::string>(), resize_opts.interp))
    {
        std::cerr << "Invalid interpolation method (use bilinear, bicubic, area, gaussian-area, or 50percent)" << std::endl;
        return 2;
    }

    if (!ParseBitDepth(args["bit-depth"].as<std::string>(), resize_opts.bit_depth))
    {
        std::cerr << "Invalid bit depth (use 8, 10, 12, 14, or 16)" << std::endl;
        return 2;
    }

    resize_opts.pseudo_olpf = static_cast<float>(args["pseudo-olpf"].as<double>());
    resize_opts.dither = static_cast<float>(args["dither"].as<double>());
    resize_opts.clear_camera_hints = args["clear-camera-hints"].as<bool>();

    // Chroma options
    resize_opts.chroma.enabled = args["chroma"].as<bool>();
    if (!ParseLaCAColor(args["laca-color"].as<std::string>(), resize_opts.chroma.laca_color))
    {
        std::cerr << "Invalid laca-color (use red, green, blue, cyan, magenta, or yellow)" << std::endl;
        return 2;
    }
    resize_opts.chroma.laca_width = static_cast<float>(args["laca-width"].as<double>());

    if (!ParseLoCAColor(args["loca-color"].as<std::string>(), resize_opts.chroma.loca_color))
    {
        std::cerr << "Invalid loca-color (use red, green, blue, cyan, magenta, or yellow)" << std::endl;
        return 2;
    }
    resize_opts.chroma.loca_strength = static_cast<float>(args["loca-strength"].as<double>());
    resize_opts.chroma.loca_width = static_cast<float>(args["loca-width"].as<double>());
    resize_opts.chroma.loca_min_sensi = static_cast<float>(args["loca-min-sensi"].as<double>());

    // If laca-width or loca-strength is explicitly set, enable chroma effects automatically
    if (resize_opts.chroma.laca_width > 0.0f || (args.count("loca-strength") && resize_opts.chroma.loca_strength > 0.0f))
    {
        resize_opts.chroma.enabled = true;
    }

    std::string input_path = args["input"].as<std::string>();
    std::string output_path = args["output"].as<std::string>();

    std::cout << "raw_resize" << std::endl;
    std::cout << "  Input:  " << input_path << std::endl;
    std::cout << "  Output: " << output_path << std::endl;
    std::cout << "  Interpolation: " << InterpName(resize_opts.interp) << std::endl;
    std::cout << "  Bit depth: " << resize_opts.bit_depth << std::endl;
    if (resize_opts.chroma.enabled)
    {
        std::cout << "  Chroma Effects: Enabled (LaCA width=" << resize_opts.chroma.laca_width
                  << "%, LoCA strength=" << resize_opts.chroma.loca_strength
                  << ", width=" << resize_opts.chroma.loca_width << "%)" << std::endl;
    }

    auto progress_cb = [](float progress, const std::string& status)
    {
        std::cout << "  [" << static_cast<int>(progress * 100.0f) << "%] " << status << std::endl;
    };

    burstmerge::RawResizeResult res = burstmerge::ProcessRawResize(input_path, output_path, resize_opts, progress_cb);
    if (!res.success)
    {
        std::cerr << "Error: " << res.error_msg << std::endl;
        return 1;
    }

    std::cout << "Done: " << output_path << std::endl;
    std::cout << "  " << res.dst_width << "x" << res.dst_height
              << " @" << resize_opts.bit_depth << "bit (white=" << res.target_white << ")"
              << std::endl;

    return 0;
}