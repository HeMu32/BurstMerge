#pragma once

#include "burstmerge/api.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace burstmerge
{

struct PipelineConstants
{
    static constexpr float kHighlightFactor = 0.92f;
    static constexpr float kClipFactor = 0.98f;
    static constexpr float kNoiseFormulaMul = 4.0f;
    static constexpr float kNoiseFloorMin = 8.0f;
    static constexpr float kRobustnessDiv = 13.0f;
    static constexpr float kRobustnessMin = 0.2f;
    // NOTE: kTemporalNrThreshold (22.5) was removed; "frame average" is now selected
    // by --merge-algo, not by noise_reduction threshold.
    static constexpr float kBracketTransmissionFallbackEv = 1.0f;
    static constexpr int32_t kProgressStart = 0;
    static constexpr float kProgressValidate = 0.03f;
    static constexpr float kProgressConvertStart = 0.08f;
    static constexpr float kProgressConvertEnd = 0.16f;
    static constexpr float kProgressDecodeStart = 0.18f;
    static constexpr float kProgressDecodeRange = 0.30f;
    static constexpr float kProgressRefFrame = 0.50f;
    static constexpr float kProgressRefSelected = 0.52f;
    static constexpr float kProgressHotpixel = 0.54f;
    static constexpr float kProgressCfaLog = 0.541f;
    static constexpr float kProgressNormalize = 0.545f;
    static constexpr float kProgressAlignStart = 0.56f;
    static constexpr float kProgressAlignRange = 0.06f;
    static constexpr float kProgressWarpStart = 0.62f;
    static constexpr float kProgressWarpRange = 0.06f;
    static constexpr float kProgressMerge = 0.70f;
    static constexpr float kProgressExposure = 0.78f;
    static constexpr float kProgressQuantize = 0.80f;
    static constexpr float kProgressContainer = 0.82f;
    static constexpr float kProgressWrite = 0.90f;
    static constexpr float kProgressDone = 1.0f;
    static constexpr std::chrono::hours kOrphanMaxAge = std::chrono::hours(24);
};

class PipelineOrchestrator
{
public:
    using ProgressFn = std::function<void(float, const std::string&)>;

    PipelineOrchestrator(BackendType backend, Settings settings);

    Result Process(const std::vector<std::string>& input_paths,
                   const std::string& output_path_or_dir,
                   ProgressFn progress);

private:
    void CleanupConvertDir();

    BackendType backend_;
    Settings settings_;
    std::string convert_dir_;
};

// Extension classification helpers (shared by pipeline.cpp and pipeline_io.cpp).
bool IsRawExtension(const std::string& ext);
bool IsImageExtension(const std::string& ext);

} // namespace burstmerge
