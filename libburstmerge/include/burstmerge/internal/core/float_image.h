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
void Downsample2x(const FloatImage& src, FloatImage& dst);
FloatImage Downsample4x(const FloatImage& src);
void Downsample4x(const FloatImage& src, FloatImage& dst);
FloatImage BoxBlur(const FloatImage& src, int radius);
FloatImage WarpTranslate(const FloatImage& src, float shift_x, float shift_y);
FloatImage ConvertMosaicToPlaneImage(const FloatImage& src, uint32_t cfa_period);
FloatImage ConvertPlaneImageToMosaic(const FloatImage& src,
                                     uint32_t mosaic_width,
                                     uint32_t mosaic_height,
                                     uint32_t cfa_period);
// Average all channels of a plane image into a single grayscale image.
// For Bayer plane images (channels=4, CFA period=2) this is equivalent
// to averaging each 2×2 RGGB block of the original mosaic.
// TODO(X-Trans): add an X-Trans specific path that handles the 6×6
// CFA pattern; the current channel-average fallback may not preserve
// the correct colour phase for non-Bayer patterns.
FloatImage ConvertPlanesToGrayscale(const FloatImage& src);
float MaxValue(const FloatImage& src);

} // namespace burstmerge
