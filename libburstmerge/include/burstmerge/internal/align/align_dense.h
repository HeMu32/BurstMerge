#pragma once

#include "burstmerge/internal/align/align.h"

namespace burstmerge
{

// Dense tile-field estimator entry point.

AlignmentResult EstimateDenseTileField(const std::vector<FloatImage>& ref_pyr,
                                       const std::vector<FloatImage>& cmp_pyr,
                                       const AlignParams& params);

} // namespace burstmerge
