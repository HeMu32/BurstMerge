#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge
{

struct AlignConstants
{
    // --- Default tile geometry -------------------------------------------
    // Default tile size in plane pixels (plane = Bayer separated by colour).
    // Actual tile geometry is taken from AlignParams::tile_size.
    // With the default value of 16, each tile covers 16x16 plane pixels
    // (= 32x32 Bayer pixels); 
    // Tiles are arranged with 50 % overlap (stride = tile_size / 2 = 8).
    static constexpr int32_t kDefaultTileSize = 16;

    // Minimum tile size (lower policy bound for resolved tile geometry).
    static constexpr int32_t kMinTileSize = 16;

    // Per-tile search: number of integer-position candidates in each direction.
    // radius=3 → 7×7 brute-force centred on the propagated seed.
    // In plane-pixel coordinates; at the finest level 1 plane-px ≡ 2 Bayer-px.
    static constexpr int32_t kTileSearchRadius = 3;

    // --- Pyramid geometry ------------------------------------------------
    // Pyramid follows a "2 then 4" pattern:
    //   Level 1 (just below full res) is always 2× half.
    //   The code currently builds this with Downsample2x(), not a true bicubic resample.
    //   Each coarser level is 4× of the previous (two Downsample2x steps).
    //
    // kMinCoarseTiles / kMaxCoarseTiles control the number of levels:
    //   Coarser levels keep being added while the coarsest level has more
    //   than kMaxCoarseTiles tiles, provided the next 4× level would still
    //   have at least kMinCoarseTiles tiles.
    //   kMinCoarseTiles = 4  →  
    //   kMaxCoarseTiles = 8  →  tames coarse-lvl tile count
    static constexpr int32_t kMinCoarseTiles = 4;
    static constexpr int32_t kMaxCoarseTiles = 8;

    // --- Standard global-search radius (Standard path only) --------------
    // Coarsest level searches 1/8 of the longest image side.
    // radius = max(kMinSearchRadius, longest >> (kSearchFractionShiftBase + ...))
    static constexpr int32_t kMinSearchRadius = 3;
    static constexpr int32_t kSearchFractionShiftBase = 3;  // 2^3 = 1/8

    // --- Standard RefineTileField (Standard path only) --------------------
    // local_radius = clamp(search_distance / kRefineLocalRadiusDiv, 1, ...)
    static constexpr int32_t kRefineLocalRadiusDiv = 16;

    // --- Standard global default -----------------------------------------
    static constexpr int32_t kDefaultSearchDistance = 64;

    // --- Standard DenseLocal / Smooth (Standard RefineTileField) ----------
    static constexpr int32_t kDenseLocalRadius = 3;
    static constexpr int32_t kSmoothNeighborRadius = 1;
};

struct AlignParams
{
    int32_t tile_size       = AlignConstants::kDefaultTileSize;
    int32_t search_distance = AlignConstants::kDefaultSearchDistance;
    uint32_t cfa_period     = 1;
    AlignmentMode mode      = AlignmentMode::Standard;
    float    align_gamma    = 1.0f;
    bool     smooth_tile_field = false;
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

// Overload that accepts a pre-built reference pyramid, avoiding redundant
// pyramid rebuilds when aligning multiple frames against the same reference.
// `reference` (full-resolution) is still needed for RefineTileField.
AlignmentResult EstimateTranslation(const std::vector<FloatImage>& ref_pyr,
                                    const FloatImage& reference,
                                    const FloatImage& comparison,
                                    const AlignParams& params);

AlignmentResult EstimateFrequencyTileField(
    const std::vector<FloatImage>& ref_pyr,
    const std::vector<FloatImage>& cmp_pyr,
    const AlignParams& params);

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment);

// Resolve a requested alignment tile size into a valid geometry.
// Tile sizes below kMinTileSize break sub-pixel matching fidelity; callers
// should always pass user-supplied tile values through this before use.
inline int32_t ResolveAlignTile(int32_t requested)
{
    if (requested < AlignConstants::kMinTileSize) return AlignConstants::kMinTileSize;
    return requested;
}

} // namespace burstmerge
