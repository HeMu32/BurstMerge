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

        uint16_t bit_count = (image.channels == 3) ? 24 : 8;
        uint32_t row_bytes = ((image.width * bit_count + 31) / 32) * 4;
        uint32_t pixel_data_size = row_bytes * image.height;

        BmpFileHeader fh;
        fh.bfType = 0x4D42;
        fh.bfSize = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + pixel_data_size;
        fh.bfReserved1 = 0;
        fh.bfReserved2 = 0;
        fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);

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
        ih.biClrUsed = 0;
        ih.biClrImportant = 0;

        std::vector<uint8_t> row(row_bytes);
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("BmpWriter: cannot open " + path);
        }

        file.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
        file.write(reinterpret_cast<const char*>(&ih), sizeof(ih));

        for (uint32_t y = 0; y < image.height; ++y)
        {
            uint32_t src_y = image.height - 1 - y;
            std::memset(row.data(), 0, row_bytes);

            for (uint32_t x = 0; x < image.width; ++x)
            {
                size_t src_idx = (static_cast<size_t>(src_y) * image.width + x) * image.channels;
                if (bit_count == 24)
                {
                    uint8_t r = Quantize(image.data[src_idx + 0]);
                    uint8_t g = Quantize(image.data[src_idx + 1]);
                    uint8_t b = Quantize(image.data[src_idx + 2]);
                    row[x * 3 + 0] = b;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                }
                else
                {
                    row[x] = Quantize(image.data[src_idx]);
                }
            }
            file.write(reinterpret_cast<const char*>(row.data()), row_bytes);
        }
    }

private:
    static uint8_t Quantize(float v)
    {
        int i = static_cast<int>(std::round(v));
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
