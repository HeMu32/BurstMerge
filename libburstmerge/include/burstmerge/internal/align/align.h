#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge
{

struct AlignConstants
{
    static constexpr int32_t kDefaultTileSize = 32;
    static constexpr int32_t kDefaultSearchDistance = 64;
    static constexpr int32_t kDefaultPyramidLevels = 3;
    static constexpr int32_t kPyramidMinDimension = 256;
    static constexpr int32_t kDenseLocalRadius = 2;
    static constexpr int32_t kSmoothNeighborRadius = 1;
    static constexpr int32_t kRefineLocalRadiusDiv = 16;
    static constexpr int32_t kRefineDenseLocalRadiusDiv = 12;
    static constexpr int32_t kRefineDenseMaxRadius = 6;
    static constexpr int32_t kMinTileSize = 8;
    static constexpr int32_t kDenseSearchDist = 2;
};

struct AlignParams
{
    int32_t tile_size       = AlignConstants::kDefaultTileSize;
    int32_t search_distance = AlignConstants::kDefaultSearchDistance;
    int32_t pyramid_levels  = AlignConstants::kDefaultPyramidLevels;
    uint32_t cfa_period     = 1;
    AlignmentMode mode      = AlignmentMode::Legacy;
};

struct AlignmentResult
{
    int32_t shift_x = 0;
    int32_t shift_y = 0;
    float   confidence = 0.0f;
    int32_t tile_size = 0;
    int32_t tile_spacing = 0;   // distance between tile centers; 0 means = tile_size
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
