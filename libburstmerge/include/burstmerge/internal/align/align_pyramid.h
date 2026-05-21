#pragma once

#include "burstmerge/internal/core/float_image.h"

#include <vector>

namespace burstmerge
{

// Alignment pyramid builder for the current "last step 1/2, earlier steps
// 1/4" policy.

void BuildPyramid(const FloatImage& ref,
                  const FloatImage& cmp,
                  std::vector<FloatImage>& ref_pyr,
                  std::vector<FloatImage>& cmp_pyr);

} // namespace burstmerge
