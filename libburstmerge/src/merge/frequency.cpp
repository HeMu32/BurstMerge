#include "burstmerge/internal/merge/frequency.h"

#include <algorithm>

namespace burstmerge {

FloatImage FrequencyMerge(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const FrequencyMergeParams& params)
{
    const int blur_radius = std::max(1, params.tile_size / 16);
    FloatImage ref_low = BoxBlur(reference, blur_radius);
    std::vector<FloatImage> comp_low;
    comp_low.reserve(aligned_comparisons.size());
    for (const auto& img : aligned_comparisons) {
        comp_low.push_back(BoxBlur(img, blur_radius));
    }

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    for (size_t i = 0; i < out.data.size(); ++i) {
        float low_sum = ref_low.data[i];
        float low_weight = 1.0f;
        float high = reference.data[i] - ref_low.data[i];

        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx) {
            float comp_high = aligned_comparisons[idx].data[i] - comp_low[idx].data[i];
            low_sum += comp_low[idx].data[i];
            low_weight += 1.0f;
            if (std::abs(comp_high) > std::abs(high)) high = comp_high;
        }

        out.data[i] = low_sum / low_weight + high;
    }

    return out;
}

} // namespace burstmerge
