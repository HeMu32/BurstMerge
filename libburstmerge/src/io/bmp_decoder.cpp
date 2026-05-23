#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace burstmerge
{
namespace io
{

// BMP file header (14 bytes)
#pragma pack(push, 1)
struct BmpFileHeader
{
    uint16_t bfType;      // 'BM'
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};

struct BmpInfoHeader
{
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
#pragma pack(pop)

static constexpr uint16_t kBM = 0x4D42; // 'B' 'M'
static constexpr uint32_t kBI_RGB = 0;

class BmpDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".bmp";
    }

    DecodedImage Decode(const std::string& path) override
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("BmpDecoder: cannot open " + path);
        }

        BmpFileHeader fh;
        file.read(reinterpret_cast<char*>(&fh), sizeof(fh));
        if (fh.bfType != kBM)
        {
            throw std::runtime_error("BmpDecoder: not a BMP file");
        }

        BmpInfoHeader ih;
        file.read(reinterpret_cast<char*>(&ih), sizeof(ih));

        // biSize must be at least 40 (BITMAPINFOHEADER) to have biClrUsed.
        if (ih.biSize < 40)
        {
            throw std::runtime_error("BmpDecoder: DIB header too small (need BITMAPINFOHEADER or later)");
        }
        if (ih.biCompression != kBI_RGB)
        {
            throw std::runtime_error("BmpDecoder: only uncompressed BMP supported");
        }
        if (ih.biBitCount != 24 && ih.biBitCount != 8)
        {
            throw std::runtime_error("BmpDecoder: only 8-bit and 24-bit BMP supported");
        }

        if (ih.biWidth <= 0)
        {
            throw std::runtime_error("BmpDecoder: invalid width (must be positive)");
        }

        if (ih.biPlanes != 1)
        {
            throw std::runtime_error("BmpDecoder: invalid biPlanes (must be 1)");
        }

        bool top_down = ih.biHeight < 0;
        uint32_t h = static_cast<uint32_t>(std::abs(ih.biHeight));
        uint32_t w = static_cast<uint32_t>(ih.biWidth);

        if (w == 0 || h == 0)
        {
            throw std::runtime_error("BmpDecoder: zero dimensions");
        }

        // Validate bfOffBits is past the header (at minimum).
        // Palette (if any) sits right after the DIB header.
        uint32_t dib_end = sizeof(BmpFileHeader) + ih.biSize;
        if (fh.bfOffBits < dib_end)
        {
            throw std::runtime_error("BmpDecoder: bfOffBits points inside DIB header");
        }

        // Read palette for 8-bit BMP.  biClrUsed tells us how many entries
        // are actually present (clamped to 256 max).  If biClrUsed is 0,
        // the full 256-entry palette is implied.
        std::vector<uint8_t> palette;
        uint32_t palette_byte_count = 0;
        uint32_t pal_entries = 0;
        uint32_t channels = 0;
        if (ih.biBitCount == 8)
        {
            pal_entries = (ih.biClrUsed != 0) ? std::min(ih.biClrUsed, 256u) : 256u;
            palette_byte_count = pal_entries * 4; // each entry is BGRA
            palette.resize(palette_byte_count, 0);

            // Seek to the palette which starts right after the DIB header.
            file.seekg(dib_end);
            file.read(reinterpret_cast<char*>(palette.data()), palette_byte_count);
            if (file.gcount() != static_cast<std::streamsize>(palette_byte_count))
            {
                throw std::runtime_error("BmpDecoder: truncated palette");
            }

            channels = 3; // palette expand to RGB
        }
        else
        {
            channels = 3; // 24-bit BMP
        }

        // bfOffBits must be past palette end (if palette exists).
        uint32_t min_data_offset = dib_end + palette_byte_count;
        if (fh.bfOffBits < min_data_offset)
        {
            throw std::runtime_error("BmpDecoder: bfOffBits overlaps palette");
        }

        uint32_t row_bytes = ((w * ih.biBitCount + 31) / 32) * 4;

        file.seekg(fh.bfOffBits);

        std::vector<uint8_t> raw_row(row_bytes);

        DecodedImage result;
        result.info.width     = w;
        result.info.height    = h;
        result.info.pix_fmt   = kPixelRGB; // palette-expanded or native BGR→RGB
        result.info.bit_depth = 8;
        result.info.is_raw    = false;
        result.info.white_level = 255.0f;

        result.pixels.resize(static_cast<size_t>(w) * h * channels);

        for (uint32_t y = 0; y < h; ++y)
        {
            file.read(reinterpret_cast<char*>(raw_row.data()), row_bytes);
            if (file.gcount() != static_cast<std::streamsize>(row_bytes))
            {
                throw std::runtime_error("BmpDecoder: truncated pixel data");
            }
            uint32_t dst_y = top_down ? y : (h - 1 - y);

            for (uint32_t x = 0; x < w; ++x)
            {
                size_t dst_idx = (static_cast<size_t>(dst_y) * w + x) * channels;
                if (ih.biBitCount == 24)
                {
                    // BMP stores BGR
                    result.pixels[dst_idx + 0] = static_cast<float>(raw_row[x * 3 + 2]); // R
                    result.pixels[dst_idx + 1] = static_cast<float>(raw_row[x * 3 + 1]); // G
                    result.pixels[dst_idx + 2] = static_cast<float>(raw_row[x * 3 + 0]); // B
                }
                else
                {
                    // 8-bit: look up palette entry (BGRA layout)
                    uint8_t idx = raw_row[x];
                    if (idx >= pal_entries)
                    {
                        throw std::runtime_error("BmpDecoder: palette index out of range");
                    }
                    size_t pal_off = static_cast<size_t>(idx) * 4;
                    float r = static_cast<float>(palette[pal_off + 2]);
                    float g = static_cast<float>(palette[pal_off + 1]);
                    float b = static_cast<float>(palette[pal_off + 0]);
                    result.pixels[dst_idx + 0] = r;
                    result.pixels[dst_idx + 1] = g;
                    result.pixels[dst_idx + 2] = b;
                }
            }
        }

        return result;
    }
};

std::unique_ptr<ImageDecoder> CreateBmpDecoder()
{
    return std::make_unique<BmpDecoder>();
}

} // namespace io
} // namespace burstmerge
