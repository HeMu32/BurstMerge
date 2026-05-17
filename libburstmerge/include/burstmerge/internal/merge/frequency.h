#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge
{

struct FrequencyConstants
{
    static constexpr int32_t kFourierSearchGrid = 7;        // 7x7 search
    static constexpr double kFourierSearchRange = 0.5;       // +/-0.5 pixel
    static constexpr int32_t kMinTileSize = 16;
    static constexpr double kRmsScale = 0.25;
    static constexpr double kRobustnessRevOffset = 26.5;
    static constexpr double kRobustnessNormBase = 7.5;
    static constexpr double kReadNoiseBase = 10.0;
    static constexpr double kReadNoiseExp = 1.6;
    static constexpr int32_t kLaplacianBlurDiv = 16;
};

struct FrequencyMergeParams
{
    FrequencyMode mode = FrequencyMode::Laplacian;
    float noise_reduction = 13.0f;
    int32_t tile_size = 32;
};

FloatImage FrequencyMerge(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const FrequencyMergeParams& params);

} // namespace burstmerge
