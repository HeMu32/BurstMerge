#pragma once

#include "burstmerge/api.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/core/pipeline.h"

#include <vector>

namespace burstmerge
{

// Pipeline-facing alignment orchestration. This module decides how burst
// frames are fed into the alignment algorithms, including grayscale guides,
// progress reporting, and bracket-transmission chaining.

std::vector<FloatImage> BuildAlignedComparisons(const std::vector<FloatImage>& float_images,
                                                const std::vector<RawImage>& raw_images,
                                                size_t ref_idx,
                                                const Settings& settings,
                                                uint32_t cfa_period,
                                                const PipelineOrchestrator::ProgressFn& progress);

} // namespace burstmerge
