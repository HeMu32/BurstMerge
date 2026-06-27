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

// Tunable parameters for clipped-green highlight recovery.
// Ported from the hdr-plus-swift reference implementation
// (texture.metal:add_texture_highlights). Values must match the GPU shader
// highlight_recovery.comp.
struct HighlightRecoveryParams
{
    // Minimum normalised value (fraction of effective dynamic range) for a
    // green photosite to be considered a candidate for recovery. Below this
    // the green channel is assumed to carry valid, un-clipped data and is
    // left untouched.
    // Reference: texture.metal:84,118  ("pixel_ratio1 > 0.8f")
    static constexpr float kBrightRatioThreshold = 0.8f;

    // An R or B neighbour whose normalised value exceeds this ratio times the
    // channel's colour factor is considered clipped, triggering green
    // extrapolation from the available non-clipped references.
    // Reference: texture.metal:105,139  ("pixel_ratio0 > 0.99f*factor_red")
    static constexpr float kNeighbourClipRatio = 0.99f;

    // Blend weight applied to the extrapolated green value when the green
    // pixel is at full saturation (ratio == 1.0). The weight decreases
    // linearly to 0 as the ratio drops towards kBrightRatioThreshold.
    // Reference: texture.metal:111,145  ("weight = 0.9f - ...")
    static constexpr float kExtrapolationWeightBase = 0.9f;

    // Rate at which the extrapolation weight decreases per unit of
    // (1 - ratio). Combined with kWeightClampRange this yields a weight of
    // exactly 0 at the kBrightRatioThreshold boundary.
    // Reference: texture.metal:111,145  ("... - 4.5f*clamp(...)")
    static constexpr float kExtrapolationWeightSlope = 4.5f;

    // Clamp range for the (1 - ratio) term so the weight never goes negative
    // and reaches 0 at the threshold boundary.
    // Reference: texture.metal:111,145  ("... clamp(1.0f-pixel_ratio1, 0.0f, 0.2f)")
    static constexpr float kWeightClampRange = 0.2f;
};

bool IsCompatibleForAverage(const RawImage& a, const RawImage& b);
float ComputeRobustness(float noise_reduction);
float EstimateNoiseFloor(const FloatImage& image, uint32_t guide_block_size);
float MeanBlackLevel(const RawMetadata& meta);
void NormalizeFrames(std::vector<FloatImage>& float_images,
                     const std::vector<RawImage>& raw_images,
                     size_t ref_idx);

// Recover clipped green-channel highlights by extrapolating from nearby R/B
// photosites using camera colour factors. Operates in-place on already-
// normalised plane images (black-level subtracted, exposure-scaled).
//
// Three tiers, dispatched by mosaic_pattern_width / channel count:
//   1. Bayer (period 2, 4 channels): full spatial-neighbour algorithm,
//      matching the hdr-plus-swift reference exactly. Handles all four Bayer
//      arrangements (RGGB / BGGR / GRBG / GBRG) via mosaic_pattern lookup.
//   2. Linear RAW (period 0, 3 channels): per-pixel extrapolation from
//      same-pixel R and B (data is already demosaiced).
//   3. Other mosaic (period >= 3): same-super-pixel extrapolation using all
//      R and B values within the super-pixel.
//
// RGB (JPEG/PNG/TIFF) inputs are never passed to this function.
// No pixel clamping is applied; extrapolated values that exceed white_level
// will be clipped naturally at uint16 quantisation.
void RecoverHighlights(std::vector<FloatImage>& float_images,
                       const std::vector<RawImage>& raw_images,
                       size_t ref_idx);
std::vector<FloatImage> BuildFloatImages(const std::vector<RawImage>& images);
size_t SelectExposureRefIndex(const std::vector<RawImage>& images);

// --- Non-RAW helpers ---
FloatImage DecodedImageToFloatImage(const io::DecodedImage& img);
std::vector<FloatImage> BuildRgbImages(const std::vector<io::DecodedImage>& images);

} // namespace burstmerge
