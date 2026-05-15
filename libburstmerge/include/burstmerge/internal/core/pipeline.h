#pragma once

#include "burstmerge/api.h"

#include <functional>
#include <string>
#include <vector>

namespace burstmerge {

class PipelineOrchestrator {
public:
    using ProgressFn = std::function<void(float, const std::string&)>;

    PipelineOrchestrator(BackendType backend, Settings settings);

    Result Process(const std::vector<std::string>& input_paths,
                   const std::string& output_path_or_dir,
                   ProgressFn progress);

private:
    BackendType backend_;
    Settings settings_;
};

} // namespace burstmerge
