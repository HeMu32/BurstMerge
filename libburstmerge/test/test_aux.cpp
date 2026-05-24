#include "test_aux.h"

#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>
#include <limits>
#include <cmath>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ExecCmd(const std::string& cmd)
{
    std::string result;
    FILE* pipe =
#ifdef _WIN32
        _popen(cmd.c_str(), "r");
#else
        popen(cmd.c_str(), "r");
#endif
    if (!pipe) return result;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe))
        result += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

bool ToolExists(const std::string& name)
{
    std::string cmd =
#ifdef _WIN32
        "where " + name + " >nul 2>nul";
#else
        "which " + name + " >/dev/null 2>/dev/null";
#endif
    return std::system(cmd.c_str()) == 0;
}

std::string CaptureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
#ifdef _WIN32
    int old = _dup(_fileno(stderr));
    FILE* cap = std::fopen("_stderr_cap.tmp", "w");
    if (!cap) { fn(); return ""; }
    _dup2(_fileno(cap), _fileno(stderr));
#else
    int old = dup(fileno(stderr));
    FILE* cap = std::fopen("_stderr_cap.tmp", "w");
    if (!cap) { fn(); return ""; }
    dup2(fileno(cap), fileno(stderr));
#endif

    fn();

    std::fflush(stderr);
#ifdef _WIN32
    _dup2(old, _fileno(stderr));
#else
    dup2(old, fileno(stderr));
#endif
    std::fclose(cap);
#ifdef _WIN32
    _close(old);
#else
    close(old);
#endif

    std::ifstream f("_stderr_cap.tmp");
    std::string r((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::error_code ec;
    fs::remove("_stderr_cap.tmp", ec);
    return r;
}

int GetExifBitsPerSample(const fs::path& path)
{
    if (!ToolExists("exiftool")) return -1;
    auto ext = Lower(path.extension().string());

    std::string cmd;
    if (ext == ".tif" || ext == ".tiff" || ext == ".jpg" || ext == ".jpeg")
    {
        cmd = "exiftool -b -BitsPerSample \"" + fs::absolute(path).string() + "\"";
    }
    else
    {
        cmd = "exiftool -b -BitDepth \"" + fs::absolute(path).string() + "\"";
    }
    std::string out = ExecCmd(cmd);
    if (out.empty()) return -1;
    std::istringstream ss(out);
    int val = 0;
    ss >> val;
    if (ext == ".bmp" && val > 16)
        val = val / static_cast<int>(val > 24 ? 4 : 3);
    return val;
}

void CreateRgb24Bmp(const fs::path& path, int w, int h)
{
#pragma pack(push, 1)
    struct BmpFH
    {
        uint16_t bfType      = 0x4D42;
        uint32_t bfSize;
        uint16_t bfReserved1 = 0;
        uint16_t bfReserved2 = 0;
        uint32_t bfOffBits;
    };
    struct BmpIH
    {
        uint32_t biSize          = 40;
        int32_t  biWidth;
        int32_t  biHeight;
        uint16_t biPlanes        = 1;
        uint16_t biBitCount      = 24;
        uint32_t biCompression   = 0;
        uint32_t biSizeImage     = 0;
        int32_t  biXPelsPerMeter = 2835;
        int32_t  biYPelsPerMeter = 2835;
        uint32_t biClrUsed       = 0;
        uint32_t biClrImportant  = 0;
    };
#pragma pack(pop)

    uint32_t row_bytes = static_cast<uint32_t>(((static_cast<uint32_t>(w) * 24 + 31) / 32) * 4);
    uint32_t pixel_data_size = row_bytes * static_cast<uint32_t>(h);
    uint32_t offset = static_cast<uint32_t>(sizeof(BmpFH) + sizeof(BmpIH));

    BmpFH fh;
    fh.bfSize    = offset + pixel_data_size;
    fh.bfOffBits = offset;

    BmpIH ih;
    ih.biWidth  = w;
    ih.biHeight = h;

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));

    std::vector<uint8_t> row(row_bytes, 0);
    for (int y = 0; y < h; ++y)
    {
        int ty = h - 1 - y;
        for (int x = 0; x < w; ++x)
        {
            row[static_cast<size_t>(x) * 3 + 0] = static_cast<uint8_t>(x * 255 / w);
            row[static_cast<size_t>(x) * 3 + 1] = static_cast<uint8_t>(ty * 255 / h);
            row[static_cast<size_t>(x) * 3 + 2] = static_cast<uint8_t>((x + ty) * 255 / (w + h));
        }
        f.write(reinterpret_cast<const char*>(row.data()), row_bytes);
    }
}

void CreateMinimalCmykTiff(const fs::path& path)
{
    struct { uint8_t order[2]; uint16_t magic; uint32_t ifd_offset; } header;
    header.order[0] = 'I'; header.order[1] = 'I';
    header.magic = 42;
    header.ifd_offset = 8;

    struct IfdEntry { uint16_t tag; uint16_t type; uint32_t count; uint32_t valoff; };

    uint32_t bps_offset = 122;
    uint32_t pixel_offset = 130;

    uint16_t entry_count = 9;
    IfdEntry entries[9];
    entries[0] = {256, 3, 1, 2};
    entries[1] = {257, 3, 1, 2};
    entries[2] = {258, 3, 4, bps_offset};
    entries[3] = {259, 3, 1, 1};
    entries[4] = {262, 3, 1, 5};
    entries[5] = {273, 3, 1, pixel_offset};
    entries[6] = {277, 3, 1, 4};
    entries[7] = {278, 3, 1, 2};
    entries[8] = {279, 3, 1, 16};

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));

    f.put(static_cast<char>(entry_count & 0xFF));
    f.put(static_cast<char>((entry_count >> 8) & 0xFF));
    for (auto& e : entries)
    {
        f.write(reinterpret_cast<const char*>(&e.tag), 2);
        f.write(reinterpret_cast<const char*>(&e.type), 2);
        f.write(reinterpret_cast<const char*>(&e.count), 4);
        f.write(reinterpret_cast<const char*>(&e.valoff), 4);
    }
    uint32_t next_ifd = 0;
    f.write(reinterpret_cast<const char*>(&next_ifd), 4);

    for (int i = 0; i < 4; ++i)
    {
        uint16_t bps = 8;
        f.write(reinterpret_cast<const char*>(&bps), 2);
    }

    for (int i = 0; i < 16; ++i)
        f.put(0x80);
}

void TagAs10BitTiff(const fs::path& path)
{
    if (!ToolExists("exiftool")) return;
    std::string cmd = "exiftool -overwrite_original"
        " -BitsPerSample=10 -n \""
        + fs::absolute(path).string() + "\" 2>nul";
    std::system(cmd.c_str());
}

burstmerge::io::DecodedImage ReadDecodedImage(const fs::path& path)
{
    return burstmerge::io::ReadImage(path.string());
}

uint32_t ChannelCount(const burstmerge::io::DecodedImage& img)
{
    return img.info.pix_fmt & 0xFFu;
}

float SampleValue(const burstmerge::io::DecodedImage& img,
                  uint32_t x,
                  uint32_t y,
                  uint32_t c)
{
    uint32_t channels = ChannelCount(img);
    if (channels == 0 || x >= img.info.width || y >= img.info.height || c >= channels)
    {
        return 0.0f;
    }
    size_t idx = (static_cast<size_t>(y) * img.info.width + x) * channels + c;
    return img.pixels[idx];
}

SamplePoint FindSampleNear(const burstmerge::io::DecodedImage& img, float target)
{
    SamplePoint best;
    float best_dist = std::numeric_limits<float>::max();
    uint32_t channels = ChannelCount(img);
    for (uint32_t y = 0; y < img.info.height; ++y)
    {
        for (uint32_t x = 0; x < img.info.width; ++x)
        {
            size_t base = (static_cast<size_t>(y) * img.info.width + x) * channels;
            for (uint32_t c = 0; c < channels; ++c)
            {
                float v = img.pixels[base + c];
                float dist = std::abs(v - target);
                if (dist < best_dist)
                {
                    best_dist = dist;
                    best = {x, y, c, v};
                }
            }
        }
    }
    return best;
}
