#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

#ifdef BURSTMERGE_HAVE_TIFF
#include <tiff.h>
#include <tiffio.h>
#endif

namespace burstmerge
{
namespace io
{

#ifdef BURSTMERGE_HAVE_TIFF

class TiffDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".tif" || e == ".tiff";
    }

    DecodedImage Decode(const std::string& path) override
    {
        TIFF* tif = TIFFOpen(path.c_str(), "r");
        if (!tif)
        {
            throw std::runtime_error("TiffDecoder: cannot open " + path);
        }

        uint32_t w, h;
        uint16_t bps, spp, photo, sample_format;

        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
        TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo);
        TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);

        if (spp == 0) spp = 1;

        uint32_t channels = spp;
        uint32_t pix_fmt = (channels == 1) ? kPixelGray
                         : (channels == 2) ? kPixelGA
                         : (channels == 3) ? kPixelRGB
                         : kPixelRGBA;

        bool is_float = (sample_format == SAMPLEFORMAT_IEEEFP);

        DecodedImage result;
        result.info.width     = w;
        result.info.height    = h;
        result.info.pix_fmt   = pix_fmt;
        result.info.bit_depth = is_float ? 32 : bps;
        result.info.is_float  = is_float;
        result.info.is_raw    = false;
        result.info.white_level = is_float ? 1.0f : static_cast<float>((1u << bps) - 1);

        result.pixels.resize(static_cast<size_t>(w) * h * channels);

        if (is_float || bps == 32)
        {
            std::vector<float> scanline(static_cast<size_t>(w) * channels);
            for (uint32_t y = 0; y < h; ++y)
            {
                if (TIFFReadScanline(tif, scanline.data(), y) < 0)
                {
                    TIFFClose(tif);
                    throw std::runtime_error("TiffDecoder: read error");
                }
                for (uint32_t x = 0; x < w; ++x)
                {
                    size_t dst = (static_cast<size_t>(y) * w + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        result.pixels[dst + c] = scanline[x * channels + c];
                    }
                }
            }
        }
        else if (bps == 16)
        {
            std::vector<uint16_t> scanline(static_cast<size_t>(w) * channels);
            for (uint32_t y = 0; y < h; ++y)
            {
                if (TIFFReadScanline(tif, scanline.data(), y) < 0)
                {
                    TIFFClose(tif);
                    throw std::runtime_error("TiffDecoder: read error");
                }
                for (uint32_t x = 0; x < w; ++x)
                {
                    size_t dst = (static_cast<size_t>(y) * w + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        result.pixels[dst + c] = static_cast<float>(scanline[x * channels + c]);
                    }
                }
            }
        }
        else
        {
            std::vector<uint8_t> scanline(static_cast<size_t>(w) * channels);
            for (uint32_t y = 0; y < h; ++y)
            {
                if (TIFFReadScanline(tif, scanline.data(), y) < 0)
                {
                    TIFFClose(tif);
                    throw std::runtime_error("TiffDecoder: read error");
                }
                for (uint32_t x = 0; x < w; ++x)
                {
                    size_t dst = (static_cast<size_t>(y) * w + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        result.pixels[dst + c] = static_cast<float>(scanline[x * channels + c]);
                    }
                }
            }
        }

        TIFFClose(tif);
        return result;
    }
};

#else // !BURSTMERGE_HAVE_TIFF

class TiffDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".tif" || e == ".tiff";
    }

    DecodedImage Decode(const std::string& path) override
    {
        throw std::runtime_error("TIFF decoding not available (libtiff not linked)");
    }
};

#endif // BURSTMERGE_HAVE_TIFF

std::unique_ptr<ImageDecoder> CreateTiffDecoder()
{
    return std::make_unique<TiffDecoder>();
}

} // namespace io
} // namespace burstmerge
