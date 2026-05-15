#include "burstmerge/api.h"

#include "burstmerge/internal/core/pipeline.h"

namespace burstmerge {

struct BurstMerge::Impl {
    BackendType backend;
    Settings settings;
    std::vector<std::string> input_paths;
    ProgressFn progress_cb;
    std::string last_error;
};

BurstMerge::BurstMerge(BackendType backend)
    : impl_(std::make_unique<Impl>())
{
    impl_->backend = backend;
}

BurstMerge::~BurstMerge() = default;

void BurstMerge::AddImage(const std::string& path) {
    impl_->input_paths.push_back(path);
}

void BurstMerge::Configure(const Settings& settings) {
    impl_->settings = settings;
}

void BurstMerge::SetProgressCallback(ProgressFn cb) {
    impl_->progress_cb = std::move(cb);
}

Result BurstMerge::Process(const std::string& output_dir) {
    PipelineOrchestrator pipeline(impl_->backend, impl_->settings);
    Result result = pipeline.Process(impl_->input_paths, output_dir, impl_->progress_cb);
    impl_->last_error = result.error_msg;
    return result;
}

std::string BurstMerge::LastError() const {
    return impl_->last_error;
}

} // namespace burstmerge
