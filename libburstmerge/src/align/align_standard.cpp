#include "burstmerge/internal/align/align_standard.h"

#include "burstmerge/internal/align/align_common.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace burstmerge
{

// Standard refinement starts from a single global translation estimate and then
// locally refines each tile around propagated seeds.
//
// The tile loop is parallelised using an anti-diagonal wavefront: all tiles
// with the same (tx + ty) value are independent because their seed
// dependencies — the left neighbour (tx-1, ty) and top neighbour (tx, ty-1) —
// both lie on the previous wavefront (tx+ty-1), which has already completed.
// This preserves the exact same seed values, candidate evaluation order, and
// floating-point results as the original serial loop, so the output is
// bit-identical.

void RefineTileField(const FloatImage& reference,
                     const FloatImage& comparison,
                     const AlignParams& params,
                     AlignmentResult& result)
{
    ProfileScope scope("time.align.refine_tile_field");

    const uint32_t tile_size = static_cast<uint32_t>(ResolveAlignTile(params.tile_size));
    result.tile_size = static_cast<int32_t>(tile_size);
    result.cfa_period = std::max<uint32_t>(1, params.cfa_period);
    result.tiles_x = (reference.width + tile_size - 1) / tile_size;
    result.tiles_y = (reference.height + tile_size - 1) / tile_size;
    result.tile_shift_x.assign(static_cast<size_t>(result.tiles_x) * result.tiles_y, static_cast<int16_t>(result.shift_x));
    result.tile_shift_y.assign(static_cast<size_t>(result.tiles_x) * result.tiles_y, static_cast<int16_t>(result.shift_y));

    const int local_radius = std::max<int>(1, std::min<int>(AlignConstants::kDenseLocalRadius, params.search_distance / AlignConstants::kRefineLocalRadiusDiv));
    const int sample_step = 1;

    const uint32_t tx_count = result.tiles_x;
    const uint32_t ty_count = result.tiles_y;
    const uint32_t total_waves = tx_count + ty_count - 1;

    for (uint32_t wave = 0; wave < total_waves; ++wave)
    {
        // Tiles on this wavefront: tx + ty = wave, 0 <= tx < tx_count, 0 <= ty < ty_count.
        const uint32_t tx_begin = (wave >= ty_count) ? (wave - ty_count + 1) : 0;
        const uint32_t tx_end = std::min(wave + 1, tx_count);
        const uint32_t wave_tiles = tx_end - tx_begin;

        ParallelFor(wave_tiles, 1, [&](size_t i0, size_t i1)
        {
            for (size_t i = i0; i < i1; ++i)
            {
                const uint32_t tx = tx_begin + static_cast<uint32_t>(i);
                const uint32_t ty = wave - tx;
                const size_t idx = static_cast<size_t>(ty) * tx_count + tx;
                const uint32_t x0 = tx * tile_size;
                const uint32_t y0 = ty * tile_size;

                int seed_x = result.shift_x;
                int seed_y = result.shift_y;
                if (tx > 0)
                {
                    size_t left = idx - 1;
                    seed_x = (seed_x + result.tile_shift_x[left]) / 2;
                    seed_y = (seed_y + result.tile_shift_y[left]) / 2;
                }
                if (ty > 0)
                {
                    size_t top = idx - tx_count;
                    seed_x = (seed_x + result.tile_shift_x[top]) / 2;
                    seed_y = (seed_y + result.tile_shift_y[top]) / 2;
                }

#if BURSTMERGE_ALIGN_WEIGHTED_AVG
                double sum_w = 0.0, sum_wx = 0.0, sum_wy = 0.0;
                for (int dy = seed_y - local_radius; dy <= seed_y + local_radius; ++dy)
                {
                    for (int dx = seed_x - local_radius; dx <= seed_x + local_radius; ++dx)
                    {
                        int snapped_dx = SnapToPeriod(dx, result.cfa_period);
                        int snapped_dy = SnapToPeriod(dy, result.cfa_period);
                        float score = TileSad(reference, comparison, x0, y0, tile_size, tile_size,
                                              snapped_dx, snapped_dy, sample_step);
                        double w = 1.0 / (static_cast<double>(score) * static_cast<double>(score) + 1e-8);
                        sum_w += w;
                        sum_wx += w * snapped_dx;
                        sum_wy += w * snapped_dy;
                    }
                }
                result.tile_shift_x[idx] = static_cast<int16_t>(std::lround(sum_wx / sum_w));
                result.tile_shift_y[idx] = static_cast<int16_t>(std::lround(sum_wy / sum_w));
#else
                float best_score = std::numeric_limits<float>::max();
                int best_x = seed_x;
                int best_y = seed_y;
                for (int dy = seed_y - local_radius; dy <= seed_y + local_radius; ++dy)
                {
                    for (int dx = seed_x - local_radius; dx <= seed_x + local_radius; ++dx)
                    {
                        int snapped_dx = SnapToPeriod(dx, result.cfa_period);
                        int snapped_dy = SnapToPeriod(dy, result.cfa_period);
                        float score = TileSad(reference, comparison, x0, y0, tile_size, tile_size,
                                              snapped_dx, snapped_dy, sample_step);
                        if (score < best_score)
                        {
                            best_score = score;
                            best_x = snapped_dx;
                            best_y = snapped_dy;
                        }
                    }
                }
                result.tile_shift_x[idx] = static_cast<int16_t>(best_x);
                result.tile_shift_y[idx] = static_cast<int16_t>(best_y);
#endif
            }
        }, "refine_tile_wave" /* named tag for profiler */);
    }

    SmoothTileField(result, params.smooth_tile_field);
}

} // namespace burstmerge
