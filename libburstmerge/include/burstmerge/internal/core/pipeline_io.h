#pragma once

#include "burstmerge/internal/core/pipeline.h"

#include <string>
#include <vector>

namespace burstmerge
{

// Pipeline IO helpers: temporary conversion directory management and RAW->DNG
// input preparation. Output path resolution now lives in pipeline.cpp as
// ResolveImageOutputPath (shared by all three pipeline paths).

std::vector<std::string> PrepareDngInputs(const std::vector<std::string>& input_paths,
                                          const std::string& output_path,
                                          const PipelineOrchestrator::ProgressFn& progress,
                                          std::string& out_convert_dir);

} // namespace burstmerge
