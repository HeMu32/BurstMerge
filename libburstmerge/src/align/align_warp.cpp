#include "burstmerge/internal/align/align.h"

#include "burstmerge/internal/align/align_common.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <cmath>

namespace burstmerge
{

namespace
{

struct WarpCell
{
    // Integer tile shifts sampled at the four corners of the current grid cell.
    int dx[4];
    int dy[4];
    // Bilinear weights for the current pixel within that cell.
    float w[4];
    // True when all four shifted source footprints stay in-bounds for the
    // whole cell, allowing us to skip per-sample clamp checks.
    bool interior = false;
};

int SampleShift(const std::vector<int16_t>& field,
                const AlignmentResult& alignment,
                int ix,
                int iy)
{
    ix = std::max(0, std::min(ix, static_cast<int>(alignment.tiles_x) - 1));
    iy = std::max(0, std::min(iy, static_cast<int>(alignment.tiles_y) - 1));
    const size_t idx = static_cast<size_t>(iy) * alignment.tiles_x + static_cast<uint32_t>(ix);
    return SnapToPeriod(static_cast<int>(field[idx]), alignment.cfa_period);
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

    const int spacing_i = alignment.tile_spacing > 0 ? alignment.tile_spacing : alignment.tile_size;
    const float inv_spacing = 1.0f / static_cast<float>(spacing_i);
    const int cell_rows = static_cast<int>(alignment.tiles_y) + 1;

    // Process the warp by tile-grid cells instead of by individual pixels.
    // Within one cell, the four corner tile shifts are constant, so we can
    // cache them once and reuse them for all pixels inside the cell.
    ParallelForRows(static_cast<uint32_t>(cell_rows), 1, [&](uint32_t row_begin, uint32_t row_end)
    {
        for (uint32_t gyu = row_begin; gyu < row_end; ++gyu)
        {
            const int gy = static_cast<int>(gyu) - 1;
            const int y_start = std::max(0, (gy + 1) * spacing_i);
            const int y_end = std::min(static_cast<int>(source.height), (gy + 2) * spacing_i);
            if (y_start >= y_end) continue;

            for (int gx = -1; gx < static_cast<int>(alignment.tiles_x); ++gx)
            {
                const int x_start = std::max(0, (gx + 1) * spacing_i);
                const int x_end = std::min(static_cast<int>(source.width), (gx + 2) * spacing_i);
                if (x_start >= x_end) continue;

                WarpCell cell{};
                cell.dx[0] = SampleShift(alignment.tile_shift_x, alignment, gx,     gy);
                cell.dx[1] = SampleShift(alignment.tile_shift_x, alignment, gx + 1, gy);
                cell.dx[2] = SampleShift(alignment.tile_shift_x, alignment, gx,     gy + 1);
                cell.dx[3] = SampleShift(alignment.tile_shift_x, alignment, gx + 1, gy + 1);
                cell.dy[0] = SampleShift(alignment.tile_shift_y, alignment, gx,     gy);
                cell.dy[1] = SampleShift(alignment.tile_shift_y, alignment, gx + 1, gy);
                cell.dy[2] = SampleShift(alignment.tile_shift_y, alignment, gx,     gy + 1);
                cell.dy[3] = SampleShift(alignment.tile_shift_y, alignment, gx + 1, gy + 1);

                // Most cells are well away from the image border. If every
                // corner-shifted source coordinate remains valid for every pixel
                // in the cell, we can use a fast unclamped path.
                cell.interior = true;
                for (int i = 0; i < 4; ++i)
                {
                    cell.interior = cell.interior &&
                        (x_start - cell.dx[i] >= 0) && ((x_end - 1) - cell.dx[i] < static_cast<int>(source.width)) &&
                        (y_start - cell.dy[i] >= 0) && ((y_end - 1) - cell.dy[i] < static_cast<int>(source.height));
                }

                const float tx0 = (static_cast<float>(x_start) + 0.5f - static_cast<float>((gx + 1) * spacing_i)) * inv_spacing;
                const float ty0 = (static_cast<float>(y_start) + 0.5f - static_cast<float>((gy + 1) * spacing_i)) * inv_spacing;

                for (int y = y_start; y < y_end; ++y)
                {
                    const float ty = ty0 + static_cast<float>(y - y_start) * inv_spacing;
                    for (int x = x_start; x < x_end; ++x)
                    {
                        // This mirrors the reference implementation's idea:
                        // do not interpolate the displacement vector and then
                        // round it. Instead, sample the source at the four
                        // neighboring tile shifts and blend the sample values.
                        const float tx = tx0 + static_cast<float>(x - x_start) * inv_spacing;
                        cell.w[0] = (1.0f - tx) * (1.0f - ty);
                        cell.w[1] = tx * (1.0f - ty);
                        cell.w[2] = (1.0f - tx) * ty;
                        cell.w[3] = tx * ty;

                        if (cell.interior)
                        {
                            // Fast path: reuse prevalidated offsets and write
                            // directly through linear indices.
                            const size_t sidx0 = (static_cast<size_t>(y - cell.dy[0]) * source.width + static_cast<uint32_t>(x - cell.dx[0])) * source.channels;
                            const size_t sidx1 = (static_cast<size_t>(y - cell.dy[1]) * source.width + static_cast<uint32_t>(x - cell.dx[1])) * source.channels;
                            const size_t sidx2 = (static_cast<size_t>(y - cell.dy[2]) * source.width + static_cast<uint32_t>(x - cell.dx[2])) * source.channels;
                            const size_t sidx3 = (static_cast<size_t>(y - cell.dy[3]) * source.width + static_cast<uint32_t>(x - cell.dx[3])) * source.channels;
                            const size_t didx = (static_cast<size_t>(y) * out.width + static_cast<uint32_t>(x)) * out.channels;
                            for (uint32_t c = 0; c < source.channels; ++c)
                            {
                                out.data[didx + c] =
                                    cell.w[0] * source.data[sidx0 + c] +
                                    cell.w[1] * source.data[sidx1 + c] +
                                    cell.w[2] * source.data[sidx2 + c] +
                                    cell.w[3] * source.data[sidx3 + c];
                            }
                        }
                        else
                        {
                            // Border-safe path: keep the same blending rule but
                            // clamp each shifted footprint independently.
                            const int sx0 = ClampInt(x - cell.dx[0], 0, static_cast<int>(source.width) - 1);
                            const int sx1 = ClampInt(x - cell.dx[1], 0, static_cast<int>(source.width) - 1);
                            const int sx2 = ClampInt(x - cell.dx[2], 0, static_cast<int>(source.width) - 1);
                            const int sx3 = ClampInt(x - cell.dx[3], 0, static_cast<int>(source.width) - 1);
                            const int sy0 = ClampInt(y - cell.dy[0], 0, static_cast<int>(source.height) - 1);
                            const int sy1 = ClampInt(y - cell.dy[1], 0, static_cast<int>(source.height) - 1);
                            const int sy2 = ClampInt(y - cell.dy[2], 0, static_cast<int>(source.height) - 1);
                            const int sy3 = ClampInt(y - cell.dy[3], 0, static_cast<int>(source.height) - 1);
                            for (uint32_t c = 0; c < source.channels; ++c)
                            {
                                out.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) =
                                    cell.w[0] * source.At(static_cast<uint32_t>(sx0), static_cast<uint32_t>(sy0), c) +
                                    cell.w[1] * source.At(static_cast<uint32_t>(sx1), static_cast<uint32_t>(sy1), c) +
                                    cell.w[2] * source.At(static_cast<uint32_t>(sx2), static_cast<uint32_t>(sy2), c) +
                                    cell.w[3] * source.At(static_cast<uint32_t>(sx3), static_cast<uint32_t>(sy3), c);
                            }
                        }
                    }
                }
            }
        }
    }, "warp_aligned" /* named tag for profiler */);

    return out;
}

} // namespace burstmerge
