#include "burstmerge/internal/io/image_writer.h"

#include <algorithm>
#include <cmath>
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

static uint16_t DepthToSampleFormat(uint32_t bit_depth)
{
    return (bit_depth == 32) ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT;
}

class TiffWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::TIFF) &&
               (bit_depth == 8 || bit_depth == 16);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        if (image.channels != 1 && image.channels != 3 && image.channels != 4)
        {
            throw std::runtime_error("TiffWriter: only 1, 3, or 4 channel images supported");
        }

        TIFF* tif = TIFFOpen(path.c_str(), "w");
        if (!tif)
        {
            throw std::runtime_error("TiffWriter: cannot open " + path);
        }

        uint32_t channels = image.channels;
        uint16_t bps = (params.bit_depth <= 8) ? 8 : 16;
        uint16_t sample_format = SAMPLEFORMAT_UINT;

        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image.width);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image.height);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, channels);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sample_format);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                     (channels >= 3) ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

        double wl = (params.white_level > 1.0f) ? static_cast<double>(params.white_level) : 255.0;
        double output_max = (bps == 16) ? 65535.0 : 255.0;
        double scale = (wl > 1.0) ? (output_max / wl) : 1.0;

        if (bps == 16)
        {
            std::vector<uint16_t> scanline(static_cast<size_t>(image.width) * channels);
            for (uint32_t y = 0; y < image.height; ++y)
            {
                for (uint32_t x = 0; x < image.width; ++x)
                {
                    size_t src = (static_cast<size_t>(y) * image.width + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        double v = std::min(std::max(static_cast<double>(image.data[src + c]) * scale, 0.0), output_max);
                        scanline[x * channels + c] = static_cast<uint16_t>(std::round(v));
                    }
                }
                if (TIFFWriteScanline(tif, scanline.data(), y) < 0)
                {
                    TIFFClose(tif);
                    throw std::runtime_error("TiffWriter: write error");
                }
            }
        }
        else
        {
            std::vector<uint8_t> scanline(static_cast<size_t>(image.width) * channels);
            for (uint32_t y = 0; y < image.height; ++y)
            {
                for (uint32_t x = 0; x < image.width; ++x)
                {
                    size_t src = (static_cast<size_t>(y) * image.width + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        double v = std::min(std::max(static_cast<double>(image.data[src + c]) * scale, 0.0), output_max);
                        scanline[x * channels + c] = static_cast<uint8_t>(std::round(v));
                    }
                }
                if (TIFFWriteScanline(tif, scanline.data(), y) < 0)
                {
                    TIFFClose(tif);
                    throw std::runtime_error("TiffWriter: write error");
                }
            }
        }

        TIFFClose(tif);
    }
};

#else // !BURSTMERGE_HAVE_TIFF

class TiffWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::TIFF) &&
               (bit_depth == 8 || bit_depth == 16);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        throw std::runtime_error("TIFF writing not available (libtiff not linked)");
    }
};

#endif // BURSTMERGE_HAVE_TIFF

std::unique_ptr<ImageWriter> CreateTiffWriter()
{
    return std::make_unique<TiffWriter>();
}

} // namespace io
} // namespace burstmerge
