#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
#include <cstdio>
#include <limits>
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

        uint32_t w = 0, h = 0;
        uint16_t bps = 0, spp = 0, photo = 0;
        uint16_t sample_format = SAMPLEFORMAT_UINT;

        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
        TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo);
        TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);

        if (w == 0 || h == 0 || bps == 0)
        {
            TIFFClose(tif);
            throw std::runtime_error("TiffDecoder: missing or invalid required tags (width/height/bps)");
        }

        if (photo == PHOTOMETRIC_SEPARATED) // CMYK
        {
            TIFFClose(tif);
            throw std::runtime_error("TiffDecoder: CMYK TIFF not supported (only grayscale and RGB)");
        }

        if (spp == 0) spp = 1;

        bool is_float = (sample_format == SAMPLEFORMAT_IEEEFP);

        if (!is_float && bps != 8 && bps != 16 && bps != 32)
        {
            TIFFClose(tif);
            throw std::runtime_error("TiffDecoder: unsupported bit depth " + std::to_string(bps) + " (only 8, 16, 32 supported)");
        }

        uint32_t channels = spp;
        uint32_t pix_fmt = (channels == 1) ? kPixelGray
                         : (channels == 2) ? kPixelGA
                         : (channels == 3) ? kPixelRGB
                         : kPixelRGBA;

        DecodedImage result;
        result.info.width     = w;
        result.info.height    = h;
        result.info.pix_fmt   = pix_fmt;
        result.info.bit_depth = is_float ? 32 : bps;
        result.info.is_float  = is_float;
        result.info.is_raw    = false;
        // Protect against UB when bps >= 32 for non-float (shouldn't happen in practice,
        // but be defensive).
        if (is_float)
        {
            result.info.white_level = 1.0f;
        }
        else if (sample_format == SAMPLEFORMAT_INT)
        {
            // Signed integer TIFF: expose the positive range ceiling so later
            // normalization does not fall back to 255 due to white_level <= 1.
            if (bps >= 32)
            {
                result.info.white_level = 2147483647.0f;
            }
            else
            {
                result.info.white_level = static_cast<float>((1ull << (bps - 1)) - 1ull);
            }
        }
        else
        {
            // Unsigned integer TIFF. Keep a meaningful white level even for
            // 32-bit sources instead of falling back to 0.
            if (bps >= 32)
            {
                result.info.white_level = 4294967295.0f;
            }
            else
            {
                result.info.white_level = static_cast<float>((1ull << bps) - 1ull);
            }
        }

        result.pixels.resize(static_cast<size_t>(w) * h * channels);

        // Read path selection strictly based on sample_format, not bps alone.
        // uint32 TIFFs with SAMPLEFORMAT_UINT must NOT go through the float
        // path — TIFFReadScanline would copy raw uint32 bit patterns, which
        // the subsequent reinterpret_cast<float> would misread completely.
        if (is_float)
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
        else if (bps == 32)
        {
            if (sample_format == SAMPLEFORMAT_INT)
            {
                std::vector<int32_t> scanline(static_cast<size_t>(w) * channels);
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
                std::vector<uint32_t> scanline(static_cast<size_t>(w) * channels);
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
        }
        else if (bps == 16)
        {
            if (sample_format == SAMPLEFORMAT_INT)
            {
                std::vector<int16_t> scanline(static_cast<size_t>(w) * channels);
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
        }
        else
        {
            if (sample_format == SAMPLEFORMAT_INT)
            {
                std::vector<int8_t> scanline(static_cast<size_t>(w) * channels);
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
