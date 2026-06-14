#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/align/align_common.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "burstmerge/internal/core/fft_util.h"

namespace burstmerge
{
namespace
{

// ===========================================================================
//  FFT helpers
// ===========================================================================

// ===========================================================================
//  Sparse SAD (same convention as align.cpp)
//    ref[x,y] vs cmp[x-dx, y-dy]  → best dx means cmp shifted RIGHT by dx.
// ===========================================================================

// SparseSad, TileSad, SmoothTileField are provided by align_common.h

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
    ProfileScope scope("time.align.frequency_tile_field");
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
    const int gstep = std::max(3, static_cast<int>(cfa) * 3);

    const int dy_begin = -coarse_max_shift;
    const int dy_end = coarse_max_shift + 1;
    std::vector<SearchBest> partial(static_cast<size_t>(dy_end - dy_begin));
    ParallelFor(partial.size(), 1, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i)
        {
            const int dy = dy_begin + static_cast<int>(i);
            SearchBest best;
            best.dy = dy;
            for (int dx = -coarse_max_shift; dx <= coarse_max_shift; ++dx)
            {
                float s = SparseSad(coarsest_ref, coarsest_cmp, dx, dy, gstep);
                if (s < best.score)
                {
                    best.score = s;
                    best.dx = dx;
                    best.dy = dy;
                }
            }
            partial[i] = best;
        }
    }, "freq_coarse" /* named tag for profiler */);
    for (const auto& best : partial)
    {
        if (best.score < gbest)
        {
            gbest = best.score;
            gdx = best.dx;
            gdy = best.dy;
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
            const uint32_t band_count = RecommendedBandCount(ny, kBandCountPerThread, kBandCountDenseMax);
            ParallelFor(static_cast<size_t>(band_count), 1, [&](size_t b0, size_t b1)
            {
                for (size_t band = b0; band < b1; ++band)
                {
                    const uint32_t ty_begin = (ny * static_cast<uint32_t>(band)) / band_count;
                    const uint32_t ty_end = (ny * static_cast<uint32_t>(band + 1)) / band_count;
                    for (uint32_t ty = ty_begin; ty < ty_end; ++ty)
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
            }, "freq_propagate" /* named tag for profiler */);
        }

        AlignmentResult next;
        next.tile_size = static_cast<int32_t>(ts);
        next.tile_spacing = static_cast<int32_t>(half);
        next.cfa_period = cur.cfa_period;
        next.tiles_x = nx;
        next.tiles_y = ny;
        next.tile_shift_x.assign(static_cast<size_t>(nx) * ny, 0);
        next.tile_shift_y.assign(static_cast<size_t>(nx) * ny, 0);

        const uint32_t band_count2 = RecommendedBandCount(ny, kBandCountPerThread, kBandCountDenseMax);
        ParallelFor(static_cast<size_t>(band_count2), 1, [&](size_t b0, size_t b1)
        {
            for (size_t band = b0; band < b1; ++band)
            {
                const uint32_t ty_begin = (ny * static_cast<uint32_t>(band)) / band_count2;
                const uint32_t ty_end = (ny * static_cast<uint32_t>(band + 1)) / band_count2;
                for (uint32_t ty = ty_begin; ty < ty_end; ++ty)
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

#if BURSTMERGE_ALIGN_WEIGHTED_AVG
                        double sum_w = 0.0, sum_wx = 0.0, sum_wy = 0.0;
#else
                        int best_int_x = seed_x;
                        int best_int_y = seed_y;
                        float best_int_cost = std::numeric_limits<float>::max();
#endif
                        for (int iy = -kTileSearchR; iy <= kTileSearchR; ++iy)
                        {
                            for (int ix = -kTileSearchR; ix <= kTileSearchR; ++ix)
                            {
                                int cand_x = seed_x + ix;
                                int cand_y = seed_y + iy;
                                float cost = TileSad(ref, cmp, x0, y0, tw, th,
                                    cand_x, cand_y, 1);
#if BURSTMERGE_ALIGN_WEIGHTED_AVG
                                if (cost >= 0.0f)
                                {
                                    double w = 1.0 / (static_cast<double>(cost) * static_cast<double>(cost) + 1e-8);
                                    sum_w += w;
                                    sum_wx += w * cand_x;
                                    sum_wy += w * cand_y;
                                }
#else
                                if (cost >= 0.0f && cost < best_int_cost)
                                {
                                    best_int_cost = cost;
                                    best_int_x = cand_x;
                                    best_int_y = cand_y;
                                }
#endif
                            }
                        }

#if BURSTMERGE_ALIGN_WEIGHTED_AVG
                        int total_ix = static_cast<int>(std::lround(sum_wx / sum_w));
                        int total_iy = static_cast<int>(std::lround(sum_wy / sum_w));
#else
                        int total_ix = best_int_x;
                        int total_iy = best_int_y;
#endif

                        if (is_finest)
                        {
                            int cmp_x0 = static_cast<int>(x0) + total_ix;
                            int cmp_y0 = static_cast<int>(y0) + total_iy;

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

                                float total_x = static_cast<float>(total_ix) + fs.dx;
                                float total_y = static_cast<float>(total_iy) + fs.dy;
                                total_ix = static_cast<int>(std::lround(static_cast<double>(total_x)));
                                total_iy = static_cast<int>(std::lround(static_cast<double>(total_y)));
                            }
                        }

                        next.tile_shift_x[idx] = static_cast<int16_t>(total_ix);
                        next.tile_shift_y[idx] = static_cast<int16_t>(total_iy);
                    }
                }
            }
        }, "freq_search" /* named tag for profiler */);

        SmoothTileField(next, params.smooth_tile_field);
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
