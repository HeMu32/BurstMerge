#include "burstmerge/api.h"

namespace burstmerge {

struct BurstMerge::Impl {
    BackendType backend;
    Settings settings;
    ProgressFn progress_cb;
};

BurstMerge::BurstMerge(BackendType backend)
    : impl_(std::make_unique<Impl>())
{
    impl_->backend = backend;
}

BurstMerge::~BurstMerge() = default;

void BurstMerge::AddImage(const std::string& path) {
    (void)path;
}

void BurstMerge::Configure(const Settings& settings) {
    impl_->settings = settings;
}

void BurstMerge::SetProgressCallback(ProgressFn cb) {
    impl_->progress_cb = std::move(cb);
}

Result BurstMerge::Process(const std::string& output_dir) {
    (void)output_dir;
    if (impl_->progress_cb) {
        impl_->progress_cb(0.0f, "Starting...");
        impl_->progress_cb(1.0f, "Done");
    }
    return Result{true, output_dir, ""};
}

std::string BurstMerge::LastError() const {
    return {};
}

} // namespace burstmerge
