#include "burstmerge/api.h"
#include "burstmerge/internal/io/dng_io.h"
#include "cxxopts.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ParseMergeAlgorithm(const std::string& value, burstmerge::MergeAlgorithm& out) {
    std::string v = Lower(value);
    if (v == "spatial") { out = burstmerge::MergeAlgorithm::Spatial; return true; }
    if (v == "frequency" || v == "freq") { out = burstmerge::MergeAlgorithm::Frequency; return true; }
    if (v == "temporal" || v == "temporal-average" || v == "average" || v == "avg") {
        out = burstmerge::MergeAlgorithm::TemporalAverage; return true; }
    return false;
}

const char* MergeAlgoName(burstmerge::MergeAlgorithm algo) {
    switch (algo) {
        case burstmerge::MergeAlgorithm::Spatial: return "Spatial";
        case burstmerge::MergeAlgorithm::Frequency: return "Frequency";
        case burstmerge::MergeAlgorithm::TemporalAverage: return "TemporalAverage";
    }
    return "Unknown";
}

bool ParseAlignmentMode(const std::string& value, burstmerge::AlignmentMode& out) {
    std::string v = Lower(value);
    if (v == "standard" || v == "legacy") { out = burstmerge::AlignmentMode::Standard; return true; }
    if (v == "dense" || v == "dense-tile") { out = burstmerge::AlignmentMode::DenseTile; return true; }
    if (v == "freq" || v == "frequency") { out = burstmerge::AlignmentMode::Frequency; return true; }
    return false;
}

bool ParseSpatialMode(const std::string& value, burstmerge::SpatialMergeMode& out) {
    std::string v = Lower(value);
    if (v == "standard" || v == "legacy") { out = burstmerge::SpatialMergeMode::Standard; return true; }
    if (v == "linear") { out = burstmerge::SpatialMergeMode::Linear; return true; }
    return false;
}

bool ParseFrequencyMode(const std::string& value, burstmerge::FrequencyMode& out) {
    std::string v = Lower(value);
    if (v == "laplacian" || v == "legacy") { out = burstmerge::FrequencyMode::Laplacian; return true; }
    if (v == "wiener" || v == "wiener-standard" || v == "wiener-v1" || v == "fft-standard" || v == "wiener-legacy" || v == "fft-legacy") {
        out = burstmerge::FrequencyMode::WienerFft; return true; }
    if (v == "wiener-robust" || v == "wiener-v2" || v == "fft" || v == "wiener-fft") {
        out = burstmerge::FrequencyMode::WienerFftRobust; return true; }
    return false;
}

bool ParseExposureMode(const std::string& value, burstmerge::ExposureMode& out) {
    std::string v = Lower(value);
    if (v == "off") { out = burstmerge::ExposureMode::Off; return true; }
    if (v == "linear") { out = burstmerge::ExposureMode::Linear; return true; }
    if (v == "curve") { out = burstmerge::ExposureMode::Curve; return true; }
    return false;
}

bool ParseExposureCurveMode(const std::string& value, burstmerge::ExposureCurveMode& out) {
    std::string v = Lower(value);
    if (v == "global" || v == "standard" || v == "legacy") { out = burstmerge::ExposureCurveMode::Global; return true; }
    if (v == "local" || v == "local-reinhard") { out = burstmerge::ExposureCurveMode::LocalReinhard; return true; }
    return false;
}

bool ParseOutputFormat(const std::string& value, burstmerge::OutputFormat& out) {
    std::string v = Lower(value);
    if (v == "auto") { out = burstmerge::OutputFormat::Auto; return true; }
    if (v == "png")  { out = burstmerge::OutputFormat::PNG; return true; }
    if (v == "jpg" || v == "jpeg") { out = burstmerge::OutputFormat::JPEG; return true; }
    if (v == "bmp")  { out = burstmerge::OutputFormat::BMP; return true; }
    if (v == "tif" || v == "tiff") { out = burstmerge::OutputFormat::TIFF; return true; }
    if (v == "dng")  { out = burstmerge::OutputFormat::DNG; return true; }
    return false;
}

void PrintInputSummary(const std::vector<std::string>& inputs) {
    std::cout << "Inputs (" << inputs.size() << "):" << std::endl;
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << inputs[i] << std::endl;
    }
}

std::vector<std::string> ExpandFolderInputs(const std::vector<std::string>& folders) {
    std::vector<std::string> inputs;
    for (const auto& folder : folders) {
        std::filesystem::path folder_path(folder);
        if (!std::filesystem::exists(folder_path)) {
            throw std::runtime_error("Input folder does not exist: " + folder);
        }
        if (!std::filesystem::is_directory(folder_path)) {
            throw std::runtime_error("Input folder is not a directory: " + folder);
        }

        for (const auto& entry : std::filesystem::directory_iterator(folder_path)) {
            if (entry.is_regular_file()) {
                inputs.push_back(entry.path().string());
            }
        }
    }

    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

} // namespace

int main(int argc, char* argv[]) {
    cxxopts::Options opts("burstmerge", "Burst merge for RAW / RGB photo bursts");
    opts.add_options()
        ("i,input", "Input RAW/DNG or image files (PNG/JPEG/BMP/TIFF)", cxxopts::value<std::vector<std::string>>())
        ("f,folder", "Input folder(s); expands regular files and reuses the normal input pipeline", cxxopts::value<std::vector<std::string>>())
        ("o,output", "Output file path or output directory", cxxopts::value<std::string>()->default_value("./out"))
        ("t,tile", "Tile size", cxxopts::value<int>()->default_value("32"))
        ("b,bit-depth", "Output bit depth (8, 10, 12, 14, or 16)", cxxopts::value<int>()->default_value("14"))
        ("frequency", "Shorthand for --merge-algo frequency (deprecated, use --merge-algo)")
        ("n,noise-reduction", "Noise reduction strength (ignored when merge-algo = temporal)", cxxopts::value<float>())
        ("merge-algo", "Merge algorithm: spatial, frequency, temporal", cxxopts::value<std::string>())
        ("alignment", "Alignment mode: standard, dense, freq", cxxopts::value<std::string>()->default_value("standard"))
        ("spatial-mode", "Spatial merge mode: standard, linear", cxxopts::value<std::string>()->default_value("standard"))
        ("frequency-mode", "Frequency mode: laplacian, wiener, wiener-robust", cxxopts::value<std::string>()->default_value("laplacian"))
        ("exposure-mode", "Exposure mode: off, linear, curve", cxxopts::value<std::string>()->default_value("off"))
        ("exposure-stops", "Exposure correction stops", cxxopts::value<float>()->default_value("0"))
        ("exposure-curve", "Exposure curve mode: global, local", cxxopts::value<std::string>()->default_value("global"))
        ("align-gamma", "Gamma correction for alignment grayscale (default 1.0=off). Value < 1.0 will boost darkness", cxxopts::value<float>()->default_value("1.0"))
        ("smooth-tile-field", "Enable median smoothing of alignment tile fields", cxxopts::value<bool>()->default_value("false"))
        ("output-format", "Output format: auto, png, jpg, bmp, tiff, dng", cxxopts::value<std::string>()->default_value("auto"))
        ("h,help", "Print help");

    cxxopts::ParseResult args;
    try {
        args = opts.parse(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << std::endl;
        std::cerr << opts.help() << std::endl;
        return 2;
    }

    if (args.count("help")) {
        std::cout << opts.help() << std::endl;
        return 0;
    }

    if (!args.count("input") && !args.count("folder")) {
        std::cerr << "No inputs. Use -i file1.dng -i file2.dng ... or -f path\\to\\folder" << std::endl;
        return 2;
    }

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    std::vector<std::string> inputs;
    if (args.count("input")) {
        auto direct_inputs = args["input"].as<std::vector<std::string>>();
        inputs.insert(inputs.end(), direct_inputs.begin(), direct_inputs.end());
    }
    if (args.count("folder")) {
        try {
            auto folder_inputs = ExpandFolderInputs(args["folder"].as<std::vector<std::string>>());
            inputs.insert(inputs.end(), folder_inputs.begin(), folder_inputs.end());
        } catch (const std::exception& e) {
            std::cerr << "Input folder error: " << e.what() << std::endl;
            return 2;
        }
    }
    if (inputs.empty()) {
        std::cerr << "No input files found after expanding arguments" << std::endl;
        return 2;
    }
    for (const auto& input : inputs) bm.AddImage(input);

    burstmerge::Settings settings;
    settings.tile_size = args["tile"].as<int>();
    int bit_depth = args["bit-depth"].as<int>();
    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12 && bit_depth != 14 && bit_depth != 16) {
        std::cerr << "Invalid bit depth: " << bit_depth << " (use 8, 10, 12, 14, or 16)" << std::endl;
        return 2;
    }
    settings.bit_depth = bit_depth;
    settings.merge_algo = burstmerge::MergeAlgorithm::Spatial;
    if (args.count("merge-algo") &&
        !ParseMergeAlgorithm(args["merge-algo"].as<std::string>(), settings.merge_algo)) {
        std::cerr << "Invalid merge algorithm (use spatial, frequency, or temporal)" << std::endl;
        return 2;
    }
    // --frequency is a deprecated shorthand for --merge-algo frequency.
    if (args.count("frequency") && args.count("merge-algo")) {
        std::cerr << "Cannot specify both --frequency and --merge-algo; use --merge-algo" << std::endl;
        return 2;
    }
    if (args.count("frequency")) {
        settings.merge_algo = burstmerge::MergeAlgorithm::Frequency;
    }
    if (!ParseAlignmentMode(args["alignment"].as<std::string>(), settings.alignment_mode)) {
        std::cerr << "Invalid alignment mode (use standard, dense, or freq)" << std::endl;
        return 2;
    }
    if (!ParseSpatialMode(args["spatial-mode"].as<std::string>(), settings.spatial_mode)) {
        std::cerr << "Invalid spatial mode (use standard or linear)" << std::endl;
        return 2;
    }
    if (!ParseFrequencyMode(args["frequency-mode"].as<std::string>(), settings.frequency_mode)) {
        std::cerr << "Invalid frequency mode (use laplacian, wiener, or wiener-robust)" << std::endl;
        return 2;
    }
    if (!ParseExposureMode(args["exposure-mode"].as<std::string>(), settings.exposure_mode)) {
        std::cerr << "Invalid exposure mode (use off, linear, or curve)" << std::endl;
        return 2;
    }
    if (!ParseExposureCurveMode(args["exposure-curve"].as<std::string>(), settings.exposure_curve_mode)) {
        std::cerr << "Invalid exposure curve mode (use global or local)" << std::endl;
        return 2;
    }
    settings.exposure_stops = args["exposure-stops"].as<float>();
    if (args.count("noise-reduction")) {
        settings.noise_reduction = args["noise-reduction"].as<float>();
    }
    settings.align_gamma = args["align-gamma"].as<float>();
    settings.smooth_tile_field = args["smooth-tile-field"].as<bool>();
    if (!ParseOutputFormat(args["output-format"].as<std::string>(), settings.output_format)) {
        std::cerr << "Invalid output format (use auto, png, jpg, bmp, tiff, or dng)" << std::endl;
        return 2;
    }
    bm.Configure(settings);

    const std::string output_target = args["output"].as<std::string>();

    std::cout << "BurstMerge CLI" << std::endl;
    std::cout << "Backend: CPU" << std::endl;
    std::cout << "Merge: " << MergeAlgoName(settings.merge_algo) << std::endl;
    std::cout << "Tile size: " << settings.tile_size << std::endl;
    std::cout << "Noise reduction: " << settings.noise_reduction << std::endl;
    std::cout << "Align gamma: " << settings.align_gamma << std::endl;
    std::cout << "Smooth tile field: " << (settings.smooth_tile_field ? "on" : "off") << std::endl;
    std::cout << "Bit depth: " << settings.bit_depth << std::endl;
    std::cout << "Output format: ";
    switch (settings.output_format) {
        case burstmerge::OutputFormat::Auto: std::cout << "Auto"; break;
        case burstmerge::OutputFormat::PNG:  std::cout << "PNG"; break;
        case burstmerge::OutputFormat::JPEG: std::cout << "JPEG"; break;
        case burstmerge::OutputFormat::BMP:  std::cout << "BMP"; break;
        case burstmerge::OutputFormat::TIFF: std::cout << "TIFF"; break;
        case burstmerge::OutputFormat::DNG:  std::cout << "DNG"; break;
    }
    std::cout << std::endl;
    std::cout << "Output target: " << output_target << std::endl;
    PrintInputSummary(inputs);

    const auto start_time = std::chrono::steady_clock::now();
    std::string last_stage;
    int last_percent = -1;
    bm.SetProgressCallback([&](float p, const std::string& stage) {
        int percent = static_cast<int>(p * 100.0f + 0.5f);
        if (stage == last_stage && percent == last_percent) {
            return;
        }
        last_stage = stage;
        last_percent = percent;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        std::cout << "["
                  << std::setw(3) << percent << "%] "
                  << "[+" << std::fixed << std::setprecision(2)
                  << (static_cast<double>(elapsed_ms) / 1000.0) << "s] "
                  << stage << std::endl;
    });

    std::cout << "Starting processing..." << std::endl;
    auto result = bm.Process(output_target);

    const auto end_time = std::chrono::steady_clock::now();
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "Finished in " << std::fixed << std::setprecision(2)
              << (static_cast<double>(total_ms) / 1000.0) << "s" << std::endl;
    std::cout << "Result: " << (result.success ? "OK" : "FAIL") << std::endl;
    if (!result.output_path.empty()) {
        std::cout << "Output: " << result.output_path << std::endl;
    }
    if (!result.error_msg.empty()) {
        std::cerr << "Error: " << result.error_msg << std::endl;
    }
    return result.success ? 0 : 1;
}
