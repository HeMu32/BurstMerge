#pragma once

#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/io/image_decoder.h"
#include "burstmerge/internal/core/pipeline.h"

#include <vector>

namespace burstmerge
{

// Frame-level pipeline helpers: compatibility checks, black/noise statistics,
// exposure normalization, float-image construction, and reference-frame
// selection.

bool IsCompatibleForAverage(const RawImage& a, const RawImage& b);
float ComputeRobustness(float noise_reduction);
float EstimateNoiseFloor(const FloatImage& image, uint32_t guide_block_size);
float MeanBlackLevel(const RawMetadata& meta);
void NormalizeFrames(std::vector<FloatImage>& float_images,
                     const std::vector<RawImage>& raw_images,
                     size_t ref_idx);
std::vector<FloatImage> BuildFloatImages(const std::vector<RawImage>& images);
size_t SelectExposureRefIndex(const std::vector<RawImage>& images);

// --- Non-RAW helpers ---
FloatImage DecodedImageToFloatImage(const io::DecodedImage& img);
std::vector<FloatImage> BuildRgbImages(const std::vector<io::DecodedImage>& images);

} // namespace burstmerge
