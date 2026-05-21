#include "burstmerge/internal/denoise/temporal.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace burstmerge
{

void RepairHotPixels(std::vector<FloatImage>& images, float white_level, uint32_t cfa_period)
{
    ProfileScope scope("time.pipeline.repair_hot_pixels");
    if (images.size() < 2 || images.empty()) return;

    const uint32_t w = images.front().width;
    const uint32_t h = images.front().height;
    const uint32_t c = images.front().channels;
    const float threshold = std::max(16.0f, white_level / 64.0f);
    const uint32_t step = std::max<uint32_t>(1, cfa_period);
    ParallelForRows(step, h > step ? h - step : step, RecommendedImageRowGrain(w, c, kRowGrainMinPixels, kRowGrainMinRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        std::vector<float> stack(images.size());
        const uint32_t y_stop = std::min<uint32_t>(y_end, h - step);
        for (uint32_t y = y_begin; y < y_stop; ++y)
        {
            for (uint32_t x = step; x + step < w; ++x)
            {
                const size_t center_px = static_cast<size_t>(y) * w + x;
                const size_t left_px = static_cast<size_t>(y) * w + (x - step);
                const size_t right_px = static_cast<size_t>(y) * w + (x + step);
                const size_t top_px = static_cast<size_t>(y - step) * w + x;
                const size_t bottom_px = static_cast<size_t>(y + step) * w + x;
                for (uint32_t ch = 0; ch < c; ++ch)
                {
                    const size_t center_idx = center_px * c + ch;
                    const size_t left_idx = left_px * c + ch;
                    const size_t right_idx = right_px * c + ch;
                    const size_t top_idx = top_px * c + ch;
                    const size_t bottom_idx = bottom_px * c + ch;
                    for (size_t i = 0; i < images.size(); ++i)
                    {
                        stack[i] = images[i].data[center_idx];
                    }
                    std::sort(stack.begin(), stack.end());
                    const float median = stack[stack.size() / 2];

                    for (auto& img : images)
                    {
                        float center = img.data[center_idx];
                        if (center > median + threshold)
                        {
                            float neighbor_mean = 0.25f * (
                                img.data[left_idx] + img.data[right_idx] +
                                img.data[top_idx] + img.data[bottom_idx]);
                            if (center > neighbor_mean + threshold)
                            {
                                img.data[center_idx] = 0.5f * (median + neighbor_mean);
                            }
                        }
                    }
                }
            }
        }
    });
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
        });
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

} // namespace burstmerge
