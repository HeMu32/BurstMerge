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

uint32_t GrainRowsForImage(uint32_t width, uint32_t channels, uint32_t min_pixels)
{
    const uint64_t denom = std::max<uint64_t>(1, static_cast<uint64_t>(width) * std::max<uint32_t>(1, channels));
    return static_cast<uint32_t>(std::max<uint64_t>(32, (static_cast<uint64_t>(min_pixels) + denom - 1) / denom));
}

} // namespace

uint32_t ChannelsForFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::RGBA32_Float: return 4;
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
            });
            break;
        }
        case PixelFormat::R16_Uint:
        {
            const auto* p = reinterpret_cast<const uint16_t*>(src.data);
            ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
            {
                for (size_t i = i0; i < i1; ++i) out.data[i] = static_cast<float>(p[i]) * scale;
            });
            break;
        }
        case PixelFormat::R32_Float:
        case PixelFormat::RGBA32_Float:
        {
            const auto* p = reinterpret_cast<const float*>(src.data);
            ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
            {
                for (size_t i = i0; i < i1; ++i) out.data[i] = p[i] * scale;
            });
            break;
        }
    }
    return out;
}

HostBuffer FloatImageToUint16HostBuffer(const FloatImage& src, uint32_t white_level)
{
    if (src.channels != 1)
    {
        throw std::runtime_error("FloatImageToUint16HostBuffer expects single-channel image");
    }

    // DNG uses 16-bit containers for all bit depths (12/14/16). The effective
    // precision is defined by the white_level metadata tag, not the container.
    // For 12-bit output data is scaled to [0, 4095] and stored in a uint16_t.
    HostBuffer out;
    out.width = src.width;
    out.height = src.height;
    out.format = PixelFormat::R16_Uint;
    out.row_stride = src.width * sizeof(uint16_t);
    out.size = static_cast<size_t>(out.row_stride) * src.height;
    out.data = new std::byte[out.size]();

    auto* dst = reinterpret_cast<uint16_t*>(out.data);
    const size_t count = static_cast<size_t>(src.width) * src.height;
    const float hi = static_cast<float>(white_level);
    ParallelFor(count, 1u << 16, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i)
        {
            float v = std::max(0.0f, std::min(src.data[i], hi));
            dst[i] = static_cast<uint16_t>(std::lround(v));
        }
    });
    return out;
}

FloatImage Downsample2x(const FloatImage& src)
{
    FloatImage out;
    out.width = std::max<uint32_t>(1, src.width / 2);
    out.height = std::max<uint32_t>(1, src.height / 2);
    out.channels = src.channels;
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    ParallelForRows(out.height, GrainRowsForImage(out.width, out.channels, 1u << 18), [&](uint32_t y_begin, uint32_t y_end)
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
    });
    return out;
}

FloatImage BoxBlur(const FloatImage& src, int radius)
{
    if (radius <= 0) return src;
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    ParallelForRows(src.height, GrainRowsForImage(src.width, src.channels, 1u << 18), [&](uint32_t y_begin, uint32_t y_end)
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
    });
    return out;
}

FloatImage WarpTranslate(const FloatImage& src, float shift_x, float shift_y)
{
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    ParallelForRows(src.height, GrainRowsForImage(src.width, src.channels, 1u << 18), [&](uint32_t y_begin, uint32_t y_end)
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
    });
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

    ParallelForRows(src.height, GrainRowsForImage(src.width, 1, 1u << 18), [&](uint32_t y_begin, uint32_t y_end)
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
    });

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

    ParallelForRows(out.height, GrainRowsForImage(out.width, 1, 1u << 18), [&](uint32_t y_begin, uint32_t y_end)
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
    });

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

    ParallelForRows(src.height, GrainRowsForImage(src.width, src.channels, 1u << 18), [&](uint32_t y_begin, uint32_t y_end)
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
    });

    return dst;
}

float MaxValue(const FloatImage& src)
{
    if (src.data.empty()) return 0.0f;
    return *std::max_element(src.data.begin(), src.data.end());
}

} // namespace burstmerge
