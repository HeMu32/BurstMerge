// test_stage3: CPU vs GPU dual-path consistency test.
// For each merge mode, runs both backends on the same burst and compares
// the output DNG pixel data. Acceptance: relative MAD below per-mode threshold.
//
// Requires: Adobe DNG Converter (for ARW samples) + Vulkan GPU.
// Usage: no arguments — auto-discovers samples in libburstmerge/test/samples/.

#include "burstmerge/api.h"
#include "burstmerge/internal/compute/vulkan_backend.h"
#include "burstmerge/internal/core/gpu_pipeline.h"
#include "burstmerge/internal/io/dng_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) \
    { \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << msg << std::endl; \
        ++g_failed; \
    } \
} while (0)

static bool ConverterAvailable()
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(
        "C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe");
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

static std::vector<std::string> FindSamples(const fs::path& dir, const std::string& ext, int max_count)
{
    std::vector<std::string> files;
    if (!fs::exists(dir)) return files;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;
        std::string e = entry.path().extension().string();
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e == ext) files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    if (int(files.size()) > max_count)
        files.resize(max_count);
    return files;
}

// Run BurstMerge on the given backend and return the output DNG path.
// Returns empty string on failure.
static std::string RunBackend(burstmerge::BackendType backend,
                              const std::vector<std::string>& inputs,
                              const burstmerge::Settings& settings,
                              const std::string& output_path)
{
    fs::remove(output_path);
    burstmerge::BurstMerge bm(backend);
    bm.Configure(settings);
    for (const auto& f : inputs) bm.AddImage(f);
    auto result = bm.Process(output_path);
    if (!result.success)
    {
        std::cerr << "  backend " << (backend == burstmerge::BackendType::Vulkan ? "GPU" : "CPU")
                  << " failed: " << result.error_msg << std::endl;
        return "";
    }
    return result.output_path;
}

// Compute mean absolute difference between two decoded DNG images.
// Returns {mad, max_value} or {-1, 0} on error.
struct CompareResult { double mad; double max_val; double rel; };
static CompareResult CompareDngOutputs(const std::string& path_a, const std::string& path_b)
{
    burstmerge::DngReader ra(path_a.c_str());
    auto img_a = ra.Read();
    burstmerge::DngReader rb(path_b.c_str());
    auto img_b = rb.Read();

    if (img_a.pixels.width != img_b.pixels.width ||
        img_a.pixels.height != img_b.pixels.height ||
        img_a.pixels.format != img_b.pixels.format)
        return {-1.0, 0.0, -1.0};

    // Compare every uint16 sample (width*height*planes). Using pixels.size/2
    // accounts for the channel count automatically: 1 plane for Bayer mosaic,
    // 3 planes for LinearRaw RGB.
    if (img_a.pixels.size != img_b.pixels.size)
        return {-1.0, 0.0, -1.0};
    uint64_t count = img_a.pixels.size / sizeof(uint16_t);
    const uint16_t* pa = reinterpret_cast<const uint16_t*>(img_a.pixels.data);
    const uint16_t* pb = reinterpret_cast<const uint16_t*>(img_b.pixels.data);

    double sum_abs = 0.0;
    uint32_t max_val = 0;
    for (uint64_t i = 0; i < count; ++i)
    {
        int diff = int(pa[i]) - int(pb[i]);
        if (diff < 0) diff = -diff;
        sum_abs += double(diff);
        if (pa[i] > max_val) max_val = pa[i];
    }
    double mad = sum_abs / double(count);
    double rel = max_val > 0 ? mad / double(max_val) : 0.0;
    return {mad, double(max_val), rel};
}

// Test one configuration: run CPU + GPU, compare outputs.
static void TestConsistency(const std::string& tag,
                            const std::vector<std::string>& inputs,
                            const burstmerge::Settings& settings,
                            float threshold_rel)
{
    std::cout << "\n--- " << tag << " ---" << std::endl;
    std::string dir = fs::temp_directory_path().string();

    std::string out_cpu = RunBackend(burstmerge::BackendType::CPU, inputs, settings,
                                     dir + "/test_stage3_cpu.dng");
    std::string out_gpu = RunBackend(burstmerge::BackendType::Vulkan, inputs, settings,
                                     dir + "/test_stage3_gpu.dng");

    CHECK(!out_cpu.empty(), tag + " CPU succeeded");
    CHECK(!out_gpu.empty(), tag + " GPU succeeded");
    if (out_cpu.empty() || out_gpu.empty()) return;

    // VRAM leak check: GPU backend was just destroyed inside RunBackend.
    // VulkanBackend::LastLeakedBuffers() should be 0 if all GPU buffers
    // were properly freed by the pipeline.
    auto leaked_bufs = burstmerge::vulkan::VulkanBackend::LastLeakedBuffers();
    auto leaked_bytes = burstmerge::vulkan::VulkanBackend::LastLeakedBytes();
    CHECK(leaked_bufs == 0,
          tag + " no VRAM leak (got " + std::to_string(leaked_bufs) +
          " buffers, " + std::to_string(leaked_bytes) + " bytes)");
    if (leaked_bufs > 0)
        std::cerr << "  WARNING: leaked " << leaked_bytes << " bytes in "
                  << leaked_bufs << " buffers" << std::endl;

    auto cmp = CompareDngOutputs(out_cpu, out_gpu);
    CHECK(cmp.rel >= 0.0, tag + " dimensions match");
    if (cmp.rel < 0.0) return;

    std::cout << "  MAD=" << cmp.mad << "  max=" << cmp.max_val
              << "  rel=" << (cmp.rel * 100.0) << "%" << std::endl;

    CHECK(cmp.rel < double(threshold_rel),
          tag + " relative MAD " + std::to_string(cmp.rel * 100.0) + "% < " +
          std::to_string(double(threshold_rel) * 100.0) + "%");

    fs::remove(out_cpu);
    fs::remove(out_gpu);
}

// LinearRaw input on the Vulkan backend must fall back to CPU (the GPU prepare
// path assumes a Bayer mosaic). Verifies the fallback succeeds, produces a
// LinearRaw output, and is bit-identical to the CPU run. Independent of the
// Seq1/converter prerequisites below — only needs Vulkan + the vendored DNG.
static void TestLinearRawGpuFallback()
{
    fs::path linear = fs::path(TEST_DATA_DIR) /
                      "3rdparty/dng_sdk/sample_files/04_PGTM2_per_profile.dng";
    if (!fs::exists(linear))
    {
        std::cout << "SKIP linear GPU-fallback: sample not found" << std::endl;
        return;
    }

    std::cout << "\n--- LinearRaw GPU CPU-fallback ---" << std::endl;
    burstmerge::Settings s;
    s.bit_depth = 16;
    std::string dir = fs::temp_directory_path().string();

    std::string out_cpu = RunBackend(burstmerge::BackendType::CPU, {linear.string()}, s,
                                     dir + "/test_stage3_lin_cpu.dng");
    std::string out_gpu = RunBackend(burstmerge::BackendType::Vulkan, {linear.string()}, s,
                                     dir + "/test_stage3_lin_gpu.dng");
    CHECK(!out_cpu.empty(), "linear CPU succeeded");
    CHECK(!out_gpu.empty(), "linear GPU (fallback) succeeded");
    if (out_cpu.empty() || out_gpu.empty()) return;

    // GPU output must still be LinearRaw (fallback preserved topology).
    {
        burstmerge::DngReader r(out_gpu.c_str());
        auto img = r.Read();
        CHECK(img.metadata.mosaic_pattern_width == 0,
              "linear GPU-fallback output mosaic_pattern_width == 0");
        CHECK(img.pixels.format == burstmerge::PixelFormat::R16_Uint_RGB,
              "linear GPU-fallback output R16_Uint_RGB");
    }

    auto cmp = CompareDngOutputs(out_cpu, out_gpu);
    CHECK(cmp.rel >= 0.0, "linear CPU/GPU comparable");
    if (cmp.rel >= 0.0)
    {
        std::cout << "  MAD=" << cmp.mad << "  rel=" << (cmp.rel * 100.0) << "%" << std::endl;
        // GPU path falls back to CPU, so outputs must be bit-identical.
        CHECK(cmp.mad == 0.0, "linear GPU-fallback bit-identical to CPU");
    }

    auto leaked_bufs = burstmerge::vulkan::VulkanBackend::LastLeakedBuffers();
    CHECK(leaked_bufs == 0,
          "linear GPU-fallback no VRAM leak (" + std::to_string(leaked_bufs) + " buffers)");

    fs::remove(out_cpu);
    fs::remove(out_gpu);
}

int main()
{
    std::cout << "test_stage3: CPU vs GPU dual-path consistency" << std::endl;

    // Prerequisites
    fs::path samples_dir = fs::path(TEST_DATA_DIR) / "libburstmerge" / "test" / "samples";
    auto seq1 = FindSamples(samples_dir / "Seq1", ".arw", 3);
    auto bkt1 = FindSamples(samples_dir / "Bkt1", ".arw", 3);

    const bool have_vulkan = burstmerge::GpuVulkanAvailable();
    const bool have_converter = ConverterAvailable();

    // LinearRaw GPU CPU-fallback: runs whenever Vulkan is available, regardless
    // of Seq1/converter (sample is an already-converted DNG).
    if (have_vulkan)
    {
        TestLinearRawGpuFallback();
    } else
    {
        std::cout << "SKIP linear GPU-fallback: no Vulkan GPU available" << std::endl;
    }

    // Seq1-dependent CPU-vs-GPU consistency matrix.
    if (seq1.size() < 2)
    {
        std::cout << "SKIP consistency matrix: need >=2 ARW samples in Seq1 (found "
                  << seq1.size() << ")" << std::endl;
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return g_failed > 0 ? 1 : 0;
    }
    if (!have_converter)
    {
        std::cout << "SKIP consistency matrix: Adobe DNG Converter not found" << std::endl;
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return g_failed > 0 ? 1 : 0;
    }
    if (!have_vulkan)
    {
        std::cout << "SKIP consistency matrix: no Vulkan GPU available" << std::endl;
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return g_failed > 0 ? 1 : 0;
    }

    std::cout << "Samples: " << seq1.size() << " ARW from Seq1";
    if (bkt1.size() >= 2) std::cout << ", " << bkt1.size() << " ARW from Bkt1";
    std::cout << std::endl;

    // --- Constant-exposure burst: spatial, temporal, freq-laplacian, freq-wiener ---
    burstmerge::Settings s;
    s.tile_size = 32;
    s.noise_reduction = 13.0f;
    s.alignment_mode = burstmerge::AlignmentMode::DenseTile;

    s.merge_algo = burstmerge::MergeAlgorithm::Spatial;
    TestConsistency("seq1 spatial-dense", seq1, s, 0.002f);  // 0.2%

    s.merge_algo = burstmerge::MergeAlgorithm::TemporalAverage;
    TestConsistency("seq1 temporal-dense", seq1, s, 0.002f);

    s.merge_algo = burstmerge::MergeAlgorithm::Frequency;
    s.frequency_mode = burstmerge::FrequencyMode::Laplacian;
    TestConsistency("seq1 freq-laplacian-dense", seq1, s, 0.003f);  // 0.3%

    s.frequency_mode = burstmerge::FrequencyMode::WienerFft;
    TestConsistency("seq1 freq-wiener-dense", seq1, s, 0.002f);

    // --- Bracketed exposure burst (higher threshold due to EV scaling) ---
    if (bkt1.size() >= 2)
    {
        s.merge_algo = burstmerge::MergeAlgorithm::Spatial;
        s.frequency_mode = burstmerge::FrequencyMode::Laplacian;
        TestConsistency("bkt1 spatial-dense", bkt1, s, 0.01f);  // 1.0%
    }

    // --- Spatial-linear mode (uses different blur/noise path) ---
    {
        burstmerge::Settings sl = s;
        sl.merge_algo = burstmerge::MergeAlgorithm::Spatial;
        sl.spatial_mode = burstmerge::SpatialMergeMode::Linear;
        sl.frequency_mode = burstmerge::FrequencyMode::Laplacian;
        TestConsistency("seq1 spatial-linear-dense", seq1, sl, 0.002f);
    }

    // --- Standard alignment (legacy) for completeness ---
    {
        burstmerge::Settings ss = s;
        ss.merge_algo = burstmerge::MergeAlgorithm::Spatial;
        ss.alignment_mode = burstmerge::AlignmentMode::Standard;
        ss.frequency_mode = burstmerge::FrequencyMode::Laplacian;
        TestConsistency("seq1 spatial-standard", seq1, ss, 0.002f);
    }

    // --- Skip alignment (no motion compensation) ---
    {
        burstmerge::Settings ss = s;
        ss.merge_algo = burstmerge::MergeAlgorithm::Spatial;
        ss.alignment_mode = burstmerge::AlignmentMode::Skip;
        ss.frequency_mode = burstmerge::FrequencyMode::Laplacian;
        TestConsistency("seq1 spatial-skip", seq1, ss, 0.002f);
    }

    // --- VRAM stress: repeat GPU run 3x, verify no accumulation ---
    std::cout << "\n--- VRAM stress (3x repeat) ---" << std::endl;
    for (int rep = 1; rep <= 3; ++rep)
    {
        std::string out = RunBackend(burstmerge::BackendType::Vulkan, seq1, s,
                                     fs::temp_directory_path().string() + "/test_stage3_stress.dng");
        CHECK(!out.empty(), "stress run " + std::to_string(rep) + " succeeded");
        auto leaked = burstmerge::vulkan::VulkanBackend::LastLeakedBuffers();
        CHECK(leaked == 0,
              "stress run " + std::to_string(rep) + " no VRAM leak (" +
              std::to_string(leaked) + " buffers leaked)");
        fs::remove(out);
    }

    std::cout << "\n" << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed > 0 ? 1 : 0;
}
