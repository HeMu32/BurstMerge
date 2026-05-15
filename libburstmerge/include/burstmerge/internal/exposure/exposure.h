#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

namespace burstmerge {

struct ExposureParams {
    ExposureMode mode = ExposureMode::Off;
    float stops       = 0.0f;
};

void ApplyExposure(FloatImage& image, uint32_t white_level, const ExposureParams& params);

} // namespace burstmerge
