#include "burstmerge/internal/align/align.h"

#include "burstmerge/internal/align/align_common.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <cmath>

namespace burstmerge
{

namespace
{

float SampleWarpedChannel(const FloatImage& source,
                         const AlignmentResult& alignment,
                         uint32_t x,
                         uint32_t y,
                         uint32_t c)
{
    const float spacing = static_cast<float>(alignment.tile_spacing > 0 ? alignment.tile_spacing : alignment.tile_size);
    const float fx = (static_cast<float>(x) + 0.5f) / spacing - 1.0f;
    const float fy = (static_cast<float>(y) + 0.5f) / spacing - 1.0f;

    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    auto sample_shift = [&](const std::vector<int16_t>& field, int ix, int iy) -> int
    {
        ix = std::max(0, std::min(ix, static_cast<int>(alignment.tiles_x) - 1));
        iy = std::max(0, std::min(iy, static_cast<int>(alignment.tiles_y) - 1));
        const size_t idx = static_cast<size_t>(iy) * alignment.tiles_x + static_cast<uint32_t>(ix);
        return SnapToPeriod(static_cast<int>(field[idx]), alignment.cfa_period);
    };

    auto sample_value = [&](int sx_shift, int sy_shift) -> float
    {
        const int sx = ClampInt(static_cast<int>(x) - sx_shift, 0, static_cast<int>(source.width) - 1);
        const int sy = ClampInt(static_cast<int>(y) - sy_shift, 0, static_cast<int>(source.height) - 1);
        return source.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), c);
    };

    const int dx00 = sample_shift(alignment.tile_shift_x, x0, y0);
    const int dx10 = sample_shift(alignment.tile_shift_x, x0 + 1, y0);
    const int dx01 = sample_shift(alignment.tile_shift_x, x0, y0 + 1);
    const int dx11 = sample_shift(alignment.tile_shift_x, x0 + 1, y0 + 1);
    const int dy00 = sample_shift(alignment.tile_shift_y, x0, y0);
    const int dy10 = sample_shift(alignment.tile_shift_y, x0 + 1, y0);
    const int dy01 = sample_shift(alignment.tile_shift_y, x0, y0 + 1);
    const int dy11 = sample_shift(alignment.tile_shift_y, x0 + 1, y0 + 1);

    const float w00 = (1.0f - tx) * (1.0f - ty);
    const float w10 = tx * (1.0f - ty);
    const float w01 = (1.0f - tx) * ty;
    const float w11 = tx * ty;

    return w00 * sample_value(dx00, dy00) +
           w10 * sample_value(dx10, dy10) +
           w01 * sample_value(dx01, dy01) +
           w11 * sample_value(dx11, dy11);
}

} // namespace

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
                for (uint32_t c = 0; c < source.channels; ++c)
                {
                    out.At(x, y, c) = SampleWarpedChannel(source, alignment, x, y, c);
                }
            }
        }
    });

    return out;
}

} // namespace burstmerge
