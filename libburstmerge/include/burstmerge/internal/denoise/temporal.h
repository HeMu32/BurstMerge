#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge
{

struct TemporalDenoiseParams
{
    float strength = 23.0f;
    float white_level = 0.0f;
    float black_level = 0.0f;
    uint32_t num_scales = 0;
    const float* exposure_scales = nullptr;
};

void RepairHotPixels(std::vector<FloatImage>& images,
                     float white_level,
                     const float black_level[4],
                     uint32_t cfa_period);
FloatImage TemporalAverage(const FloatImage& reference,
                           const std::vector<FloatImage>& aligned_comparisons,
                           const TemporalDenoiseParams& params);

// Per-pixel median across the reference and all aligned comparison frames.
// Robust to outliers (e.g. residual misalignment, walking people, hot pixels);
// noise_reduction and exposure_scales are accepted for API symmetry with
// TemporalAverage but are not used by the median computation.
FloatImage TemporalMedian(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const TemporalDenoiseParams& params);

} // namespace burstmerge
