#include "burstmerge/internal/core/float_image.h"

#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace burstmerge
{
namespace
{

float SampleClamped(const FloatImage& src, int x, int y, uint32_t c)
{
    x = std::max(0, std::min(x, static_cast<int>(src.width) - 1));
    y = std::max(0, std::min(y, static_cast<int>(src.height) - 1));
    return src.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c);
}

} // namespace

uint32_t ChannelsForFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::RGBA32_Float: return 4;
        case PixelFormat::R16_Uint_RGB: return 3;
        default: return 1;
    }
}

FloatImage HostBufferToFloatImage(const HostBuffer& src, float scale)
{
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = ChannelsForFormat(src.format);
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    if (!src.data) return out;

    const size_t count = static_cast<size_t>(out.width) * out.height * out.channels;
    switch (src.format)
    {
        case PixelFormat::R8_Uint:
        {
            const auto* p = reinterpret_cast<const uint8_t*>(src.data);
            ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
            {
                for (size_t i = i0; i < i1; ++i) out.data[i] = static_cast<float>(p[i]) * scale;
            }, "format_R8" /* named tag for profiler */);
            break;
        }
        case PixelFormat::R16_Uint:
        case PixelFormat::R16_Uint_RGB:
        {
            const auto* p = reinterpret_cast<const uint16_t*>(src.data);
            ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
            {
                for (size_t i = i0; i < i1; ++i) out.data[i] = static_cast<float>(p[i]) * scale;
            }, "format_R16" /* named tag for profiler */);
            break;
        }
        case PixelFormat::R32_Float:
        case PixelFormat::RGBA32_Float:
        {
            const auto* p = reinterpret_cast<const float*>(src.data);
            ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
            {
                for (size_t i = i0; i < i1; ++i) out.data[i] = p[i] * scale;
            }, "format_R32" /* named tag for profiler */);
            break;
        }
    }
    return out;
}

HostBuffer FloatImageToUint16HostBuffer(const FloatImage& src, uint32_t white_level)
{
    // DNG uses a 16-bit integer container for 12/14/16-bit outputs. The actual
    // target precision is expressed by white_level, and the float pipeline is
    // expected to have already rescaled samples into that target range.
    // Supports both single-channel (Bayer mosaic) and 3-channel (LinearRaw RGB)
    // images; the channel count is carried through to the HostBuffer format.
    HostBuffer out;
    out.width = src.width;
    out.height = src.height;
    out.format = (src.channels == 3) ? PixelFormat::R16_Uint_RGB : PixelFormat::R16_Uint;
    out.row_stride = src.width * src.channels * sizeof(uint16_t);
    out.size = static_cast<size_t>(out.row_stride) * src.height;
    out.data = new std::byte[out.size]();

    auto* dst = reinterpret_cast<uint16_t*>(out.data);
    const size_t count = src.data.size();
    const float hi = static_cast<float>(white_level);
    ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i)
        {
            float v = std::max(0.0f, std::min(src.data[i], hi));
            dst[i] = static_cast<uint16_t>(std::lround(v));
        }
    }, "convert_uint16" /* named tag for profiler */);
    return out;
}

FloatImage Downsample2x(const FloatImage& src)
{
    FloatImage out;
    out.width = std::max<uint32_t>(1, src.width / 2);
    out.height = std::max<uint32_t>(1, src.height / 2);
    out.channels = src.channels;
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < out.width; ++x)
            {
                for (uint32_t c = 0; c < out.channels; ++c)
                {
                    uint32_t sx = x * 2;
                    uint32_t sy = y * 2;
                    float sum = 0.0f;
                    int n = 0;
                    for (uint32_t dy = 0; dy < 2 && sy + dy < src.height; ++dy)
                    {
                        for (uint32_t dx = 0; dx < 2 && sx + dx < src.width; ++dx)
                        {
                            sum += src.At(sx + dx, sy + dy, c);
                            ++n;
                        }
                    }
                    out.At(x, y, c) = n > 0 ? sum / static_cast<float>(n) : 0.0f;
                }
            }
        }
    }, "downsample2x" /* named tag for profiler */);
    return out;
}

void Downsample2x(const FloatImage& src, FloatImage& dst)
{
    dst.width = std::max<uint32_t>(1, src.width / 2);
    dst.height = std::max<uint32_t>(1, src.height / 2);
    dst.channels = src.channels;
    dst.data.resize(static_cast<size_t>(dst.width) * dst.height * dst.channels, 0.0f);

    ParallelForRows(dst.height, RecommendedImageRowGrain(dst.width, dst.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < dst.width; ++x)
            {
                for (uint32_t c = 0; c < dst.channels; ++c)
                {
                    uint32_t sx = x * 2;
                    uint32_t sy = y * 2;
                    float sum = 0.0f;
                    int n = 0;
                    for (uint32_t dy = 0; dy < 2 && sy + dy < src.height; ++dy)
                    {
                        for (uint32_t dx = 0; dx < 2 && sx + dx < src.width; ++dx)
                        {
                            sum += src.At(sx + dx, sy + dy, c);
                            ++n;
                        }
                    }
                    dst.At(x, y, c) = n > 0 ? sum / static_cast<float>(n) : 0.0f;
                }
            }
        }
    }, "downsample2x" /* named tag for profiler */);
}

FloatImage Downsample4x(const FloatImage& src)
{
    FloatImage dst;
    Downsample4x(src, dst);
    return dst;
}

void Downsample4x(const FloatImage& src, FloatImage& dst)
{
    dst.width = std::max<uint32_t>(1, src.width / 4);
    dst.height = std::max<uint32_t>(1, src.height / 4);
    dst.channels = src.channels;
    dst.data.resize(static_cast<size_t>(dst.width) * dst.height * dst.channels, 0.0f);

    ParallelForRows(dst.height, RecommendedImageRowGrain(dst.width, dst.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < dst.width; ++x)
            {
                for (uint32_t c = 0; c < dst.channels; ++c)
                {
                    uint32_t sx = x * 4;
                    uint32_t sy = y * 4;
                    float sum = 0.0f;
                    int n = 0;
                    for (uint32_t dy = 0; dy < 4 && sy + dy < src.height; ++dy)
                    {
                        for (uint32_t dx = 0; dx < 4 && sx + dx < src.width; ++dx)
                        {
                            sum += src.At(sx + dx, sy + dy, c);
                            ++n;
                        }
                    }
                    dst.At(x, y, c) = n > 0 ? sum / static_cast<float>(n) : 0.0f;
                }
            }
        }
    }, "downsample4x" /* named tag for profiler */);
}

FloatImage BoxBlur(const FloatImage& src, int radius)
{
    if (radius <= 0) return src;
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    ParallelForRows(src.height, RecommendedImageRowGrain(src.width, src.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < src.width; ++x)
            {
                for (uint32_t c = 0; c < src.channels; ++c)
                {
                    float sum = 0.0f;
                    int n = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                    {
                        for (int dx = -radius; dx <= radius; ++dx)
                        {
                            sum += SampleClamped(src, static_cast<int>(x) + dx, static_cast<int>(y) + dy, c);
                            ++n;
                        }
                    }
                    out.At(x, y, c) = sum / static_cast<float>(n);
                }
            }
        }
    }, "box_blur" /* named tag for profiler */);
    return out;
}

FloatImage GaussianBlur(const FloatImage& src, float sigma, int radius)
{
    if (src.width == 0 || src.height == 0 || src.channels == 0) return src;
    if (sigma <= 0.0f) return src;
    if (radius < 0) radius = static_cast<int>(std::ceil(3.0f * sigma));
    if (radius < 1) radius = 1;

    const int W = static_cast<int>(src.width);
    const int H = static_cast<int>(src.height);
    const int C = static_cast<int>(src.channels);

    // Precompute 1D kernel (normalized).
    std::vector<float> kernel(2 * radius + 1);
    const float inv_two_sigma_sq = 1.0f / (2.0f * sigma * sigma);
    float ksum = 0.0f;
    for (int i = -radius; i <= radius; ++i)
    {
        float v = std::exp(-(static_cast<float>(i) * static_cast<float>(i)) * inv_two_sigma_sq);
        kernel[i + radius] = v;
        ksum += v;
    }
    for (float& k : kernel) k /= ksum;

    // Horizontal pass: column read, column write into a transient buffer.
    FloatImage tmp;
    tmp.width = src.width;
    tmp.height = src.height;
    tmp.channels = src.channels;
    tmp.data.resize(src.data.size(), 0.0f);

    ParallelForRows(src.height, RecommendedImageRowGrain(src.width, src.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            size_t row_off = static_cast<size_t>(y) * W * C;
            for (int x = 0; x < W; ++x)
            {
                int x_clamped = std::max(0, std::min(x, W - 1));
                float acc[64];
                for (int c = 0; c < C; ++c) acc[c] = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                {
                    int sx = x + k;
                    if (sx < 0) sx = 0;
                    else if (sx >= W) sx = W - 1;
                    float w = kernel[k + radius];
                    const float* sp = &src.data[row_off + static_cast<size_t>(sx) * C];
                    for (int c = 0; c < C; ++c) acc[c] += w * sp[c];
                }
                float* dp = &tmp.data[row_off + static_cast<size_t>(x_clamped) * C];
                for (int c = 0; c < C; ++c) dp[c] = acc[c];
            }
        }
    }, "gauss_h");

    // Vertical pass: row read into final output.
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    ParallelForRows(src.height, RecommendedImageRowGrain(src.width, src.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            int y_clamped = static_cast<int>(y);
            size_t row_off = static_cast<size_t>(y) * W * C;
            for (int x = 0; x < W; ++x)
            {
                float acc[64];
                for (int c = 0; c < C; ++c) acc[c] = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                {
                    int sy = static_cast<int>(y) + k;
                    if (sy < 0) sy = 0;
                    else if (sy >= H) sy = H - 1;
                    float w = kernel[k + radius];
                    const float* sp = &tmp.data[static_cast<size_t>(sy) * W * C + static_cast<size_t>(x) * C];
                    for (int c = 0; c < C; ++c) acc[c] += w * sp[c];
                }
                float* dp = &out.data[row_off + static_cast<size_t>(x) * C];
                for (int c = 0; c < C; ++c) dp[c] = acc[c];
                (void)y_clamped;
            }
        }
    }, "gauss_v");

    return out;
}

FloatImage WarpTranslate(const FloatImage& src, float shift_x, float shift_y)
{
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    ParallelForRows(src.height, RecommendedImageRowGrain(src.width, src.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < src.width; ++x)
            {
                float sx = static_cast<float>(x) - shift_x;
                float sy = static_cast<float>(y) - shift_y;
                int nx = static_cast<int>(std::lround(sx));
                int ny = static_cast<int>(std::lround(sy));
                for (uint32_t c = 0; c < src.channels; ++c)
                {
                    out.At(x, y, c) = SampleClamped(src, nx, ny, c);
                }
            }
        }
    }, "warp_translate" /* named tag for profiler */);
    return out;
}

FloatImage ConvertMosaicToPlaneImage(const FloatImage& src, uint32_t cfa_period)
{
    if (src.channels != 1 || cfa_period <= 1) return src;

    FloatImage out;
    out.width = (src.width + cfa_period - 1) / cfa_period;
    out.height = (src.height + cfa_period - 1) / cfa_period;
    out.channels = cfa_period * cfa_period;
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    ParallelForRows(src.height, RecommendedImageRowGrain(src.width, 1, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            uint32_t py = y % cfa_period;
            uint32_t oy = y / cfa_period;
            for (uint32_t x = 0; x < src.width; ++x)
            {
                uint32_t px = x % cfa_period;
                uint32_t ox = x / cfa_period;
                uint32_t c = py * cfa_period + px;
                out.At(ox, oy, c) = src.At(x, y, 0);
            }
        }
    }, "mosaic_to_plane" /* named tag for profiler */);

    return out;
}

FloatImage ConvertPlaneImageToMosaic(const FloatImage& src,
                                     uint32_t mosaic_width,
                                     uint32_t mosaic_height,
                                     uint32_t cfa_period)
{
    if (src.channels != cfa_period * cfa_period || cfa_period <= 1)
    {
        return src;
    }

    FloatImage out;
    out.width = mosaic_width;
    out.height = mosaic_height;
    out.channels = 1;
    out.data.resize(static_cast<size_t>(out.width) * out.height, 0.0f);

    ParallelForRows(out.height, RecommendedImageRowGrain(out.width, 1, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            uint32_t py = y % cfa_period;
            uint32_t sy = y / cfa_period;
            for (uint32_t x = 0; x < out.width; ++x)
            {
                uint32_t px = x % cfa_period;
                uint32_t sx = x / cfa_period;
                uint32_t c = py * cfa_period + px;
                out.At(x, y, 0) = src.At(sx, sy, c);
            }
        }
    }, "plane_to_mosaic" /* named tag for profiler */);

    return out;
}

FloatImage ConvertPlanesToGrayscale(const FloatImage& src)
{
    // TODO(X-Trans): add an X-Trans specific path that handles the 6×6
    // CFA pattern; the current channel-average fallback may not preserve
    // the correct colour phase for non-Bayer patterns.
    FloatImage dst;
    dst.width = src.width;
    dst.height = src.height;
    dst.channels = 1;
    dst.data.assign(static_cast<size_t>(dst.width) * dst.height, 0.0f);

    const uint32_t ch = std::max<uint32_t>(1, src.channels);
    const float inv_ch = 1.0f / static_cast<float>(ch);

    ParallelForRows(src.height, RecommendedImageRowGrain(src.width, src.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < src.width; ++x)
            {
                float sum = 0.0f;
                for (uint32_t c = 0; c < ch; ++c)
                {
                    sum += src.At(x, y, c);
                }
                dst.At(x, y, 0) = sum * inv_ch;
            }
        }
    }, "to_grayscale" /* named tag for profiler */);

    return dst;
}

namespace
{

float SampleBilinear(const FloatImage& src, float x, float y, uint32_t c)
{
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);

    int ix1 = std::min(ix + 1, static_cast<int>(src.width) - 1);
    int iy1 = std::min(iy + 1, static_cast<int>(src.height) - 1);
    ix = std::max(ix, 0);
    iy = std::max(iy, 0);

    float v00 = src.At(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), c);
    float v10 = src.At(static_cast<uint32_t>(ix1), static_cast<uint32_t>(iy), c);
    float v01 = src.At(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy1), c);
    float v11 = src.At(static_cast<uint32_t>(ix1), static_cast<uint32_t>(iy1), c);

    float v0 = v00 + fx * (v10 - v00);
    float v1 = v01 + fx * (v11 - v01);
    return v0 + fy * (v1 - v0);
}

float CubicKernel(float t)
{
    // Catmull-Rom: a = -0.5
    float at = std::abs(t);
    float at2 = at * at;
    float at3 = at2 * at;
    if (at < 1.0f) return 1.5f * at3 - 2.5f * at2 + 1.0f;
    if (at < 2.0f) return -0.5f * at3 + 2.5f * at2 - 4.0f * at + 2.0f;
    return 0.0f;
}

float SampleBicubic(const FloatImage& src, float x, float y, uint32_t c)
{
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);

    float sum = 0.0f;
    float norm = 0.0f;
    int hw = static_cast<int>(src.width);
    int hh = static_cast<int>(src.height);

    for (int dy = -1; dy <= 2; ++dy)
    {
        int sy = iy + dy;
        if (sy < 0) sy = 0;
        if (sy >= hh) sy = hh - 1;
        float wy = CubicKernel(static_cast<float>(dy) - fy);

        for (int dx = -1; dx <= 2; ++dx)
        {
            int sx = ix + dx;
            if (sx < 0) sx = 0;
            if (sx >= hw) sx = hw - 1;
            float w = wy * CubicKernel(static_cast<float>(dx) - fx);
            sum += w * src.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), c);
            norm += w;
        }
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace

FloatImage ResizeImage(const FloatImage& src,
                       uint32_t dst_width, uint32_t dst_height,
                       InterpolationMethod method)
{
    if (dst_width == 0) dst_width = 1;
    if (dst_height == 0) dst_height = 1;

    FloatImage out;
    out.width = dst_width;
    out.height = dst_height;
    out.channels = src.channels;
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    float scale_x = static_cast<float>(src.width) / static_cast<float>(dst_width);
    float scale_y = static_cast<float>(src.height) / static_cast<float>(dst_height);

    const float half_texel_x = scale_x * 0.5f;
    const float half_texel_y = scale_y * 0.5f;

    if (method == InterpolationMethod::HalfSample)
    {
        const bool downscale = (dst_width <= src.width) && (dst_height <= src.height);
        if (!downscale)
        {
            method = InterpolationMethod::Bilinear;
        }
        else
        {
            const int32_t step_x = static_cast<int32_t>(src.width)  / static_cast<int32_t>(dst_width);
            const int32_t step_y = static_cast<int32_t>(src.height) / static_cast<int32_t>(dst_height);
            // The algorithm samples 50% of the step window on each axis; if the
            // minimum step is <2 the half-window collapses to 0 and the method
            // no longer makes sense. Fall back to plain area-average.
            if (step_x < 2 || step_y < 2)
            {
                method = InterpolationMethod::AreaAverage;
            }
            else
            {
            const int32_t sw = static_cast<int32_t>(src.width);
            const int32_t sh = static_cast<int32_t>(src.height);

            // Bayer 2x2 deinterleaved plane: route each of the 4 channels to a
            // distinct 2x2 quadrant of the source block matching its physical
            // bayer offset, so channel windows tile without overlap. Each
            // channel still consumes 50% of the block width and 50% of the
            // block height. Other channel counts fall back to the original
            // centered-window behavior.
            const bool bayer_quadrant = (out.channels == 4);

            if (bayer_quadrant)
            {
                ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
                {
                    for (uint32_t y = y_begin; y < y_end; ++y)
                    {
                        const int32_t block_y_lo = static_cast<int32_t>(static_cast<int64_t>(y) * sh / static_cast<int32_t>(out.height));
                        const int32_t block_y_hi = (static_cast<int32_t>(y) == static_cast<int32_t>(out.height) - 1)
                            ? sh
                            : static_cast<int32_t>(static_cast<int64_t>(y + 1) * sh / static_cast<int32_t>(out.height));
                        const int32_t block_h = block_y_hi - block_y_lo;
                        const int32_t quar_h = block_h / 2;
                        for (uint32_t x = 0; x < out.width; ++x)
                        {
                            const int32_t block_x_lo = static_cast<int32_t>(static_cast<int64_t>(x) * sw / static_cast<int32_t>(out.width));
                            const int32_t block_x_hi = (static_cast<int32_t>(x) == static_cast<int32_t>(out.width) - 1)
                                ? sw
                                : static_cast<int32_t>(static_cast<int64_t>(x + 1) * sw / static_cast<int32_t>(out.width));
                            const int32_t block_w = block_x_hi - block_x_lo;
                            const int32_t quar_w = block_w / 2;

                            for (uint32_t c = 0; c < out.channels; ++c)
                            {
                                const int32_t qx = (c & 1) * quar_w;
                                const int32_t qy = (c >> 1) * quar_h;
                                int32_t sx_lo = block_x_lo + qx;
                                int32_t sx_hi = sx_lo + quar_w;
                                int32_t sy_lo = block_y_lo + qy;
                                int32_t sy_hi = sy_lo + quar_h;
                                if (sx_lo < 0) sx_lo = 0;
                                if (sx_hi > sw) sx_hi = sw;
                                if (sy_lo < 0) sy_lo = 0;
                                if (sy_hi > sh) sy_hi = sh;
                                const int32_t aw = sx_hi - sx_lo;
                                const int32_t ah = sy_hi - sy_lo;
                                if (aw <= 0 || ah <= 0)
                                {
                                    out.At(x, y, c) = 0.0f;
                                    continue;
                                }
                                float sum = 0.0f;
                                for (int32_t by = sy_lo; by < sy_hi; ++by)
                                {
                                    const float* row = &src.data[(static_cast<size_t>(by) * src.width + sx_lo) * src.channels + c];
                                    for (int32_t bx = 0; bx < aw; ++bx)
                                    {
                                        sum += row[bx * static_cast<int32_t>(src.channels)];
                                    }
                                }
                                out.At(x, y, c) = sum / static_cast<float>(aw * ah);
                            }
                        }
                    }
                }, "resize_half_sample_bayer");
                return out;
            }

            const int32_t half_w = step_x / 2;
            const int32_t half_h = step_y / 2;
            const int32_t hx = half_w / 2;
            const int32_t hy = half_h / 2;

            ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
            {
                for (uint32_t y = y_begin; y < y_end; ++y)
                {
                    int32_t cy = static_cast<int32_t>(static_cast<int64_t>(y) * sh / static_cast<int32_t>(out.height));
                    int32_t sy_lo = cy - hy;
                    int32_t sy_hi = sy_lo + half_h;
                    if (sy_lo < 0) { sy_lo = 0; sy_hi = half_h; }
                    if (static_cast<int32_t>(y) == static_cast<int32_t>(out.height) - 1)
                    {
                        sy_hi = sh;
                        sy_lo = sh - half_h;
                        if (sy_lo < 0) sy_lo = 0;
                    }
                    int32_t ah = sy_hi - sy_lo;
                    for (uint32_t x = 0; x < out.width; ++x)
                    {
                        int32_t cx = static_cast<int32_t>(static_cast<int64_t>(x) * sw / static_cast<int32_t>(out.width));
                        int32_t sx_lo = cx - hx;
                        int32_t sx_hi = sx_lo + half_w;
                        if (sx_lo < 0) { sx_lo = 0; sx_hi = half_w; }
                        if (static_cast<int32_t>(x) == static_cast<int32_t>(out.width) - 1)
                        {
                            sx_hi = sw;
                            sx_lo = sw - half_w;
                            if (sx_lo < 0) sx_lo = 0;
                        }
                        int32_t aw = sx_hi - sx_lo;
                        float inv = (aw > 0 && ah > 0) ? 1.0f / static_cast<float>(aw * ah) : 0.0f;
                        for (uint32_t c = 0; c < out.channels; ++c)
                        {
                            float sum = 0.0f;
                            for (int32_t by = sy_lo; by < sy_hi; ++by)
                            {
                                const float* row = &src.data[(static_cast<size_t>(by) * src.width + sx_lo) * src.channels + c];
                                for (int32_t bx = 0; bx < aw; ++bx)
                                {
                                    sum += row[bx * static_cast<int32_t>(src.channels)];
                                }
                            }
                            out.At(x, y, c) = sum * inv;
                        }
                    }
                }
            }, "resize_half_sample");
            return out;
            }
        }
    }

    if (method == InterpolationMethod::GaussianArea)
    {
        const bool downscale = (dst_width <= src.width) && (dst_height <= src.height);
        if (!downscale)
        {
            method = InterpolationMethod::Bilinear;
        }
        else
        {
            const float downscale_factor_x = static_cast<float>(src.width)  / static_cast<float>(dst_width);
            const float downscale_factor_y = static_cast<float>(src.height) / static_cast<float>(dst_height);

            ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
            {
                for (uint32_t y = y_begin; y < y_end; ++y)
                {
                    int32_t sy0 = static_cast<int32_t>(static_cast<int64_t>(y) * src.height / dst_height);
                    int32_t sy1 = static_cast<int32_t>((static_cast<int64_t>(y) + 1) * src.height + dst_height - 1) / static_cast<int32_t>(dst_height);
                    if (sy1 <= sy0) sy1 = sy0 + 1;
                    if (sy1 > static_cast<int32_t>(src.height)) sy1 = static_cast<int32_t>(src.height);
                    if (sy0 < 0) sy0 = 0;

                    // Gaussian center in source space for this destination row.
                    const float cy = (static_cast<float>(y) + 0.5f) * downscale_factor_y;
                    // Sigma scales with downscale factor so the kernel covers more
                    // source pixels when downscaling harder; floor stops it from
                    // collapsing to zero for sub-3x downscale handled earlier.
                    const float sigma_y = std::max(1.0f, 0.5f * downscale_factor_y);
                    const float inv_two_sigma_sq_y = 1.0f / (2.0f * sigma_y * sigma_y);

                    for (uint32_t x = 0; x < out.width; ++x)
                    {
                        int32_t sx0 = static_cast<int32_t>(static_cast<int64_t>(x) * src.width / dst_width);
                        int32_t sx1 = static_cast<int32_t>((static_cast<int64_t>(x) + 1) * src.width + dst_width - 1) / static_cast<int32_t>(dst_width);
                        if (sx1 <= sx0) sx1 = sx0 + 1;
                        if (sx1 > static_cast<int32_t>(src.width)) sx1 = static_cast<int32_t>(src.width);
                        if (sx0 < 0) sx0 = 0;

                        const float cx = (static_cast<float>(x) + 0.5f) * downscale_factor_x;
                        const float sigma_x = std::max(1.0f, 0.5f * downscale_factor_x);
                        const float inv_two_sigma_sq_x = 1.0f / (2.0f * sigma_x * sigma_x);

                        for (uint32_t c = 0; c < out.channels; ++c)
                        {
                            float wsum = 0.0f;
                            float sum = 0.0f;
                            for (int32_t by = sy0; by < sy1; ++by)
                            {
                                const float ty = (static_cast<float>(by) + 0.5f) - cy;
                                const float wy = std::exp(-ty * ty * inv_two_sigma_sq_y);
                                const float* row = &src.data[(static_cast<size_t>(by) * src.width + sx0) * src.channels + c];
                                for (int32_t bx = 0; bx < static_cast<int32_t>(sx1 - sx0); ++bx)
                                {
                                    const float tx = (static_cast<float>(sx0 + bx) + 0.5f) - cx;
                                    const float wx = std::exp(-tx * tx * inv_two_sigma_sq_x);
                                    const float w = wx * wy;
                                    sum  += w * row[bx * static_cast<int32_t>(src.channels)];
                                    wsum += w;
                                }
                            }
                            out.At(x, y, c) = wsum > 0.0f ? sum / wsum : 0.0f;
                        }
                    }
                }
            }, "resize_gauss_area");
            return out;
        }
    }

    if (method == InterpolationMethod::AreaAverage)
    {
        const bool downscale = (dst_width <= src.width) && (dst_height <= src.height);
        if (downscale)
        {
            ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
            {
                for (uint32_t y = y_begin; y < y_end; ++y)
                {
                    int32_t sy0 = static_cast<int32_t>(static_cast<int64_t>(y) * src.height / dst_height);
                    int32_t sy1 = static_cast<int32_t>((static_cast<int64_t>(y) + 1) * src.height + dst_height - 1) / static_cast<int32_t>(dst_height);
                    if (sy1 <= sy0) sy1 = sy0 + 1;
                    if (sy1 > static_cast<int32_t>(src.height)) sy1 = static_cast<int32_t>(src.height);
                    if (sy0 < 0) sy0 = 0;
                    for (uint32_t x = 0; x < out.width; ++x)
                    {
                        int32_t sx0 = static_cast<int32_t>(static_cast<int64_t>(x) * src.width / dst_width);
                        int32_t sx1 = static_cast<int32_t>((static_cast<int64_t>(x) + 1) * src.width + dst_width - 1) / static_cast<int32_t>(dst_width);
                        if (sx1 <= sx0) sx1 = sx0 + 1;
                        if (sx1 > static_cast<int32_t>(src.width)) sx1 = static_cast<int32_t>(src.width);
                        if (sx0 < 0) sx0 = 0;
                        int32_t area_w = sx1 - sx0;
                        int32_t area_h = sy1 - sy0;
                        float area = static_cast<float>(area_w) * static_cast<float>(area_h);
                        float inv_area = area > 0.0f ? 1.0f / area : 0.0f;
                        for (uint32_t c = 0; c < out.channels; ++c)
                        {
                            float sum = 0.0f;
                            for (int32_t by = sy0; by < sy1; ++by)
                            {
                                const float* row = &src.data[(static_cast<size_t>(by) * src.width + sx0) * src.channels + c];
                                for (int32_t bx = 0; bx < area_w; ++bx)
                                {
                                    sum += row[bx * src.channels];
                                }
                            }
                            out.At(x, y, c) = sum * inv_area;
                        }
                    }
                }
            }, "resize_area_avg");
            return out;
        }
        method = InterpolationMethod::Bilinear;
    }

    if (method == InterpolationMethod::Bilinear)
    {
        ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                float sy = static_cast<float>(y) * scale_y + half_texel_y;
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    float sx = static_cast<float>(x) * scale_x + half_texel_x;
                    for (uint32_t c = 0; c < out.channels; ++c)
                    {
                        out.At(x, y, c) = SampleBilinear(src, sx, sy, c);
                    }
                }
            }
        }, "resize_bilinear");
    }
    else
    {
        ParallelForRows(out.height, RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows), [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                float sy = static_cast<float>(y) * scale_y + half_texel_y;
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    float sx = static_cast<float>(x) * scale_x + half_texel_x;
                    for (uint32_t c = 0; c < out.channels; ++c)
                    {
                        out.At(x, y, c) = SampleBicubic(src, sx, sy, c);
                    }
                }
            }
        }, "resize_bicubic");
    }

    return out;
}

float MaxValue(const FloatImage& src)
{
    if (src.data.empty()) return 0.0f;
    return *std::max_element(src.data.begin(), src.data.end());
}

} // namespace burstmerge
