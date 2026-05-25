#include "burstmerge/internal/align/align_common.h"

#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    const uint32_t ch = a.channels;
    const uint32_t aw = a.width;
    const uint32_t bw = b.width;
    if (ch == 1)
    {
        for (int y = margin_y; y < static_cast<int>(a.height) - margin_y; y += step)
        {
            for (int x = margin_x; x < static_cast<int>(a.width) - margin_x; x += step)
            {
                const size_t a_idx = static_cast<size_t>(y) * aw + static_cast<uint32_t>(x);
                const size_t b_idx = static_cast<size_t>(static_cast<uint32_t>(y - dy)) * bw +
                    static_cast<uint32_t>(x - dx);
                sad += std::abs(a.data[a_idx] - b.data[b_idx]);
                ++count;
            }
        }
        return count ? static_cast<float>(sad / static_cast<double>(count))
                     : std::numeric_limits<float>::max();
    }
    for (int y = margin_y; y < static_cast<int>(a.height) - margin_y; y += step)
    {
        for (int x = margin_x; x < static_cast<int>(a.width) - margin_x; x += step)
        {
            const size_t a_base = (static_cast<size_t>(y) * aw + static_cast<uint32_t>(x)) * ch;
            const size_t b_base =
                (static_cast<size_t>(static_cast<uint32_t>(y - dy)) * bw +
                 static_cast<uint32_t>(x - dx)) * ch;
            for (uint32_t c = 0; c < ch; ++c)
            {
                sad += std::abs(a.data[a_base + c] - b.data[b_base + c]);
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
    return TileCost(a, b, x0, y0, tile_w, tile_h, dx, dy, sample_step, false);
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
    const uint32_t ch = a.channels;
    const uint32_t aw = a.width;
    const uint32_t bw = b.width;

    // Out-of-bounds penalty per pixel, matching the reference convention of
    // applying a large fixed cost instead of skipping boundary pixels.
    // This ensures that displacements which push most of the tile out of frame
    // incur a high cost and are not incorrectly selected as the best match.
    static const double kOBPenalty = 65504.0;
    static const double kOBPenaltySq = kOBPenalty * kOBPenalty;

    if (ch == 1)
    {
        for (int y = ay0; y < ay1; y += sample_step)
        {
            int by = y - dy;
            for (int x = ax0; x < ax1; x += sample_step)
            {
                int bx = x - dx;
                if (by < 0 || by >= static_cast<int>(b.height) ||
                    bx < 0 || bx >= static_cast<int>(b.width))
                {
                    cost += ssd ? kOBPenaltySq : kOBPenalty;
                    ++count;
                    continue;
                }
                const size_t a_idx = static_cast<size_t>(y) * aw + static_cast<uint32_t>(x);
                const size_t b_idx = static_cast<size_t>(static_cast<uint32_t>(by)) * bw +
                    static_cast<uint32_t>(bx);
                float d = std::abs(a.data[a_idx] - b.data[b_idx]);
                cost += ssd ? static_cast<double>(d) * d : d;
                ++count;
            }
        }
        // Return raw sum (not mean) so that tiles with many out-of-bounds
        // pixels receive correctly high costs.
        return count ? static_cast<float>(cost) : std::numeric_limits<float>::max();
    }
    for (int y = ay0; y < ay1; y += sample_step)
    {
        int by = y - dy;
        for (int x = ax0; x < ax1; x += sample_step)
        {
            int bx = x - dx;
            if (by < 0 || by >= static_cast<int>(b.height) ||
                bx < 0 || bx >= static_cast<int>(b.width))
            {
                cost += ssd ? kOBPenaltySq * ch : kOBPenalty * ch;
                count += ch;
                continue;
            }
            const size_t a_base = (static_cast<size_t>(y) * aw + static_cast<uint32_t>(x)) * ch;
            const size_t b_base = (static_cast<size_t>(static_cast<uint32_t>(by)) * bw +
                                   static_cast<uint32_t>(bx)) * ch;
            for (uint32_t c = 0; c < ch; ++c)
            {
                float d = std::abs(a.data[a_base + c] - b.data[b_base + c]);
                cost += ssd ? static_cast<double>(d) * d : d;
                ++count;
            }
        }
    }
    return count ? static_cast<float>(cost) : std::numeric_limits<float>::max();
}

void SmoothTileField(AlignmentResult& result, bool enabled)
{
    if (!enabled) return;
    if (result.tiles_x == 0 || result.tiles_y == 0) return;

    std::vector<int16_t> smoothed_x = result.tile_shift_x;
    std::vector<int16_t> smoothed_y = result.tile_shift_y;
    const int rad = AlignConstants::kSmoothNeighborRadius;

    for (uint32_t ty = 0; ty < result.tiles_y; ++ty)
    {
        for (uint32_t tx = 0; tx < result.tiles_x; ++tx)
        {
            int vals_x[9], vals_y[9];
            int n = 0;
            for (int oy = -rad; oy <= rad; ++oy)
            {
                int ny = static_cast<int>(ty) + oy;
                if (ny < 0 || ny >= static_cast<int>(result.tiles_y)) continue;
                const uint32_t row_base = static_cast<uint32_t>(ny) * result.tiles_x;
                for (int ox = -rad; ox <= rad; ++ox)
                {
                    int nx = static_cast<int>(tx) + ox;
                    if (nx < 0 || nx >= static_cast<int>(result.tiles_x)) continue;
                    size_t idx = row_base + static_cast<uint32_t>(nx);
                    vals_x[n] = result.tile_shift_x[idx];
                    vals_y[n] = result.tile_shift_y[idx];
                    ++n;
                }
            }
            auto cmp_int = [](const void* a, const void* b) -> int
            {
                return (*static_cast<const int*>(a) - *static_cast<const int*>(b));
            };
            std::qsort(vals_x, static_cast<size_t>(n), sizeof(int), cmp_int);
            std::qsort(vals_y, static_cast<size_t>(n), sizeof(int), cmp_int);
            size_t idx = static_cast<size_t>(ty) * result.tiles_x + tx;
            smoothed_x[idx] = static_cast<int16_t>(vals_x[n / 2]);
            smoothed_y[idx] = static_cast<int16_t>(vals_y[n / 2]);
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

void BinomialBlur5Tap(const FloatImage& src, FloatImage& dst, FloatImage& tmp)
{
    const uint32_t sw = src.width;
    const uint32_t sh = src.height;
    const uint32_t ch = src.channels;

    dst.width = sw;
    dst.height = sh;
    dst.channels = ch;
    dst.data.resize(src.data.size());

    tmp.width = sw;
    tmp.height = sh;
    tmp.channels = ch;
    tmp.data.assign(src.data.size(), 0.0f);

    // Horizontal pass: src → tmp
    ParallelForRows(sh, 1, [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < sw; ++x)
            {
                for (uint32_t c = 0; c < ch; ++c)
                {
                    double sum = 0.0;
                    for (int d = -2; d <= 2; ++d)
                    {
                        uint32_t sx = ClampInt(static_cast<int>(x) + d, 0, static_cast<int>(sw) - 1);
                        double k;
                        switch (std::abs(d))
                        {
                            case 0:  k = 6.0; break;
                            case 1:  k = 4.0; break;
                            default: k = 1.0; break;
                        }
                        sum += k * static_cast<double>(src.At(sx, y, c));
                    }
                    tmp.At(x, y, c) = static_cast<float>(sum / 16.0);
                }
            }
            }
        }, "align_blur_h" /* named tag for profiler */);

        // Vertical pass: tmp → dst
        ParallelForRows(sh, 1, [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < sw; ++x)
            {
                for (uint32_t c = 0; c < ch; ++c)
                {
                    double sum = 0.0;
                    for (int d = -2; d <= 2; ++d)
                    {
                        uint32_t sy = ClampInt(static_cast<int>(y) + d, 0, static_cast<int>(sh) - 1);
                        double k;
                        switch (std::abs(d))
                        {
                            case 0:  k = 6.0; break;
                            case 1:  k = 4.0; break;
                            default: k = 1.0; break;
                        }
                        sum += k * static_cast<double>(tmp.At(x, sy, c));
                    }
                    dst.At(x, y, c) = static_cast<float>(sum / 16.0);
                }
            }
        }
    }, "align_blur_v" /* named tag for profiler */);
}

FloatImage BinomialBlur5Tap(const FloatImage& src)
{
    FloatImage dst, tmp;
    BinomialBlur5Tap(src, dst, tmp);
    return dst;
}

} // namespace burstmerge
