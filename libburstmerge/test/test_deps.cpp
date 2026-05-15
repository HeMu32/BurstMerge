#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

// 1. 测试 PocketFFT (C API, 需 C 链接)
extern "C" {
#include "pocketfft.h"
}

// 2. 测试 cxxopts (header-only)
#include "cxxopts.hpp"

// 3. 测试 Vulkan 头文件
#include <vulkan/vulkan.h>

// 4. 测试 DNG SDK (通过 burstmerge 接口)
#include "burstmerge/api.h"

// 5. 测试 OpenMP
#include <omp.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int test_pocketfft() {
    std::cout << "[test] PocketFFT..." << std::endl;
    // 创建+销毁长度为1的plan验证API可用
    auto plan = make_cfft_plan(1);
    if (!plan) return 1;
    double data[2] = {1.0, 0.0};
    cfft_forward(plan, data, 1.0);
    destroy_cfft_plan(plan);
    std::cout << "  PocketFFT C API OK" << std::endl;
    return 0;
}

static int test_cxxopts() {
    std::cout << "[test] cxxopts header..." << std::endl;
    cxxopts::Options options("test", "Test");
    options.add_options()("h,help", "Print help");
    std::cout << "  cxxopts parse OK" << std::endl;
    return 0;
}

static int test_vulkan() {
    std::cout << "[test] Vulkan header..." << std::endl;
    std::cout << "  VK_API_VERSION = "
              << VK_VERSION_MAJOR(VK_API_VERSION_1_3) << "."
              << VK_VERSION_MINOR(VK_API_VERSION_1_3) << std::endl;

#ifdef _WIN32
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    if (h) {
        std::cout << "  vulkan-1.dll loaded OK" << std::endl;
        FreeLibrary(h);
    } else {
        std::cout << "  WARNING: vulkan-1.dll not found" << std::endl;
    }
#endif
    return 0;
}

static int test_openmp() {
    std::cout << "[test] OpenMP..." << std::endl;
    int nth = 0;
#pragma omp parallel
    {
#pragma omp atomic
        nth++;
    }
    std::cout << "  _OPENMP = " << _OPENMP << std::endl;
    std::cout << "  num_threads = " << nth << std::endl;
    return 0;
}

static int test_burstmerge_api() {
    std::cout << "[test] burstmerge API..." << std::endl;
    using namespace burstmerge;

    auto bm = std::make_unique<BurstMerge>(BackendType::CPU);
    Settings s;
    s.tile_size = 64;
    s.noise_reduction = 13.0f;
    bm->Configure(s);
    bm->AddImage(std::string(TEST_DATA_DIR) + "/libburstmerge/test/samples/X1M5_Wide.dng");
    auto result = bm->Process(std::string(TEST_DATA_DIR) + "/build/test_deps_output.dng");
    std::cout << "  Process result: success=" << result.success << std::endl;
    if (!result.success) {
        std::cout << "  Error: " << result.error_msg << std::endl;
    }
    return result.success ? 0 : 1;
}

int main() {
    int failed = 0;

    failed += test_pocketfft();
    failed += test_cxxopts();
    failed += test_vulkan();
    failed += test_openmp();
    failed += test_burstmerge_api();

    if (failed == 0) {
        std::cout << "\nAll dependency tests PASSED." << std::endl;
    } else {
        std::cout << "\nSome tests FAILED." << std::endl;
    }
    return failed;
}
