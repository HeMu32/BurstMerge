#pragma once

#include "burstmerge/internal/core/pipeline.h"

#include <string>
#include <vector>

namespace burstmerge
{

// Pipeline IO helpers: output-path resolution, temporary conversion directory
// management, and RAW->DNG input preparation. These functions are pure
// orchestration-side utilities and intentionally stay out of pipeline.cpp.

std::string ResolveOutputPath(const std::string& output_path_or_dir);

std::vector<std::string> PrepareDngInputs(const std::vector<std::string>& input_paths,
                                          const std::string& output_path,
                                          const PipelineOrchestrator::ProgressFn& progress,
                                          std::string& out_convert_dir);

} // namespace burstmerge
