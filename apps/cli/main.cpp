#include "burstmerge/api.h"
#include "cxxopts.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* MergeAlgoName(burstmerge::MergeAlgorithm algo) {
    switch (algo) {
        case burstmerge::MergeAlgorithm::Spatial: return "Spatial";
        case burstmerge::MergeAlgorithm::Frequency: return "Frequency";
    }
    return "Unknown";
}

void PrintInputSummary(const std::vector<std::string>& inputs) {
    std::cout << "Inputs (" << inputs.size() << "):" << std::endl;
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << inputs[i] << std::endl;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    cxxopts::Options opts("burstmerge", "Burst merge for RAW photos");
    opts.add_options()
        ("i,input", "Input RAW/DNG files", cxxopts::value<std::vector<std::string>>())
        ("o,output", "Output DNG path or output directory", cxxopts::value<std::string>()->default_value("./out"))
        ("t,tile", "Tile size", cxxopts::value<int>()->default_value("32"))
        ("f,frequency", "Use frequency merge placeholder flag")
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

    if (!args.count("input")) {
        std::cerr << "No input files. Use -i file1.dng -i file2.dng ..." << std::endl;
        return 2;
    }

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    auto inputs = args["input"].as<std::vector<std::string>>();
    for (const auto& input : inputs) bm.AddImage(input);

    burstmerge::Settings settings;
    settings.tile_size = args["tile"].as<int>();
    settings.merge_algo = args.count("frequency")
        ? burstmerge::MergeAlgorithm::Frequency
        : burstmerge::MergeAlgorithm::Spatial;
    bm.Configure(settings);

    const std::string output_target = args["output"].as<std::string>();

    std::cout << "BurstMerge CLI" << std::endl;
    std::cout << "Backend: CPU" << std::endl;
    std::cout << "Merge: " << MergeAlgoName(settings.merge_algo) << std::endl;
    std::cout << "Tile size: " << settings.tile_size << std::endl;
    std::cout << "Noise reduction: " << settings.noise_reduction << std::endl;
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
