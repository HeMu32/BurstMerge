#include "burstmerge/internal/merge/spatial.h"

#include "burstmerge/internal/core/profiler.h"
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

float ComputeHighlightWeight(bool linear_mode,
                             float diff,
                             float noise_floor,
                             float robustness,
                             float min_comparison_weight)
{
    // Highlight regions are prone to color casts when slightly misregistered.
    // We still accept comparison frames there, but with a looser consistency
    // gate instead of the old unconditional w=1 fast path.
    const float relaxed_mul = 2.5f;
    if (linear_mode)
    {
        const float base = std::max(1.0f, noise_floor / std::max(0.001f, robustness));
        const float threshold = relaxed_mul * base;
        return diff >= threshold ? 0.0f : Clamp01(1.0f - diff / threshold);
    }

    const float ratio = diff / std::max(1.0f, noise_floor * relaxed_mul);
    float w = 1.0f / (1.0f + ratio * ratio * robustness);
    return ClampMin(w, min_comparison_weight);
}

float ComputePlaneBiasVeto(float chroma_shift,
                           float noise_floor,
                           float min_comparison_weight)
{
    // If a comparison frame already shows a local RB-vs-G imbalance relative
    // to the reference, blending it tends to produce magenta blocks/halos
    // after demosaic. In that case, reduce its contribution conservatively.
    const float threshold = std::max(2.0f, noise_floor * 0.35f);
    if (chroma_shift <= threshold) return 1.0f;
    const float t = Clamp01((chroma_shift - threshold) / std::max(1.0f, threshold));
    return std::max(min_comparison_weight, 1.0f - t);
}

burstmerge::FloatImage BinomialBlur(const burstmerge::FloatImage& src,
                                    int radius = burstmerge::SpatialConstants::kBinomialRadius)
{
    if (src.data.empty()) return src;
    const float* bw = burstmerge::kBinomialWeights;
    burstmerge::FloatImage tmp;
    tmp.width = src.width;
    tmp.height = src.height;
    tmp.channels = src.channels;
    tmp.data.resize(src.data.size(), 0.0f);

    burstmerge::FloatImage out = tmp;

    burstmerge::ParallelForRows(src.height, burstmerge::RecommendedImageRowGrain(src.width, src.channels, burstmerge::kRowGrainMinPixels, burstmerge::kRowGrainMinRows),
    [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
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
    });

    burstmerge::ParallelForRows(src.height, burstmerge::RecommendedImageRowGrain(src.width, src.channels, burstmerge::kRowGrainMinPixels, burstmerge::kRowGrainMinRows),
    [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
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
    });
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

float EstimateLinearNoise(const burstmerge::FloatImage& image,
                          const burstmerge::FloatImage& blurred,
                          uint32_t sample_step)
{
    if (image.data.empty() || blurred.data.empty()) return burstmerge::SpatialConstants::kNoiseFloorFallback;
    const uint32_t step = std::max<uint32_t>(1, sample_step);
    const uint32_t sample_rows = (image.height + step - 1) / step;
    const uint32_t grain_rows = burstmerge::RecommendedImageRowGrain(
        image.width,
        image.channels,
        burstmerge::kRowGrainMinPixels,
        std::max<uint32_t>(burstmerge::kRowGrainMinRows, step));

    std::vector<double> partial_sum(sample_rows, 0.0);
    std::vector<uint64_t> partial_count(sample_rows, 0);

    burstmerge::ParallelForRows(image.height, grain_rows, [&](uint32_t y_begin, uint32_t y_end)
    {
        uint32_t y = ((y_begin + step - 1) / step) * step;
        while (y < y_end && y < image.height)
        {
            const uint32_t sample_idx = y / step;
            double local_sum = 0.0;
            uint64_t local_count = 0;
            for (uint32_t x = 0; x < image.width; x += step)
            {
                size_t idx = static_cast<size_t>(y) * image.width + x;
                local_sum += ColorDifferenceAt(image, blurred, idx);
                ++local_count;
            }
            partial_sum[sample_idx] = local_sum;
            partial_count[sample_idx] = local_count;
            y += step;
        }
    });

    double sum = 0.0;
    uint64_t count = 0;
    for (uint32_t i = 0; i < sample_rows; ++i)
    {
        sum += partial_sum[i];
        count += partial_count[i];
    }
    if (count == 0) return burstmerge::SpatialConstants::kNoiseFloorFallback;
    return std::max(burstmerge::SpatialConstants::kNoiseFloorMin, static_cast<float>(sum / static_cast<double>(count)));
}

burstmerge::FloatImage BuildBlockMeanGuide(const burstmerge::FloatImage& img,
                                           uint32_t block_size)
{
    burstmerge::FloatImage guide;
    guide.width = img.width;
    guide.height = img.height;
    guide.channels = 1;
    guide.data.resize(static_cast<size_t>(guide.width) * guide.height, 0.0f);

    block_size = std::max<uint32_t>(1, block_size);
    const uint32_t by_count = (img.height + block_size - 1) / block_size;
    burstmerge::ParallelFor(0, by_count, 1, [&](size_t by0, size_t by1)
    {
        for (size_t by = by0; by < by1; ++by)
        {
            const uint32_t y0 = static_cast<uint32_t>(by) * block_size;
            const uint32_t y1 = std::min(y0 + block_size, img.height);
            for (uint32_t x0 = 0; x0 < img.width; x0 += block_size)
            {
                const uint32_t x1 = std::min(x0 + block_size, img.width);
                double sum = 0.0;
                uint32_t n = 0;
                for (uint32_t y = y0; y < y1; ++y)
                {
                    for (uint32_t x = x0; x < x1; ++x)
                    {
                        sum += img.At(x, y, 0);
                        ++n;
                    }
                }
                const float mean = n > 0 ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
                for (uint32_t y = y0; y < y1; ++y)
                {
                    for (uint32_t x = x0; x < x1; ++x)
                    {
                        guide.At(x, y, 0) = mean;
                    }
                }
            }
        }
    });

    return guide;
}

}

namespace burstmerge
{

FloatImage SpatialMerge(const FloatImage& reference,
                        const std::vector<FloatImage>& aligned_comparisons,
                        const SpatialMergeParams& params)
{
    ProfileScope scope("time.merge.spatial_total");
    if (aligned_comparisons.empty())
    {
        return reference;
    }

    const bool linear_mode = params.mode == SpatialMergeMode::Linear;
    FloatImage ref_blur;
    {
        ProfileScope scope(linear_mode ? "time.merge.spatial.linear.ref_blur" : "time.merge.spatial.standard.ref_blur");
        ref_blur = linear_mode ? BinomialBlur(reference) : BoxBlur(reference, 2);
    }
    std::vector<FloatImage> cmp_blurs;
    cmp_blurs.reserve(aligned_comparisons.size());
    if (linear_mode)
    {
        // Avoid nested parallelism here. BinomialBlur() already parallelizes
        // over rows; calling it inside another ParallelFor() causes the inner
        // row-parallel loops to hit omp_in_parallel() and fall back to serial
        // execution, which underutilizes the CPU for typical 3-5 frame bursts.
        cmp_blurs.reserve(aligned_comparisons.size());
        ProfileScope scope("time.merge.spatial.linear.cmp_blurs");
        for (const auto& img : aligned_comparisons)
        {
            cmp_blurs.push_back(BinomialBlur(img));
        }
    }
    else
    {
        ProfileScope scope("time.merge.spatial.standard.cmp_blurs");
        for (const auto& img : aligned_comparisons)
        {
            cmp_blurs.push_back(BoxBlur(img, 2));
        }
    }

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    const float noise_floor = params.noise_floor > 0.0f
        ? params.noise_floor
        : (linear_mode
            ? EstimateLinearNoise(reference, ref_blur, params.guide_block_size)
            : std::max(burstmerge::SpatialConstants::kNoiseFloorFallback, params.noise_reduction * 4.0f));
    const float robustness = std::max(0.0f, params.robustness);
    const float min_comparison_weight = linear_mode ? burstmerge::SpatialConstants::kLinearMinComparisonWeight : burstmerge::SpatialConstants::kStandardMinComparisonWeight;
    const float highlight_threshold = params.highlight_threshold > 0.0f
        ? params.highlight_threshold
        : MaxValue(reference) * burstmerge::SpatialConstants::kHighlightThresholdFactor;
    const uint32_t guide_block = std::max<uint32_t>(1, params.guide_block_size);
    FloatImage ref_guide_map;
    if (!linear_mode)
    {
        ProfileScope scope("time.merge.spatial.standard.ref_guide_map");
        ref_guide_map = BuildBlockMeanGuide(reference, guide_block);
    }
    std::vector<FloatImage> cmp_guide_maps;
    if (!linear_mode)
    {
        cmp_guide_maps.reserve(aligned_comparisons.size());
        ProfileScope scope("time.merge.spatial.standard.cmp_guide_maps");
        for (const auto& img : aligned_comparisons)
        {
            cmp_guide_maps.push_back(BuildBlockMeanGuide(img, guide_block));
        }
    }

    if (reference.channels == 1)
    {
        ProfileScope scope(linear_mode ? "time.merge.spatial.linear.merge_loop" : "time.merge.spatial.standard.merge_loop");
        // Single-channel (mosaic): per-pixel weight, each position one guide value
        ParallelForRows(out.height, RecommendedImageRowGrain(out.width, 1, kRowGrainMinPixels, kRowGrainMinRows), [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    const size_t i = (static_cast<size_t>(y) * out.width + x);

                    float ref_guide = linear_mode ? ref_blur.data[i] : ref_guide_map.data[i];
                    float weighted_sum = reference.data[i];
                    float weight_sum = 1.0f;

                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                    {
                        float cmp_guide = linear_mode ? cmp_blurs[idx].data[i] : cmp_guide_maps[idx].data[i];

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
                        // Bright structures remain vulnerable to false color when
                        // slightly misregistered, so we still measure guide-space
                        // agreement here instead of blindly accepting the frame.
                        float diff = std::abs(cmp_guide - ref_guide);
                        w = ComputeHighlightWeight(linear_mode, diff, noise_floor, robustness, min_comparison_weight);
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
        ProfileScope scope(linear_mode ? "time.merge.spatial.linear.merge_loop" : "time.merge.spatial.standard.merge_loop");
        // Multi-channel (CFA planes or RGB): use a single shared weight per
        // comparison frame (from max_diff across all channels) so that one
        // channel's misalignment does not create different blend factors per
        // channel. Per-channel weight divergence causes color banding and
        // gradient mixing artifacts that look like demosaicing failures.
        ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainMinRows), [&](uint32_t y_begin, uint32_t y_end)
        {
            std::vector<float> shared_w(aligned_comparisons.size(), 1.0f);
            std::vector<float> cmp_raw_max(aligned_comparisons.size(), 0.0f);
            std::vector<float> cmp_max_diff(aligned_comparisons.size(), 0.0f);
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    const size_t pixel_base = (static_cast<size_t>(y) * out.width + x) * out.channels;
                    float ref_raw_guide = 0.0f;
                    float ref_vals[4] = {};
                    float ref_blur_vals[4] = {};
                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                    {
                        cmp_raw_max[idx] = 0.0f;
                        cmp_max_diff[idx] = 0.0f;
                    }
                    for (uint32_t c = 0; c < out.channels; ++c)
                    {
                        const size_t ci = pixel_base + c;
                        const float ref_v = reference.data[ci];
                        const float ref_b = ref_blur.data[ci];
                        ref_vals[c] = ref_v;
                        ref_blur_vals[c] = ref_b;
                        ref_raw_guide = std::max(ref_raw_guide, ref_v);
                        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                        {
                            const float cmp_v = aligned_comparisons[idx].data[ci];
                            cmp_raw_max[idx] = std::max(cmp_raw_max[idx], cmp_v);
                            const float d = std::abs(cmp_blurs[idx].data[ci] - ref_b);
                            cmp_max_diff[idx] = std::max(cmp_max_diff[idx], d);
                        }
                    }

                    std::fill(shared_w.begin(), shared_w.end(), 1.0f);
                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                    {
                        // First reject obviously unsafe comparison samples:
                        // clipped pixels cannot be trusted, and highlight pixels
                        // must still pass a relaxed consistency check before use.
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
                            shared_w[idx] = ComputeHighlightWeight(linear_mode, cmp_max_diff[idx], noise_floor, robustness, min_comparison_weight);
                        } else
                        {
                            if (linear_mode)
                            {
                                float threshold = std::max(1.0f, noise_floor / std::max(0.001f, robustness));
                                shared_w[idx] = cmp_max_diff[idx] >= threshold ? 0.0f : Clamp01(1.0f - cmp_max_diff[idx] / threshold);
                            } else
                            {
                                float ratio = cmp_max_diff[idx] / noise_floor;
                                shared_w[idx] = 1.0f / (1.0f + ratio * ratio * robustness);
                                shared_w[idx] = ClampMin(shared_w[idx], min_comparison_weight);
                            }
                        }

                        if (out.channels >= 4 && shared_w[idx] > 0.0f)
                        {
                            // Compare local chroma balance in blur space so we reject
                            // frames that would shift white/high-contrast interiors toward
                            // RB-heavy (magenta/purple) mixes even when luminance matches.
                            const float ref_rb = 0.5f * (ref_blur_vals[0] + ref_blur_vals[3]);
                            const float ref_g  = 0.5f * (ref_blur_vals[1] + ref_blur_vals[2]);
                            const float cmp_rb = 0.5f * (cmp_blurs[idx].data[pixel_base + 0] + cmp_blurs[idx].data[pixel_base + 3]);
                            const float cmp_g  = 0.5f * (cmp_blurs[idx].data[pixel_base + 1] + cmp_blurs[idx].data[pixel_base + 2]);
                            const float chroma_shift = std::abs((cmp_rb - cmp_g) - (ref_rb - ref_g));
                            shared_w[idx] *= ComputePlaneBiasVeto(chroma_shift, noise_floor, min_comparison_weight);
                        }
                    }

                    for (uint32_t c = 0; c < out.channels; ++c)
                    {
                        const size_t ci = pixel_base + c;
                        float weighted_sum = ref_vals[c];
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
