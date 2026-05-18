#include "burstmerge/internal/align/align.h"

#include "burstmerge/internal/align/align_common.h"

#include <cmath>

namespace burstmerge
{

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment)
{
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

    for (uint32_t y = 0; y < source.height; ++y)
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

            if (source.channels == 1 || alignment.cfa_period > 1)
            {
                int isx = SnapToPeriod(static_cast<int>(std::lround(shift_x)), alignment.cfa_period);
                int isy = SnapToPeriod(static_cast<int>(std::lround(shift_y)), alignment.cfa_period);
                int sx = ClampInt(static_cast<int>(x) - isx, 0, static_cast<int>(source.width) - 1);
                int sy = ClampInt(static_cast<int>(y) - isy, 0, static_cast<int>(source.height) - 1);
                for (uint32_t c = 0; c < source.channels; ++c)
                {
                    out.At(x, y, c) = source.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), c);
                }
                continue;
            }

            float sx = static_cast<float>(x) - shift_x;
            float sy = static_cast<float>(y) - shift_y;
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            float tx = sx - static_cast<float>(x0);
            float ty = sy - static_cast<float>(y0);

            for (uint32_t c = 0; c < source.channels; ++c)
            {
                float p00 = source.At(static_cast<uint32_t>(ClampInt(x0, 0, static_cast<int>(source.width) - 1)),
                                      static_cast<uint32_t>(ClampInt(y0, 0, static_cast<int>(source.height) - 1)), c);
                float p10 = source.At(static_cast<uint32_t>(ClampInt(x1, 0, static_cast<int>(source.width) - 1)),
                                      static_cast<uint32_t>(ClampInt(y0, 0, static_cast<int>(source.height) - 1)), c);
                float p01 = source.At(static_cast<uint32_t>(ClampInt(x0, 0, static_cast<int>(source.width) - 1)),
                                      static_cast<uint32_t>(ClampInt(y1, 0, static_cast<int>(source.height) - 1)), c);
                float p11 = source.At(static_cast<uint32_t>(ClampInt(x1, 0, static_cast<int>(source.width) - 1)),
                                      static_cast<uint32_t>(ClampInt(y1, 0, static_cast<int>(source.height) - 1)), c);
                float a = p00 * (1.0f - tx) + p10 * tx;
                float b = p01 * (1.0f - tx) + p11 * tx;
                out.At(x, y, c) = a * (1.0f - ty) + b * ty;
            }
        }
    }

    return out;
}

} // namespace burstmerge
