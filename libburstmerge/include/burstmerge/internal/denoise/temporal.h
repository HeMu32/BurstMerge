#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge {

struct TemporalDenoiseParams {
    float strength = 23.0f;
};

void RepairHotPixels(std::vector<FloatImage>& images, float white_level, uint32_t cfa_period);
FloatImage TemporalAverage(const FloatImage& reference,
                           const std::vector<FloatImage>& aligned_comparisons,
                           const TemporalDenoiseParams& params);

} // namespace burstmerge
