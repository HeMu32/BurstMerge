#include "burstmerge/internal/merge/spatial.h"

#include <cmath>
#include <vector>

namespace {

float ClampMin(float v, float lo) {
    return v < lo ? lo : v;
}

float BlockMean(const burstmerge::FloatImage& img, uint32_t x, uint32_t y, uint32_t block_size) {
    const uint32_t x0 = (x / block_size) * block_size;
    const uint32_t y0 = (y / block_size) * block_size;
    float sum = 0.0f;
    uint32_t n = 0;
    for (uint32_t yy = y0; yy < std::min(y0 + block_size, img.height); ++yy) {
        for (uint32_t xx = x0; xx < std::min(x0 + block_size, img.width); ++xx) {
            sum += img.At(xx, yy, 0);
            ++n;
        }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

}

namespace burstmerge {

FloatImage SpatialMerge(const FloatImage& reference,
                        const std::vector<FloatImage>& aligned_comparisons,
                        const SpatialMergeParams& params)
{
    if (aligned_comparisons.empty()) {
        return reference;
    }

    // Approximate the original pipeline's ref_blurred / diff-based weighting.
    // We derive weights from low-frequency content so that raw CFA noise and
    // small phase errors do not immediately zero out contribution from other frames.
    const int blur_radius = 2;
    const FloatImage ref_blur = BoxBlur(reference, blur_radius);
    std::vector<FloatImage> cmp_blurs;
    cmp_blurs.reserve(aligned_comparisons.size());
    for (const auto& img : aligned_comparisons) {
        cmp_blurs.push_back(BoxBlur(img, blur_radius));
    }

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    const float noise_floor = params.noise_floor > 0.0f
        ? params.noise_floor
        : std::max(8.0f, params.noise_reduction * 4.0f);
    const float robustness = std::max(0.1f, params.robustness);
    const float min_comparison_weight = 0.08f;
    const float highlight_threshold = params.highlight_threshold > 0.0f
        ? params.highlight_threshold
        : 65535.0f * 0.92f;
    const uint32_t guide_block = std::max<uint32_t>(1, params.guide_block_size);

    if (reference.channels == 1) {
        // Single-channel (mosaic): per-pixel weight, each position one guide value
        for (uint32_t y = 0; y < out.height; ++y) {
            for (uint32_t x = 0; x < out.width; ++x) {
                const size_t i = (static_cast<size_t>(y) * out.width + x);

                float ref_guide = BlockMean(reference, x, y, guide_block);
                float weighted_sum = reference.data[i];
                float weight_sum = 1.0f;

                for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx) {
                    float cmp_guide = BlockMean(aligned_comparisons[idx], x, y, guide_block);

                    float w = 1.0f;
                    if (ref_guide >= highlight_threshold || cmp_guide >= highlight_threshold) {
                        w = 1.0f;
                    } else {
                        float diff = std::abs(cmp_guide - ref_guide);
                        float ratio = diff / noise_floor;
                        w = 1.0f / (1.0f + ratio * ratio * robustness);
                        w = ClampMin(w, min_comparison_weight);
                    }

                    weighted_sum += aligned_comparisons[idx].data[i] * w;
                    weight_sum += w;
                }

                out.data[i] = weighted_sum / weight_sum;
            }
        }
    } else {
        // Multi-channel (CFA planes or RGB): each channel gets its own weight
        // from its own blurred diff, so one channel's misalignment does not
        // penalize the others. Clipped-pixel detection remains global.
        for (uint32_t y = 0; y < out.height; ++y) {
            for (uint32_t x = 0; x < out.width; ++x) {
                // Global saturated/clipped flags (shared across channels)
                float ref_raw_guide = 0.0f;
                float cmp_raw_max[32] = {};
                for (uint32_t c = 0; c < out.channels; ++c) {
                    const size_t ci = (static_cast<size_t>(y) * out.width + x) * out.channels + c;
                    ref_raw_guide = std::max(ref_raw_guide, reference.data[ci]);
                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx) {
                        cmp_raw_max[idx] = std::max(cmp_raw_max[idx], aligned_comparisons[idx].data[ci]);
                    }
                }

                // Per-channel diff-based weight
                for (uint32_t c = 0; c < out.channels; ++c) {
                    const size_t ci = (static_cast<size_t>(y) * out.width + x) * out.channels + c;
                    float ref_g = ref_blur.data[ci];

                    float weighted_sum = reference.data[ci];
                    float weight_sum = 1.0f;

                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx) {
                        // Detect clipped comparison pixels (global check)
                        bool cmp_clipped = false;
                        if (params.clip_threshold > 0.0f && params.exposure_scales &&
                            idx < params.num_scales && params.exposure_scales[idx] > 0.0f) {
                            float estimated_original = cmp_raw_max[idx] / params.exposure_scales[idx];
                            if (estimated_original >= params.clip_threshold) cmp_clipped = true;
                        }

                        float w = 1.0f;
                        if (cmp_clipped) {
                            // Comparison was clipped: its post-scaling value is unreliable.
                            w = 0.0f;
                        } else if (ref_raw_guide >= highlight_threshold || cmp_raw_max[idx] >= highlight_threshold) {
                            // Truly saturated on both sides: equal-weight accumulation is safe.
                            w = 1.0f;
                        } else {
                            float cmp_g = cmp_blurs[idx].data[ci];
                            float diff = std::abs(cmp_g - ref_g);
                            float ratio = diff / noise_floor;
                            w = 1.0f / (1.0f + ratio * ratio * robustness);
                            w = ClampMin(w, min_comparison_weight);
                        }

                        weighted_sum += aligned_comparisons[idx].data[ci] * w;
                        weight_sum += w;
                    }

                    out.data[ci] = weighted_sum / weight_sum;
                }
            }
        }
    }
    return out;
}

} // namespace burstmerge
