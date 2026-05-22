#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
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

static void PngReadData(png_structp png_ptr, png_bytep data, png_size_t length)
{
    FILE* fp = static_cast<FILE*>(png_get_io_ptr(png_ptr));
    size_t n = std::fread(data, 1, length, fp);
    (void)n;
}

class PngDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".png";
    }

    DecodedImage Decode(const std::string& path) override
    {
        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp)
        {
            throw std::runtime_error("PngDecoder: cannot open " + path);
        }

        uint8_t sig[8];
        std::fread(sig, 1, 8, fp);
        if (png_sig_cmp(sig, 0, 8))
        {
            std::fclose(fp);
            throw std::runtime_error("PngDecoder: not a PNG file");
        }

        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
        {
            std::fclose(fp);
            throw std::runtime_error("PngDecoder: png_create_read_struct failed");
        }

        png_infop info = png_create_info_struct(png);
        if (!info)
        {
            png_destroy_read_struct(&png, nullptr, nullptr);
            std::fclose(fp);
            throw std::runtime_error("PngDecoder: png_create_info_struct failed");
        }

        if (setjmp(png_jmpbuf(png)))
        {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(fp);
            throw std::runtime_error("PngDecoder: decode error");
        }

        png_set_read_fn(png, fp, PngReadData);
        png_set_sig_bytes(png, 8);
        png_read_info(png, info);

        uint32_t w      = png_get_image_width(png, info);
        uint32_t h      = png_get_image_height(png, info);
        int bit_depth   = png_get_bit_depth(png, info);
        int color_type  = png_get_color_type(png, info);

        if (color_type == PNG_COLOR_TYPE_PALETTE)
        {
            png_set_palette_to_rgb(png);
        }

        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        {
            png_set_expand_gray_1_2_4_to_8(png);
        }

        if (png_get_valid(png, info, PNG_INFO_tRNS))
        {
            png_set_tRNS_to_alpha(png);
        }

        if (bit_depth == 16)
        {
            png_set_scale_16(png);
            bit_depth = 8;
        }

        if (color_type == PNG_COLOR_TYPE_GRAY ||
            color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        {
            png_set_gray_to_rgb(png);
        }

        png_read_update_info(png, info);

        uint32_t channels = png_get_channels(png, info);
        uint32_t row_bytes = png_get_rowbytes(png, info);

        DecodedImage result;
        result.info.width     = w;
        result.info.height    = h;
        result.info.pix_fmt   = (channels == 3) ? kPixelRGB : kPixelRGBA;
        result.info.bit_depth = 8;
        result.info.is_raw    = false;
        result.info.white_level = 255.0f;

        result.pixels.resize(static_cast<size_t>(w) * h * channels);

        std::vector<uint8_t> row(row_bytes);
        for (uint32_t y = 0; y < h; ++y)
        {
            png_read_row(png, row.data(), nullptr);
            for (uint32_t x = 0; x < w; ++x)
            {
                size_t dst = (static_cast<size_t>(y) * w + x) * channels;
                for (uint32_t c = 0; c < channels; ++c)
                {
                    result.pixels[dst + c] = static_cast<float>(row[x * channels + c]);
                }
            }
        }

        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);

        return result;
    }
};

#else // !BURSTMERGE_HAVE_PNG

class PngDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".png";
    }

    DecodedImage Decode(const std::string& path) override
    {
        throw std::runtime_error("PNG decoding not available (libpng not linked)");
    }
};

#endif // BURSTMERGE_HAVE_PNG

std::unique_ptr<ImageDecoder> CreatePngDecoder()
{
    return std::make_unique<PngDecoder>();
}

} // namespace io
} // namespace burstmerge
