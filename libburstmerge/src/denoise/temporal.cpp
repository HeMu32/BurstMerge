#include "burstmerge/internal/denoise/temporal.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace burstmerge
{

void RepairHotPixels(std::vector<FloatImage>& images,
                     float white_level,
                     const float black_level[4],
                     uint32_t cfa_period)
{
    ProfileScope scope("time.pipeline.repair_hot_pixels");
    if (images.empty()) return;

    const uint32_t w = images.front().width;
    const uint32_t h = images.front().height;
    const uint32_t c = images.front().channels;
    if (w < 5 || h < 5) return;
    const uint32_t grain_rows = RecommendedImageRowGrain(w, c, kRowGrainMinPixels, kRowGrainMinRows);

    FloatImage avg = images.front();
    std::fill(avg.data.begin(), avg.data.end(), 0.0f);
    const size_t total = avg.data.size();
    ParallelFor(total, 1u << 16, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i)
        {
            float s = 0.0f;
            for (const auto& img : images) s += img.data[i];
            avg.data[i] = s;
        }
    }, "hotpix_avg_sum" /* named tag for profiler */);
    const float inv_n = 1.0f / static_cast<float>(images.size());
    ParallelFor(total, 1u << 16, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i) avg.data[i] *= inv_n;
    }, "hotpix_avg_norm" /* named tag for profiler */);

    std::vector<float> mean_subpixel(std::max<uint32_t>(c, 4u), 0.0f);
    std::vector<uint64_t> mean_count(std::max<uint32_t>(c, 4u), 0);
    const uint32_t bins = static_cast<uint32_t>(mean_subpixel.size());
    const uint32_t row_groups = std::max<uint32_t>(1, (h + grain_rows - 1) / grain_rows);
    std::vector<float> partial_sum(static_cast<size_t>(row_groups) * bins, 0.0f);
    std::vector<uint64_t> partial_cnt(static_cast<size_t>(row_groups) * bins, 0);
    ParallelForRows(h, grain_rows, [&](uint32_t y_begin, uint32_t y_end)
    {
        const uint32_t group = y_begin / grain_rows;
        float* ps = partial_sum.data() + static_cast<size_t>(group) * bins;
        uint64_t* pc = partial_cnt.data() + static_cast<size_t>(group) * bins;
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t sub = (c == 1)
                    ? (y % std::max<uint32_t>(2, cfa_period)) * std::max<uint32_t>(2, cfa_period) + (x % std::max<uint32_t>(2, cfa_period))
                    : std::min<uint32_t>(c - 1, 0);
                if (c == 1)
                {
                    ps[sub] += avg.At(x, y, 0);
                    ++pc[sub];
                }
                else
                {
                    for (uint32_t ch = 0; ch < c; ++ch)
                    {
                        ps[ch] += avg.At(x, y, ch);
                        ++pc[ch];
                    }
                }
            }
        }
    }, "hotpix_mean_sub" /* named tag for profiler */);
    for (uint32_t g = 0; g < row_groups; ++g)
    {
        for (uint32_t i = 0; i < bins; ++i)
        {
            mean_subpixel[i] += partial_sum[static_cast<size_t>(g) * bins + i];
            mean_count[i] += partial_cnt[static_cast<size_t>(g) * bins + i];
        }
    }
    for (uint32_t i = 0; i < mean_subpixel.size(); ++i)
    {
        if (mean_count[i]) mean_subpixel[i] /= static_cast<float>(mean_count[i]);
    }

    std::vector<float> hot_weight(static_cast<size_t>(w) * h * c, 0.0f);
    const float hot_pixel_threshold = (black_level != nullptr) ? 2.0f : 1.0f;
    const float correction_strength = 1.0f;

    auto get_black = [&](uint32_t x, uint32_t y, uint32_t ch) -> float
    {
        if (!black_level) return 0.0f;
        if (c == 1)
        {
            const uint32_t pw = std::max<uint32_t>(2, cfa_period);
            const uint32_t idx = (y % pw) * pw + (x % pw);
            return idx < 4 ? black_level[idx] : black_level[0];
        }
        return ch < 4 ? black_level[ch] : black_level[0];
    };

    if (c == 1)
    {
        const uint32_t step = std::max<uint32_t>(2, cfa_period);
        ParallelForRows(h, grain_rows, [&](uint32_t y_begin, uint32_t y_end)
        {
            const uint32_t ys = std::max(step, y_begin);
            const uint32_t ye = std::min<uint32_t>(y_end, h - step);
            for (uint32_t y = ys; y < ye; ++y)
            {
                for (uint32_t x = step; x + step < w; ++x)
                {
                    const float center = avg.At(x, y, 0);
                    const float sum = (
                        avg.At(x - step, y - step, 0) + avg.At(x + step, y - step, 0) +
                        avg.At(x - step, y + step, 0) + avg.At(x + step, y + step, 0) +
                        2.0f * avg.At(x - step, y, 0) + 2.0f * avg.At(x + step, y, 0) +
                        2.0f * avg.At(x, y - step, 0) + 2.0f * avg.At(x, y + step, 0)) / 12.0f;
                    const float black = get_black(x, y, 0);
                    const uint32_t sub = (y % step) * step + (x % step);
                    const float mean_tex = mean_subpixel[std::min<uint32_t>(sub, static_cast<uint32_t>(mean_subpixel.size() - 1))];
                    const float ratio = std::max(1.0f, center - black) / std::max(1.0f, sum - black);
                    if (ratio >= hot_pixel_threshold && center >= 2.0f * mean_tex)
                    {
                        float wgt = 0.5f * correction_strength * std::min(2.0f, ratio - hot_pixel_threshold);
                        wgt = std::max(0.0f, std::min(1.0f, wgt));
                        hot_weight[(static_cast<size_t>(y) * w + x)] = wgt;
                    }
                }
            }
        });

        for (auto& img : images)
        {
            ParallelForRows(h, grain_rows, [&](uint32_t y_begin, uint32_t y_end)
            {
                const uint32_t ys = std::max(step, y_begin);
                const uint32_t ye = std::min<uint32_t>(y_end, h - step);
                for (uint32_t y = ys; y < ye; ++y)
                {
                    for (uint32_t x = step; x + step < w; ++x)
                    {
                        const float wgt = hot_weight[(static_cast<size_t>(y) * w + x)];
                        if (wgt <= 0.001f) continue;
                        const float neigh = 0.25f * (img.At(x - step, y, 0) + img.At(x + step, y, 0) +
                                                     img.At(x, y - step, 0) + img.At(x, y + step, 0));
                        img.At(x, y, 0) = wgt * neigh + (1.0f - wgt) * img.At(x, y, 0);
                    }
                }
            }, "hotpix_correct_mono" /* named tag for profiler */);
        }
        return;
    }

    ParallelFor(c, 1, [&](size_t ch0, size_t ch1)
    {
        for (size_t chs = ch0; chs < ch1; ++chs)
        {
            const uint32_t ch = static_cast<uint32_t>(chs);
            ParallelForRows(h, grain_rows, [&](uint32_t y_begin, uint32_t y_end)
            {
                const uint32_t ys = std::max<uint32_t>(2, y_begin);
                const uint32_t ye = std::min<uint32_t>(y_end, h - 2);
                for (uint32_t y = ys; y < ye; ++y)
                {
                    for (uint32_t x = 2; x + 2 < w; ++x)
                    {
                        const float center = avg.At(x, y, ch);
                        const float sum = (
                            avg.At(x - 2, y - 2, ch) + avg.At(x + 2, y - 2, ch) +
                            avg.At(x - 2, y + 2, ch) + avg.At(x + 2, y + 2, ch) +
                            2.0f * avg.At(x - 2, y, ch) + 2.0f * avg.At(x + 2, y, ch) +
                            2.0f * avg.At(x, y - 2, ch) + 2.0f * avg.At(x, y + 2, ch)) / 12.0f;
                        const float black = get_black(x, y, ch);
                        const float mean_tex = mean_subpixel[std::min<uint32_t>(ch, static_cast<uint32_t>(mean_subpixel.size() - 1))];
                        const float ratio = std::max(1.0f, center - black) / std::max(1.0f, sum - black);
                        if (ratio >= hot_pixel_threshold && center >= 2.0f * mean_tex)
                        {
                            float wgt = 0.5f * correction_strength * std::min(2.0f, ratio - hot_pixel_threshold);
                            wgt = std::max(0.0f, std::min(1.0f, wgt));
                            hot_weight[(static_cast<size_t>(y) * w + x) * c + ch] = wgt;
                        }
                    }
                }
            }, "hotpix_detect_rows" /* named tag for profiler */);
        }
    }, "hotpix_detect_ch" /* named tag for profiler */);

    for (auto& img : images)
    {
        ParallelFor(c, 1, [&](size_t ch0, size_t ch1)
        {
            for (size_t chs = ch0; chs < ch1; ++chs)
            {
                const uint32_t ch = static_cast<uint32_t>(chs);
                ParallelForRows(h, grain_rows, [&](uint32_t y_begin, uint32_t y_end)
                {
                    const uint32_t ys = std::max<uint32_t>(2, y_begin);
                    const uint32_t ye = std::min<uint32_t>(y_end, h - 2);
                    for (uint32_t y = ys; y < ye; ++y)
                    {
                        for (uint32_t x = 2; x + 2 < w; ++x)
                        {
                            const float wgt = hot_weight[(static_cast<size_t>(y) * w + x) * c + ch];
                            if (wgt <= 0.001f) continue;
                            const float neigh = 0.25f * (img.At(x - 2, y, ch) + img.At(x + 2, y, ch) +
                                                         img.At(x, y - 2, ch) + img.At(x, y + 2, ch));
                            img.At(x, y, ch) = wgt * neigh + (1.0f - wgt) * img.At(x, y, ch);
                        }
                    }
                }, "hotpix_correct_rows" /* named tag for profiler */);
            }
        }, "hotpix_correct_ch" /* named tag for profiler */);
    }
}

FloatImage TemporalAverage(const FloatImage& reference,
                           const std::vector<FloatImage>& aligned_comparisons,
                           const TemporalDenoiseParams& params)
{
    ProfileScope scope("time.merge.temporal_average_total");
    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    if (params.exposure_scales && params.num_scales == aligned_comparisons.size() &&
        params.white_level > params.black_level + 1.0f)
        {
        const float white = params.white_level;
        const float black = params.black_level;
        const float range = std::max(1.0f, white - black);
        FloatImage ref_blur = BoxBlur(reference, 2);
        std::vector<FloatImage> comp_blur;
        comp_blur.reserve(aligned_comparisons.size());
        for (const auto& img : aligned_comparisons) comp_blur.push_back(BoxBlur(img, 2));

        ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainMinRows), [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                const size_t base = static_cast<size_t>(y) * out.width * out.channels;
                for (size_t i = base; i < base + static_cast<size_t>(out.width) * out.channels; ++i)
                {
                    float sum = reference.data[i];
                    float weight_sum = 1.0f;
                    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
                    {
                        float scale = std::max(1e-6f, params.exposure_scales[idx]);
                        float exposure_factor = 1.0f / scale;
                        float luminance = std::max(0.0f, std::min(1.0f, (comp_blur[idx].data[i] - black) / range));
                        float w = std::sqrt(exposure_factor);
                        if (luminance < 0.25f)
                        {
                            w = exposure_factor;
                        } else
                        {
                            float t = std::max(0.0f, std::min(1.0f, (luminance - 0.25f) / 0.74f));
                            w = exposure_factor * (1.0f - t) + 1.0f * t;
                        }
                        float highlight_w = std::max(0.0f, std::min(1.0f, 0.99f / 0.74f - luminance / 0.74f));
                        w = std::max(1.0f, w) * highlight_w;
                        sum += aligned_comparisons[idx].data[i] * w;
                        weight_sum += w;
                    }
                    out.data[i] = sum / std::max(1e-6f, weight_sum);
                }
            }
        }, "temporal_avg" /* named tag for profiler */);
        return out;
    }

    const float inv = 1.0f / static_cast<float>(aligned_comparisons.size() + 1);
    for (size_t i = 0; i < out.data.size(); ++i)
    {
        float sum = reference.data[i];
        for (const auto& img : aligned_comparisons) sum += img.data[i];
        out.data[i] = sum * inv;
    }
    return out;
}

FloatImage TemporalMedian(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const TemporalDenoiseParams& params)
{
    ProfileScope scope("time.merge.temporal_median_total");
    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    const size_t num_comp = aligned_comparisons.size();
    if (num_comp == 0)
    {
        out.data = reference.data;
        return out;
    }

    const float* ref_ptr = reference.data.data();
    float* out_ptr = out.data.data();
    const uint32_t channels = reference.channels;
    const size_t num_pixels = static_cast<size_t>(reference.width) * reference.height;

    std::vector<const float*> comp_ptrs;
    comp_ptrs.reserve(num_comp);
    for (const auto& img : aligned_comparisons) comp_ptrs.push_back(img.data.data());

    const float* exp_scales = params.exposure_scales;
    const uint32_t num_scales = params.num_scales;

    // Clip threshold (0.98 of dynamic range, matching SpatialMerge kClipFactor).
    constexpr float kClipFactor = 0.98f;
    const float clip_threshold = (params.white_level > params.black_level + 1.0f)
        ? (params.white_level - params.black_level) * kClipFactor
        : 0.0f;
    const bool have_clip = (clip_threshold > 0.0f);

    constexpr size_t kStackFrames = 64;
    const size_t grain_pixels = std::max<size_t>(1, (1u << 16) / std::max<uint32_t>(1u, channels));

    // Precompute per-comparison clip thresholds so the hot loop does a single
    // comparison instead of a division per comparison per pixel.
    float clip_scaled[kStackFrames];
    if (have_clip)
    {
        for (size_t k = 0; k < num_comp; ++k)
        {
            float scale = (exp_scales && k < num_scales && exp_scales[k] > 0.0f)
                ? exp_scales[k] : 1.0f;
            clip_scaled[k] = clip_threshold * scale;
        }
    }

    ParallelFor(num_pixels, grain_pixels, [&](size_t p0, size_t p1)
    {
        float buf[kStackFrames];
        bool clipped[kStackFrames];
        for (size_t p = p0; p < p1; ++p)
        {
            const size_t base = p * channels;

            for (size_t k = 0; k < num_comp; ++k)
            {
                clipped[k] = false;
                if (!have_clip) continue;

                float cmp_max = comp_ptrs[k][base];
                for (uint32_t c = 1; c < channels; ++c)
                {
                    const float v = comp_ptrs[k][base + c];
                    if (v > cmp_max) cmp_max = v;
                }
                if (cmp_max >= clip_scaled[k]) clipped[k] = true;
            }

            for (uint32_t c = 0; c < channels; ++c)
            {
                const size_t ci = base + c;

                // Bypass: preserve reference values at/near clipping so the
                // RAW converter's highlight recovery can detect and reconstruct
                // them.  Without this, the median pulls clipped values below
                // white_level by averaging with non-clipped comparisons,
                // hiding the clipping signal → no recovery → color cast.
                if (have_clip && ref_ptr[ci] >= clip_threshold)
                {
                    out_ptr[ci] = ref_ptr[ci];
                    continue;
                }

                size_t cnt = 0;
                buf[cnt++] = ref_ptr[ci];
                for (size_t k = 0; k < num_comp; ++k)
                    if (!clipped[k]) buf[cnt++] = comp_ptrs[k][ci];

                if (cnt == 1) { out_ptr[ci] = buf[0]; continue; }

                const size_t mid = cnt / 2;
                std::nth_element(buf, buf + mid, buf + cnt);
                if (cnt & 1u)
                {
                    out_ptr[ci] = buf[mid];
                }
                else
                {
                    const float hi = buf[mid];
                    float lo = buf[0];
                    for (size_t k = 1; k < mid; ++k)
                        if (buf[k] > lo) lo = buf[k];
                    out_ptr[ci] = 0.5f * (lo + hi);
                }
            }
        }
    }, "temporal_median" /* named tag for profiler */);

    return out;
}

} // namespace burstmerge
