#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"

namespace burstmerge {

struct ExposureParams {
    ExposureMode mode = ExposureMode::Off;
    float stops       = 0.0f;
};

} // namespace burstmerge
