#pragma once

#include "burstmerge/internal/core/image_buffer.h"

#include <cstdint>
#include <vector>

namespace burstmerge
{

struct FloatImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 1;
    std::vector<float> data;

    float& At(uint32_t x, uint32_t y, uint32_t c = 0)
    {
        return data[(static_cast<size_t>(y) * width + x) * channels + c];
    }

    const float& At(uint32_t x, uint32_t y, uint32_t c = 0) const
    {
        return data[(static_cast<size_t>(y) * width + x) * channels + c];
    }
};

uint32_t ChannelsForFormat(PixelFormat format);
FloatImage HostBufferToFloatImage(const HostBuffer& src, float scale = 1.0f);
HostBuffer FloatImageToUint16HostBuffer(const FloatImage& src, uint32_t white_level);
FloatImage Downsample2x(const FloatImage& src);
FloatImage BoxBlur(const FloatImage& src, int radius);
FloatImage WarpTranslate(const FloatImage& src, float shift_x, float shift_y);
FloatImage ConvertMosaicToPlaneImage(const FloatImage& src, uint32_t cfa_period);
FloatImage ConvertPlaneImageToMosaic(const FloatImage& src,
                                     uint32_t mosaic_width,
                                     uint32_t mosaic_height,
                                     uint32_t cfa_period);
float MaxValue(const FloatImage& src);

} // namespace burstmerge
