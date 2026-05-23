#include "burstmerge/api.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/core/float_image.h"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cmath>
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

bool FileExists(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

long FileSize(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fclose(f);
    return size;
}

std::vector<std::string> FilesWithExt(const fs::path& dir, const std::string& ext)
{
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;
        std::string e = entry.path().extension().string();
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e == ext) files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool ConverterAvailable()
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA("C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe");
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

void ProcessAndVerify(const std::string& name,
                      const std::vector<std::string>& inputs,
                      const std::string& output_path)
{
    std::cout << "[test] process " << name << "..." << std::endl;
    CHECK(!inputs.empty(), name + " inputs non-empty");
    if (inputs.empty()) return;

    fs::create_directories(fs::path(output_path).parent_path());
    std::remove(output_path.c_str());

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    burstmerge::Settings settings;
    settings.merge_algo = burstmerge::MergeAlgorithm::Spatial;
    settings.noise_reduction = 13.0f;
    bm.Configure(settings);

    int progress_calls = 0;
    float last_progress = -1.0f;
    bm.SetProgressCallback([&](float p, const std::string& stage)
    {
        (void)stage;
        ++progress_calls;
        last_progress = p;
    });

    for (const auto& input : inputs) bm.AddImage(input);
    auto result = bm.Process(output_path);

    if (!result.success)
    {
        std::cout << "  error: " << result.error_msg << std::endl;
    }
    CHECK(result.success, name + " process succeeds");
    CHECK(result.output_path == output_path, name + " output path exact");
    CHECK(FileExists(output_path), name + " output exists");
    CHECK(FileSize(output_path) > 1024, name + " output size > 1KB");
    CHECK(progress_calls >= 2, name + " progress callbacks");
    CHECK(last_progress == 1.0f, name + " progress reaches 1");

    if (FileExists(output_path))
    {
        burstmerge::DngReader reader(output_path.c_str());
        auto image = reader.Read();
        CHECK(image.metadata.width > 0, name + " readable output width");
        CHECK(image.metadata.height > 0, name + " readable output height");
        CHECK(image.pixels.data != nullptr, name + " readable output pixels");
    }
}

void TestBitDepthOutput(const std::string& name, const std::string& input, int bit_depth, uint32_t expected_white_level, const std::string& output_path)
{
    std::cout << "[test] process " << name << " (bit depth " << bit_depth << ")..." << std::endl;

    fs::create_directories(fs::path(output_path).parent_path());
    std::remove(output_path.c_str());

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    burstmerge::Settings settings;
    settings.bit_depth = bit_depth;
    bm.Configure(settings);

    bm.AddImage(input);
    auto result = bm.Process(output_path);
    CHECK(result.success, name + " process succeeds");
    CHECK(FileExists(output_path), name + " output exists");

    if (FileExists(output_path))
    {
        burstmerge::DngReader reader(output_path.c_str());
        auto image = reader.Read();

        uint32_t target_white = expected_white_level;
        if (target_white == 0)
        {
            // Read original to find its native white level
            burstmerge::DngReader orig_reader(input.c_str());
            target_white = orig_reader.Read().metadata.white_level;
        }

        CHECK(image.metadata.white_level == target_white,
              name + " white level matches expected (" + std::to_string(target_white) + ", got " + std::to_string(image.metadata.white_level) + ")");

        // We can also sample the image data to ensure it's bounded by the new white level
        burstmerge::FloatImage fi = burstmerge::HostBufferToFloatImage(image.pixels);
        float max_val = 0.0f;
        for (float v : fi.data)
        {
            if (v > max_val) max_val = v;
        }
        CHECK(max_val <= static_cast<float>(target_white) * 1.001f,
              name + " pixel max value within white level bounds");
    }
}

// Regression: after merging a bracketed burst with a bright source at center,
// the center pixels must stay near white_level and not be dragged down by
// clipped comparison frames. This would manifest as purple/magenta highlights.
void CheckCenterSaturated(const std::string& name, const std::string& dng_path)
{
    if (!FileExists(dng_path)) return;

    burstmerge::DngReader reader(dng_path.c_str());
    auto img = reader.Read();

    float white = static_cast<float>(img.metadata.white_level);
    uint32_t cx = img.metadata.width / 2;
    uint32_t cy = img.metadata.height / 2;

    // Read center 2x2 Bayer block
    burstmerge::FloatImage fi = burstmerge::HostBufferToFloatImage(img.pixels);
    float vals[4];
    vals[0] = fi.At(cx, cy, 0);
    vals[1] = fi.At(cx + 1, cy, 0);
    vals[2] = fi.At(cx, cy + 1, 0);
    vals[3] = fi.At(cx + 1, cy + 1, 0);

    float min_val = *std::min_element(vals, vals + 4);
    float max_val = *std::max_element(vals, vals + 4);
    float threshold = white * 0.90f;

    // Each of the 4 CFA pixels at the center must be near white_level,
    // confirming comparison-frame clipping did not drag the highlight down.
    // The max/min spread should also be small (all channels equally saturated).
    CHECK(min_val >= threshold, name + " center saturated (min=" + std::to_string((int)min_val) + " < 90% white)");
    CHECK(max_val - min_val <= white * 0.15f,
        name + " center channels balanced (spread=" + std::to_string((int)(max_val - min_val)) + ")");

    std::cout << "  center: " << (int)vals[0] << " " << (int)vals[1] << " "
              << (int)vals[2] << " " << (int)vals[3] << " (thresh=" << (int)threshold << ")" << std::endl;
}

int main()
{
    fs::path root(TEST_DATA_DIR);
    fs::path build(TEST_BINARY_DIR);
    fs::path samples = root / "libburstmerge" / "test" / "samples";
    fs::path out_dir = build / "stage1_outputs";

    ProcessAndVerify("single_dng",
        { (samples / "X1M5_Wide.dng").string() },
        (out_dir / "single_dng_output.dng").string());

    TestBitDepthOutput("single_12bit", (samples / "X1M5_Wide.dng").string(), 12, 4095, (out_dir / "single_12bit_output.dng").string());
    TestBitDepthOutput("single_14bit", (samples / "X1M5_Wide.dng").string(), 14, 16383, (out_dir / "single_14bit_output.dng").string());
    TestBitDepthOutput("single_16bit", (samples / "X1M5_Wide.dng").string(), 16, 65535, (out_dir / "single_16bit_output.dng").string());
    // Test robustness: invalid/unsupported bit depth requests should gracefully fall back to the sensor's native white level.
    TestBitDepthOutput("single_invalid_bit", (samples / "X1M5_Wide.dng").string(), 99, 0, (out_dir / "single_invalid_bit_output.dng").string());

    if (ConverterAvailable())
    {
        auto seq = FilesWithExt(samples / "Seq1", ".arw");
        // auto bkt = FilesWithExt(samples / "Bkt", ".arw");
        std::vector<std::string> bkt2;
        if (fs::exists(samples / "Bkt2"))
        {
            bkt2 = FilesWithExt(samples / "Bkt2", ".arw");
        }
        ProcessAndVerify("seq_arw_5", seq, (out_dir / "seq_output.dng").string());
        if (!bkt2.empty())
        {
            ProcessAndVerify("bkt2_arw_5", bkt2, (out_dir / "bkt2_output.dng").string());
            CheckCenterSaturated("bkt2_center", (out_dir / "bkt2_output.dng").string());
        } else
        {
            std::cout << "[test] SKIP bkt2_arw_5 (samples/Bkt2 not found)" << std::endl;
        }
    } else
    {
        std::cout << "[test] SKIP ARW process tests: Adobe DNG Converter not installed" << std::endl;
    }

    std::cout << "\nStage 1: " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed > 0 ? 1 : 0;
}
