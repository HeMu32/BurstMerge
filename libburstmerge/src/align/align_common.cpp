#include "burstmerge/internal/align/align_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace burstmerge
{

// Shared alignment math and sampling helpers.

int SnapToPeriod(int value, uint32_t period)
{
    if (period <= 1) return value;
    int p = static_cast<int>(period);
    return value >= 0 ? (value / p) * p : -((-value) / p) * p;
}

float SparseSad(const FloatImage& a, const FloatImage& b, int dx, int dy, int step)
{
    const int margin_x = std::abs(dx);
    const int margin_y = std::abs(dy);
    if (a.width <= static_cast<uint32_t>(margin_x * 2) || a.height <= static_cast<uint32_t>(margin_y * 2))
    {
        return std::numeric_limits<float>::max();
    }

    double sad = 0.0;
    uint64_t count = 0;
    for (int y = margin_y; y < static_cast<int>(a.height) - margin_y; y += step)
    {
        for (int x = margin_x; x < static_cast<int>(a.width) - margin_x; x += step)
        {
            for (uint32_t c = 0; c < a.channels; ++c)
            {
                sad += std::abs(a.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) -
                                b.At(static_cast<uint32_t>(x - dx), static_cast<uint32_t>(y - dy), c));
                ++count;
            }
        }
    }
    return count ? static_cast<float>(sad / static_cast<double>(count)) : std::numeric_limits<float>::max();
}

float TileSad(const FloatImage& a,
              const FloatImage& b,
              uint32_t x0,
              uint32_t y0,
              uint32_t tile_w,
              uint32_t tile_h,
              int dx,
              int dy,
              int sample_step)
{
    const int ax0 = static_cast<int>(x0);
    const int ay0 = static_cast<int>(y0);
    const int ax1 = static_cast<int>(std::min<uint32_t>(a.width, x0 + tile_w));
    const int ay1 = static_cast<int>(std::min<uint32_t>(a.height, y0 + tile_h));

    double sad = 0.0;
    uint64_t count = 0;
    for (int y = ay0; y < ay1; y += sample_step)
    {
        int by = y - dy;
        if (by < 0 || by >= static_cast<int>(b.height)) continue;
        for (int x = ax0; x < ax1; x += sample_step)
        {
            int bx = x - dx;
            if (bx < 0 || bx >= static_cast<int>(b.width)) continue;
            for (uint32_t c = 0; c < a.channels; ++c)
            {
                sad += std::abs(a.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) -
                                b.At(static_cast<uint32_t>(bx), static_cast<uint32_t>(by), c));
                ++count;
            }
        }
    }

    return count ? static_cast<float>(sad / static_cast<double>(count)) : std::numeric_limits<float>::max();
}

float TileCost(const FloatImage& a,
               const FloatImage& b,
               uint32_t x0,
               uint32_t y0,
               uint32_t tile_w,
               uint32_t tile_h,
               int dx,
               int dy,
               int sample_step,
               bool ssd)
{
    const int ax0 = static_cast<int>(x0);
    const int ay0 = static_cast<int>(y0);
    const int ax1 = static_cast<int>(std::min<uint32_t>(a.width, x0 + tile_w));
    const int ay1 = static_cast<int>(std::min<uint32_t>(a.height, y0 + tile_h));
    double cost = 0.0;
    uint64_t count = 0;
    for (int y = ay0; y < ay1; y += sample_step)
    {
        int by = y - dy;
        for (int x = ax0; x < ax1; x += sample_step)
        {
            int bx = x - dx;
            if (bx < 0 || by < 0 || bx >= static_cast<int>(b.width) || by >= static_cast<int>(b.height)) continue;
            for (uint32_t c = 0; c < a.channels; ++c)
            {
                float d = std::abs(a.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) -
                                   b.At(static_cast<uint32_t>(bx), static_cast<uint32_t>(by), c));
                cost += ssd ? static_cast<double>(d) * d : d;
                ++count;
            }
        }
    }
    return count ? static_cast<float>(cost / static_cast<double>(count)) : std::numeric_limits<float>::max();
}

void SmoothTileField(AlignmentResult& result)
{
    if (result.tiles_x == 0 || result.tiles_y == 0) return;

    std::vector<int16_t> smoothed_x = result.tile_shift_x;
    std::vector<int16_t> smoothed_y = result.tile_shift_y;
    const int rad = AlignConstants::kSmoothNeighborRadius;

    for (uint32_t ty = 0; ty < result.tiles_y; ++ty)
    {
        for (uint32_t tx = 0; tx < result.tiles_x; ++tx)
        {
            int sx = 0;
            int sy = 0;
            int n = 0;
            for (int oy = -rad; oy <= rad; ++oy)
            {
                int ny = static_cast<int>(ty) + oy;
                if (ny < 0 || ny >= static_cast<int>(result.tiles_y)) continue;
                for (int ox = -1; ox <= 1; ++ox)
                {
                    int nx = static_cast<int>(tx) + ox;
                    if (nx < 0 || nx >= static_cast<int>(result.tiles_x)) continue;
                    size_t idx = static_cast<size_t>(ny) * result.tiles_x + static_cast<uint32_t>(nx);
                    sx += result.tile_shift_x[idx];
                    sy += result.tile_shift_y[idx];
                    ++n;
                }
            }
            size_t idx = static_cast<size_t>(ty) * result.tiles_x + tx;
            smoothed_x[idx] = static_cast<int16_t>(std::lround(static_cast<double>(sx) / std::max(1, n)));
            smoothed_y[idx] = static_cast<int16_t>(std::lround(static_cast<double>(sy) / std::max(1, n)));
        }
    }

    result.tile_shift_x.swap(smoothed_x);
    result.tile_shift_y.swap(smoothed_y);
}

float InterpolateTileShift(const std::vector<int16_t>& field,
                           uint32_t tiles_x,
                           uint32_t tiles_y,
                           int32_t tile_size,
                           int32_t tile_spacing,
                           uint32_t x,
                           uint32_t y)
{
    if (tiles_x == 0 || tiles_y == 0 || tile_size <= 0) return 0.0f;

    float spacing = static_cast<float>(tile_spacing > 0 ? tile_spacing : tile_size);
    float fx = (static_cast<float>(x) + 0.5f) / spacing - 1.0f;
    float fy = (static_cast<float>(y) + 0.5f) / spacing - 1.0f;

    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto sample = [&](int ix, int iy) -> float
    {
        ix = std::max(0, std::min(ix, static_cast<int>(tiles_x) - 1));
        iy = std::max(0, std::min(iy, static_cast<int>(tiles_y) - 1));
        size_t idx = static_cast<size_t>(iy) * tiles_x + static_cast<uint32_t>(ix);
        return static_cast<float>(field[idx]);
    };

    float v00 = sample(x0, y0);
    float v10 = sample(x0 + 1, y0);
    float v01 = sample(x0, y0 + 1);
    float v11 = sample(x0 + 1, y0 + 1);
    float a = v00 * (1.0f - tx) + v10 * tx;
    float b = v01 * (1.0f - tx) + v11 * tx;
    return a * (1.0f - ty) + b * ty;
}

int ClampInt(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

} // namespace burstmerge
