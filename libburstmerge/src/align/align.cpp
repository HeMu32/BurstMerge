#include "burstmerge/internal/align/align.h"

#include "burstmerge/internal/align/align_common.h"
#include "burstmerge/internal/align/align_dense.h"
#include "burstmerge/internal/align/align_standard.h"
#include "burstmerge/internal/align/align_pyramid.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace burstmerge
{

namespace
{

// Base for the pixel subsampling stride used inside the Standard estimator's
// coarse-to-fine global SAD seed search (the SparseSad loop below). The actual
// stride at a given pyramid level is kGlobalSadStepBase >> (level + 1), i.e. it
// halves toward finer levels. This is a search-acceleration heuristic ONLY; it
// is intentionally decoupled from AlignParams::tile_size (which governs tile
// geometry elsewhere) because seed-search density must not change with the
// user's tile-size choice, or the global seed may miss the true shift and the
// per-tile RefineTileField (local_radius search) cannot recover it.
constexpr int kGlobalSadStepBase = 16;

// Core alignment logic shared by both EstimateTranslation entry points.
// Receives a pre-built reference pyramid; builds only the comparison pyramid.
AlignmentResult EstimateTranslationImpl(
    const std::vector<FloatImage>& ref_pyr,
    const FloatImage& reference,
    const FloatImage& comparison,
    const AlignParams& params)
{
    std::vector<FloatImage> cmp_pyr = BuildPyramidSingle(comparison, params.tile_size);

    if (params.mode == AlignmentMode::DenseTile)
    {
        return EstimateDenseTileField(ref_pyr, cmp_pyr, params);
    }

    if (params.mode == AlignmentMode::Frequency)
    {
        return EstimateFrequencyTileField(ref_pyr, cmp_pyr, params);
    }

    int best_x = 0;
    int best_y = 0;
    float best_score = std::numeric_limits<float>::max();

    for (int level = static_cast<int>(ref_pyr.size()) - 1; level >= 0; --level)
    {
        const FloatImage& ref = ref_pyr[static_cast<size_t>(level)];
        const FloatImage& cmp = cmp_pyr[static_cast<size_t>(level)];

        if (level < static_cast<int>(ref_pyr.size()) - 1)
        {
            const int scale = static_cast<int>(ref_pyr[static_cast<size_t>(level)].width /
                ref_pyr[static_cast<size_t>(level) + 1].width);
            best_x *= scale;
            best_y *= scale;
        }
        const int pyr_n = static_cast<int>(ref_pyr.size());
        const int shift = AlignConstants::kSearchFractionShiftBase + (pyr_n - 1 - level);
        const uint32_t longest = std::max(ref.width, ref.height);
        int radius = std::max(AlignConstants::kMinSearchRadius,
                              static_cast<int>(longest >> shift));
        int step = std::max(1, kGlobalSadStepBase >> (level + 1));

        float level_best = std::numeric_limits<float>::max();
        int level_x = best_x;
        int level_y = best_y;
        const int dy_begin = best_y - radius;
        const int dy_end = best_y + radius + 1;
        const size_t rows = static_cast<size_t>(dy_end - dy_begin);
        std::vector<SearchBest> partial(rows);
        ParallelFor(rows, 1, [&](size_t i0, size_t i1)
        {
            for (size_t i = i0; i < i1; ++i)
            {
                const int dy = dy_begin + static_cast<int>(i);
                SearchBest best;
                best.dx = best_x;
                best.dy = dy;
                for (int dx = best_x - radius; dx <= best_x + radius; ++dx)
                {
                    float score = SparseSad(ref, cmp, dx, dy, step);
                    if (score < best.score)
                    {
                        best.score = score;
                        best.dx = dx;
                        best.dy = dy;
                    }
                }
                partial[i] = best;
            }
        }, "align_search" /* named tag for profiler */);
        for (const auto& best : partial)
        {
            if (best.score < level_best)
            {
                level_best = best.score;
                level_x = best.dx;
                level_y = best.dy;
            }
        }
        best_x = level_x;
        best_y = level_y;
        best_score = level_best;
    }

    AlignmentResult out;
    out.shift_x = SnapToPeriod(best_x, params.cfa_period);
    out.shift_y = SnapToPeriod(best_y, params.cfa_period);
    out.confidence = 1.0f / (1.0f + best_score);
    RefineTileField(reference, comparison, params, out);
    return out;
}

} // namespace

AlignmentResult EstimateTranslation(const FloatImage& reference,
                                    const FloatImage& comparison,
                                    const AlignParams& params)
{
    ProfileScope scope("time.align.estimate_translation_total");
    std::vector<FloatImage> ref_pyr = BuildPyramidSingle(reference, params.tile_size);
    return EstimateTranslationImpl(ref_pyr, reference, comparison, params);
}

AlignmentResult EstimateTranslation(const std::vector<FloatImage>& ref_pyr,
                                    const FloatImage& reference,
                                    const FloatImage& comparison,
                                    const AlignParams& params)
{
    ProfileScope scope("time.align.estimate_translation_total");
    return EstimateTranslationImpl(ref_pyr, reference, comparison, params);
}

} // namespace burstmerge
