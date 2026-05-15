#pragma once
#include "burstmerge/internal/core/image_buffer.h"

namespace burstmerge {

struct FrequencyMergeParams {
    float noise_reduction;
    int32_t tile_size;
};

} // namespace burstmerge
