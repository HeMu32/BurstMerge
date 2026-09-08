#pragma once

#include "burstmerge/api.h"
#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/dng_io.h"

#include <cstdint>
#include <vector>

namespace burstmerge
{

struct SuperResolutionConstants
{
    static constexpr float kDirectSampleWeight = 1.0f;
    static constexpr float kInterpolatedSampleWeight = 0.05f;
    // Continuous sample weight falloff: w(d) = 1 / (1 + kSampleWeightFalloff*d^2),
    // where d is the distance of the (sub-pixel) sample position to the nearest
    // integer grid point (0..0.5). Chosen so w(0) = kDirectSampleWeight and
    // w(0.5) = kInterpolatedSampleWeight, giving a smooth transition that
    // avoids the hard even/odd parity boundary (the period-2px artifact).
    static constexpr float kSampleWeightFalloff = 76.0f;
    static constexpr float kResidualSearchRadius = 0.5f;
    static constexpr float kResidualSearchStep = 0.25f;
    static constexpr float kExactSampleEpsilon = 1.0e-4f;
    static constexpr uint32_t kResidualSampleStride = 8;
    static constexpr uint32_t kResidualTileSize = 64;
};

FloatImage SuperResolve2x(const FloatImage& reference,
                          const std::vector<FloatImage>& comparisons,
                          const std::vector<float>& exposure_scales,
                          SuperResolutionInterpolation interpolation,
                          float clip_threshold,
                          const std::vector<FloatImage>* original_comparisons = nullptr,
                          const std::vector<AlignmentResult>* alignments = nullptr);

// 2x super-resolution operating directly on the Bayer mosaic (route A). Each
// frame's normalized 4-channel plane image is sampled per-channel at the
// sub-pixel positions given by `alignments`, accumulated into a 2x HR Bayer
// mosaic, then demosaiced ONCE. Unlike the demosaic-then-upscale path, this
// preserves CFA phase and produces a single demosaic (no even/odd phase
// conflict -> no green fringing at edges, no period-2px artifact).
// ref_plane and comp_planes are W/2 x H/2 x 4 normalized planes (mean-black
// removed, exposure-normalised to the reference). alignments[k] is the
// full-resolution (fractional) per-tile shift for comp_planes[k].
FloatImage SuperResolve2xMosaic(const FloatImage& ref_plane,
                                const std::vector<FloatImage>& comp_planes,
                                const std::vector<AlignmentResult>& alignments,
                                const RawMetadata& metadata,
                                PreprocessInterpolation demosaic_method,
                                float clip_threshold);

// 2x super-resolution via direct kernel regression on the Bayer samples
// (Wronski-style): each HR output pixel's R, G, B is gathered directly from the
// nearby Bayer samples of that colour (all frames, sub-pixel-shifted), weighted
// by their distance. There is NO intermediate 2x Bayer mosaic and NO final
// demosaic step, so the output does not carry the Bayer-cell (period-4)
// structure that the upsample-then-demosaic paths exhibit.
FloatImage SuperResolve2xKernel(const FloatImage& ref_plane,
                                const std::vector<FloatImage>& comp_planes,
                                const std::vector<AlignmentResult>& alignments,
                                const RawMetadata& metadata,
                                float clip_threshold,
                                const FloatImage* reference_guide = nullptr,
                                const std::vector<FloatImage>* aligned_guides = nullptr);

} // namespace burstmerge
