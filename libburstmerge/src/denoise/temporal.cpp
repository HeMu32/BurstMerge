#include "burstmerge/internal/denoise/temporal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace burstmerge {

void RepairHotPixels(std::vector<FloatImage>& images, float white_level, uint32_t cfa_period) {
    if (images.size() < 2 || images.empty()) return;

    const uint32_t w = images.front().width;
    const uint32_t h = images.front().height;
    const uint32_t c = images.front().channels;
    const float threshold = std::max(16.0f, white_level / 64.0f);
    const uint32_t step = std::max<uint32_t>(1, cfa_period);
    std::vector<float> stack(images.size());

    for (uint32_t y = step; y + step < h; ++y) {
        for (uint32_t x = step; x + step < w; ++x) {
            for (uint32_t ch = 0; ch < c; ++ch) {
                for (size_t i = 0; i < images.size(); ++i) {
                    stack[i] = images[i].At(x, y, ch);
                }
                std::nth_element(stack.begin(), stack.begin() + stack.size() / 2, stack.end());
                const float median = stack[stack.size() / 2];

                for (auto& img : images) {
                    float center = img.At(x, y, ch);
                    // Fast temporal check
                    if (center > median + threshold) {
                        float neighbor_mean = 0.25f * (
                            img.At(x - step, y, ch) + img.At(x + step, y, ch) +
                            img.At(x, y - step, ch) + img.At(x, y + step, ch));
                        if (center > neighbor_mean + threshold) {
                            img.At(x, y, ch) = 0.5f * (median + neighbor_mean);
                        }
                    }
                }
            }
        }
    }
}

FloatImage TemporalAverage(const FloatImage& reference,
                           const std::vector<FloatImage>& aligned_comparisons,
                           const TemporalDenoiseParams& params)
{
    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    if (params.exposure_scales && params.num_scales == aligned_comparisons.size() &&
        params.white_level > params.black_level + 1.0f) {
        const float white = params.white_level;
        const float black = params.black_level;
        const float range = std::max(1.0f, white - black);
        FloatImage ref_blur = BoxBlur(reference, 2);
        std::vector<FloatImage> comp_blur;
        comp_blur.reserve(aligned_comparisons.size());
        for (const auto& img : aligned_comparisons) comp_blur.push_back(BoxBlur(img, 2));

        for (size_t i = 0; i < out.data.size(); ++i) {
            float sum = reference.data[i];
            float weight_sum = 1.0f;
            for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx) {
                float scale = std::max(1e-6f, params.exposure_scales[idx]);
                float exposure_factor = 1.0f / scale;
                float luminance = std::max(0.0f, std::min(1.0f, (comp_blur[idx].data[i] - black) / range));
                float w = std::sqrt(exposure_factor);
                if (luminance < 0.25f) {
                    w = exposure_factor;
                } else {
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
        return out;
    }

    const float inv = 1.0f / static_cast<float>(aligned_comparisons.size() + 1);
    for (size_t i = 0; i < out.data.size(); ++i) {
        float sum = reference.data[i];
        for (const auto& img : aligned_comparisons) sum += img.data[i];
        out.data[i] = sum * inv;
    }
    return out;
}

} // namespace burstmerge
