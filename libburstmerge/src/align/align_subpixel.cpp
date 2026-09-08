#include "burstmerge/internal/align/align_subpixel.h"

#include "burstmerge/internal/align/align_common.h"
#include "burstmerge/internal/core/fft_util.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace burstmerge
{
namespace
{

// Separable parabola peak estimator. Given costs at offsets -1, 0, +1 along one
// axis (holding the other fixed), returns the fractional offset of the peak,
// clamped to [-0.5, 0.5]. Returns 0 when the surface is flat (ill-conditioned).
float ParabolaPeak(double c_m1, double c_0, double c_p1)
{
    const double denom = c_m1 - 2.0 * c_0 + c_p1;
    if (std::abs(denom) < 1e-9)
    {
        return 0.0f;
    }
    double peak = 0.5 * (c_m1 - c_p1) / denom;
    if (peak < -0.5) peak = -0.5;
    if (peak > 0.5) peak = 0.5;
    return static_cast<float>(peak);
}

// Per-tile separable SSD parabola refinement around the integer shift.
void ParabolaRefineTile(const FloatImage& ref,
                        const FloatImage& cmp,
                        uint32_t x0,
                        uint32_t y0,
                        uint32_t w,
                        uint32_t h,
                        int base_dx,
                        int base_dy,
                        float& out_dx,
                        float& out_dy)
{
    // x axis (y held at integer base).
    const double cx_m1 = TileCost(ref, cmp, x0, y0, w, h, base_dx - 1, base_dy, 1, true);
    const double cx_0  = TileCost(ref, cmp, x0, y0, w, h, base_dx,     base_dy, 1, true);
    const double cx_p1 = TileCost(ref, cmp, x0, y0, w, h, base_dx + 1, base_dy, 1, true);
    float px = ParabolaPeak(cx_m1, cx_0, cx_p1);

    int rx = static_cast<int>(std::lround(static_cast<double>(base_dx) + px));
    // y axis (x held at the refined integer).
    const double cy_m1 = TileCost(ref, cmp, x0, y0, w, h, rx, base_dy - 1, 1, true);
    const double cy_0  = TileCost(ref, cmp, x0, y0, w, h, rx, base_dy,     1, true);
    const double cy_p1 = TileCost(ref, cmp, x0, y0, w, h, rx, base_dy + 1, 1, true);
    float py = ParabolaPeak(cy_m1, cy_0, cy_p1);

    out_dx = static_cast<float>(base_dx) + px;
    out_dy = static_cast<float>(base_dy) + py;
}

// Fourier phase-shift sub-pixel search over an odd grid (range +/-0.5).
// Mirrors the legacy freq_align FourierShiftSearch but returns the fractional
// peak directly (no rounding) and lets the grid be smaller for the SR path.
struct FracShift
{
    float dx = 0.0f;
    float dy = 0.0f;
};

FracShift FourierSubpixelSearch(const std::vector<std::complex<double>>& ref_fft,
                                const std::vector<std::complex<double>>& cmp_fft,
                                uint32_t fw,
                                uint32_t fh,
                                int grid)
{
    const size_t n = static_cast<size_t>(fw) * fh;
    const double kPi = 3.14159265358979323846;
    const double range = 0.5;
    double best_diff = 1e300;
    double best_sx = 0.0;
    double best_sy = 0.0;

    for (int iy = 0; iy < grid; ++iy)
    {
        double sy = (grid == 1) ? 0.0 : -range + 2.0 * range * static_cast<double>(iy) / static_cast<double>(grid - 1);
        for (int ix = 0; ix < grid; ++ix)
        {
            double sx = (grid == 1) ? 0.0 : -range + 2.0 * range * static_cast<double>(ix) / static_cast<double>(grid - 1);
            double diff = 0.0;
            for (uint32_t fy = 0; fy < fh; ++fy)
            {
                for (uint32_t fx = 0; fx < fw; ++fx)
                {
                    size_t k = static_cast<size_t>(fy) * fw + fx;
                    double theta = 2.0 * kPi *
                        (static_cast<double>(fx) * sx / static_cast<double>(fw) +
                         static_cast<double>(fy) * sy / static_cast<double>(fh));
                    double c = std::cos(theta);
                    double s = std::sin(theta);
                    double sr = cmp_fft[k].real() * c - cmp_fft[k].imag() * s;
                    double si = cmp_fft[k].real() * s + cmp_fft[k].imag() * c;
                    double dr = ref_fft[k].real() - sr;
                    double di = ref_fft[k].imag() - si;
                    diff += dr * dr + di * di;
                }
            }
            if (diff < best_diff)
            {
                best_diff = diff;
                best_sx = sx;
                best_sy = sy;
            }
        }
    }
    (void)n;
    return {static_cast<float>(best_sx), static_cast<float>(best_sy)};
}

// Per-tile Fourier sub-pixel refinement. Extracts the tile at the integer
// offset (base_dx,base_dy), FFTs both, searches the fractional grid, and adds
// the fractional peak to the integer base.
void FourierRefineTile(const FloatImage& ref,
                       const FloatImage& cmp,
                       uint32_t x0,
                       uint32_t y0,
                       uint32_t w,
                       uint32_t h,
                       int base_dx,
                       int base_dy,
                       int grid,
                       float& out_dx,
                       float& out_dy)
{
    const int cmp_x0 = static_cast<int>(x0) + base_dx;
    const int cmp_y0 = static_cast<int>(y0) + base_dy;
    if (cmp_x0 < 0 || cmp_x0 + static_cast<int>(w) > static_cast<int>(cmp.width) ||
        cmp_y0 < 0 || cmp_y0 + static_cast<int>(h) > static_cast<int>(cmp.height))
    {
        out_dx = static_cast<float>(base_dx);
        out_dy = static_cast<float>(base_dy);
        return;
    }

    const size_t n = static_cast<size_t>(w) * h;
    std::vector<std::complex<double>> ref_fft(n, 0.0);
    std::vector<std::complex<double>> cmp_fft(n, 0.0);
    const uint32_t ch = std::min(ref.channels, cmp.channels);
    const double inv_ch = 1.0 / std::max<uint32_t>(1, ch);
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            size_t k = static_cast<size_t>(y) * w + x;
            double ra = 0.0, ca = 0.0;
            for (uint32_t c = 0; c < ch; ++c)
            {
                ra += ref.At(x0 + x, y0 + y, c);
                ca += cmp.At(static_cast<uint32_t>(cmp_x0 + x),
                             static_cast<uint32_t>(cmp_y0 + y), c);
            }
            ref_fft[k] = ra * inv_ch;
            cmp_fft[k] = ca * inv_ch;
        }
    }
    Fft2D(ref_fft, w, h, false);
    Fft2D(cmp_fft, w, h, false);

    FracShift frac = FourierSubpixelSearch(ref_fft, cmp_fft, w, h, grid);
    out_dx = static_cast<float>(base_dx) + frac.dx;
    out_dy = static_cast<float>(base_dy) + frac.dy;
}

} // namespace

void RefineTileFieldSubpixel(const FloatImage& ref,
                             const FloatImage& cmp,
                             AlignmentResult& result,
                             SubpixelMethod method,
                             int fourier_grid)
{
    if (result.tile_shift_x.empty() || result.tile_shift_y.empty() ||
        result.tiles_x == 0 || result.tiles_y == 0 || result.tile_size <= 0)
    {
        return;
    }
    // CFA-phase-sensitive data (non-deinterleaved mosaic): fractional offsets
    // would straddle phase boundaries; keep the integer field as-is.
    if (result.cfa_period > 1)
    {
        result.tile_shift_x_sub.assign(result.tile_shift_x.begin(),
                                       result.tile_shift_x.end());
        result.tile_shift_y_sub.assign(result.tile_shift_y.begin(),
                                       result.tile_shift_y.end());
        result.shift_x_sub = static_cast<float>(result.shift_x);
        result.shift_y_sub = static_cast<float>(result.shift_y);
        return;
    }

    const int grid = std::max(1, fourier_grid);
    const uint32_t spacing = static_cast<uint32_t>(result.tile_spacing > 0
        ? result.tile_spacing : result.tile_size);
    const uint32_t tile_size = static_cast<uint32_t>(result.tile_size);
    const uint32_t tiles_x = result.tiles_x;
    const uint32_t tiles_y = result.tiles_y;
    const size_t tile_count = static_cast<size_t>(tiles_x) * tiles_y;

    std::vector<float> sub_x(tile_count);
    std::vector<float> sub_y(tile_count);

    // The Frequency method calls Fft2D, whose plan creation is not safe to run
    // concurrently from many OMP threads. Run it serially (per-tile FFTs are
    // the expensive path anyway). The SAD-parabola method (no FFT) parallelises.
    const bool parallel_ok = (method == SubpixelMethod::SadParabola);
    auto body = [&](size_t idx)
    {
        const uint32_t tx = static_cast<uint32_t>(idx % tiles_x);
        const uint32_t ty = static_cast<uint32_t>(idx / tiles_x);
        const uint32_t x0 = tx * spacing;
        const uint32_t y0 = ty * spacing;
        const uint32_t w = std::min(ref.width - x0, tile_size);
        const uint32_t h = std::min(ref.height - y0, tile_size);
        const int base_dx = static_cast<int>(result.tile_shift_x[idx]);
        const int base_dy = static_cast<int>(result.tile_shift_y[idx]);

        float fx = 0.0f, fy = 0.0f;
        if (method == SubpixelMethod::Frequency)
        {
            FourierRefineTile(ref, cmp, x0, y0, w, h, base_dx, base_dy, grid, fx, fy);
        }
        else
        {
            ParabolaRefineTile(ref, cmp, x0, y0, w, h, base_dx, base_dy, fx, fy);
        }
        sub_x[idx] = fx;
        sub_y[idx] = fy;
    };

    if (parallel_ok)
    {
        ParallelFor(tile_count, 1, [&](size_t begin, size_t end)
        {
            for (size_t idx = begin; idx < end; ++idx) body(idx);
        }, "subpixel_refine" /* named tag for profiler */);
    }
    else
    {
        for (size_t idx = 0; idx < tile_count; ++idx) body(idx);
    }

    double sum_x = 0.0, sum_y = 0.0;
    for (float v : sub_x) sum_x += v;
    for (float v : sub_y) sum_y += v;
    result.tile_shift_x_sub.swap(sub_x);
    result.tile_shift_y_sub.swap(sub_y);
    result.shift_x_sub = static_cast<float>(sum_x / static_cast<double>(tile_count));
    result.shift_y_sub = static_cast<float>(sum_y / static_cast<double>(tile_count));
}

void SmoothSubpixelTileField(AlignmentResult& result)
{
    if (result.tile_shift_x_sub.size() != result.tile_shift_x.size() ||
        result.tile_shift_y_sub.size() != result.tile_shift_y.size() ||
        result.tiles_x == 0 || result.tiles_y == 0)
    {
        return;
    }

    std::vector<float> smooth_x = result.tile_shift_x_sub;
    std::vector<float> smooth_y = result.tile_shift_y_sub;
    std::array<float, 9> values_x{};
    std::array<float, 9> values_y{};
    for (uint32_t ty = 0; ty < result.tiles_y; ++ty)
    {
        for (uint32_t tx = 0; tx < result.tiles_x; ++tx)
        {
            int count = 0;
            for (int oy = -1; oy <= 1; ++oy)
            {
                const int sy = static_cast<int>(ty) + oy;
                if (sy < 0 || sy >= static_cast<int>(result.tiles_y)) continue;
                for (int ox = -1; ox <= 1; ++ox)
                {
                    const int sx = static_cast<int>(tx) + ox;
                    if (sx < 0 || sx >= static_cast<int>(result.tiles_x)) continue;
                    const size_t index = static_cast<size_t>(sy) * result.tiles_x +
                                         static_cast<uint32_t>(sx);
                    values_x[count] = result.tile_shift_x_sub[index];
                    values_y[count] = result.tile_shift_y_sub[index];
                    ++count;
                }
            }
            if (count == 0) continue;
            const size_t index = static_cast<size_t>(ty) * result.tiles_x + tx;
            std::nth_element(values_x.begin(), values_x.begin() + count / 2,
                             values_x.begin() + count);
            std::nth_element(values_y.begin(), values_y.begin() + count / 2,
                             values_y.begin() + count);
            if ((count & 1u) == 0u)
            {
                const auto compare_x = [](float a, float b) { return a < b; };
                const auto compare_y = [](float a, float b) { return a < b; };
                const float upper_x = *std::min_element(
                    values_x.begin() + count / 2 + 1, values_x.begin() + count,
                    compare_x);
                const float upper_y = *std::min_element(
                    values_y.begin() + count / 2 + 1, values_y.begin() + count,
                    compare_y);
                smooth_x[index] = 0.5f *
                    (values_x[count / 2] + upper_x);
                smooth_y[index] = 0.5f *
                    (values_y[count / 2] + upper_y);
            }
            else
            {
                smooth_x[index] = values_x[count / 2];
                smooth_y[index] = values_y[count / 2];
            }
        }
    }
    result.tile_shift_x_sub.swap(smooth_x);
    result.tile_shift_y_sub.swap(smooth_y);
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (float value : result.tile_shift_x_sub) sum_x += value;
    for (float value : result.tile_shift_y_sub) sum_y += value;
    const size_t count = result.tile_shift_x_sub.size();
    if (count > 0)
    {
        result.shift_x_sub = static_cast<float>(sum_x / count);
        result.shift_y_sub = static_cast<float>(sum_y / count);
    }
}

} // namespace burstmerge
