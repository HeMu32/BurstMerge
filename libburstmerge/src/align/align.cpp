#include "burstmerge/internal/align/align.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace burstmerge {
namespace {

float SparseSad(const FloatImage& a, const FloatImage& b, int dx, int dy, int step) {
    const int margin_x = std::abs(dx);
    const int margin_y = std::abs(dy);
    if (a.width <= static_cast<uint32_t>(margin_x * 2) || a.height <= static_cast<uint32_t>(margin_y * 2)) {
        return std::numeric_limits<float>::max();
    }

    double sad = 0.0;
    uint64_t count = 0;
    for (int y = margin_y; y < static_cast<int>(a.height) - margin_y; y += step) {
        for (int x = margin_x; x < static_cast<int>(a.width) - margin_x; x += step) {
            sad += std::abs(a.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y)) -
                            b.At(static_cast<uint32_t>(x - dx), static_cast<uint32_t>(y - dy)));
            ++count;
        }
    }
    return count ? static_cast<float>(sad / static_cast<double>(count)) : std::numeric_limits<float>::max();
}

} // namespace

AlignmentResult EstimateTranslation(const FloatImage& reference,
                                    const FloatImage& comparison,
                                    const AlignParams& params)
{
    std::vector<FloatImage> ref_pyr{reference};
    std::vector<FloatImage> cmp_pyr{comparison};
    while (ref_pyr.back().width > 256 && ref_pyr.back().height > 256 &&
           static_cast<int>(ref_pyr.size()) < params.pyramid_levels) {
        ref_pyr.push_back(Downsample2x(ref_pyr.back()));
        cmp_pyr.push_back(Downsample2x(cmp_pyr.back()));
    }

    int best_x = 0;
    int best_y = 0;
    float best_score = std::numeric_limits<float>::max();

    for (int level = static_cast<int>(ref_pyr.size()) - 1; level >= 0; --level) {
        const FloatImage& ref = ref_pyr[static_cast<size_t>(level)];
        const FloatImage& cmp = cmp_pyr[static_cast<size_t>(level)];

        best_x *= 2;
        best_y *= 2;
        int radius = std::max(1, params.search_distance >> level);
        radius = std::min(radius, 8);
        int step = std::max(1, params.tile_size >> (level + 1));

        float level_best = std::numeric_limits<float>::max();
        int level_x = best_x;
        int level_y = best_y;
        for (int dy = best_y - radius; dy <= best_y + radius; ++dy) {
            for (int dx = best_x - radius; dx <= best_x + radius; ++dx) {
                float score = SparseSad(ref, cmp, dx, dy, step);
                if (score < level_best) {
                    level_best = score;
                    level_x = dx;
                    level_y = dy;
                }
            }
        }
        best_x = level_x;
        best_y = level_y;
        best_score = level_best;
    }

    AlignmentResult out;
    out.shift_x = best_x;
    out.shift_y = best_y;
    out.confidence = 1.0f / (1.0f + best_score);
    return out;
}

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment) {
    return WarpTranslateBilinear(source,
                                 static_cast<float>(alignment.shift_x),
                                 static_cast<float>(alignment.shift_y));
}

} // namespace burstmerge
