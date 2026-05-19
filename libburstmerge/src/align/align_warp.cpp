#include "burstmerge/internal/align/align.h"

#include "burstmerge/internal/align/align_common.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <cmath>

namespace burstmerge
{

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment)
{
    ProfileScope scope("time.align.warp_aligned");
    // Warping is separated from motion estimation so resampling behavior can
    // evolve independently from search strategy and tile-field estimation.
    if (alignment.tile_shift_x.empty() || alignment.tile_shift_y.empty() ||
        alignment.tiles_x == 0 || alignment.tiles_y == 0 || alignment.tile_size <= 0)
    {
        return WarpTranslate(source,
                             static_cast<float>(alignment.shift_x),
                             static_cast<float>(alignment.shift_y));
    }

    FloatImage out;
    out.width = source.width;
    out.height = source.height;
    out.channels = source.channels;
    out.data.resize(source.data.size(), 0.0f);

    ParallelForRows(source.height, RecommendedImageRowGrain(source.width, source.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < source.width; ++x)
            {
                float shift_x = InterpolateTileShift(alignment.tile_shift_x,
                                                     alignment.tiles_x,
                                                     alignment.tiles_y,
                                                     alignment.tile_size,
                                                     alignment.tile_spacing,
                                                     x,
                                                     y);
                float shift_y = InterpolateTileShift(alignment.tile_shift_y,
                                                     alignment.tiles_x,
                                                     alignment.tiles_y,
                                                     alignment.tile_size,
                                                     alignment.tile_spacing,
                                                     x,
                                                     y);

            // All current paths (mosaic + plane) use integer tile shifts, so
            // integer copy with CFA-period snapping is the correct warp.
            // (Bilinear branch removed — it was dead reachable only when the
            // condition `ch==1 || ch>1` was tautological.)
            int isx = SnapToPeriod(static_cast<int>(std::lround(shift_x)), alignment.cfa_period);
            int isy = SnapToPeriod(static_cast<int>(std::lround(shift_y)), alignment.cfa_period);
            int sx = ClampInt(static_cast<int>(x) - isx, 0, static_cast<int>(source.width) - 1);
            int sy = ClampInt(static_cast<int>(y) - isy, 0, static_cast<int>(source.height) - 1);
            for (uint32_t c = 0; c < source.channels; ++c)
            {
                out.At(x, y, c) = source.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), c);
            }
            }
        }
    });

    return out;
}

} // namespace burstmerge
