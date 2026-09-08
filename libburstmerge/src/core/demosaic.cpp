#include "burstmerge/internal/core/demosaic.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace burstmerge
{
namespace
{

struct DemosaicContext
{
    const FloatImage& planes;
    const RawMetadata& metadata;
    float black_delta[4];
};

int ClampCoord(int value, int upper)
{
    return std::max(0, std::min(value, upper - 1));
}

uint16_t ColorAt(const RawMetadata& metadata, int x, int y)
{
    return metadata.mosaic_pattern[static_cast<size_t>((y & 1) * 2 + (x & 1))];
}

float MeanBlack(const RawMetadata& metadata)
{
    float sum = 0.0f;
    int count = 0;
    for (float value : metadata.black_level)
    {
        if (value > 0.0f)
        {
            sum += value;
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

float RawAt(const DemosaicContext& context, int x, int y)
{
    const FloatImage& planes = context.planes;
    const RawMetadata& metadata = context.metadata;
    x = ClampCoord(x, static_cast<int>(metadata.width));
    y = ClampCoord(y, static_cast<int>(metadata.height));
    uint32_t channel = static_cast<uint32_t>((y & 1) * 2 + (x & 1));
    uint32_t px = std::min<uint32_t>(static_cast<uint32_t>(x / 2), planes.width - 1);
    uint32_t py = std::min<uint32_t>(static_cast<uint32_t>(y / 2), planes.height - 1);
    return planes.At(px, py, channel) - context.black_delta[channel];
}

float NearestColor(const DemosaicContext& context,
                   int x,
                   int y,
                   uint16_t color)
{
    float best = RawAt(context, x, y);
    int best_distance = std::numeric_limits<int>::max();
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int sx = ClampCoord(x + dx, static_cast<int>(context.metadata.width));
            int sy = ClampCoord(y + dy, static_cast<int>(context.metadata.height));
            if (ColorAt(context.metadata, sx, sy) != color) continue;
            int distance = std::abs(dx) + std::abs(dy);
            if (distance < best_distance)
            {
                best_distance = distance;
                best = RawAt(context, sx, sy);
            }
        }
    }
    return best;
}

// Gradient-corrected green (Hamilton-Adams): interpolate green along the axis
// with the smaller gradient, using the center red/blue second-difference as a
// correction term. This avoids the classic green zipper / checkerboard that
// plain averaging produces at high-contrast edges.
float InterpolateGreen(const DemosaicContext& context,
                       int x,
                       int y)
{
    const float c = RawAt(context, x, y);
    const float gn = RawAt(context, x, y - 1);
    const float gs = RawAt(context, x, y + 1);
    const float gw = RawAt(context, x - 1, y);
    const float ge = RawAt(context, x + 1, y);
    const float cn = RawAt(context, x, y - 2);
    const float cs = RawAt(context, x, y + 2);
    const float cw = RawAt(context, x - 2, y);
    const float ce = RawAt(context, x + 2, y);
    const float dh = std::fabs(gw - ge) + std::fabs(2.0f * c - cw - ce);
    const float dv = std::fabs(gn - gs) + std::fabs(2.0f * c - cn - cs);
    float g;
    if (dh < dv)
        g = (gw + ge) * 0.5f + (2.0f * c - cw - ce) * 0.25f;
    else if (dv < dh)
        g = (gn + gs) * 0.5f + (2.0f * c - cn - cs) * 0.25f;
    else
        g = (gw + ge + gn + gs) * 0.25f +
            (4.0f * c - cw - ce - cn - cs) * 0.125f;
    // The H-A second-difference correction can overshoot the local green range
    // at high-contrast edges, producing a green halo/fringe. Clamp the result
    // to the range of the immediate green neighbours to suppress that overshoot.
    const float gmin = std::min(gn, std::min(gs, std::min(gw, ge)));
    const float gmax = std::max(gn, std::max(gs, std::max(gw, ge)));
    if (g < gmin) g = gmin;
    if (g > gmax) g = gmax;
    return g;
}

// Color-difference (chroma) interpolation for red/blue at green sites. The
// color difference R-G / B-G is low-frequency, so averaging it along the axis
// of same-color neighbours and adding back the (interpolated) green is far
// less prone to edge artifacts than interpolating the channel directly.
float InterpolateChroma(const DemosaicContext& context,
                        const FloatImage& green,
                        int x,
                        int y,
                        uint16_t color)
{
    auto is_color = [&](int sx, int sy) -> bool
    {
        sx = ClampCoord(sx, static_cast<int>(context.metadata.width));
        sy = ClampCoord(sy, static_cast<int>(context.metadata.height));
        return ColorAt(context.metadata, sx, sy) == color;
    };
    auto add_diff = [&](int sx, int sy, float& sum, int& count) -> void
    {
        sx = ClampCoord(sx, static_cast<int>(context.metadata.width));
        sy = ClampCoord(sy, static_cast<int>(context.metadata.height));
        if (ColorAt(context.metadata, sx, sy) == color)
        {
            sum += RawAt(context, sx, sy) - green.At(static_cast<uint32_t>(sx),
                                                     static_cast<uint32_t>(sy), 0);
            ++count;
        }
    };

    float sum = 0.0f;
    int count = 0;
    if (is_color(x - 1, y) || is_color(x + 1, y))
    {
        add_diff(x - 1, y, sum, count);
        add_diff(x + 1, y, sum, count);
    }
    else if (is_color(x, y - 1) || is_color(x, y + 1))
    {
        add_diff(x, y - 1, sum, count);
        add_diff(x, y + 1, sum, count);
    }
    else
    {
        for (int dy = -1; dy <= 1; dy += 2)
            for (int dx = -1; dx <= 1; dx += 2)
                add_diff(x + dx, y + dy, sum, count);
    }
    return green.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0) +
           (count > 0 ? sum / static_cast<float>(count) : 0.0f);
}

float BilinearColor(const DemosaicContext& context,
                    const FloatImage& green,
                    int x,
                    int y,
                    uint16_t color)
{
    if (ColorAt(context.metadata, x, y) == color) return RawAt(context, x, y);
    if (color == 1) return InterpolateGreen(context, x, y);
    return InterpolateChroma(context, green, x, y, color);
}

float MalvarColor(const DemosaicContext& context,
                  const FloatImage& green,
                  int x,
                  int y,
                  uint16_t color)
{
    if (ColorAt(context.metadata, x, y) == color) return RawAt(context, x, y);
    if (color == 1) return InterpolateGreen(context, x, y);
    // Malvar's own red/blue filters read across hard edges and reintroduce the
    // zipper (e.g. the 0.5*(N2+S2) green term at a horizontal edge). The
    // color-difference form is edge-safe and keeps flat/neutral colors exact.
    return InterpolateChroma(context, green, x, y, color);
}

} // namespace

FloatImage DemosaicBayer(const FloatImage& planes,
                          const RawMetadata& metadata,
                          PreprocessInterpolation method,
                          float exposure_scale)
{
    ProfileScope scope("time.pipeline.demosaic");
    if (method == PreprocessInterpolation::Off) return planes;
    if (metadata.mosaic_pattern_width != 2 || planes.channels != 4 ||
        planes.width == 0 || planes.height == 0)
    {
        throw std::runtime_error("Pre-processing interpolation requires a 2x2 Bayer DNG input");
    }

    FloatImage output;
    output.width = metadata.width;
    output.height = metadata.height;
    output.channels = 3;
    output.data.resize(static_cast<size_t>(output.width) * output.height * 3);

    const float mean_black = MeanBlack(metadata);
    DemosaicContext context{planes, metadata, {}};
    for (size_t channel = 0; channel < 4; ++channel)
    {
        const float channel_black = metadata.black_level[channel] > 0.0f
            ? metadata.black_level[channel] : mean_black;
        context.black_delta[channel] = (channel_black - mean_black) * exposure_scale;
    }

    const bool use_nearest = (method == PreprocessInterpolation::Nearest);

    // Pass 1: interpolate the green channel (gradient-corrected Hamilton-Adams
    // unless nearest is requested). Green is needed by the chroma pass below.
    FloatImage green;
    green.width = output.width;
    green.height = output.height;
    green.channels = 1;
    green.data.resize(static_cast<size_t>(output.width) * output.height);
    ParallelForRows(output.height,
        RecommendedImageRowGrain(output.width, 1, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < output.width; ++x)
            {
                int ix = static_cast<int>(x), iy = static_cast<int>(y);
                float value;
                if (ColorAt(metadata, ix, iy) == 1)
                    value = RawAt(context, ix, iy);
                else if (use_nearest)
                    value = NearestColor(context, ix, iy, 1);
                else
                    value = InterpolateGreen(context, ix, iy);
                green.At(x, y, 0) = std::max(0.0f, value);
            }
        }
    }, "demosaic_green");

    // Pass 2: chroma channels. Bilinear uses color differences against the
    // interpolated green; Malvar keeps its R/B filters. Nearest stays simple.
    ParallelForRows(output.height,
        RecommendedImageRowGrain(output.width, 3, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < output.width; ++x)
            {
                int ix = static_cast<int>(x), iy = static_cast<int>(y);
                output.At(x, y, 1) = green.At(x, y, 0);
                for (uint16_t color = 0; color < 3; color += 2)
                {
                    float value;
                    if (use_nearest)
                    {
                        value = NearestColor(context, ix, iy, color);
                    }
                    else if (method == PreprocessInterpolation::Bilinear)
                    {
                        value = BilinearColor(context, green, ix, iy, color);
                    }
                    else
                    {
                        value = MalvarColor(context, green, ix, iy, color);
                    }
                    output.At(x, y, color) = std::max(0.0f, value);
                }
            }
        }
    }, "demosaic_chroma");
    return output;
}

} // namespace burstmerge
