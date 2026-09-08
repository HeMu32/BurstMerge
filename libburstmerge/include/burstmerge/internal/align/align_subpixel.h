#pragma once

#include "burstmerge/internal/align/align.h"

namespace burstmerge
{

// Sub-pixel refinement method for the super-resolution-dedicated alignment
// stage. Kept as independent new code so the existing alignment algorithms are
// never modified.
enum class SubpixelMethod
{
    SadParabola,  // separable parabola fit on the SSD surface around each tile's
                  // integer shift. Cheap, continuous, no FFT.
    Frequency     // Fourier phase-shift sub-pixel search (grid-configurable).
                  // The integer tile field is used as seed; the fractional peak
                  // is returned (NOT rounded), unlike the legacy freq_align path.
};

// Refine an integer tile field (any alignment mode) to fractional per-tile
// shifts, filling AlignmentResult::tile_shift_x_sub/y_sub and
// shift_x_sub/y_sub. `ref`/`cmp` are the grayscale images the tile field was
// estimated on. On non-Bayer-deinterleaved inputs (cfa_period>1) the refine is
// a no-op copy of the integer field, because fractional corrections would
// straddle CFA phase boundaries.
//
// fourier_grid: odd grid size used by SubpixelMethod::Frequency (e.g. 5 => 5x5).
// The legacy freq search uses a 7x7-equivalent grid; for the dedicated SR path
// a smaller grid is sufficient because the integer seed is already reliable and
// this stage is not expected to run in very high-noise scenes.
void RefineTileFieldSubpixel(const FloatImage& ref,
                             const FloatImage& cmp,
                             AlignmentResult& result,
                             SubpixelMethod method,
                             int fourier_grid = 5);

} // namespace burstmerge
