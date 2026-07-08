#include "burstmerge/internal/core/image_resize.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace burstmerge
{
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

FloatImage GaussianBlur(const FloatImage& src, float sigma, int radius)
{
    if (src.width == 0 || src.height == 0 || src.channels == 0) return src;
    if (sigma <= 0.0f) return src;
    if (radius < 0) radius = static_cast<int>(std::ceil(3.0f * sigma));
    if (radius < 1) radius = 1;

    const int W = static_cast<int>(src.width);
    const int H = static_cast<int>(src.height);
    const int C = static_cast<int>(src.channels);

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
            if (step_x < 2 || step_y < 2)
            {
                method = InterpolationMethod::AreaAverage;
            }
            else
            {
            const int32_t sw = static_cast<int32_t>(src.width);
            const int32_t sh = static_cast<int32_t>(src.height);

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

                    const float cy = (static_cast<float>(y) + 0.5f) * downscale_factor_y;
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

} // namespace burstmerge
