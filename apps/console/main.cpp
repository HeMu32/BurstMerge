#include "burstmerge/api.h"
#include <iostream>
#include <string>

int main() {
    std::cout << "BurstMerge Interactive Console (placeholder)" << std::endl;

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    bm.Configure({});

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line == "exit" || line == "quit") break;
        if (line == "process") {
            auto result = bm.Process("./out");
            std::cout << "  Result: " << (result.success ? "OK" : "FAIL") << std::endl;
        }
    }
    return 0;
}
