#include "burstmerge/internal/io/image_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#ifdef BURSTMERGE_HAVE_PNG
#include <png.h>
#endif

namespace burstmerge
{
namespace io
{

#ifdef BURSTMERGE_HAVE_PNG

static void PngWriteData(png_structp png_ptr, png_bytep data, png_size_t length)
{
    FILE* fp = static_cast<FILE*>(png_get_io_ptr(png_ptr));
    std::fwrite(data, 1, length, fp);
}

static void PngFlushData(png_structp png_ptr)
{
    FILE* fp = static_cast<FILE*>(png_get_io_ptr(png_ptr));
    std::fflush(fp);
}

class PngWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::PNG) &&
               (bit_depth == 8 || bit_depth == 16);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        if (image.channels != 1 && image.channels != 3 && image.channels != 4)
        {
            throw std::runtime_error("PngWriter: only 1, 3, or 4 channel images supported");
        }

        uint32_t channels = image.channels;
        int bit_depth = (params.bit_depth <= 8) ? 8 : 16;
        int color_type;

        switch (channels)
        {
            case 1: color_type = PNG_COLOR_TYPE_GRAY; break;
            case 3: color_type = PNG_COLOR_TYPE_RGB; break;
            case 4: color_type = PNG_COLOR_TYPE_RGBA; break;
            default: throw std::runtime_error("PngWriter: unsupported channel count");
        }

        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp)
        {
            throw std::runtime_error("PngWriter: cannot open " + path);
        }

        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
        {
            std::fclose(fp);
            throw std::runtime_error("PngWriter: png_create_write_struct failed");
        }

        png_infop info = png_create_info_struct(png);
        if (!info)
        {
            png_destroy_write_struct(&png, nullptr);
            std::fclose(fp);
            throw std::runtime_error("PngWriter: png_create_info_struct failed");
        }

        if (setjmp(png_jmpbuf(png)))
        {
            png_destroy_write_struct(&png, &info);
            std::fclose(fp);
            throw std::runtime_error("PngWriter: write error");
        }

        png_set_write_fn(png, fp, PngWriteData, PngFlushData);

        png_set_IHDR(png, info, image.width, image.height,
                     bit_depth, color_type, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

        png_write_info(png, info);

        double max_val = (bit_depth == 16) ? 65535.0 : 255.0;
        double white = static_cast<double>(params.bit_depth <= 8 ? 255 : 65535);

        std::vector<uint8_t> row8;
        std::vector<uint16_t> row16;

        for (uint32_t y = 0; y < image.height; ++y)
        {
            if (bit_depth == 16)
            {
                row16.resize(static_cast<size_t>(image.width) * channels);
                for (uint32_t x = 0; x < image.width; ++x)
                {
                    size_t src = (static_cast<size_t>(y) * image.width + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        double v = std::min(std::max(static_cast<double>(image.data[src + c]), 0.0), white);
                        row16[x * channels + c] = static_cast<uint16_t>(std::round(v));
                    }
                }
                png_write_row(png, reinterpret_cast<png_bytep>(row16.data()));
            }
            else
            {
                row8.resize(static_cast<size_t>(image.width) * channels);
                for (uint32_t x = 0; x < image.width; ++x)
                {
                    size_t src = (static_cast<size_t>(y) * image.width + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        double v = std::min(std::max(static_cast<double>(image.data[src + c]), 0.0), 255.0);
                        row8[x * channels + c] = static_cast<uint8_t>(std::round(v));
                    }
                }
                png_write_row(png, reinterpret_cast<png_bytep>(row8.data()));
            }
        }

        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
    }
};

#else // !BURSTMERGE_HAVE_PNG

class PngWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::PNG) &&
               (bit_depth == 8 || bit_depth == 16);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        throw std::runtime_error("PNG writing not available (libpng not linked)");
    }
};

#endif // BURSTMERGE_HAVE_PNG

std::unique_ptr<ImageWriter> CreatePngWriter()
{
    return std::make_unique<PngWriter>();
}

} // namespace io
} // namespace burstmerge
