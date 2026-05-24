#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace burstmerge
{

enum class OutputFormat
{
    Auto,  // automatic inference (DNG for RAW, PNG for non-RAW)
    PNG,
    JPEG,
    BMP,
    TIFF,
    DNG
};

enum class BackendType
{ CPU, Vulkan };
enum class MergeAlgorithm
{
    Spatial,         // Pixel-domain weighted blending (Standard or Linear sub-mode)
    Frequency,       // Frequency-domain merge (Laplacian or WienerFft sub-mode)
    TemporalAverage  // Simple exposure-weighted frame average; ignores noise_reduction
};
enum class ExposureMode
{ Off, Linear, Curve };
enum class AlignmentMode
{ Standard, DenseTile, Frequency };
enum class SpatialMergeMode
{ Standard, Linear };
enum class FrequencyMode
{ Laplacian, WienerFft, WienerFftRobust };
enum class ExposureCurveMode
{ Global, LocalReinhard };

struct Settings
{
    int            tile_size        = 32;
    int            search_distance  = 64;
    MergeAlgorithm merge_algo       = MergeAlgorithm::Spatial;
    AlignmentMode   alignment_mode  = AlignmentMode::Standard;
    SpatialMergeMode spatial_mode   = SpatialMergeMode::Standard;
    FrequencyMode   frequency_mode  = FrequencyMode::Laplacian;
    ExposureCurveMode exposure_curve_mode = ExposureCurveMode::Global;
    float          noise_reduction  = 13.0f;
    ExposureMode   exposure_mode    = ExposureMode::Off;
    float          exposure_stops   = 0.0f;
    // bit_depth: unified output precision (8/10/12/14/16);
    // for DNG output this is the effective bit depth (white_level target);
    // for non-DNG formats, auto-adjusted to nearest supported depth (e.g. 8 or 16 for PNG)
    int            bit_depth        = 14;
    float          align_gamma      = 1.0f;
    bool           smooth_tile_field = false;
    // output_format: Auto = auto-infer (DNG for RAW, PNG for non-RAW)
    OutputFormat   output_format    = OutputFormat::Auto;
};

struct Result
{
    bool        success;
    std::string output_path;
    std::string error_msg;
};

class BurstMerge
{
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
