#include "burstmerge/internal/align/align.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace burstmerge {
namespace {

int SnapToPeriod(int value, uint32_t period) {
    if (period <= 1) return value;
    int p = static_cast<int>(period);
    return value >= 0 ? (value / p) * p : -((-value) / p) * p;
}

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
            for (uint32_t c = 0; c < a.channels; ++c) {
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
    for (int y = ay0; y < ay1; y += sample_step) {
        int by = y - dy;
        if (by < 0 || by >= static_cast<int>(b.height)) continue;
        for (int x = ax0; x < ax1; x += sample_step) {
            int bx = x - dx;
            if (bx < 0 || bx >= static_cast<int>(b.width)) continue;
            for (uint32_t c = 0; c < a.channels; ++c) {
                sad += std::abs(a.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) -
                                b.At(static_cast<uint32_t>(bx), static_cast<uint32_t>(by), c));
                ++count;
            }
        }
    }

    return count ? static_cast<float>(sad / static_cast<double>(count)) : std::numeric_limits<float>::max();
}

void SmoothTileField(AlignmentResult& result) {
    if (result.tiles_x == 0 || result.tiles_y == 0) return;

    std::vector<int16_t> smoothed_x = result.tile_shift_x;
    std::vector<int16_t> smoothed_y = result.tile_shift_y;

    for (uint32_t ty = 0; ty < result.tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < result.tiles_x; ++tx) {
            int sx = 0;
            int sy = 0;
            int n = 0;
            for (int oy = -1; oy <= 1; ++oy) {
                int ny = static_cast<int>(ty) + oy;
                if (ny < 0 || ny >= static_cast<int>(result.tiles_y)) continue;
                for (int ox = -1; ox <= 1; ++ox) {
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

void RefineTileField(const FloatImage& reference,
                     const FloatImage& comparison,
                     const AlignParams& params,
                     AlignmentResult& result)
{
    const uint32_t tile_size = static_cast<uint32_t>(std::max(8, params.tile_size));
    result.tile_size = static_cast<int32_t>(tile_size);
    result.cfa_period = std::max<uint32_t>(1, params.cfa_period);
    result.tiles_x = (reference.width + tile_size - 1) / tile_size;
    result.tiles_y = (reference.height + tile_size - 1) / tile_size;
    result.tile_shift_x.assign(static_cast<size_t>(result.tiles_x) * result.tiles_y, static_cast<int16_t>(result.shift_x));
    result.tile_shift_y.assign(static_cast<size_t>(result.tiles_x) * result.tiles_y, static_cast<int16_t>(result.shift_y));

    const int local_radius = std::max<int>(1, std::min<int>(4, params.search_distance / 16));
    const int sample_step = std::max<int>(1, params.cfa_period);

    for (uint32_t ty = 0; ty < result.tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < result.tiles_x; ++tx) {
            size_t idx = static_cast<size_t>(ty) * result.tiles_x + tx;
            const uint32_t x0 = tx * tile_size;
            const uint32_t y0 = ty * tile_size;

            int seed_x = result.shift_x;
            int seed_y = result.shift_y;
            if (tx > 0) {
                size_t left = idx - 1;
                seed_x = (seed_x + result.tile_shift_x[left]) / 2;
                seed_y = (seed_y + result.tile_shift_y[left]) / 2;
            }
            if (ty > 0) {
                size_t top = idx - result.tiles_x;
                seed_x = (seed_x + result.tile_shift_x[top]) / 2;
                seed_y = (seed_y + result.tile_shift_y[top]) / 2;
            }

            float best_score = std::numeric_limits<float>::max();
            int best_x = seed_x;
            int best_y = seed_y;
            for (int dy = seed_y - local_radius; dy <= seed_y + local_radius; ++dy) {
                for (int dx = seed_x - local_radius; dx <= seed_x + local_radius; ++dx) {
                    int snapped_dx = SnapToPeriod(dx, result.cfa_period);
                    int snapped_dy = SnapToPeriod(dy, result.cfa_period);
                    float score = TileSad(reference,
                                          comparison,
                                          x0,
                                          y0,
                                          tile_size,
                                          tile_size,
                                          snapped_dx,
                                          snapped_dy,
                                          sample_step);
                    if (score < best_score) {
                        best_score = score;
                        best_x = snapped_dx;
                        best_y = snapped_dy;
                    }
                }
            }

            result.tile_shift_x[idx] = static_cast<int16_t>(best_x);
            result.tile_shift_y[idx] = static_cast<int16_t>(best_y);
        }
    }

    SmoothTileField(result);
}

float InterpolateTileShift(const std::vector<int16_t>& field,
                           uint32_t tiles_x,
                           uint32_t tiles_y,
                           int32_t tile_size,
                           uint32_t x,
                           uint32_t y)
{
    if (tiles_x == 0 || tiles_y == 0 || tile_size <= 0) return 0.0f;

    float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(tile_size) - 0.5f;
    float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(tile_size) - 0.5f;

    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto sample = [&](int ix, int iy) -> float {
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

int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(v, hi));
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
    out.shift_x = SnapToPeriod(best_x, params.cfa_period);
    out.shift_y = SnapToPeriod(best_y, params.cfa_period);
    out.confidence = 1.0f / (1.0f + best_score);
    RefineTileField(reference, comparison, params, out);
    return out;
}

FloatImage WarpAligned(const FloatImage& source, const AlignmentResult& alignment) {
    if (alignment.tile_shift_x.empty() || alignment.tile_shift_y.empty() ||
        alignment.tiles_x == 0 || alignment.tiles_y == 0 || alignment.tile_size <= 0) {
        return WarpTranslateBilinear(source,
                                     static_cast<float>(alignment.shift_x),
                                     static_cast<float>(alignment.shift_y));
    }

    FloatImage out;
    out.width = source.width;
    out.height = source.height;
    out.channels = source.channels;
    out.data.resize(source.data.size(), 0.0f);

    for (uint32_t y = 0; y < source.height; ++y) {
        for (uint32_t x = 0; x < source.width; ++x) {
            float shift_x = InterpolateTileShift(alignment.tile_shift_x,
                                                 alignment.tiles_x,
                                                 alignment.tiles_y,
                                                 alignment.tile_size,
                                                 x,
                                                 y);
            float shift_y = InterpolateTileShift(alignment.tile_shift_y,
                                                 alignment.tiles_x,
                                                 alignment.tiles_y,
                                                 alignment.tile_size,
                                                 x,
                                                 y);

            // RAW mosaic: snap interpolated shift to CFA period then do integer copy.
            // This preserves Bayer phase perfectly while still leveraging the tile field.
            if (source.channels == 1 || alignment.cfa_period > 1) {
                int isx = SnapToPeriod(static_cast<int>(std::lround(shift_x)), alignment.cfa_period);
                int isy = SnapToPeriod(static_cast<int>(std::lround(shift_y)), alignment.cfa_period);
                int sx = ClampInt(static_cast<int>(x) - isx, 0, static_cast<int>(source.width) - 1);
                int sy = ClampInt(static_cast<int>(y) - isy, 0, static_cast<int>(source.height) - 1);
                for (uint32_t c = 0; c < source.channels; ++c) {
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

            for (uint32_t c = 0; c < source.channels; ++c) {
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
