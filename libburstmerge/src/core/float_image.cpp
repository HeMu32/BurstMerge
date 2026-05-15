#include "burstmerge/internal/core/float_image.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace burstmerge {
namespace {

float SampleClamped(const FloatImage& src, int x, int y, uint32_t c) {
    x = std::max(0, std::min(x, static_cast<int>(src.width) - 1));
    y = std::max(0, std::min(y, static_cast<int>(src.height) - 1));
    return src.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c);
}

} // namespace

uint32_t ChannelsForFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA32_Float: return 4;
        default: return 1;
    }
}

FloatImage HostBufferToFloatImage(const HostBuffer& src, float scale) {
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = ChannelsForFormat(src.format);
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    if (!src.data) return out;

    const size_t count = static_cast<size_t>(out.width) * out.height * out.channels;
    switch (src.format) {
        case PixelFormat::R8_Uint: {
            const auto* p = reinterpret_cast<const uint8_t*>(src.data);
            for (size_t i = 0; i < count; ++i) out.data[i] = static_cast<float>(p[i]) * scale;
            break;
        }
        case PixelFormat::R16_Uint: {
            const auto* p = reinterpret_cast<const uint16_t*>(src.data);
            for (size_t i = 0; i < count; ++i) out.data[i] = static_cast<float>(p[i]) * scale;
            break;
        }
        case PixelFormat::R32_Float:
        case PixelFormat::RGBA32_Float: {
            const auto* p = reinterpret_cast<const float*>(src.data);
            for (size_t i = 0; i < count; ++i) out.data[i] = p[i] * scale;
            break;
        }
    }
    return out;
}

HostBuffer FloatImageToUint16HostBuffer(const FloatImage& src, uint32_t white_level) {
    if (src.channels != 1) {
        throw std::runtime_error("FloatImageToUint16HostBuffer expects single-channel image");
    }

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
    for (size_t i = 0; i < count; ++i) {
        float v = std::max(0.0f, std::min(src.data[i], hi));
        dst[i] = static_cast<uint16_t>(std::lround(v));
    }
    return out;
}

FloatImage Downsample2x(const FloatImage& src) {
    FloatImage out;
    out.width = std::max<uint32_t>(1, src.width / 2);
    out.height = std::max<uint32_t>(1, src.height / 2);
    out.channels = src.channels;
    out.data.resize(static_cast<size_t>(out.width) * out.height * out.channels, 0.0f);

    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            for (uint32_t c = 0; c < out.channels; ++c) {
                uint32_t sx = x * 2;
                uint32_t sy = y * 2;
                float sum = 0.0f;
                int n = 0;
                for (uint32_t dy = 0; dy < 2 && sy + dy < src.height; ++dy) {
                    for (uint32_t dx = 0; dx < 2 && sx + dx < src.width; ++dx) {
                        sum += src.At(sx + dx, sy + dy, c);
                        ++n;
                    }
                }
                out.At(x, y, c) = n > 0 ? sum / static_cast<float>(n) : 0.0f;
            }
        }
    }
    return out;
}

FloatImage BoxBlur(const FloatImage& src, int radius) {
    if (radius <= 0) return src;
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    for (uint32_t y = 0; y < src.height; ++y) {
        for (uint32_t x = 0; x < src.width; ++x) {
            for (uint32_t c = 0; c < src.channels; ++c) {
                float sum = 0.0f;
                int n = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        sum += SampleClamped(src, static_cast<int>(x) + dx, static_cast<int>(y) + dy, c);
                        ++n;
                    }
                }
                out.At(x, y, c) = sum / static_cast<float>(n);
            }
        }
    }
    return out;
}

FloatImage WarpTranslateBilinear(const FloatImage& src, float shift_x, float shift_y) {
    FloatImage out;
    out.width = src.width;
    out.height = src.height;
    out.channels = src.channels;
    out.data.resize(src.data.size(), 0.0f);

    for (uint32_t y = 0; y < src.height; ++y) {
        for (uint32_t x = 0; x < src.width; ++x) {
            float sx = static_cast<float>(x) - shift_x;
            float sy = static_cast<float>(y) - shift_y;
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            float tx = sx - static_cast<float>(x0);
            float ty = sy - static_cast<float>(y0);

            for (uint32_t c = 0; c < src.channels; ++c) {
                float p00 = SampleClamped(src, x0, y0, c);
                float p10 = SampleClamped(src, x1, y0, c);
                float p01 = SampleClamped(src, x0, y1, c);
                float p11 = SampleClamped(src, x1, y1, c);
                float a = p00 * (1.0f - tx) + p10 * tx;
                float b = p01 * (1.0f - tx) + p11 * tx;
                out.At(x, y, c) = a * (1.0f - ty) + b * ty;
            }
        }
    }
    return out;
}

float MaxValue(const FloatImage& src) {
    if (src.data.empty()) return 0.0f;
    return *std::max_element(src.data.begin(), src.data.end());
}

} // namespace burstmerge
