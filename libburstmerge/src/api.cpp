#include "burstmerge/api.h"

namespace burstmerge {

struct BurstMerge::Impl {
    BackendType backend;
    Settings settings;
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
    (void)cb;
}

Result BurstMerge::Process(const std::string& output_dir) {
    (void)output_dir;
    return Result{true, "", ""};
}

std::string BurstMerge::LastError() const {
    return {};
}

} // namespace burstmerge
