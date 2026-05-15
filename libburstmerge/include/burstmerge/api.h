#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace burstmerge {

enum class BackendType      { CPU, Vulkan };
enum class MergeAlgorithm   { Spatial, Frequency };
enum class ExposureMode     { Off, Linear, Curve };

struct Settings {
    int            tile_size        = 32;
    int            search_distance  = 64;
    MergeAlgorithm merge_algo       = MergeAlgorithm::Spatial;
    float          noise_reduction  = 13.0f;
    ExposureMode   exposure_mode    = ExposureMode::Off;
    float          exposure_stops   = 0.0f;
};

struct Result {
    bool        success;
    std::string output_path;
    std::string error_msg;
};

class BurstMerge {
public:
    explicit BurstMerge(BackendType backend);
    ~BurstMerge();

    void AddImage(const std::string& path);
    void Configure(const Settings& settings);

    using ProgressFn = std::function<void(float, const std::string&)>;
    void SetProgressCallback(ProgressFn cb);

    Result Process(const std::string& output_dir);
    std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace burstmerge
