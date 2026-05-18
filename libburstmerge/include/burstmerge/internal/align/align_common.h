#pragma once

#include "burstmerge/internal/align/align.h"

namespace burstmerge
{

// Shared alignment primitives reused by multiple alignment modes and the warp
// stage. Keeping them here avoids rebuilding a monolithic align.cpp.

int SnapToPeriod(int value, uint32_t period);
float SparseSad(const FloatImage& a, const FloatImage& b, int dx, int dy, int step);
float TileSad(const FloatImage& a,
              const FloatImage& b,
              uint32_t x0,
              uint32_t y0,
              uint32_t tile_w,
              uint32_t tile_h,
              int dx,
              int dy,
              int sample_step);
float TileCost(const FloatImage& a,
               const FloatImage& b,
               uint32_t x0,
               uint32_t y0,
               uint32_t tile_w,
               uint32_t tile_h,
               int dx,
               int dy,
               int sample_step,
               bool ssd);
void SmoothTileField(AlignmentResult& result);
float InterpolateTileShift(const std::vector<int16_t>& field,
                           uint32_t tiles_x,
                           uint32_t tiles_y,
                           int32_t tile_size,
                           int32_t tile_spacing,
                           uint32_t x,
                           uint32_t y);
int ClampInt(int v, int lo, int hi);

} // namespace burstmerge
