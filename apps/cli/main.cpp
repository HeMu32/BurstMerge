#include "burstmerge/api.h"
#include "cxxopts.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    cxxopts::Options opts("burstmerge", "Burst merge for RAW photos");
    opts.add_options()
        ("i,input", "Input DNG files", cxxopts::value<std::vector<std::string>>())
        ("o,output", "Output directory", cxxopts::value<std::string>()->default_value("./out"))
        ("t,tile", "Tile size", cxxopts::value<int>()->default_value("32"))
        ("h,help", "Print help");

    auto args = opts.parse(argc, argv);
    if (args.count("help")) {
        std::cout << opts.help() << std::endl;
        return 0;
    }

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    bm.Configure({});
    auto result = bm.Process(args["output"].as<std::string>());
    std::cout << "Result: " << (result.success ? "OK" : "FAIL") << std::endl;
    return result.success ? 0 : 1;
}
