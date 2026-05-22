#pragma once

#include "burstmerge/internal/align/align.h"

#include <cstdint>
#include <limits>

namespace burstmerge
{

// Shared alignment primitives reused by multiple alignment modes and the warp
// stage. Keeping them here avoids rebuilding a monolithic align.cpp.

struct SearchBest
{
    float score = std::numeric_limits<float>::max();
    int dx = 0;
    int dy = 0;
};

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

// 5-tap separable binomial blur [1,4,6,4,1]/16 applied before
// downsampling to serve as an anti-aliasing prefilter.
// This mirrors the reference implementation's blur(kernel_size=2)
// applied before each avg_pool step in pyramid construction.
//
// Two interfaces:
//   BinomialBlur5Tap(src)          — value-returning convenience wrapper.
//   BinomialBlur5Tap(src, dst, tmp) — zero-copy: dst and tmp must be
//                                     pre-allocated to src dimensions.
FloatImage BinomialBlur5Tap(const FloatImage& src);
void BinomialBlur5Tap(const FloatImage& src, FloatImage& dst, FloatImage& tmp);

} // namespace burstmerge
