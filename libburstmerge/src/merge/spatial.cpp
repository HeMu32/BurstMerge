#include "burstmerge/internal/merge/spatial.h"

#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace burstmerge
{

const float kBinomialWeights[9] =
{
    601080390.0f, 565722720.0f, 471435600.0f, 347373600.0f,
    225792840.0f, 129024480.0f, 64512240.0f,  28048800.0f,
    10518300.0f
};

} // namespace burstmerge

namespace
{

float Clamp01(float v)
{
    return std::max(0.0f, std::min(1.0f, v));
}

float ClampMin(float v, float lo)
{
    return v < lo ? lo : v;
}

burstmerge::FloatImage BinomialBlur(const burstmerge::FloatImage& src, int radius = 8)
{
    if (src.data.empty()) return src;
    const float* bw = burstmerge::kBinomialWeights;
    burstmerge::FloatImage tmp;
    tmp.width = src.width;
    tmp.height = src.height;
    tmp.channels = src.channels;
    tmp.data.resize(src.data.size(), 0.0f);

    burstmerge::FloatImage out = tmp;

    for (uint32_t y = 0; y < src.height; ++y)
    {
        for (uint32_t x = 0; x < src.width; ++x)
        {
            for (uint32_t c = 0; c < src.channels; ++c)
            {
                double sum = 0.0;
                double weight = 0.0;
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    int sx = static_cast<int>(x) + dx;
                    if (sx < 0 || sx >= static_cast<int>(src.width)) continue;
                    float w = bw[std::abs(dx)];
                    sum += static_cast<double>(w) * src.At(static_cast<uint32_t>(sx), y, c);
                    weight += w;
                }
                tmp.At(x, y, c) = static_cast<float>(sum / std::max(1.0, weight));
            }
        }
    }

    for (uint32_t y = 0; y < src.height; ++y)
    {
        for (uint32_t x = 0; x < src.width; ++x)
        {
            for (uint32_t c = 0; c < src.channels; ++c)
            {
                double sum = 0.0;
                double weight = 0.0;
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    int sy = static_cast<int>(y) + dy;
                    if (sy < 0 || sy >= static_cast<int>(src.height)) continue;
                    float w = bw[std::abs(dy)];
                    sum += static_cast<double>(w) * tmp.At(x, static_cast<uint32_t>(sy), c);
                    weight += w;
                }
                out.At(x, y, c) = static_cast<float>(sum / std::max(1.0, weight));
            }
        }
    }
    return out;
}

float ColorDifferenceAt(const burstmerge::FloatImage& a, const burstmerge::FloatImage& b, size_t pixel_index)
{
    float diff = 0.0f;
    const size_t base = pixel_index * a.channels;
    for (uint32_t c = 0; c < a.channels; ++c)
    {
        diff += std::abs(a.data[base + c] - b.data[base + c]);
    }
    return diff;
}

float EstimateLinearNoise(const burstmerge::FloatImage& image, uint32_t sample_step)
{
    if (image.data.empty()) return burstmerge::SpatialConstants::kNoiseFloorFallback;
    const burstmerge::FloatImage blurred = BinomialBlur(image);
    const uint32_t step = std::max<uint32_t>(1, sample_step);
    double sum = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 0; y < image.height; y += step)
    {
        for (uint32_t x = 0; x < image.width; x += step)
        {
            size_t idx = static_cast<size_t>(y) * image.width + x;
            sum += ColorDifferenceAt(image, blurred, idx);
            ++count;
        }
    }
    if (count == 0) return burstmerge::SpatialConstants::kNoiseFloorFallback;
    return std::max(burstmerge::SpatialConstants::kNoiseFloorMin, static_cast<float>(sum / static_cast<double>(count)));
}

float BlockMean(const burstmerge::FloatImage& img, uint32_t x, uint32_t y, uint32_t block_size)
{
    const uint32_t x0 = (x / block_size) * block_size;
    const uint32_t y0 = (y / block_size) * block_size;
    float sum = 0.0f;
    uint32_t n = 0;
    for (uint32_t yy = y0; yy < std::min(y0 + block_size, img.height); ++yy)
    {
        for (uint32_t xx = x0; xx < std::min(x0 + block_size, img.width); ++xx)
        {
            sum += img.At(xx, yy, 0);
            ++n;
        }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

}

namespace burstmerge
{

FloatImage SpatialMerge(const FloatImage& reference,
                        const std::vector<FloatImage>& aligned_comparisons,
                        const SpatialMergeParams& params)
{
    if (aligned_comparisons.empty())
    {
        return reference;
    }

    const bool linear_mode = params.mode == SpatialMergeMode::Linear;
    const FloatImage ref_blur = linear_mode ? BinomialBlur(reference) : BoxBlur(reference, 2);
    std::vector<FloatImage> cmp_blurs;
    cmp_blurs.reserve(aligned_comparisons.size());
    for (const auto& img : aligned_comparisons)
    {
        cmp_blurs.push_back(linear_mode ? BinomialBlur(img) : BoxBlur(img, 2));
    }

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    const float noise_floor = linear_mode
        ? EstimateLinearNoise(reference, params.guide_block_size)
        : (params.noise_floor > 0.0f ? params.noise_floor : std::max(burstmerge::SpatialConstants::kNoiseFloorFallback, params.noise_reduction * 4.0f));
    const float robustness = std::max(0.0f, params.robustness);
    const float min_comparison_weight = linear_mode ? burstmerge::SpatialConstants::kLinearMinComparisonWeight : burstmerge::SpatialConstants::kLegacyMinComparisonWeight;
    const float highlight_threshold = params.highlight_threshold > 0.0f
        ? params.highlight_threshold
        : MaxValue(reference) * burstmerge::SpatialConstants::kHighlightThresholdFactor;
    const uint32_t guide_block = std::max<uint32_t>(1, params.guide_block_size);

    if (reference.channels == 1)
    {
        // Single-channel (mosaic): per-pixel weight, each position one guide value
        ParallelForRows(out.height, 32, [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    const size_t i = (static_cast<size_t>(y) * out.width + x);

                        float ref_guide = linear_mode ? ref_blur.data[i] : BlockMean(reference, x, y, guide_block);
                    float weighted_sum = reference.data[i];
                    float weight_sum = 1.0f;

                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                    {
                        float cmp_guide = linear_mode ? cmp_blurs[idx].data[i] : BlockMean(aligned_comparisons[idx], x, y, guide_block);

                    // Detect clipped comparison pixels: estimated original value
                    // (before normalization scaling) above clip_threshold means
                    // the pixel has reached or exceeded the sensor's saturation
                    // and its normalized value is unreliable.
                    bool cmp_clipped = false;
                    if (params.clip_threshold > 0.0f && params.exposure_scales &&
                        idx < params.num_scales && params.exposure_scales[idx] > 0.0f)
                        {
                        float scale = params.exposure_scales[idx];
                        float estimated_original = aligned_comparisons[idx].data[i] / scale;
                        if (estimated_original >= params.clip_threshold) cmp_clipped = true;
                    }

                    float w = 1.0f;
                    if (cmp_clipped)
                    {
                        w = 0.0f;
                    } else if (ref_guide >= highlight_threshold || cmp_guide >= highlight_threshold)
                    {
                        w = 1.0f;
                    } else
                    {
                        float diff = std::abs(cmp_guide - ref_guide);
                        if (linear_mode)
                        {
                            w = robustness == 0.0f ? 1.0f : Clamp01(1.0f - diff / std::max(1.0f, noise_floor / robustness));
                        } else
                        {
                            float ratio = diff / noise_floor;
                            w = 1.0f / (1.0f + ratio * ratio * robustness);
                            w = ClampMin(w, min_comparison_weight);
                        }
                    }

                        weighted_sum += aligned_comparisons[idx].data[i] * w;
                        weight_sum += w;
                    }

                    out.data[i] = weighted_sum / weight_sum;
                }
            }
        });
    } else
    {
        // Multi-channel (CFA planes or RGB): use a single shared weight per
        // comparison frame (from max_diff across all channels) so that one
        // channel's misalignment does not create different blend factors per
        // channel. Per-channel weight divergence causes color banding and
        // gradient mixing artifacts that look like demosaicing failures.
        ParallelForRows(out.height, 16, [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    float ref_raw_guide = 0.0f;
                    float cmp_raw_max[32] = {};
                    for (uint32_t c = 0; c < out.channels; ++c)
                    {
                        const size_t ci = (static_cast<size_t>(y) * out.width + x) * out.channels + c;
                        ref_raw_guide = std::max(ref_raw_guide, reference.data[ci]);
                        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                        {
                            cmp_raw_max[idx] = std::max(cmp_raw_max[idx], aligned_comparisons[idx].data[ci]);
                        }
                    }

                    std::vector<float> shared_w(aligned_comparisons.size(), 1.0f);
                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                    {
                        bool cmp_clipped = false;
                        if (params.clip_threshold > 0.0f && params.exposure_scales &&
                            idx < params.num_scales && params.exposure_scales[idx] > 0.0f)
                        {
                            float estimated_original = cmp_raw_max[idx] / params.exposure_scales[idx];
                            if (estimated_original >= params.clip_threshold) cmp_clipped = true;
                        }
                        if (cmp_clipped)
                        {
                            shared_w[idx] = 0.0f;
                        } else if (ref_raw_guide >= highlight_threshold || cmp_raw_max[idx] >= highlight_threshold)
                        {
                            shared_w[idx] = 1.0f;
                        } else
                        {
                            float max_diff = 0.0f;
                            for (uint32_t c = 0; c < out.channels; ++c)
                            {
                                const size_t ci = (static_cast<size_t>(y) * out.width + x) * out.channels + c;
                                float d = std::abs(cmp_blurs[idx].data[ci] - ref_blur.data[ci]);
                                max_diff = std::max(max_diff, d);
                            }
                            if (linear_mode)
                            {
                                float threshold = std::max(1.0f, noise_floor / std::max(0.001f, robustness));
                                shared_w[idx] = max_diff >= threshold ? 0.0f : Clamp01(1.0f - max_diff / threshold);
                            } else
                            {
                                float ratio = max_diff / noise_floor;
                                shared_w[idx] = 1.0f / (1.0f + ratio * ratio * robustness);
                                shared_w[idx] = ClampMin(shared_w[idx], min_comparison_weight);
                            }
                        }
                    }

                    for (uint32_t c = 0; c < out.channels; ++c)
                    {
                        const size_t ci = (static_cast<size_t>(y) * out.width + x) * out.channels + c;

                        float weighted_sum = reference.data[ci];
                        float weight_sum = 1.0f;

                        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                        {
                            float w = shared_w[idx];
                            weighted_sum += aligned_comparisons[idx].data[ci] * w;
                            weight_sum += w;
                        }

                        out.data[ci] = weighted_sum / weight_sum;
                    }
                }
            }
        });
    }
    return out;
}

} // namespace burstmerge
