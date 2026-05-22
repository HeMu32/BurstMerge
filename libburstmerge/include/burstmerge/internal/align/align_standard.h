#pragma once

#include "burstmerge/internal/align/align.h"

namespace burstmerge
{

// Standard alignment refinement entry points.

void RefineTileField(const FloatImage& reference,
                     const FloatImage& comparison,
                     const AlignParams& params,
                     AlignmentResult& result);

} // namespace burstmerge
