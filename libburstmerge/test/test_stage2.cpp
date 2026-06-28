// test_stage2: GPU alignment unit tests — CPU vs GPU tile-motion consistency.
// Tests standard + dense alignment across multiple tile sizes and sample pairs.
// Acceptance: max |dx|, |dy| <= 2 px on every tile.
//
// Auto-discovers samples in libburstmerge/test/samples/. No CLI args needed.

#include "burstmerge/api.h"
#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/gpu_pipeline.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/pipeline_io.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/dng_io.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace bm = burstmerge;

static int g_checks = 0, g_failed = 0;
#define CHECK(c, m) do { ++g_checks; if(!(c)){ std::cerr << "  FAIL [" << __LINE__ << "]: " << m << std::endl; ++g_failed; } } while(0)

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

static std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), buf.size());
    return buf;
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
    if (int(files.size()) > max_count) files.resize(max_count);
    return files;
}

// Build grayscale plane images from first two ARW files in a sample dir.
// Returns empty on failure.
struct GrayPair { bm::FloatImage g0, g1; std::string tag; };
static GrayPair LoadGrayPair(const std::vector<std::string>& arw_files, const std::string& tag)
{
    GrayPair gp;
    gp.tag = tag;
    if (arw_files.size() < 2) return gp;

    std::string convert_dir = fs::temp_directory_path().string() + "/ts2_convert";
    std::vector<std::string> subset = {arw_files[0], arw_files[1]};
    auto dng_paths = bm::PrepareDngInputs(subset, convert_dir,
        [](float, const std::string&){}, convert_dir);
    if (dng_paths.size() < 2) return gp;

    auto buf0 = ReadFile(dng_paths[0]);
    auto buf1 = ReadFile(dng_paths[1]);
    bm::RawImage r0 = bm::ReadDngFromBuffer(buf0.data(), uint32_t(buf0.size()));
    bm::RawImage r1 = bm::ReadDngFromBuffer(buf1.data(), uint32_t(buf1.size()));

    std::vector<bm::RawImage> raws;
    raws.push_back(std::move(r0));
    raws.push_back(std::move(r1));
    auto planes = bm::BuildFloatImages(raws);
    gp.g0 = bm::ConvertPlanesToGrayscale(planes[0]);
    gp.g1 = bm::ConvertPlanesToGrayscale(planes[1]);

    std::error_code ec;
    fs::remove_all(convert_dir, ec);
    return gp;
}

static void CompareTileMotion(const std::string& tag,
                              const bm::AlignmentResult& cpu,
                              const bm::AlignmentResult& gpu)
{
    std::printf("  [%s] tiles=%ux%u shift=(%d,%d)\n",
                tag.c_str(), cpu.tiles_x, cpu.tiles_y, cpu.shift_x, cpu.shift_y);
    CHECK(cpu.tiles_x == gpu.tiles_x && cpu.tiles_y == gpu.tiles_y,
          tag + ": tile geometry mismatch");
    if (cpu.tiles_x != gpu.tiles_x || cpu.tiles_y != gpu.tiles_y) return;

    int max_dx = 0, max_dy = 0;
    int over2 = 0;
    size_t n = cpu.tile_shift_x.size();
    for (size_t i = 0; i < n; ++i)
    {
        int dx = std::abs(int(cpu.tile_shift_x[i]) - int(gpu.tile_shift_x[i]));
        int dy = std::abs(int(cpu.tile_shift_y[i]) - int(gpu.tile_shift_y[i]));
        if (dx > max_dx) max_dx = dx;
        if (dy > max_dy) max_dy = dy;
        if (dx > 2 || dy > 2) ++over2;
    }
    std::printf("  [%s] max|d|=(%d,%d)  >2px:%d/%zu (%.1f%%)\n",
                tag.c_str(), max_dx, max_dy, over2, n,
                n > 0 ? 100.0 * over2 / n : 0.0);
    CHECK(max_dx <= 2 && max_dy <= 2,
          tag + ": max tile-motion diff (" + std::to_string(max_dx) + "," +
          std::to_string(max_dy) + ") > 2px");
}

int main()
{
    std::cout << "test_stage2: GPU alignment unit tests" << std::endl;

    // ---- Synthetic Skip-alignment unit tests (always run, no deps) ----
    std::cout << "\n--- Skip alignment unit tests (synthetic) ---" << std::endl;
    {
        bm::FloatImage ref;
        ref.width = 32; ref.height = 32; ref.channels = 1;
        ref.data.resize(32u * 32u);
        for (size_t i = 0; i < ref.data.size(); ++i)
            ref.data[i] = static_cast<float>(i % 1024);

        bm::FloatImage cmp = ref;

        bm::AlignParams params;
        params.tile_size = 16;
        params.search_distance = 64;
        params.mode = bm::AlignmentMode::Skip;
        params.cfa_period = 1;
        params.align_gamma = 1.0f;
        params.smooth_tile_field = false;

        auto ar = bm::EstimateTranslation(ref, cmp, params);
        CHECK(ar.shift_x == 0, "skip shift_x == 0");
        CHECK(ar.shift_y == 0, "skip shift_y == 0");
        CHECK(ar.tiles_x == 0, "skip tiles_x == 0");
        CHECK(ar.tiles_y == 0, "skip tiles_y == 0");
        CHECK(ar.tile_shift_x.empty(), "skip tile_shift_x empty");
        CHECK(ar.tile_shift_y.empty(), "skip tile_shift_y empty");
        CHECK(ar.confidence > 0.0f, "skip confidence > 0");

        {
            auto warped = bm::WarpAligned(ref, ar);
            CHECK(warped.width == ref.width && warped.height == ref.height,
                  "skip warp preserves geometry (1ch)");
            bool identity = true;
            for (size_t i = 0; i < ref.data.size(); ++i)
                if (warped.data[i] != ref.data[i]) { identity = false; break; }
            CHECK(identity, "skip WarpAligned identity (1ch)");
        }

        {
            bm::FloatImage rgb;
            rgb.width = 16; rgb.height = 16; rgb.channels = 3;
            rgb.data.resize(16u * 16u * 3u);
            for (size_t i = 0; i < rgb.data.size(); ++i)
                rgb.data[i] = static_cast<float>(i);
            auto warped = bm::WarpAligned(rgb, ar);
            CHECK(warped.width == rgb.width && warped.height == rgb.height &&
                  warped.channels == rgb.channels,
                  "skip warp preserves geometry (3ch)");
            bool identity = true;
            for (size_t i = 0; i < rgb.data.size(); ++i)
                if (warped.data[i] != rgb.data[i]) { identity = false; break; }
            CHECK(identity, "skip WarpAligned identity (3ch)");
        }

        {
            bm::FloatImage shifted;
            shifted.width = 32; shifted.height = 32; shifted.channels = 1;
            shifted.data.resize(32u * 32u, 0.0f);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                {
                    int sx = x - 3, sy = y - 2;
                    if (sx >= 0 && sx < 32 && sy >= 0 && sy < 32)
                        shifted.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0) =
                            ref.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), 0);
                }

            bm::AlignParams skip_p = params;
            auto skip_ar = bm::EstimateTranslation(ref, shifted, skip_p);
            CHECK(skip_ar.shift_x == 0 && skip_ar.shift_y == 0,
                  "skip on offset frames: shift still 0");

            bm::AlignParams std_p = params;
            std_p.mode = bm::AlignmentMode::Standard;
            auto std_ar = bm::EstimateTranslation(ref, shifted, std_p);
            CHECK(std_ar.shift_x != 0 || std_ar.shift_y != 0,
                  "standard on offset frames: detects motion (regression guard)");
        }
    }
    std::cout << g_checks << " checks so far, " << g_failed << " failed" << std::endl;

    fs::path samples = fs::path(TEST_DATA_DIR) / "libburstmerge" / "test" / "samples";
    auto seq1 = FindSamples(samples / "Seq1", ".arw", 2);
    auto seq3 = FindSamples(samples / "Seq3", ".arw", 2);

    if (seq1.size() < 2)
    {
        std::cout << "SKIP: need >=2 ARW samples in Seq1" << std::endl;
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return 0;
    }
    if (!ConverterAvailable())
    {
        std::cout << "SKIP: Adobe DNG Converter not found" << std::endl;
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return 0;
    }
    if (!bm::GpuVulkanAvailable())
    {
        std::cout << "SKIP: no Vulkan GPU available" << std::endl;
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return 0;
    }

    // Load sample pairs
    std::vector<GrayPair> pairs;
    std::cout << "Loading Seq1..." << std::endl;
    auto p1 = LoadGrayPair(seq1, "seq1");
    if (p1.g0.width > 0) pairs.push_back(std::move(p1));
    if (seq3.size() >= 2)
    {
        std::cout << "Loading Seq3..." << std::endl;
        auto p3 = LoadGrayPair(seq3, "seq3");
        if (p3.g0.width > 0) pairs.push_back(std::move(p3));
    }
    CHECK(!pairs.empty(), "at least one sample pair loaded");
    if (pairs.empty())
    {
        std::cout << g_checks << " checks, " << g_failed << " failed" << std::endl;
        return 1;
    }

    // Configurations to test: {tile_size, search_distance, mode_name}
    struct Config { int tile_size; int search_distance; bm::AlignmentMode mode; };
    std::vector<Config> configs = {
        {32, 64, bm::AlignmentMode::Standard},
        {32, 64, bm::AlignmentMode::DenseTile},
        {16, 64, bm::AlignmentMode::DenseTile},
        {64, 64, bm::AlignmentMode::DenseTile},
        {32, 32, bm::AlignmentMode::DenseTile},
        {32, 128, bm::AlignmentMode::DenseTile},
        {32, 64, bm::AlignmentMode::Skip},
    };

    for (auto& pair : pairs)
    {
        std::cout << "\n=== " << pair.tag << " (" << pair.g0.width << "x"
                  << pair.g0.height << ") ===" << std::endl;
        for (auto& cfg : configs)
        {
            bm::AlignParams params;
            params.tile_size = cfg.tile_size;
            params.search_distance = cfg.search_distance;
            params.cfa_period = 1;
            params.align_gamma = 1.0f;
            params.smooth_tile_field = false;
            params.mode = cfg.mode;

            const char* mode_str;
            if (cfg.mode == bm::AlignmentMode::DenseTile) mode_str = "dense";
            else if (cfg.mode == bm::AlignmentMode::Skip) mode_str = "skip";
            else mode_str = "std";
            std::string tag = pair.tag + "/" + mode_str + "/ts" +
                std::to_string(cfg.tile_size) + "/sd" + std::to_string(cfg.search_distance);

            auto cpu = bm::EstimateTranslation(pair.g0, pair.g1, params);
            auto gpu = bm::GpuEstimateTranslation(pair.g0, pair.g1, params);
            CompareTileMotion(tag, cpu, gpu);
        }
    }

    std::cout << "\n" << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed ? 1 : 0;
}
