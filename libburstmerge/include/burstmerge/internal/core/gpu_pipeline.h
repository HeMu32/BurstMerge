#pragma once
#include "burstmerge/api.h"
#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/pipeline.h"
#include "burstmerge/internal/io/dng_io.h"

#include <functional>
#include <string>
#include <vector>

namespace burstmerge
{

// GPU-native burst processing pipeline (Vulkan). Mirrors the CPU RAW pipeline
// (prepare -> align -> merge) entirely on the GPU with minimal host sync.
// Returns the merged plane image (channels = mosaic_period^2 for Bayer,
// black-subtracted) exactly like the CPU merge stage output, so the existing
// CPU tail (bit-depth scaling / exposure / mosaic convert / DNG write) can run
// unchanged. Throws std::runtime_error on GPU failure.
// Non-const images: the function releases pixel data and DNG SDK objects
// for comparison frames after uploading to GPU, to reduce peak system RAM.
FloatImage GpuRunBurstPipeline(std::vector<RawImage>& images,
                               size_t ref_idx,
                               const Settings& settings,
                               const PipelineOrchestrator::ProgressFn& progress);

// GPU pipeline for pre-demosaiced RGB images (e.g. TIFF/JPEG/PNG input).
// Uploads float data directly as plane buffers (no CFA deinterleave), then
// shares the same align / merge / download tail as GpuRunBurstPipeline.
// white_level / black_level come from the decoded image metadata.
// Non-const images: float data is released per-frame after GPU upload.
FloatImage GpuRunBurstPipelineRgb(std::vector<FloatImage>& images,
                                  size_t ref_idx,
                                  float white_level,
                                  const Settings& settings,
                                  const PipelineOrchestrator::ProgressFn& progress);

// GPU alignment unit: mirrors CPU EstimateTranslation. Takes the reference and
// comparison GRAYSCALE plane images (single-channel, plane resolution), runs the
// full GPU alignment (pyramid + global search + tile refine, mode per
// params.mode), and returns the tile motion field. Used by tile-motion unit
// tests that compare directly against EstimateTranslation (no image comparison).
// `vk` must already be Initialize()'d.
AlignmentResult GpuEstimateTranslation(const FloatImage& ref_gray,
                                       const FloatImage& cmp_gray,
                                       const AlignParams& params);

// Whether a Vulkan device is available ( Initialize succeeds ). Used by the
// orchestrator to fall back gracefully.
bool GpuVulkanAvailable();

// List available compute-capable Vulkan device names (for --list-gpus).
std::vector<std::string> GpuEnumerateDevices();

} // namespace burstmerge
