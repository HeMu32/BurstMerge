#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

namespace burstmerge {

struct ExposureConstants {
    static constexpr float kMaxGain = 16.0f;
    static constexpr float kLinearGainFactor = 0.9f;
    static constexpr float kMinGainStops = 0.0f;
    static constexpr float kMaxGainStops = 4.0f;
    static constexpr float kGainStopsFalloff = 0.05f;
    static constexpr float kGainStopsFalloffThreshold = 1.5f;
    static constexpr float kGain1Divisor = 1.4f;
    static constexpr float kLocalBlurRadius = 1;
    static constexpr float kGlobalBlurRadius = 2;
    static constexpr float kReinhardShadowMid = 1.0f;
    static constexpr float kReinhardHighlightMid = 0.4f;
    static constexpr float kReinhardLuminanceMin = 1e-12f;
    static constexpr float kColorFactorMin = 1e-6f;
    static constexpr float kWeightStopsFactor = 0.25f;
};

struct ExposureParams {
    ExposureMode mode = ExposureMode::Off;
    ExposureCurveMode curve_mode = ExposureCurveMode::Global;
    float stops       = 0.0f;
    uint32_t mosaic_pattern_width = 1;
    float black_level[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float color_factor_mean = 1.0f;
};

void ApplyExposure(FloatImage& image, uint32_t white_level, const ExposureParams& params);

} // namespace burstmerge
