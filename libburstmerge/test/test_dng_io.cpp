#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "burstmerge/internal/io/dng_io.h"

#ifdef _WIN32
#include <windows.h>
#define BM_POPEN _popen
#define BM_PCLOSE _pclose
#else
#define BM_POPEN popen
#define BM_PCLOSE pclose
#endif

namespace
{

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) \
    { \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << msg << std::endl; \
        ++g_failed; \
    } \
} while (0)

struct SampleExpectation
{
    const char* relative_path;
    uint32_t expected_width;
    uint32_t expected_height;
    bool expect_read_success;
    const char* note;
};

bool SupportsRoundTrip(const burstmerge::RawImage& image)
{
    switch (image.pixels.format)
    {
        case burstmerge::PixelFormat::R16_Uint:
        case burstmerge::PixelFormat::R16_Uint_RGB:
        case burstmerge::PixelFormat::R32_Float:
            return true;
        default:
            return false;
    }
}

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

void CreateDirectoryIfNotExist(const std::string& path)
{
#ifdef _WIN32
    CreateDirectoryA(path.c_str(), nullptr);
#else
    (void)path;
#endif
}

std::string BaseName(const std::string& path)
{
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string NormalizeWindowsPath(std::string path)
{
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

std::string RunCommand(const std::string& command)
{
    FILE* pipe = BM_POPEN(command.c_str(), "r");
    if (!pipe) return
    {};

    std::array<char, 512> buffer
    {};
    std::string result;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
    {
        result += buffer.data();
    }
    BM_PCLOSE(pipe);
    return result;
}

bool ExifToolAvailable()
{
    static int cached = -1;
    if (cached == -1)
    {
        cached = RunCommand("exiftool -ver").empty() ? 0 : 1;
    }
    return cached == 1;
}

std::string ExifField(const std::string& file, const char* field)
{
    std::string command = "exiftool -s3 -";
    command += field;
    command += " \"";
    command += NormalizeWindowsPath(file);
    command += "\"";
    std::string value = RunCommand(command);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    {
        value.pop_back();
    }
    return value;
}

void CheckMetadata(const burstmerge::RawImage& image, const SampleExpectation& sample)
{
    CHECK(image.metadata.width == sample.expected_width, std::string(sample.relative_path) + " width");
    CHECK(image.metadata.height == sample.expected_height, std::string(sample.relative_path) + " height");
    CHECK(image.metadata.white_level > 0, std::string(sample.relative_path) + " white_level > 0");
    CHECK(image.metadata.exposure_bias >= -16.0f && image.metadata.exposure_bias <= 16.0f,
          std::string(sample.relative_path) + " exposure_bias sane");
    CHECK(image.metadata.dng_negative != nullptr, std::string(sample.relative_path) + " negative exists");
    CHECK(image.pixels.data != nullptr, std::string(sample.relative_path) + " pixel data exists");
    CHECK(image.pixels.width == image.metadata.width, std::string(sample.relative_path) + " pixel width matches");
    CHECK(image.pixels.height == image.metadata.height, std::string(sample.relative_path) + " pixel height matches");
    CHECK(image.pixels.size > 0, std::string(sample.relative_path) + " pixel size > 0");
    CHECK(image.pixels.row_stride >= image.metadata.width * 2u, std::string(sample.relative_path) + " row stride sane");

    bool has_non_zero_pixel = false;
    size_t scan = std::min<size_t>(image.pixels.size, 4096);
    for (size_t i = 0; i < scan; ++i)
    {
        if (image.pixels.data[i] != std::byte
        {0}) {
            has_non_zero_pixel = true;
            break;
        }
    }
    CHECK(has_non_zero_pixel, std::string(sample.relative_path) + " has non-zero pixel data");

    CHECK(image.metadata.mosaic_pattern_width == 0 ||
          image.metadata.mosaic_pattern_width == 2 ||
          image.metadata.mosaic_pattern_width == 6,
          std::string(sample.relative_path) + " mosaic pattern width is supported (0=LinearRaw, 2/6=Bayer)");
    for (float color_factor : image.metadata.color_factors)
    {
        CHECK(color_factor > 0.0f, std::string(sample.relative_path) + " color factor > 0");
    }

    switch (image.metadata.dng_pixel_type)
    {
        case burstmerge::DngPixelType::Uint8:
            CHECK(image.pixels.format == burstmerge::PixelFormat::R8_Uint,
                  std::string(sample.relative_path) + " Uint8 maps to R8_Uint");
            break;
        case burstmerge::DngPixelType::Uint16:
            CHECK(image.pixels.format == burstmerge::PixelFormat::R16_Uint ||
                  image.pixels.format == burstmerge::PixelFormat::R16_Uint_RGB,
                  std::string(sample.relative_path) + " Uint16 maps to R16_Uint (Bayer) or R16_Uint_RGB (LinearRaw)");
            break;
        case burstmerge::DngPixelType::Float32:
            CHECK(image.pixels.format == burstmerge::PixelFormat::R32_Float,
                  std::string(sample.relative_path) + " Float32 maps to R32_Float");
            break;
        default:
            break;
    }
}

void TestDngRoundTrips()
{
    std::cout << "[test] DNG read/write round trips..." << std::endl;

    const std::string root = TEST_DATA_DIR;
    const std::string out_dir = root + "/build/test_dng_io_outputs";
    CreateDirectoryIfNotExist(root + "/build");
    CreateDirectoryIfNotExist(out_dir);

    const std::vector<SampleExpectation> samples =
    {
        {"3rdparty/dng_sdk/sample_files/04_PGTM2_per_profile.dng", 1000, 1000, true, "SDK per-profile sample"},
        {"3rdparty/dng_sdk/sample_files/05_PGTM2_unsigned8.dng", 200, 200, true, "SDK sample; round-trip support depends on decoded host format"},
        {"3rdparty/dng_sdk/sample_files/06_PGTM2_unsigned16.dng", 200, 200, true, "SDK uint16 sample"},
        {"3rdparty/dng_sdk/sample_files/09_ImageSequenceInfo_1_of_3.dng", 200, 200, true, "SDK image sequence sample"},
        {"3rdparty/dng_sdk/sample_files/12_ImageStats_WeightedAverage.dng", 3000, 2000, true, "SDK image stats sample"},
        {"libburstmerge/test/samples/X1M5_Wide.dng", 4000, 3000, true, "project Sony DNG sample"},
        {"3rdparty/dng_sdk/sample_files/03_jxl_bayer_raw_integer.dng", 0, 0, false, "JXL unsupported by current SDK build"},
    };

    int read_ok = 0;
    int write_ok = 0;
    const bool has_exiftool = ExifToolAvailable();
    if (!has_exiftool)
    {
        std::cout << "  SKIP: exiftool not available; metadata field checks disabled" << std::endl;
    }

    for (const auto& sample : samples)
    {
        const std::string in_path = root + "/" + sample.relative_path;
        const std::string name = BaseName(in_path);

        if (!FileExists(in_path))
        {
            CHECK(false, std::string(sample.relative_path) + " exists");
            continue;
        }

        try
        {
            burstmerge::DngReader reader(in_path.c_str());
            auto image = reader.Read();

            if (!sample.expect_read_success)
            {
                CHECK(false, std::string(sample.relative_path) + " should fail: " + sample.note);
                continue;
            }

            ++read_ok;
            std::cout << "  Read OK: " << name << " (" << image.metadata.width << "x" << image.metadata.height << ")" << std::endl;
            CheckMetadata(image, sample);

            if (!SupportsRoundTrip(image))
            {
                CHECK(image.pixels.format == burstmerge::PixelFormat::R8_Uint ||
                      image.pixels.format == burstmerge::PixelFormat::RGBA32_Float,
                      name + " unsupported roundtrip format recognized");
                burstmerge::io::DngNegativeHolder* holder = image.metadata.dng_negative;
                try
                {
                    burstmerge::DngWriter writer(holder);
                    writer.Write((out_dir + "/unsupported_" + name).c_str(), image);
                    CHECK(false, name + " unsupported roundtrip should throw");
                } catch (const std::exception&)
                {
                    image.metadata.dng_negative = nullptr;
                    CHECK(true, name + " unsupported roundtrip throws");
                }
                continue;
            }

            const std::string out_path = out_dir + "/roundtrip_" + name;
            {
                burstmerge::DngWriter writer(image.metadata.dng_negative);
                writer.Write(out_path.c_str(), image);
            }

            CHECK(FileExists(out_path), name + " output exists");
            CHECK(FileSize(out_path) > 1024, name + " output size > 1KB");

            burstmerge::DngReader rereader(out_path.c_str());
            auto reread = rereader.Read();
            CHECK(reread.metadata.width == image.metadata.width, name + " reread width");
            CHECK(reread.metadata.height == image.metadata.height, name + " reread height");
            CHECK(reread.metadata.white_level == image.metadata.white_level, name + " reread white level");
            CHECK(reread.metadata.dng_pixel_type == image.metadata.dng_pixel_type, name + " reread pixel type");
            CHECK(reread.pixels.format == image.pixels.format, name + " reread pixel format");
            CHECK(reread.pixels.size > 0, name + " reread pixels exist");
            ++write_ok;

            if (has_exiftool)
            {
                const char* fields[] =
                {"Make", "Model", "ISO"};
                for (const char* field : fields)
                {
                    const std::string original = ExifField(in_path, field);
                    const std::string written = ExifField(out_path, field);
                    if (!original.empty())
                    {
                        CHECK(original == written, name + std::string(" EXIF ") + field + " preserved");
                    }
                }
            }

            std::remove(out_path.c_str());
        } catch (const std::exception& e)
        {
            if (sample.expect_read_success)
            {
                CHECK(false, std::string(sample.relative_path) + " threw: " + e.what());
            } else
            {
                std::cout << "  Expected failure: " << name << " (" << e.what() << ")" << std::endl;
            }
        } catch (...)
        {
            CHECK(!sample.expect_read_success, std::string(sample.relative_path) + " threw unknown exception");
        }
    }

    CHECK(read_ok >= 6, "at least six DNG samples read successfully");
    CHECK(write_ok >= 4, "at least four DNG samples write and reread successfully");
}

void TestInvalidInput()
{
    std::cout << "[test] invalid DNG input handling..." << std::endl;

    try
    {
        burstmerge::DngReader reader("/definitely/not/a/real/file.dng");
        (void)reader.Read();
        CHECK(false, "nonexistent file should throw");
    } catch (...)
    {
        CHECK(true, "nonexistent file throws without crashing");
    }
}

bool OptionalConverterTestsEnabled()
{
    const char* value = std::getenv("BURSTMERGE_TEST_DNG_CONVERTER");
    return value && std::strcmp(value, "0") != 0;
}

void TestAdobeDngConverterOptional()
{
#ifdef _WIN32
    std::cout << "[test] Adobe DNG Converter API..." << std::endl;
    if (!OptionalConverterTestsEnabled())
    {
        std::cout << "  SKIP: set BURSTMERGE_TEST_DNG_CONVERTER=1 to enable external converter test" << std::endl;
        return;
    }

    const std::string root = TEST_DATA_DIR;
    const std::string arw_path = root + "/libburstmerge/test/samples/7M4_Crop_35DNC020_Close.ARW";
    const std::string out_dir = root + "/build/test_dng_converter_outputs";
    const std::string expected_output = out_dir + "/7M4_Crop_35DNC020_Close.dng";
    CreateDirectoryIfNotExist(root + "/build");
    CreateDirectoryIfNotExist(out_dir);

    if (!FileExists(arw_path))
    {
        std::cout << "  SKIP: ARW sample not found" << std::endl;
        return;
    }

    std::remove(expected_output.c_str());

    std::vector<std::string> output_files;
    const bool ok = burstmerge::RunAdobeDngConverter(
    {arw_path}, out_dir, output_files);
    CHECK(ok, "Adobe DNG Converter returns success");
    CHECK(output_files.size() == 1, "Adobe DNG Converter produced one file");
    if (!output_files.empty())
    {
        CHECK(NormalizeWindowsPath(output_files[0]) == NormalizeWindowsPath(expected_output),
              "Adobe DNG Converter output path is deterministic");
    }

    for (const auto& output : output_files)
    {
        CHECK(FileExists(output), "converted DNG exists");
        burstmerge::DngReader reader(output.c_str());
        auto image = reader.Read();
        CHECK(image.metadata.width == 4736, "converted ARW width");
        CHECK(image.metadata.height == 3132, "converted ARW height");
        std::remove(output.c_str());
    }
#endif
}

} // namespace

void TestLinearRawDngOptional()
{
    // Reads a LinearRaw (PhotometricInterpretation 34892, 3 SamplesPerPixel)
    // DNG — e.g. DxO DeepPRIME output — and validates that the reader tags it
    // as R16_Uint_RGB with mosaic_pattern_width==0, and that it round-trips
    // through DngWriter as a 3-plane LinearRaw DNG.
    //
    // Enabled via BURSTMERGE_TEST_LINEAR_DNG=<path-to-linear-dng>.
    std::cout << "[test] LinearRaw (3-plane RGB) DNG..." << std::endl;
    const char* path = std::getenv("BURSTMERGE_TEST_LINEAR_DNG");
    if (!path || std::strcmp(path, "0") == 0 || path[0] == '\0')
    {
        std::cout << "  SKIP: set BURSTMERGE_TEST_LINEAR_DNG=<path> to enable" << std::endl;
        return;
    }
    if (!FileExists(path))
    {
        std::cout << "  SKIP: sample not found: " << path << std::endl;
        return;
    }

    burstmerge::DngReader reader(path);
    burstmerge::RawImage img = reader.Read();

    CHECK(img.metadata.mosaic_pattern_width == 0, "linear DNG reports mosaic_pattern_width == 0");
    CHECK(img.pixels.format == burstmerge::PixelFormat::R16_Uint_RGB, "linear DNG tagged R16_Uint_RGB");
    CHECK(img.metadata.width > 0 && img.metadata.height > 0, "linear DNG has dimensions");
    const size_t expect_size = static_cast<size_t>(img.metadata.width) *
                               img.metadata.height * 3u * sizeof(uint16_t);
    CHECK(img.pixels.size == expect_size, "linear DNG pixel buffer sized for 3 planes");
    CHECK(img.pixels.row_stride == img.metadata.width * 3u * sizeof(uint16_t),
          "linear DNG row stride accounts for 3 planes");

    bool has_non_zero = false;
    for (size_t i = 0; i < std::min<size_t>(img.pixels.size, 4096); ++i)
        if (img.pixels.data[i] != std::byte{0}) { has_non_zero = true; break; }
    CHECK(has_non_zero, "linear DNG has non-zero pixel data");

    // Round-trip through the writer into a LinearRaw output DNG.
    const std::string root = TEST_DATA_DIR;
    const std::string out_dir = root + "/build/test_dng_linear_outputs";
    CreateDirectoryIfNotExist(root + "/build");
    CreateDirectoryIfNotExist(out_dir);
    const std::string out_path = out_dir + "/linear_roundtrip.dng";
    std::remove(out_path.c_str());

    burstmerge::RawImage out_image;
    out_image.metadata = std::move(img.metadata);
    out_image.pixels = std::move(img.pixels);
    burstmerge::DngWriter writer(out_image.metadata.dng_negative);
    writer.Write(out_path.c_str(), out_image);
    CHECK(FileExists(out_path), "linear round-trip DNG written");

    // Re-read and confirm it is still detected as linear RGB.
    burstmerge::DngReader r2(out_path.c_str());
    burstmerge::RawImage img2 = r2.Read();
    CHECK(img2.metadata.mosaic_pattern_width == 0, "round-trip linear DNG still mosaic_pattern_width == 0");
    CHECK(img2.pixels.format == burstmerge::PixelFormat::R16_Uint_RGB, "round-trip linear DNG still R16_Uint_RGB");
    CHECK(img2.metadata.width == out_image.metadata.width, "round-trip linear DNG width preserved");

    std::remove(out_path.c_str());
}

int main()
{
    TestDngRoundTrips();
    TestInvalidInput();
    TestLinearRawDngOptional();
    TestAdobeDngConverterOptional();

    std::cout << "\nDNG I/O: " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed == 0 ? 0 : 1;
}
