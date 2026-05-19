#include "burstmerge/internal/align/align_dense.h"

#include "burstmerge/internal/align/align_common.h"

#include "burstmerge/internal/core/task_executor.h"

#include <cmath>
#include <limits>

namespace burstmerge
{
namespace
{
// Dense alignment helpers are grouped here because propagation, upsampling
// correction, and local tile search all share the same geometry and costs.

void CorrectUpsamplingError(const FloatImage& ref,
                            const FloatImage& cmp,
                            uint32_t tiles_x,
                            uint32_t tiles_y,
                            uint32_t tile_size,
                            uint32_t half_tile,
                            const std::vector<int16_t>& prev_x,
                            const std::vector<int16_t>& prev_y,
                            int sample_step,
                            int weight_ssd,
                            std::vector<int16_t>& out_x,
                            std::vector<int16_t>& out_y)
{
    ParallelForRows(tiles_y, 16, [&](uint32_t ty_begin, uint32_t ty_end)
    {
        for (uint32_t ty = ty_begin; ty < ty_end; ++ty)
        {
            for (uint32_t tx = 0; tx < tiles_x; ++tx)
            {
                size_t idx = static_cast<size_t>(ty) * tiles_x + tx;
                int cand_x[3] =
                {static_cast<int>(tx), static_cast<int>(tx), static_cast<int>(tx)};
                int cand_y[3] =
                {static_cast<int>(ty), static_cast<int>(ty), static_cast<int>(ty)};
                cand_x[1] = std::max(0, std::min(static_cast<int>(tiles_x) - 1,
                    static_cast<int>(tx) + ((tx % 2 == 0) ? -1 : 1)));
                cand_y[2] = std::max(0, std::min(static_cast<int>(tiles_y) - 1,
                    static_cast<int>(ty) + ((ty % 2 == 0) ? -1 : 1)));

                float best_score = std::numeric_limits<float>::max();
                int best_dx = prev_x[idx];
                int best_dy = prev_y[idx];

                for (int c = 0; c < 3; ++c)
                {
                    size_t cidx = static_cast<size_t>(cand_y[c]) * tiles_x + static_cast<uint32_t>(cand_x[c]);
                    int dx = prev_x[cidx];
                    int dy = prev_y[cidx];
                    float score = TileCost(ref, cmp,
                        tx * half_tile, ty * half_tile,
                        tile_size, tile_size,
                        dx, dy, sample_step, weight_ssd > 0);
                    if (score < best_score)
                    {
                        best_score = score;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }

                out_x[idx] = static_cast<int16_t>(best_dx);
                out_y[idx] = static_cast<int16_t>(best_dy);
            }
        }
    });
}

void SearchDenseLocal(const FloatImage& ref,
                      const FloatImage& cmp,
                      uint32_t tiles_x,
                      uint32_t tiles_y,
                      uint32_t tile_size,
                      uint32_t half_tile,
                      const std::vector<int16_t>& seed_x,
                      const std::vector<int16_t>& seed_y,
                      int sample_step,
                      int weight_ssd,
                      uint32_t cfa_period,
                      std::vector<int16_t>& out_x,
                      std::vector<int16_t>& out_y)
{
    const int kSearchDist = AlignConstants::kTileSearchRadius;
    ParallelForRows(tiles_y, 16, [&](uint32_t ty_begin, uint32_t ty_end)
    {
        for (uint32_t ty = ty_begin; ty < ty_end; ++ty)
        {
            for (uint32_t tx = 0; tx < tiles_x; ++tx)
            {
                size_t idx = static_cast<size_t>(ty) * tiles_x + tx;
                int sx0 = seed_x[idx];
                int sy0 = seed_y[idx];

                float best_score = std::numeric_limits<float>::max();
                int best_x = sx0;
                int best_y = sy0;

                for (int dy = sy0 - kSearchDist; dy <= sy0 + kSearchDist; ++dy)
                {
                    for (int dx = sx0 - kSearchDist; dx <= sx0 + kSearchDist; ++dx)
                    {
                        int sx = SnapToPeriod(dx, cfa_period);
                        int sy = SnapToPeriod(dy, cfa_period);
                        float score = TileCost(ref, cmp,
                            tx * half_tile, ty * half_tile,
                            tile_size, tile_size,
                            sx, sy, sample_step, weight_ssd > 0);
                        if (score < best_score)
                        {
                            best_score = score;
                            best_x = sx;
                            best_y = sy;
                        }
                    }
                }

                out_x[idx] = static_cast<int16_t>(best_x);
                out_y[idx] = static_cast<int16_t>(best_y);
            }
        }
    });
}

} // namespace

AlignmentResult EstimateDenseTileField(const std::vector<FloatImage>& ref_pyr,
                                       const std::vector<FloatImage>& cmp_pyr,
                                       const AlignParams& params)
{
    const FloatImage& coarsest_ref = ref_pyr.back();
    const FloatImage& coarsest_cmp = cmp_pyr.back();
    const int coarse_shift = AlignConstants::kSearchFractionShiftBase;
    const uint32_t coarse_longest = std::max(coarsest_ref.width, coarsest_ref.height);
    const int coarse_max_shift = std::max(AlignConstants::kMinSearchRadius,
                                           static_cast<int>(coarse_longest >> coarse_shift));

    int global_dx = 0;
    int global_dy = 0;
    float global_best = std::numeric_limits<float>::max();
    const uint32_t cfa = std::max<uint32_t>(1, params.cfa_period);
    const int global_step = std::max(3, static_cast<int>(cfa) * 3);
    for (int dy = -coarse_max_shift; dy <= coarse_max_shift; ++dy)
    {
        for (int dx = -coarse_max_shift; dx <= coarse_max_shift; ++dx)
        {
            float score = SparseSad(coarsest_ref, coarsest_cmp, dx, dy, global_step);
            if (score < global_best)
            {
                global_best = score;
                global_dx = dx;
                global_dy = dy;
            }
        }
    }

    AlignmentResult cur;
    cur.cfa_period = std::max<uint32_t>(1, params.cfa_period);
    cur.tile_size = AlignConstants::kDefaultTileSize;
    cur.tile_spacing = cur.tile_size;
    cur.tiles_x = 1;
    cur.tiles_y = 1;
    cur.tile_shift_x.assign(1, static_cast<int16_t>(global_dx));
    cur.tile_shift_y.assign(1, static_cast<int16_t>(global_dy));
    cur.shift_x = global_dx;
    cur.shift_y = global_dy;
    cur.confidence = 1.0f;

    for (int level = static_cast<int>(ref_pyr.size()) - 1; level >= 0; --level)
    {
        const FloatImage& ref = ref_pyr[static_cast<size_t>(level)];
        const FloatImage& cmp = cmp_pyr[static_cast<size_t>(level)];
        const uint32_t tile_size = AlignConstants::kDefaultTileSize;
        const uint32_t half_tile = tile_size / 2;
        const uint32_t tiles_x = std::max<uint32_t>(1,
            static_cast<uint32_t>(std::ceil(static_cast<double>(ref.width) / static_cast<double>(half_tile))) - 1);
        const uint32_t tiles_y = std::max<uint32_t>(1,
            static_cast<uint32_t>(std::ceil(static_cast<double>(ref.height) / static_cast<double>(half_tile))) - 1);
        const bool is_coarsest = (level == static_cast<int>(ref_pyr.size()) - 1);
        const int level_scale = is_coarsest ? 0 :
            static_cast<int>(ref_pyr[static_cast<size_t>(level)].width /
                             ref_pyr[static_cast<size_t>(level) + 1].width);

        std::vector<int16_t> prev_x(static_cast<size_t>(tiles_x) * tiles_y, 0);
        std::vector<int16_t> prev_y(static_cast<size_t>(tiles_x) * tiles_y, 0);
        if (!is_coarsest)
        {
            const int tile_ratio_x = std::max<int>(1, static_cast<int>(tiles_x) /
                std::max<int>(1, static_cast<int>(cur.tiles_x)));
            const int tile_ratio_y = std::max<int>(1, static_cast<int>(tiles_y) /
                std::max<int>(1, static_cast<int>(cur.tiles_y)));
            ParallelForRows(tiles_y, 16, [&](uint32_t ty_begin, uint32_t ty_end)
            {
                for (uint32_t ty = ty_begin; ty < ty_end; ++ty)
                {
                    for (uint32_t tx = 0; tx < tiles_x; ++tx)
                    {
                        uint32_t px = std::min(cur.tiles_x - 1, tx / tile_ratio_x);
                        uint32_t py = std::min(cur.tiles_y - 1, ty / tile_ratio_y);
                        size_t dst = static_cast<size_t>(ty) * tiles_x + tx;
                        size_t src = static_cast<size_t>(py) * cur.tiles_x + px;
                        prev_x[dst] = static_cast<int16_t>(cur.tile_shift_x[src] * level_scale);
                        prev_y[dst] = static_cast<int16_t>(cur.tile_shift_y[src] * level_scale);
                    }
                }
            });
        }

        const int weight_ssd = (level > 0) ? 1 : 0;
        const int sample_step = std::max<int>(1, cur.cfa_period);

        std::vector<int16_t> corrected_x(static_cast<size_t>(tiles_x) * tiles_y);
        std::vector<int16_t> corrected_y(static_cast<size_t>(tiles_x) * tiles_y);
        if (!is_coarsest)
        {
            CorrectUpsamplingError(ref, cmp, tiles_x, tiles_y, tile_size, half_tile,
                                   prev_x, prev_y, sample_step, weight_ssd,
                                   corrected_x, corrected_y);
        }
        else
        {
            corrected_x = prev_x;
            corrected_y = prev_y;
        }

        AlignmentResult next;
        next.tile_size = static_cast<int32_t>(tile_size);
        next.tile_spacing = static_cast<int32_t>(half_tile);
        next.cfa_period = cur.cfa_period;
        next.tiles_x = tiles_x;
        next.tiles_y = tiles_y;
        next.tile_shift_x.assign(static_cast<size_t>(tiles_x) * tiles_y, 0);
        next.tile_shift_y.assign(static_cast<size_t>(tiles_x) * tiles_y, 0);

        SearchDenseLocal(ref, cmp, tiles_x, tiles_y, tile_size, half_tile,
                         corrected_x, corrected_y, sample_step, weight_ssd,
                         cur.cfa_period, next.tile_shift_x, next.tile_shift_y);

        SmoothTileField(next);
        cur = std::move(next);
    }

    long sx = 0;
    long sy = 0;
    for (int16_t v : cur.tile_shift_x) sx += v;
    for (int16_t v : cur.tile_shift_y) sy += v;
    int n = static_cast<int>(std::max<size_t>(1, cur.tile_shift_x.size()));
    cur.shift_x = static_cast<int32_t>(std::lround(static_cast<double>(sx) / n));
    cur.shift_y = static_cast<int32_t>(std::lround(static_cast<double>(sy) / n));
    cur.confidence = 1.0f;
    return cur;
}

} // namespace burstmerge
