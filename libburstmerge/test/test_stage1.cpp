#include "burstmerge/api.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/core/float_image.h"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failed = 0;
static uint32_t g_single_frame_white_level = 0;

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

void TestComprehensiveBitDepth(const std::string& name,
                                const std::string& input,
                                int bit_depth,
                                uint32_t expected_white_level,
                                const std::string& output_path)
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
            burstmerge::DngReader orig_reader(input.c_str());
            target_white = orig_reader.Read().metadata.white_level;
        }

        CHECK(image.metadata.white_level == target_white,
              name + " white level matches expected (" + std::to_string(target_white) + ", got " + std::to_string(image.metadata.white_level) + ")");
        CHECK(image.metadata.dng_pixel_type == burstmerge::DngPixelType::Uint16,
              name + " dng_pixel_type is Uint16");
        CHECK(image.pixels.format == burstmerge::PixelFormat::R16_Uint,
              name + " pixels.format is R16_Uint");

        burstmerge::FloatImage fi = burstmerge::HostBufferToFloatImage(image.pixels);
        float max_val = 0.0f;
        float min_val = fi.data.empty() ? 0.0f : fi.data[0];
        for (float v : fi.data)
        {
            if (v > max_val) max_val = v;
            if (v < min_val) min_val = v;
        }
        CHECK(min_val >= 0.0f, name + " pixel min >= 0");
        CHECK(max_val <= static_cast<float>(target_white) * 1.001f,
              name + " pixel max within white level bounds (" + std::to_string(static_cast<int>(max_val)) + " <= " + std::to_string(target_white) + ")");
        if (expected_white_level > 0 && !fi.data.empty())
        {
            CHECK(max_val > 0.99f * static_cast<float>(target_white),
                  name + " max pixel reaches near white (" + std::to_string(static_cast<int>(max_val)) + " > " + std::to_string(static_cast<int>(0.99f * target_white)) + ")");
            size_t n = std::max<size_t>(100, fi.data.size() / 100);
            std::vector<float> bright(fi.data);
            std::partial_sort(bright.begin(),
                              bright.begin() + n,
                              bright.end(),
                              std::greater<float>());
            double top_mean = 0.0;
            for (size_t i = 0; i < n; ++i) top_mean += bright[i];
            top_mean /= static_cast<double>(n);
            CHECK(top_mean > 0.5 * static_cast<double>(target_white),
                  name + " top ~1% average > 50% white (" + std::to_string(static_cast<int>(top_mean)) + " vs " + std::to_string(static_cast<int>(0.5 * target_white)) + ")");
        }
    }
}

void TestSingleFrameProcessing(const std::string& name,
                                const std::string& input,
                                const std::string& output_path)
{
    std::cout << "[test] " << name << "..." << std::endl;

    fs::create_directories(fs::path(output_path).parent_path());
    std::remove(output_path.c_str());

    burstmerge::DngReader orig_reader(input.c_str());
    auto orig = orig_reader.Read();
    uint32_t orig_w = orig.metadata.width;
    uint32_t orig_h = orig.metadata.height;

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    burstmerge::Settings settings;
    settings.bit_depth = 14;
    bm.Configure(settings);

    int progress_calls = 0;
    float last_progress = -1.0f;
    bm.SetProgressCallback([&](float p, const std::string&) {
        ++progress_calls;
        last_progress = p;
    });

    bm.AddImage(input);
    auto result = bm.Process(output_path);

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

        CHECK(image.metadata.width == orig_w, name + " width preserved");
        CHECK(image.metadata.height == orig_h, name + " height preserved");
        g_single_frame_white_level = image.metadata.white_level;
        CHECK(image.metadata.white_level == 16383, name + " default 14-bit white level");
        CHECK(image.metadata.dng_pixel_type == burstmerge::DngPixelType::Uint16,
              name + " dng_pixel_type Uint16");
        CHECK(image.pixels.format == burstmerge::PixelFormat::R16_Uint,
              name + " pixels.format R16_Uint");
        CHECK(image.pixels.data != nullptr, name + " pixels not null");
        CHECK(image.pixels.size > 0, name + " pixels size > 0");

        bool has_non_zero = false;
        for (size_t i = 0; i < image.pixels.size && i < 4096; ++i)
        {
            if (image.pixels.data[i] != std::byte{0}) {
                has_non_zero = true;
                break;
            }
        }
        CHECK(has_non_zero, name + " pixel data non-zero");
    }
}

void TestMultiFrameProcessing(const std::string& name,
                               const std::vector<std::string>& inputs,
                               const std::string& output_path)
{
    std::cout << "[test] multi " << name << "..." << std::endl;

    fs::create_directories(fs::path(output_path).parent_path());
    std::remove(output_path.c_str());

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    burstmerge::Settings settings;
    settings.bit_depth = 14;
    bm.Configure(settings);

    int progress_calls = 0;
    float last_progress = -1.0f;
    bm.SetProgressCallback([&](float p, const std::string&) {
        ++progress_calls;
        last_progress = p;
    });

    for (const auto& input : inputs) bm.AddImage(input);
    auto result = bm.Process(output_path);

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
        CHECK(image.metadata.width > 0, name + " width > 0");
        CHECK(image.metadata.height > 0, name + " height > 0");
        CHECK(image.metadata.white_level == 16383,
              name + " white_level == 16383 (got " + std::to_string(image.metadata.white_level) + ")");
        CHECK(image.metadata.dng_pixel_type == burstmerge::DngPixelType::Uint16,
              name + " dng_pixel_type Uint16");
        CHECK(image.pixels.format == burstmerge::PixelFormat::R16_Uint,
              name + " pixels.format R16_Uint");
        CHECK(g_single_frame_white_level == 0 ||
              image.metadata.white_level == g_single_frame_white_level,
              name + " white_level consistent with single-frame ("
              + std::to_string(g_single_frame_white_level) + " vs "
              + std::to_string(image.metadata.white_level) + ")");

        bool has_non_zero = false;
        for (size_t i = 0; i < image.pixels.size && i < 4096; ++i)
        {
            if (image.pixels.data[i] != std::byte{0}) {
                has_non_zero = true;
                break;
            }
        }
        CHECK(has_non_zero, name + " pixel data non-zero");
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
    std::string dng_path = (samples / "X1M5_Wide.dng").string();

    ProcessAndVerify("single_dng",
        { dng_path },
        (out_dir / "single_dng_output.dng").string());

    TestComprehensiveBitDepth("single_12bit", dng_path, 12, 4095, (out_dir / "single_12bit_output.dng").string());
    TestComprehensiveBitDepth("single_14bit", dng_path, 14, 16383, (out_dir / "single_14bit_output.dng").string());
    TestComprehensiveBitDepth("single_16bit", dng_path, 16, 65535, (out_dir / "single_16bit_output.dng").string());
    TestComprehensiveBitDepth("single_invalid_bit", dng_path, 99, 0, (out_dir / "single_invalid_bit_output.dng").string());

    TestSingleFrameProcessing("single_frame", dng_path, (out_dir / "single_frame_output.dng").string());
    TestMultiFrameProcessing("two_identical_frames",
        { dng_path, dng_path },
        (out_dir / "two_frames_output.dng").string());
    TestMultiFrameProcessing("three_identical_frames",
        { dng_path, dng_path, dng_path },
        (out_dir / "three_frames_output.dng").string());

    if (ConverterAvailable())
    {
        auto seq = FilesWithExt(samples / "Seq1", ".arw");
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
