#include "burstmerge/internal/io/image_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace burstmerge
{
namespace io
{

#pragma pack(push, 1)
struct BmpFileHeader
{
    uint16_t bfType;
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

class BmpWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::BMP) && (bit_depth <= 8);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        if (image.channels != 1 && image.channels != 3)
        {
            throw std::runtime_error("BmpWriter: only 1 or 3 channel images supported");
        }

        float wl = (params.white_level > 1.0f) ? params.white_level : 255.0f;
        float scale = (wl > 1.0f) ? (255.0f / wl) : 1.0f;

        uint16_t bit_count = (image.channels == 3) ? 24 : 8;
        uint32_t row_bytes = ((image.width * bit_count + 31) / 32) * 4;
        uint32_t pixel_data_size = row_bytes * image.height;

        uint32_t palette_size = (bit_count == 8) ? (256 * 4) : 0;

        BmpFileHeader fh;
        fh.bfType = 0x4D42;
        fh.bfSize = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + palette_size + pixel_data_size;
        fh.bfReserved1 = 0;
        fh.bfReserved2 = 0;
        fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + palette_size;

        BmpInfoHeader ih;
        ih.biSize = sizeof(BmpInfoHeader);
        ih.biWidth = static_cast<int32_t>(image.width);
        ih.biHeight = static_cast<int32_t>(image.height);
        ih.biPlanes = 1;
        ih.biBitCount = bit_count;
        ih.biCompression = 0;
        ih.biSizeImage = pixel_data_size;
        ih.biXPelsPerMeter = 2835;
        ih.biYPelsPerMeter = 2835;
        ih.biClrUsed = (bit_count == 8) ? 256 : 0;
        ih.biClrImportant = (bit_count == 8) ? 256 : 0;

        std::vector<uint8_t> row(row_bytes);
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("BmpWriter: cannot open " + path);
        }

        file.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
        file.write(reinterpret_cast<const char*>(&ih), sizeof(ih));

        // Write 256-entry grayscale palette for 8-bit BMP.
        // Each entry is BGRA (4 bytes), where the alpha/reserved byte is 0.
        if (bit_count == 8)
        {
            uint8_t palette[256][4];
            for (int i = 0; i < 256; ++i)
            {
                palette[i][0] = static_cast<uint8_t>(i); // B
                palette[i][1] = static_cast<uint8_t>(i); // G
                palette[i][2] = static_cast<uint8_t>(i); // R
                palette[i][3] = 0;                       // reserved
            }
            file.write(reinterpret_cast<const char*>(palette), sizeof(palette));
        }

        for (uint32_t y = 0; y < image.height; ++y)
        {
            uint32_t src_y = image.height - 1 - y;
            std::memset(row.data(), 0, row_bytes);

            for (uint32_t x = 0; x < image.width; ++x)
            {
                size_t src_idx = (static_cast<size_t>(src_y) * image.width + x) * image.channels;
                if (bit_count == 24)
                {
                    uint8_t r = Quantize(image.data[src_idx + 0], scale);
                    uint8_t g = Quantize(image.data[src_idx + 1], scale);
                    uint8_t b = Quantize(image.data[src_idx + 2], scale);
                    row[x * 3 + 0] = b;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                }
                else
                {
                    row[x] = Quantize(image.data[src_idx], scale);
                }
            }
            file.write(reinterpret_cast<const char*>(row.data()), row_bytes);
        }
    }

private:
    static uint8_t Quantize(float v, float scale)
    {
        int i = static_cast<int>(std::round(v * scale));
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return static_cast<uint8_t>(i);
    }
};

std::unique_ptr<ImageWriter> CreateBmpWriter()
{
    return std::make_unique<BmpWriter>();
}

} // namespace io
} // namespace burstmerge
