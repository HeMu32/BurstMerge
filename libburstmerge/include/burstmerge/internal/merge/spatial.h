#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge {

struct SpatialMergeParams {
    float noise_reduction = 13.0f;
    float robustness = 1.0f;
    float noise_floor = 0.0f;
    float highlight_threshold = 0.0f;
    uint32_t guide_block_size = 2;
};

FloatImage SpatialMerge(const FloatImage& reference,
                        const std::vector<FloatImage>& aligned_comparisons,
                        const SpatialMergeParams& params);

} // namespace burstmerge
