#pragma once

#include "burstmerge/internal/core/float_image.h"

#include <cstdint>
#include <vector>

namespace burstmerge
{

// Alignment pyramid builder for the current "last step 1/2, earlier steps
// 1/4" policy.
// tile_size drives the pyramid depth heuristic so that the coarsest level
// keeps a consistent tile count relative to the estimator's tile geometry.

void BuildPyramid(const FloatImage& ref,
                  const FloatImage& cmp,
                  std::vector<FloatImage>& ref_pyr,
                  std::vector<FloatImage>& cmp_pyr,
                  int32_t tile_size);

// Build a single-image alignment pyramid. Extracted from BuildPyramid so the
// reference pyramid can be built once and reused across all comparison frames
// in the fixed-reference alignment path, avoiding redundant rebuilds.
std::vector<FloatImage> BuildPyramidSingle(const FloatImage& img, int32_t tile_size);

} // namespace burstmerge
