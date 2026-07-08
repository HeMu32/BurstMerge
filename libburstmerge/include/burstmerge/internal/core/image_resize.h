#pragma once

#include "burstmerge/internal/core/float_image.h"

#include <cstdint>

namespace burstmerge
{

FloatImage GaussianBlur(const FloatImage& src, float sigma, int radius = -1);

enum class InterpolationMethod
{
    Bilinear,
    Bicubic,
    AreaAverage,
    GaussianArea,
    HalfSample
};

FloatImage ResizeImage(const FloatImage& src,
                       uint32_t dst_width, uint32_t dst_height,
                       InterpolationMethod method);

} // namespace burstmerge
