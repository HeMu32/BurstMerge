#include "burstmerge/internal/io/image_decoder.h"
#include "burstmerge/internal/io/dng_io.h"

#include "dng_sdk_bridge.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace burstmerge
{
namespace io
{

class DngDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".dng";
    }

    DecodedImage Decode(const std::string& path) override
    {
        DngReader reader(path.c_str());
        RawImage raw = reader.Read();

        DecodedImage result;
        result.info.width       = raw.metadata.width;
        result.info.height      = raw.metadata.height;
        result.info.bit_depth   = raw.metadata.white_level <= 255 ? 8
                                : raw.metadata.white_level <= 4095 ? 12
                                : raw.metadata.white_level <= 16383 ? 14
                                : 16;
        result.info.is_float    = (raw.metadata.dng_pixel_type == DngPixelType::Float32);
        // pix_fmt must match the actual channel count from the HostBuffer format.
        // R16_Uint_RGB                 → 3 channels → kPixelRGB (LinearRaw DNG).
        // RGBA32_Float                 → 4 channels → kPixelRGBA.
        // R16_Uint / R8_Uint / R32_Float → 1 channel → kPixelGray.
        result.info.pix_fmt     = (raw.pixels.format == PixelFormat::RGBA32_Float)
                                ? kPixelRGBA
                                : (raw.pixels.format == PixelFormat::R16_Uint_RGB)
                                    ? kPixelRGB : kPixelGray;
        result.info.is_raw      = true;

        result.info.ev_value = raw.metadata.ev_value;
        result.info.exposure_bias     = raw.metadata.exposure_bias;
        result.info.white_level       = static_cast<float>(raw.metadata.white_level);
        std::memcpy(result.info.black_level, raw.metadata.black_level, sizeof(float) * 4);
        result.info.mosaic_pattern_width = raw.metadata.mosaic_pattern_width;

        result.info.dng_negative = nullptr;
        if (raw.metadata.dng_negative)
        {
            auto* holder = raw.metadata.dng_negative;
            raw.metadata.dng_negative = nullptr;
            result.info.dng_negative.reset(holder, [](DngNegativeHolder* h)
            {
                DestroyNegativeHolder(h);
            });
        }

        uint32_t src_channels = (raw.pixels.format == PixelFormat::RGBA32_Float) ? 4
                              : (raw.pixels.format == PixelFormat::R16_Uint_RGB) ? 3
                              : 1;
        size_t src_pixel_count = static_cast<size_t>(raw.metadata.width) * raw.metadata.height;
        size_t total_pixels = src_pixel_count * src_channels;

        result.pixels.resize(total_pixels);

        if (raw.pixels.format == PixelFormat::R16_Uint ||
            raw.pixels.format == PixelFormat::R16_Uint_RGB)
        {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(raw.pixels.data);
            for (size_t i = 0; i < total_pixels; ++i)
            {
                result.pixels[i] = static_cast<float>(src[i]);
            }
        }
        else if (raw.pixels.format == PixelFormat::R8_Uint)
        {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(raw.pixels.data);
            for (size_t i = 0; i < src_pixel_count; ++i)
            {
                result.pixels[i] = static_cast<float>(src[i]);
            }
        }
        else if (raw.pixels.format == PixelFormat::R32_Float)
        {
            const float* src = reinterpret_cast<const float*>(raw.pixels.data);
            std::copy(src, src + total_pixels, result.pixels.begin());
        }
        else if (raw.pixels.format == PixelFormat::RGBA32_Float)
        {
            const float* src = reinterpret_cast<const float*>(raw.pixels.data);
            std::copy(src, src + src_pixel_count * 4, result.pixels.begin());
        }
        else
        {
            throw std::runtime_error("DngDecoder: unsupported HostBuffer pixel format");
        }

        return result;
    }
};

std::unique_ptr<ImageDecoder> CreateDngDecoder()
{
    return std::make_unique<DngDecoder>();
}

} // namespace io
} // namespace burstmerge
