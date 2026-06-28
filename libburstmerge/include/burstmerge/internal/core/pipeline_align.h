#pragma once

#include "burstmerge/api.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/pipeline.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/io/dng_io.h"

#include <vector>

namespace burstmerge
{

// Pipeline-facing alignment orchestration. This module decides how burst
// frames are fed into the alignment algorithms, including grayscale guides,
// progress reporting, and bracket-transmission chaining.
//
// The bracketing decision (fixed-reference vs chained alignment) is no longer
// re-derived here: the orchestrator computes ExposureClassification once and
// passes it in. Chained alignment is enabled iff
// `ec.needs_chained_alignment` (the stricter 2^kBracketTransmissionFallbackEv
// threshold), and the EV-sorted `ec.exposure_order` is reused directly.

std::vector<FloatImage> BuildAlignedComparisons(const std::vector<FloatImage>& float_images,
                                                 const std::vector<RawImage>& raw_images,
                                                 size_t ref_idx,
                                                 const Settings& settings,
                                                 uint32_t cfa_period,
                                                 const ExposureClassification& exposure,
                                                 const PipelineOrchestrator::ProgressFn& progress);

} // namespace burstmerge
