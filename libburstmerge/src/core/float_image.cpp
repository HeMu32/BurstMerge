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
