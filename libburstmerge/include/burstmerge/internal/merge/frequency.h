#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge {

struct FrequencyMergeParams {
    float noise_reduction = 13.0f;
    int32_t tile_size = 32;
};

FloatImage FrequencyMerge(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const FrequencyMergeParams& params);

} // namespace burstmerge
