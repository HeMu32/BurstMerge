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

    // --- Spatial-merge robustness curve -------------------------------------
    // The user-facing knob is `noise_reduction` (nr). Internally the spatial
    // merge weight is `w = 1 / (1 + (diff/noise_floor)^2 * robustness)`, so a
    // LARGER robustness value rejects comparison frames MORE (less averaging,
    // less denoising). Therefore, to make "higher nr => more denoising" hold,
    // robustness must DECREASE as nr increases. This is the inverse of what
    // the naive `nr / 13` mapping did (that bug made the knob act backwards).
    //
    // The reference (hdr-plus-swift spatial.swift:21-22) derives robustness
    // from nr via an exponential decay, designed so that every 4 steps in nr
    // roughly halve the robustness norm (matching the sqrt(2) shot-noise
    // scaling per ISO stop):
    //
    //     robustness = kRobustnessBase * kRobustnessExpBase ^
    //                  (kRobustnessRevHalf * (kRobustnessRevOffset - nr))
    //                - kRobustnessSubtract
    //
    // Reference values: nr=8 -> ~5.5, nr=13 -> ~2.0, nr=17 -> ~1.0,
    // nr=20 -> ~0.53. For nr > ~23 the formula drops below kRobustnessMin and
    // is clamped, keeping a small residual rejection rather than collapsing to
    // "full weight everywhere" (a deliberate, conservative deviation from the
    // Swift/Metal behaviour where robustness<=0 disables robust merge).
    static constexpr float kRobustnessRevOffset = 36.0f;
    static constexpr float kRobustnessRevHalf   = 0.5f;
    static constexpr float kRobustnessBase      = 0.12f;
    static constexpr float kRobustnessExpBase   = 1.3f;
    static constexpr float kRobustnessSubtract  = 0.4529822f;
    static constexpr float kRobustnessMin       = 0.2f;
    // Lower bound for Settings::tile_size; the orchestrator clamps undersized
    // user input so every downstream stage (alignment + merge) gets a geometry
    // that preserves sub-pixel matching fidelity.
    static constexpr int32_t kMinTileSize = 16;
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
