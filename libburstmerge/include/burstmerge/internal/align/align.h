#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge {

struct AlignParams {
    int32_t tile_size       = 32;
    int32_t search_distance = 64;
    int32_t pyramid_levels  = 3;
    uint32_t cfa_period     = 1;
};

struct AlignmentResult {
    int32_t shift_x = 0;
    int32_t shift_y = 0;
    float   confidence = 0.0f;
    int32_t tile_size = 0;
    uint32_t cfa_period = 1;
    uint32_t tiles_x = 0;
    uint32_t tiles_y = 0;
    std::vector<int16_t> tile_shift_x;
    std::vector<int16_t> tile_shift_y;
};

AlignmentResult EstimateTranslation(const FloatImage& reference,
                                    const FloatImage& comparison,
                                    const AlignParams& params);

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment);

} // namespace burstmerge
