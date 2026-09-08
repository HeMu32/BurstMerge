#pragma once

#include "burstmerge/api.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/dng_io.h"

namespace burstmerge
{

FloatImage DemosaicBayer(const FloatImage& planes,
                          const RawMetadata& metadata,
                          PreprocessInterpolation method,
                          float exposure_scale = 1.0f);

} // namespace burstmerge
