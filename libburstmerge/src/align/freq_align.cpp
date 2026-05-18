#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/float_image.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

extern "C"
{
#include "pocketfft.h"
}

namespace burstmerge
{
namespace
{

// ===========================================================================
//  FFT helpers
// ===========================================================================

void Fft2D(std::vector<std::complex<double>>& data, size_t w, size_t h,
    bool inverse)
{
    if (w == 0 || h == 0)
    {
        return;
    }

    cfft_plan row = make_cfft_plan(w);
    cfft_plan col = make_cfft_plan(h);
    std::vector<double> tmp(2 * std::max(w, h));

    for (size_t y = 0; y < h; ++y)
    {
        for (size_t x = 0; x < w; ++x)
        {
            auto v = data[y * w + x];
            tmp[2 * x] = v.real();
            tmp[2 * x + 1] = v.imag();
        }

        if (inverse)
        {
            cfft_backward(row, tmp.data(), 1.0);
        }
        else
        {
            cfft_forward(row, tmp.data(), 1.0);
        }

        for (size_t x = 0; x < w; ++x)
        {
            data[y * w + x] = {tmp[2 * x], tmp[2 * x + 1]};
        }
    }

    for (size_t x = 0; x < w; ++x)
    {
        for (size_t y = 0; y < h; ++y)
        {
            auto v = data[y * w + x];
            tmp[2 * y] = v.real();
            tmp[2 * y + 1] = v.imag();
        }

        if (inverse)
        {
            cfft_backward(col, tmp.data(), 1.0 / static_cast<double>(w * h));
        }
        else
        {
            cfft_forward(col, tmp.data(), 1.0);
        }

        for (size_t y = 0; y < h; ++y)
        {
            data[y * w + x] = {tmp[2 * y], tmp[2 * y + 1]};
        }
    }

    destroy_cfft_plan(row);
    destroy_cfft_plan(col);
}

// ===========================================================================
//  Sparse SAD (same convention as align.cpp)
//    ref[x,y] vs cmp[x-dx, y-dy]  → best dx means cmp shifted RIGHT by dx.
// ===========================================================================

float SparseSad2(const FloatImage& a, const FloatImage& b, int dx, int dy,
    int step)
{
    int mx = std::abs(dx);
    int my = std::abs(dy);

    if (a.width <= static_cast<uint32_t>(mx * 2) ||
        a.height <= static_cast<uint32_t>(my * 2))
    {
        return std::numeric_limits<float>::max();
    }

    double sad = 0.0;
    uint64_t cnt = 0;

    for (int y = my; y < static_cast<int>(a.height) - my; y += step)
    {
        for (int x = mx; x < static_cast<int>(a.width) - mx; x += step)
        {
            for (uint32_t c = 0; c < a.channels; ++c)
            {
                sad += std::abs(
                    a.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) -
                    b.At(static_cast<uint32_t>(x - dx), static_cast<uint32_t>(y - dy), c));
                ++cnt;
            }
        }
    }

    return cnt ? static_cast<float>(sad / static_cast<double>(cnt))
               : std::numeric_limits<float>::max();
}

// ---------------------------------------------------------------------------
//  Per-tile SAD  (for integer search at a tile position)
// ---------------------------------------------------------------------------

float TileSad2(const FloatImage& ref, const FloatImage& cmp,
    uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
    int dx, int dy)
{
    double sad = 0.0;
    uint64_t cnt = 0;

    for (uint32_t y = 0; y < th; ++y)
    {
        int by = static_cast<int>(y0 + y) - dy;
        if (by < 0 || by >= static_cast<int>(cmp.height))
        {
            continue;
        }

        for (uint32_t x = 0; x < tw; ++x)
        {
            int bx = static_cast<int>(x0 + x) - dx;
            if (bx < 0 || bx >= static_cast<int>(cmp.width))
            {
                continue;
            }

            for (uint32_t c = 0; c < ref.channels; ++c)
            {
                sad += std::abs(ref.At(x0 + x, y0 + y, c) -
                                cmp.At(static_cast<uint32_t>(bx),
                                       static_cast<uint32_t>(by), c));
                ++cnt;
            }
        }
    }

    return cnt ? static_cast<float>(sad / static_cast<double>(cnt))
               : std::numeric_limits<float>::max();
}

// ===========================================================================
//  Smooth tile field
// ===========================================================================

void SmoothField(AlignmentResult& r)
{
    if (r.tiles_x == 0 || r.tiles_y == 0)
    {
        return;
    }

    auto sxv = r.tile_shift_x;
    auto syv = r.tile_shift_y;

    for (uint32_t ty = 0; ty < r.tiles_y; ++ty)
    {
        for (uint32_t tx = 0; tx < r.tiles_x; ++tx)
        {
            int sx = 0;
            int sy = 0;
            int n = 0;

            for (int oy = -1; oy <= 1; ++oy)
            {
                int ny = static_cast<int>(ty) + oy;
                if (ny < 0 || ny >= static_cast<int>(r.tiles_y))
                {
                    continue;
                }

                for (int ox = -1; ox <= 1; ++ox)
                {
                    int nx = static_cast<int>(tx) + ox;
                    if (nx < 0 || nx >= static_cast<int>(r.tiles_x))
                    {
                        continue;
                    }

                    size_t i = static_cast<size_t>(ny) * r.tiles_x + static_cast<uint32_t>(nx);
                    sx += r.tile_shift_x[i];
                    sy += r.tile_shift_y[i];
                    ++n;
                }
            }

            size_t i = static_cast<size_t>(ty) * r.tiles_x + tx;
            sxv[i] = static_cast<int16_t>(std::lround(static_cast<double>(sx) /
                std::max(1, n)));
            syv[i] = static_cast<int16_t>(std::lround(static_cast<double>(sy) /
                std::max(1, n)));
        }
    }

    r.tile_shift_x.swap(sxv);
    r.tile_shift_y.swap(syv);
}

// ===========================================================================
//  Fourier shift theorem sub-pixel refinement
//
//  Given forward FFT of a reference tile and a comparison tile (aligned to
//  roughly the same spatial region via an integer seed), search a 7×7 grid
//  of sub-pixel shifts (step 1/6, range [-0.5, +0.5]) using the Fourier
//  shift theorem to find the shift that minimises Σ|ref - shifted(cmp)|².
//
//  This is identical in principle to the approach used in the Reference
//  merge_frequency_domain kernel (frequency.metal:59-111).
//
//  FFT convention:
//    forward:  H[k] = Σ h[n] * exp(-2πi·k·n/N)
//    backward: h[n] = 1/N Σ H[k] * exp(+2πi·k·n/N)
//
//  Fourier shift theorem:
//    F{f[x - dx]}[k] = F{f}[k] * exp(-2πi·k·dx/N)
//
//  To shift cmp FORWARD so that it aligns with ref:
//    desired:  cmp_aligned[x,y] = cmp[x + dx, y + dy] ≈ ref[x,y]
//    F{cmp_aligned}[k] = cmp_fft[k] * exp(+2πi·(kx·dx + ky·dy)/N)
//
//  The phase factor is  exp(+i·θ)  with  θ = 2π·(kx·dx + ky·dy)/N,
//  implemented as:
//    shifted_re = cmp_re * cos(θ) - cmp_im * sin(θ)
//    shifted_im = cmp_re * sin(θ) + cmp_im * cos(θ)
//
//  We accumulate  |ref - shifted|²  over all frequency bins and pick the
//  (dx, dy) that gives the minimum total.  The returned sub-pixel delta is
//  the shift to apply to cmp to best match ref.
//
//  Returns (sub_dx, sub_dy, confidence) where confidence = 1/(1+best_diff).
// ===========================================================================

struct FourierResult
{
    float dx, dy;       // sub-pixel shift to apply to cmp to match ref
    float confidence;
};

FourierResult FourierShiftSearch(
    const std::vector<std::complex<double>>& ref_fft,
    const std::vector<std::complex<double>>& cmp_fft,
    uint32_t fw, uint32_t fh)
{
    const size_t n = static_cast<size_t>(fw) * fh;
    const double kPi = 3.14159265358979323846;
    // 6×6 grid covering ≈ ±0.417 plane-px with step 1/6.
    // (ix - 2.5) * step  →  -0.417, -0.25, -0.083, 0.083, 0.25, 0.417
    constexpr int kGrid = 6;
    constexpr double kStep = 1.0 / 6.0;
    constexpr double kOffset = (kGrid - 1) / 2.0;  // 2.5

    double best_diff = 1e300;
    double best_sx = 0.0;
    double best_sy = 0.0;

    for (int iy = 0; iy < kGrid; ++iy)
    {
        for (int ix = 0; ix < kGrid; ++ix)
        {
            double sx = (static_cast<double>(ix) - kOffset) * kStep;
            double sy = (static_cast<double>(iy) - kOffset) * kStep;

            double diff = 0.0;
            for (uint32_t fy = 0; fy < fh; ++fy)
            {
                for (uint32_t fx = 0; fx < fw; ++fx)
                {
                    size_t k = static_cast<size_t>(fy) * fw + fx;

                    double theta = 2.0 * kPi * (static_cast<double>(fx) * sx /
                        static_cast<double>(fw) + static_cast<double>(fy) * sy /
                        static_cast<double>(fh));
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

    float confidence = static_cast<float>(1.0 / (1.0 + best_diff / static_cast<double>(n)));
    return {static_cast<float>(best_sx), static_cast<float>(best_sy), confidence};
}

// ===========================================================================
//  Frequency alignment entry point
// ===========================================================================

} // anonymous namespace

AlignmentResult EstimateFrequencyTileField(
    const std::vector<FloatImage>& ref_pyr,
    const std::vector<FloatImage>& cmp_pyr,
    const AlignParams& params)
{
    const int n_levels = static_cast<int>(ref_pyr.size());

    // ---- coarse global seed (SparseSad) ------------------------------------
    const FloatImage& coarsest_ref = ref_pyr.back();
    const FloatImage& coarsest_cmp = cmp_pyr.back();
    const int coarse_shift = AlignConstants::kSearchFractionShiftBase;
    const uint32_t coarse_longest = std::max(coarsest_ref.width, coarsest_ref.height);
    const int coarse_max_shift = std::max(AlignConstants::kMinSearchRadius,
                                           static_cast<int>(coarse_longest >> coarse_shift));

    int gdx = 0;
    int gdy = 0;
    float gbest = std::numeric_limits<float>::max();
    const uint32_t cfa = std::max<uint32_t>(1, params.cfa_period);
    const int gstep = std::max(2, static_cast<int>(cfa) * 2);

    for (int dy = -coarse_max_shift; dy <= coarse_max_shift; ++dy)
    {
        for (int dx = -coarse_max_shift; dx <= coarse_max_shift; ++dx)
        {
            float s = SparseSad2(coarsest_ref, coarsest_cmp, dx, dy, gstep);
            if (s < gbest)
            {
                gbest = s;
                gdx = dx;
                gdy = dy;
            }
        }
    }

    AlignmentResult cur;
    cur.cfa_period = cfa;
    cur.tile_size = AlignConstants::kDefaultTileSize;
    cur.tile_spacing = cur.tile_size;
    cur.tiles_x = 1;
    cur.tiles_y = 1;
    cur.tile_shift_x = {static_cast<int16_t>(gdx)};
    cur.tile_shift_y = {static_cast<int16_t>(gdy)};
    cur.shift_x = gdx;
    cur.shift_y = gdy;
    cur.confidence = 1.0f;

    const int kTileSearchR = AlignConstants::kTileSearchRadius;

    for (int level = n_levels - 1; level >= 0; --level)
    {
        const bool is_finest = (level == 0);
        const FloatImage& ref = ref_pyr[static_cast<size_t>(level)];
        const FloatImage& cmp = cmp_pyr[static_cast<size_t>(level)];

        const uint32_t ts = AlignConstants::kDefaultTileSize;
        const uint32_t half = ts / 2;
        const uint32_t fw = ts;
        const uint32_t fh = ts;

        const uint32_t nx = std::max<uint32_t>(
            1, static_cast<uint32_t>(
                std::ceil(static_cast<double>(ref.width) / half)) - 1);
        const uint32_t ny = std::max<uint32_t>(
            1, static_cast<uint32_t>(
                std::ceil(static_cast<double>(ref.height) / half)) - 1);
        const bool is_coarsest = (level == n_levels - 1);
        const int level_scale = is_coarsest ? 0 :
            static_cast<int>(ref_pyr[static_cast<size_t>(level)].width /
                             ref_pyr[static_cast<size_t>(level) + 1].width);

        std::vector<int16_t> ux(static_cast<size_t>(nx) * ny, 0);
        std::vector<int16_t> uy(static_cast<size_t>(nx) * ny, 0);
        if (!is_coarsest)
        {
            const int tile_ratio_x = std::max<int>(1, static_cast<int>(nx) /
                std::max<int>(1, static_cast<int>(cur.tiles_x)));
            const int tile_ratio_y = std::max<int>(1, static_cast<int>(ny) /
                std::max<int>(1, static_cast<int>(cur.tiles_y)));
            for (uint32_t ty = 0; ty < ny; ++ty)
            {
                for (uint32_t tx = 0; tx < nx; ++tx)
                {
                    uint32_t ppx = std::min(cur.tiles_x - 1, tx / tile_ratio_x);
                    uint32_t ppy = std::min(cur.tiles_y - 1, ty / tile_ratio_y);
                    size_t d = static_cast<size_t>(ty) * nx + tx;
                    size_t s = static_cast<size_t>(ppy) * cur.tiles_x + ppx;
                    ux[d] = static_cast<int16_t>(cur.tile_shift_x[s] * level_scale);
                    uy[d] = static_cast<int16_t>(cur.tile_shift_y[s] * level_scale);
                }
            }
        }

        AlignmentResult next;
        next.tile_size = static_cast<int32_t>(ts);
        next.tile_spacing = static_cast<int32_t>(half);
        next.cfa_period = cur.cfa_period;
        next.tiles_x = nx;
        next.tiles_y = ny;
        next.tile_shift_x.assign(static_cast<size_t>(nx) * ny, 0);
        next.tile_shift_y.assign(static_cast<size_t>(nx) * ny, 0);

        for (uint32_t ty = 0; ty < ny; ++ty)
        {
            for (uint32_t tx = 0; tx < nx; ++tx)
            {
                size_t idx = static_cast<size_t>(ty) * nx + tx;
                uint32_t x0 = tx * half;
                uint32_t y0 = ty * half;
                uint32_t tw = std::min(ts, ref.width - x0);
                uint32_t th = std::min(ts, ref.height - y0);
                int seed_x = ux[idx];
                int seed_y = uy[idx];

                int best_int_x = seed_x;
                int best_int_y = seed_y;
                float best_int_cost = std::numeric_limits<float>::max();

                for (int iy = -kTileSearchR; iy <= kTileSearchR; ++iy)
                {
                    for (int ix = -kTileSearchR; ix <= kTileSearchR; ++ix)
                    {
                        int cand_x = seed_x + ix;
                        int cand_y = seed_y + iy;
                        float cost = TileSad2(ref, cmp, x0, y0, tw, th,
                            cand_x, cand_y);
                        if (cost >= 0.0f && cost < best_int_cost)
                        {
                            best_int_cost = cost;
                            best_int_x = cand_x;
                            best_int_y = cand_y;
                        }
                    }
                }

                int total_ix = best_int_x;
                int total_iy = best_int_y;

                if (is_finest)
                {
                    int cmp_x0 = static_cast<int>(x0) + best_int_x;
                    int cmp_y0 = static_cast<int>(y0) + best_int_y;

                    if (cmp_x0 >= 0 &&
                        cmp_x0 + static_cast<int>(tw) <= static_cast<int>(cmp.width) &&
                        cmp_y0 >= 0 &&
                        cmp_y0 + static_cast<int>(th) <= static_cast<int>(cmp.height))
                    {
                        const size_t n = static_cast<size_t>(fw) * fh;
                        std::vector<std::complex<double>> ref_fft(n, 0.0);
                        std::vector<std::complex<double>> cmp_fft(n, 0.0);
                        const uint32_t ch = std::min(ref.channels, cmp.channels);
                        const double inv_ch = 1.0 / std::max<uint32_t>(1, ch);

                        for (uint32_t y = 0; y < th; ++y)
                        {
                            for (uint32_t x = 0; x < tw; ++x)
                            {
                                size_t k = static_cast<size_t>(y) * fw + x;
                                double ra = 0.0;
                                double ca = 0.0;
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

                        Fft2D(ref_fft, fw, fh, false);
                        Fft2D(cmp_fft, fw, fh, false);

                        auto fs = FourierShiftSearch(ref_fft, cmp_fft, fw, fh);

                        float total_x = static_cast<float>(best_int_x) + fs.dx;
                        float total_y = static_cast<float>(best_int_y) + fs.dy;
                        total_ix = static_cast<int>(std::lround(static_cast<double>(total_x)));
                        total_iy = static_cast<int>(std::lround(static_cast<double>(total_y)));
                    }
                }

                next.tile_shift_x[idx] = static_cast<int16_t>(total_ix);
                next.tile_shift_y[idx] = static_cast<int16_t>(total_iy);
            }
        }

        SmoothField(next);
        cur = std::move(next);
    }

    long sx = 0;
    long sy = 0;
    for (int16_t v : cur.tile_shift_x)
    {
        sx += v;
    }
    for (int16_t v : cur.tile_shift_y)
    {
        sy += v;
    }
    int n = static_cast<int>(std::max<size_t>(1, cur.tile_shift_x.size()));
    cur.shift_x = static_cast<int32_t>(std::lround(static_cast<double>(sx) / n));
    cur.shift_y = static_cast<int32_t>(std::lround(static_cast<double>(sy) / n));
    cur.confidence = 1.0f;
    return cur;
}

} // namespace burstmerge
