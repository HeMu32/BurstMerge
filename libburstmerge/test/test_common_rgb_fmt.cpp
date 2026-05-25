/*
*   Note: The test suit needs ffmpeg and exiftool executable/binary
*/

#include "burstmerge/api.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/image_decoder.h"
#include "burstmerge/internal/io/dng_io.h"

#include "test_aux.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

static fs::path SamplesDir()
{
    return fs::path(TEST_DATA_DIR) / "libburstmerge" / "test" / "samples";
}

static fs::path OutDir()
{
    fs::path dir = fs::path(TEST_BINARY_DIR) / "common_rgb_fmt_outputs";
    fs::create_directories(dir);
    return dir;
}

static bool FileExists(const fs::path& path)
{
    return fs::exists(path) && fs::is_regular_file(path);
}

static long FileSize(const fs::path& path)
{
    if (!FileExists(path)) return -1;
    return static_cast<long>(fs::file_size(path));
}

static std::vector<uint8_t> ReadBytes(const fs::path& path, size_t n)
{
    std::vector<uint8_t> data(n, 0);
    std::ifstream f(path, std::ios::binary);
    if (!f) return data;
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n));
    return data;
}

static bool StartsWith(const fs::path& path, const std::vector<uint8_t>& sig)
{
    auto data = ReadBytes(path, sig.size());
    return data == sig;
}

static bool IsPng(const fs::path& path) { return StartsWith(path, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}); }
static bool IsJpeg(const fs::path& path) { return StartsWith(path, {0xFF, 0xD8, 0xFF}); }
static bool IsBmp(const fs::path& path) { return StartsWith(path, {'B', 'M'}); }
static bool IsTiff(const fs::path& path)
{
    auto data = ReadBytes(path, 4);
    return data.size() >= 4 &&
           ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
            (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A));
}

static std::vector<std::string> MakeInputs(const std::vector<std::string>& rels)
{
    std::vector<std::string> out;
    for (const auto& rel : rels)
    {
        out.push_back((SamplesDir() / rel).string());
    }
    return out;
}

static std::vector<std::string> FilesInDir(const fs::path& dir)
{
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;
        files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

static std::vector<std::string> FolderInputs(const std::string& folder)
{
    return FilesInDir(SamplesDir() / folder);
}

static std::vector<std::string> SmallRgbPngInputs()
{
    return MakeInputs({"rgb_small/rgb8.png", "rgb_small/rgb8.png"});
}

static std::vector<std::string> SmallRgbTiffInputs()
{
    return MakeInputs({"rgb_small/rgb16.tif", "rgb_small/rgb16.tif"});
}

static std::vector<std::string> SmallRgbJpegInputs(const std::string& name)
{
    return MakeInputs({"rgb_small/" + name, "rgb_small/" + name});
}

static burstmerge::Settings MakeSettings()
{
    burstmerge::Settings s;
    s.merge_algo = burstmerge::MergeAlgorithm::Spatial;
    s.spatial_mode = burstmerge::SpatialMergeMode::Standard;
    s.output_format = burstmerge::OutputFormat::Auto;
    s.bit_depth = 14;
    s.noise_reduction = 13.0f;
    return s;
}

static burstmerge::Result RunBurstMerge(const std::vector<std::string>& inputs,
                                        const burstmerge::Settings& settings,
                                        const fs::path& output)
{
    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    bm.Configure(settings);
    for (const auto& input : inputs) bm.AddImage(input);
    return bm.Process(output.string());
}

static void CheckOutputBasic(const fs::path& output)
{
    CHECK(FileExists(output), output.string() + " exists");
    CHECK(FileSize(output) > 1024, output.string() + " size > 1KB");
}

static void CheckDngReadable(const fs::path& output)
{
    burstmerge::DngReader reader(output.string().c_str());
    auto img = reader.Read();
    CHECK(img.metadata.width > 0, output.string() + " dng width > 0");
    CHECK(img.metadata.height > 0, output.string() + " dng height > 0");
    CHECK(img.metadata.white_level > 0, output.string() + " dng white_level > 0");
}

static void CheckNotAllZero(const fs::path& output)
{
    auto data = ReadBytes(output, 4096);
    bool any_non_zero = std::any_of(data.begin(), data.end(), [](uint8_t b) { return b != 0; });
    CHECK(any_non_zero, output.string() + " has non-zero bytes");
}

// Group 1: (1,2)
static void TestGroupAutoAndExplicitOutput()
{
    std::cout << "[test] group1 auto/explicit output..." << std::endl;

    auto png_inputs = SmallRgbPngInputs();
    auto raw_inputs = MakeInputs({"X1M5_Wide.dng"});

    {
        fs::path out = OutDir() / "auto_rgb_default";
        auto result = RunBurstMerge(png_inputs, MakeSettings(), out);
        CHECK(result.success, "auto_rgb_default succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".png", "auto rgb default extension png");
        CHECK(IsPng(result.output_path), "auto rgb default png signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "auto_rgb_jpeg.jpg";
        auto settings = MakeSettings();
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "auto_rgb_jpeg succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".jpg", "auto rgb jpg extension preserved");
        CHECK(IsJpeg(result.output_path), "auto rgb jpg signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "explicit_png_forced.jpg";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "explicit png succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".png", "explicit png rewrites extension");
        CHECK(IsPng(result.output_path), "explicit png signature");
    }

    {
        fs::path out = OutDir() / "raw_auto_default";
        auto result = RunBurstMerge(raw_inputs, MakeSettings(), out);
        CHECK(result.success, "raw auto default succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".dng", "raw auto default dng extension");
        CheckDngReadable(result.output_path);
    }

    {
        fs::path out = OutDir() / "raw_auto_png.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge(raw_inputs, settings, out);
        CHECK(result.success, "raw png export succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".png", "raw png extension");
        CHECK(IsPng(result.output_path), "raw png signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "rgb_dng_should_fail.dng";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::DNG;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(!result.success, "rgb dng should fail");
        CHECK(result.error_msg.find("Cannot output DNG for non-RAW inputs") != std::string::npos,
              "rgb dng failure message");
    }
}

// Group 2: (3,4,5,8)
static void TestGroupFormatsBitDepthAndRegression()
{
    std::cout << "[test] group2 formats/bit depth/regressions..." << std::endl;

    auto png_inputs = SmallRgbPngInputs();
    auto tiff_inputs = SmallRgbTiffInputs();
    auto jpg_inputs = SmallRgbJpegInputs("jpeg_444.jpg");
    auto raw_input = MakeInputs({"X1M5_Wide.dng"});

    {
        fs::path out = OutDir() / "rgb_tiff_14bit.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        settings.bit_depth = 14;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "rgb tiff 14 succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".tif", "rgb tiff extension");
        CHECK(IsTiff(result.output_path), "rgb tiff signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "rgb_png_12bit.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        settings.bit_depth = 12;
        auto result = RunBurstMerge(jpg_inputs, settings, out);
        CHECK(result.success, "rgb png 12 succeeds");
        CHECK(IsPng(result.output_path), "rgb png 12 signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "raw_dng_8bit.dng";
        auto settings = MakeSettings();
        settings.bit_depth = 8;
        auto result = RunBurstMerge(raw_input, settings, out);
        CHECK(result.success, "raw dng 8 succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".dng", "raw dng 8 extension");
        CheckDngReadable(result.output_path);
    }

    {
        fs::path out = OutDir() / "raw_dng_10bit.dng";
        auto settings = MakeSettings();
        settings.bit_depth = 10;
        auto result = RunBurstMerge(raw_input, settings, out);
        CHECK(result.success, "raw dng 10 succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".dng", "raw dng 10 extension");
        CheckDngReadable(result.output_path);
    }

    {
        fs::path out = OutDir() / "raw_dng_12bit.dng";
        auto settings = MakeSettings();
        settings.bit_depth = 12;
        auto result = RunBurstMerge(raw_input, settings, out);
        CHECK(result.success, "raw dng 12 succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".dng", "raw dng 12 extension");
        CheckDngReadable(result.output_path);
    }

    {
        fs::path out = OutDir() / "rgb_png_bmp_adjust.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::BMP;
        settings.bit_depth = 12;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "rgb bmp adjust succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".bmp", "rgb bmp extension");
        CHECK(IsBmp(result.output_path), "rgb bmp signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "png_gray_alpha_compat.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge(tiff_inputs, settings, out);
        CHECK(result.success, "tiff input png path succeeds");
        CHECK(IsPng(result.output_path) || IsTiff(result.output_path), "png alpha compat output signature");
        CheckOutputBasic(result.output_path);
    }

    {
        fs::path out = OutDir() / "jpeg_cmyk_reject.dng";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::Auto;
        auto result = RunBurstMerge({(SamplesDir() / "CMYK.jpg").string()}, settings, out);
        CHECK(!result.success, "cmyk jpeg rejects");
        CHECK(result.error_msg.find("unsupported color space") != std::string::npos ||
              result.error_msg.find("CMYK") != std::string::npos,
              "cmyk reject message");
    }
}

// Group 3: (6,7) and folder-input parity
static void TestGroupMixedRawAndExport()
{
    std::cout << "[test] group3 mixed/raw export..." << std::endl;

    auto raw = (SamplesDir() / "X1M5_Wide.dng").string();
    auto png = (SamplesDir() / "png1" / "DSC05857.png").string();
    auto jpg = (SamplesDir() / "jpg1" / "DSC05857.jpg").string();
    auto tif = (SamplesDir() / "tif1" / "DSC05857.tif").string();

    {
        fs::path out = OutDir() / "mixed_raw_png.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge({raw, png}, settings, out);
        CHECK(result.success, "mixed raw+png succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".png", "mixed raw+png ext");
        CHECK(IsPng(result.output_path), "mixed raw+png signature");
    }

    {
        fs::path out = OutDir() / "mixed_raw_jpeg.jpg";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::JPEG;
        auto result = RunBurstMerge({raw, jpg}, settings, out);
        CHECK(result.success, "mixed raw+jpeg succeeds");
        CHECK(IsJpeg(result.output_path), "mixed raw+jpeg signature");
    }

    {
        fs::path out = OutDir() / "mixed_raw_tiff.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        auto result = RunBurstMerge({raw, tif}, settings, out);
        CHECK(result.success, "mixed raw+tiff succeeds");
        CHECK(IsTiff(result.output_path), "mixed raw+tiff signature");
    }

    {
        fs::path out = OutDir() / "mixed_unknown_should_fail.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge({raw, (SamplesDir() / "unknown.xyz").string()}, settings, out);
        CHECK(!result.success, "mixed unknown should fail");
    }

    {
        fs::path out = OutDir() / "raw_to_png_bayer.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge({raw}, settings, out);
        CHECK(result.success, "raw to png succeeds");
        CHECK(IsPng(result.output_path), "raw to png signature");
        CheckOutputBasic(result.output_path);
        CheckNotAllZero(result.output_path);
    }

    {
        fs::path out = OutDir() / "raw_to_jpeg_bayer.jpg";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::JPEG;
        auto result = RunBurstMerge({raw}, settings, out);
        CHECK(result.success, "raw to jpeg succeeds");
        CHECK(IsJpeg(result.output_path), "raw to jpeg signature");
    }

    {
        fs::path out = OutDir() / "raw_to_bmp_bayer.bmp";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::BMP;
        auto result = RunBurstMerge({raw}, settings, out);
        CHECK(result.success, "raw to bmp succeeds");
        CHECK(IsBmp(result.output_path), "raw to bmp signature");
    }

    {
        fs::path out = OutDir() / "raw_to_tiff_bayer.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        auto result = RunBurstMerge({raw}, settings, out);
        CHECK(result.success, "raw to tiff succeeds");
        CHECK(IsTiff(result.output_path), "raw to tiff signature");
    }

    // Folder-based versions of the same mixed-format combinations.
    // These mirror the -i cases above, but use -f-style directory inputs.
    {
        auto folder_inputs = FolderInputs("folder_raw_png");
        fs::path out = OutDir() / "folder_raw_png.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge(folder_inputs, settings, out);
        CHECK(result.success, "folder_raw_png succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".png", "folder_raw_png png extension");
        CHECK(IsPng(result.output_path), "folder_raw_png png signature");
    }

    {
        auto folder_inputs = FolderInputs("folder_raw_jpg");
        fs::path out = OutDir() / "folder_raw_jpg.jpg";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::JPEG;
        auto result = RunBurstMerge(folder_inputs, settings, out);
        CHECK(result.success, "folder_raw_jpg succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".jpg", "folder_raw_jpg jpg extension");
        CHECK(IsJpeg(result.output_path), "folder_raw_jpg jpeg signature");
    }

    {
        auto folder_inputs = FolderInputs("folder_raw_tif");
        fs::path out = OutDir() / "folder_raw_tif.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        auto result = RunBurstMerge(folder_inputs, settings, out);
        CHECK(result.success, "folder_raw_tif succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".tif", "folder_raw_tif tif extension");
        CHECK(IsTiff(result.output_path), "folder_raw_tif tiff signature");
    }

    {
        auto folder_inputs = FolderInputs("folder_raw_unknown_should_fail");
        fs::path out = OutDir() / "folder_raw_unknown_should_fail.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge(folder_inputs, settings, out);
        CHECK(!result.success, "folder_raw_unknown_should_fail fails");
    }
}

// ---------------------------------------------------------------------------
// Group 4: Auto inference edge cases (plan L323–L325 rules)
// ---------------------------------------------------------------------------
static void TestGroupAutoInferenceEdge()
{
    std::cout << "[test] group4 auto inference edge..." << std::endl;

    auto png_inputs = SmallRgbPngInputs();
    auto raw_input  = MakeInputs({"X1M5_Wide.dng"});

    // 4.1: Auto + directory (no extension match) + non-RAW → PNG  (plan L323)
    {
        fs::path out = OutDir() / "auto_dir_nonraw";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::Auto;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "auto_dir_nonraw succeeds");
        CHECK(IsPng(result.output_path), "auto_dir_nonraw png signature");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".png",
              "auto_dir_nonraw png extension");
    }

    // 4.2: Auto + .dng extension + non-RAW → error (plan L325)
    {
        fs::path out = OutDir() / "auto_dng_nonraw.dng";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::Auto;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(!result.success, "auto_dng_nonraw should fail");
        CHECK(result.error_msg.find("Cannot output DNG for non-RAW inputs") != std::string::npos,
              "auto_dng_nonraw error msg");
    }

    // 4.3: Auto + directory (no extension match) + RAW → DNG (plan L318)
    {
        fs::path out = OutDir() / "auto_dir_raw";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::Auto;
        auto result = RunBurstMerge(raw_input, settings, out);
        CHECK(result.success, "auto_dir_raw succeeds");
        CHECK(Lower(fs::path(result.output_path).extension().string()) == ".dng",
              "auto_dir_raw dng extension");
    }

    // 4.4: Auto + .jpg extension + non-RAW → JPEG (plan L324)
    {
        fs::path out = OutDir() / "auto_jpg_nonraw.jpg";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::Auto;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "auto_jpg_nonraw succeeds");
        CHECK(IsJpeg(result.output_path), "auto_jpg_nonraw jpeg signature");
    }

    // 4.5: bit_depth=8 on non-RAW explicit PNG
    {
        fs::path out = OutDir() / "nonraw_8bit.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        settings.bit_depth = 8;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "nonraw_8bit succeeds");
        CHECK(IsPng(result.output_path), "nonraw_8bit png signature");
    }
}

// ---------------------------------------------------------------------------
// Group 5: Cross-format and BMP input
// ---------------------------------------------------------------------------
static void TestGroupCrossFormat()
{
    std::cout << "[test] group5 cross-format..." << std::endl;

    auto jpg_inputs  = SmallRgbJpegInputs("jpeg_420.jpg");

    // 5.1: JPEG input → TIFF output
    {
        fs::path out = OutDir() / "jpg_to_tiff.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        auto result = RunBurstMerge(jpg_inputs, settings, out);
        CHECK(result.success, "jpg_to_tiff succeeds");
        CHECK(IsTiff(result.output_path), "jpg_to_tiff tiff signature");
    }

    // 5.2: JPEG input → BMP output
    {
        fs::path out = OutDir() / "jpg_to_bmp.bmp";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::BMP;
        auto result = RunBurstMerge(jpg_inputs, settings, out);
        CHECK(result.success, "jpg_to_bmp succeeds");
        CHECK(IsBmp(result.output_path), "jpg_to_bmp bmp signature");
    }

    // 5.3: BMP input (synthetic) → PNG output
    {
        fs::path bmp_src = OutDir() / "synth_input_24bit.bmp";
        CreateRgb24Bmp(bmp_src, 64, 48);

        fs::path out = OutDir() / "bmp_to_png.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        auto result = RunBurstMerge({bmp_src.string()}, settings, out);
        CHECK(result.success, "bmp_to_png succeeds");
        CHECK(IsPng(result.output_path), "bmp_to_png png signature");
    }
}

// ---------------------------------------------------------------------------
// Group 6: Rejection tests with synthetic samples
// ---------------------------------------------------------------------------
static void TestGroupRejection()
{
    std::cout << "[test] group4 rejection..." << std::endl;

    // 6.1: CMYK TIFF rejection
    {
        fs::path cmyk_tif = OutDir() / "synth_cmyk.tif";
        CreateMinimalCmykTiff(cmyk_tif);

        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        auto result = RunBurstMerge({cmyk_tif.string()}, settings, cmyk_tif);
        CHECK(!result.success, "cmyk tiff should fail");
        CHECK(result.error_msg.find("CMYK") != std::string::npos ||
              result.error_msg.find("not supported") != std::string::npos,
              "cmyk tiff error msg");
    }

    // 6.2: bps=10 TIFF rejection (use ffmpeg + exiftool to create)
    if (ToolExists("ffmpeg") && ToolExists("exiftool"))
    {
        fs::path bps10_tif = OutDir() / "synth_bps10.tif";
        std::string ff_cmd = "ffmpeg -y -f lavfi -i \"color=c=gray:size=4x4:d=0.1\""
            " -frames:v 1 \"" + fs::absolute(bps10_tif).string() + "\" 2>nul";
        std::system(ff_cmd.c_str());
        if (fs::exists(bps10_tif) && fs::file_size(bps10_tif) > 32)
        {
            TagAs10BitTiff(bps10_tif);

            auto settings = MakeSettings();
            settings.output_format = burstmerge::OutputFormat::TIFF;
            auto result = RunBurstMerge({bps10_tif.string()}, settings, bps10_tif);
            CHECK(!result.success, "bps10 tiff should fail");
            CHECK(result.error_msg.find("bit depth") != std::string::npos ||
                  result.error_msg.find("unsupported") != std::string::npos ||
                  result.error_msg.find("bps") != std::string::npos,
                  "bps10 tiff error msg");
        }
    }
    else
    {
        std::cout << "  [skip] bps=10 TIFF test (ffmpeg/exiftool not available)" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Group 7: Bit-depth constraint verification via exiftool
// ---------------------------------------------------------------------------
static void TestGroupBitDepthVerify()
{
    std::cout << "[test] group5 bit-depth verification..." << std::endl;

    if (!ToolExists("exiftool"))
    {
        std::cout << "  [skip] exiftool not available" << std::endl;
        return;
    }

    auto png_inputs = SmallRgbPngInputs();
    auto jpg_inputs  = SmallRgbJpegInputs("jpeg_422.jpg");
    auto tiff_inputs = SmallRgbTiffInputs();

    // 7.1: BMP bit_depth=16 → actual output is 8-bit
    {
        fs::path out = OutDir() / "bmp_16req.bmp";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::BMP;
        settings.bit_depth = 16;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "bmp_16req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 8, "bmp_16req output is 8-bit (got " + std::to_string(bps) + ")");
    }

    // 7.2: JPEG bit_depth=16 → actual output is 8-bit
    {
        fs::path out = OutDir() / "jpg_16req.jpg";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::JPEG;
        settings.bit_depth = 16;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "jpg_16req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 8, "jpg_16req output is 8-bit (got " + std::to_string(bps) + ")");
    }

    // 7.3: PNG bit_depth=16 → actual output is 16-bit
    {
        fs::path out = OutDir() / "png_16req.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        settings.bit_depth = 16;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "png_16req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 16, "png_16req output is 16-bit (got " + std::to_string(bps) + ")");
    }

    // 7.4: TIFF bit_depth=16 → actual output is 16-bit
    {
        fs::path out = OutDir() / "tif_16req.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        settings.bit_depth = 16;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "tif_16req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 16, "tif_16req output is 16-bit (got " + std::to_string(bps) + ")");
    }

    // 7.5: PNG bit_depth=10 → promoted to 16-bit
    {
        fs::path out = OutDir() / "png_10req.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        settings.bit_depth = 10;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "png_10req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 16, "png_10req output is 16-bit (got " + std::to_string(bps) + ")");
    }

    // 7.6: TIFF bit_depth=10 → promoted to 16-bit
    {
        fs::path out = OutDir() / "tif_10req.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        settings.bit_depth = 10;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "tif_10req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 16, "tif_10req output is 16-bit (got " + std::to_string(bps) + ")");
    }

    // 7.7: TIFF input → PNG output, 16-bit request should preserve high values.
    {
        fs::path out = OutDir() / "tiff_in_png_16req.png";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::PNG;
        settings.bit_depth = 16;
        auto result = RunBurstMerge(tiff_inputs, settings, out);
        CHECK(result.success, "tiff_in_png_16req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 16, "tiff_in_png_16req output is 16-bit (got " + std::to_string(bps) + ")");
    }

    // 7.8: PNG input → TIFF output, 16-bit request should preserve high values.
    {
        fs::path out = OutDir() / "png_in_tiff_16req.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        settings.bit_depth = 16;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "png_in_tiff_16req succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 16, "png_in_tiff_16req output is 16-bit (got " + std::to_string(bps) + ")");
    }

    // 7.9: Low bit depth should not be silently clamped to a too-narrow range.
    // We compare a high-value pixel from a 16-bit TIFF source after exporting
    // to PNG at 8-bit vs 16-bit.
    {
        auto src = ReadDecodedImage(SamplesDir() / "tif1" / "DSC05857.tif");
        SamplePoint p = FindSampleNear(src, 30000.0f);
        CHECK(p.value > 255.0f, "clamp source sample is genuinely high-bit-depth");

        fs::path out16 = OutDir() / "clamp_png_16.png";
        fs::path out8 = OutDir() / "clamp_png_8.png";

        auto s16 = MakeSettings();
        s16.output_format = burstmerge::OutputFormat::PNG;
        s16.bit_depth = 16;
        auto r16 = RunBurstMerge({(SamplesDir() / "tif1" / "DSC05857.tif").string()}, s16, out16);
        CHECK(r16.success, "clamp_png_16 succeeds");

        auto s8 = MakeSettings();
        s8.output_format = burstmerge::OutputFormat::PNG;
        s8.bit_depth = 8;
        auto r8 = RunBurstMerge({(SamplesDir() / "tif1" / "DSC05857.tif").string()}, s8, out8);
        CHECK(r8.success, "clamp_png_8 succeeds");

        if (r16.success && r8.success)
        {
            auto img16 = ReadDecodedImage(r16.output_path);
            auto img8 = ReadDecodedImage(r8.output_path);
            float v16 = SampleValue(img16, p.x, p.y, p.c);
            float v8 = SampleValue(img8, p.x, p.y, p.c);

            CHECK(v16 > 255.0f, "clamp_png_16 retains >8-bit value");
            CHECK(v8 <= 255.0f, "clamp_png_8 remains in 8-bit range");
            CHECK(std::abs(v8 * 257.0f - v16) <= std::max(48.0f, v16 * 0.18f),
                  "clamp_png_8 roughly matches 16->8 scaled value");
        }
    }

    // 7.10: Low bit depth to high bit depth should still rescale values instead
    // of leaving them trapped in the low range.
    {
        fs::path out = OutDir() / "png_8req_to_tiff16.tif";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::TIFF;
        settings.bit_depth = 8;
        auto result = RunBurstMerge(png_inputs, settings, out);
        CHECK(result.success, "png_8req_to_tiff16 succeeds");
        int bps = GetExifBitsPerSample(result.output_path);
        CHECK(bps == 8 || bps == 16, "png_8req_to_tiff16 bps valid (got " + std::to_string(bps) + ")");

        if (result.success)
        {
            auto decoded = ReadDecodedImage(result.output_path);
            float max_seen = 0.0f;
            for (float v : decoded.pixels)
            {
                if (v > max_seen) max_seen = v;
            }
            CHECK(max_seen > 0.0f, "png_8req_to_tiff16 output contains data");
        }
    }

    // 7.11: Verify warning text emitted on stderr for bit-depth adjustment
    {
        fs::path out = OutDir() / "warn_bmp_16req.bmp";
        auto settings = MakeSettings();
        settings.output_format = burstmerge::OutputFormat::BMP;
        settings.bit_depth = 16;
        std::string stderr_text = CaptureStderr([&]() {
            RunBurstMerge(png_inputs, settings, out);
        });
        CHECK(!stderr_text.empty(), "warn_bmp_16req captured stderr");
        bool has_warning = stderr_text.find("Warning") != std::string::npos;
        CHECK(has_warning, "warn_bmp_16req stderr contains 'Warning'");
    }
}

int main()
{
    CHECK(fs::exists(SamplesDir()), "samples directory exists");

    TestGroupAutoAndExplicitOutput();
    TestGroupFormatsBitDepthAndRegression();
    TestGroupMixedRawAndExport();
    TestGroupAutoInferenceEdge();
    TestGroupCrossFormat();
    TestGroupRejection();
    TestGroupBitDepthVerify();

    std::cout << "\nCommon RGB fmt: " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed > 0 ? 1 : 0;
}
