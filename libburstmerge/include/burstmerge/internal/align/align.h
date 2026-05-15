#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

namespace burstmerge {

struct AlignParams {
    int32_t tile_size       = 32;
    int32_t search_distance = 64;
    int32_t pyramid_levels  = 3;
};

struct AlignmentResult {
    int32_t shift_x = 0;
    int32_t shift_y = 0;
    float   confidence = 0.0f;
};

AlignmentResult EstimateTranslation(const FloatImage& reference,
                                    const FloatImage& comparison,
                                    const AlignParams& params);

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment);

} // namespace burstmerge
