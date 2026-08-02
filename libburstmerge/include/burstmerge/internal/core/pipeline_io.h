#pragma once

#include "burstmerge/internal/core/pipeline.h"

#include <string>
#include <vector>

namespace burstmerge
{

// Pipeline IO helpers: temporary conversion directory management and RAW->DNG
// input preparation. Output path resolution now lives in pipeline.cpp as
// ResolveImageOutputPath (shared by all three pipeline paths).

// `dng_convert_dir` controls where the per-run "burstmerge_converted"
// intermediates folder is created:
//   - "" (empty): legacy behaviour, alongside `output_path`'s parent.
//   - non-empty: used as the parent of "burstmerge_converted" instead, so the
//               conversion work directory is decoupled from the output
//               location. The directory itself is never removed; only the
//               "burstmerge_converted" subfolder and its per-run children are
//               cleaned up after processing.
std::vector<std::string> PrepareDngInputs(const std::vector<std::string>& input_paths,
                                          const std::string& output_path,
                                          const std::string& dng_convert_dir,
                                          const PipelineOrchestrator::ProgressFn& progress,
                                          std::string& out_convert_dir);

} // namespace burstmerge
