#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge
{

struct SpatialConstants
{
    static constexpr int32_t kBinomialKernelSize = 16;
    static constexpr int32_t kBinomialRadius = 5;
    static constexpr float kNoiseFloorMin = 1.0f;
    static constexpr float kNoiseFloorFallback = 8.0f;
    static constexpr float kStandardMinComparisonWeight = 0.08f;
    static constexpr float kLinearMinComparisonWeight = 0.04f;
    static constexpr float kHighlightThresholdFactor = 0.92f;
    static constexpr float kClipThresholdFactor = 0.98f;
    // NOTE: the robustness curve constants (kRobustnessBase / ExpBase /
    // RevOffset / RevHalf / Subtract / Min) live in PipelineConstants
    // (core/pipeline.h) next to ComputeRobustness(), which is their sole
    // consumer. They were previously declared here but never used.
};

// Binomial weights: C(32, 16+k) for k=0..8 (reference kernel_size=16)
extern const float kBinomialWeights[9];

struct SpatialMergeParams
{
    SpatialMergeMode mode = SpatialMergeMode::Standard;
    float noise_reduction = 13.0f;
    float robustness = 1.0f;
    float noise_floor = 0.0f;
    float highlight_threshold = 0.0f;
    uint32_t guide_block_size = 2;
    float clip_threshold = 0.0f;
    uint32_t num_scales = 0;
    const float* exposure_scales = nullptr;
};

FloatImage SpatialMerge(const FloatImage& reference,
                        const std::vector<FloatImage>& aligned_comparisons,
                        const SpatialMergeParams& params);

} // namespace burstmerge
