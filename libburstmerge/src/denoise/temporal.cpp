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
                           const TemporalDenoiseParams&)
{
    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    const float inv = 1.0f / static_cast<float>(aligned_comparisons.size() + 1);
    for (size_t i = 0; i < out.data.size(); ++i) {
        float sum = reference.data[i];
        for (const auto& img : aligned_comparisons) sum += img.data[i];
        out.data[i] = sum * inv;
    }
    return out;
}

} // namespace burstmerge
