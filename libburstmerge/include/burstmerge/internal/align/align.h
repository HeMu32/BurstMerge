#pragma once
#include "burstmerge/internal/core/image_buffer.h"

namespace burstmerge {

struct AlignParams {
    int32_t tile_size       = 32;
    int32_t search_distance = 64;
    int32_t pyramid_levels  = 3;
};

struct AlignmentResult {
    int32_t shift_x;
    int32_t shift_y;
    float   confidence;
};

} // namespace burstmerge
