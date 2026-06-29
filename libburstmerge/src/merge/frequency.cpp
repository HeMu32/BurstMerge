#include "burstmerge/internal/merge/frequency.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"
#include "burstmerge/internal/merge/fft8.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "burstmerge/internal/core/fft_util.h"

namespace burstmerge
{
namespace
{

double RaisedCosineWindow(size_t i, size_t n)
{
    if (n <= 1) return 1.0;
    const double kPi = 3.14159265358979323846;
    return 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) /
                                static_cast<double>(n));
}

double ModifiedMismatchWindow(int i, int n)
{
    const double kPi = 3.14159265358979323846;
    return 0.5 - 0.17 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) /
                                 static_cast<double>(n));
}

double ClampDouble(double v, double lo, double hi)
{
    return std::max(lo, std::min(v, hi));
}

struct FourierPhaseTable
{
    int grid = 0;
    int tile = 0;
    int half = 0;
    double range = 0.0;
    std::vector<std::complex<float>> values;

    size_t Index(int shift_idx, size_t fy, size_t fx) const
    {
        return (static_cast<size_t>(shift_idx) * static_cast<size_t>(tile) + fy) *
            static_cast<size_t>(tile) + fx;
    }

    const std::complex<float>& At(int shift_idx, size_t fy, size_t fx) const
    {
        return values[Index(shift_idx, fy, fx)];
    }
};

const FourierPhaseTable& GetCachedPhaseTable(int grid, int tile, double range)
{
    thread_local FourierPhaseTable table;
    if (!table.values.empty() && table.grid == grid && table.tile == tile &&
        std::abs(table.range - range) < 1e-12)
    {
        return table;
    }

    table.grid = grid;
    table.tile = tile;
    table.half = grid / 2;
    table.range = range;
    table.values.assign(static_cast<size_t>(grid * grid * tile * tile),
                        std::complex<float>(1.0f, 0.0f));

    const double kPi = 3.14159265358979323846;
    int shift_idx = 0;
    for (int iy = -table.half; iy <= table.half; ++iy)
    {
        for (int ix = -table.half; ix <= table.half; ++ix, ++shift_idx)
        {
            const double sx = range * static_cast<double>(ix) /
                static_cast<double>(table.half);
            const double sy = range * static_cast<double>(iy) /
                static_cast<double>(table.half);
            for (int fy = 0; fy < tile; ++fy)
            {
                for (int fx = 0; fx < tile; ++fx)
                {
                    const double angle = -2.0 * kPi *
                        (static_cast<double>(fx) * sx / static_cast<double>(tile) +
                         static_cast<double>(fy) * sy / static_cast<double>(tile));
                    table.values[table.Index(shift_idx,
                                             static_cast<size_t>(fy),
                                             static_cast<size_t>(fx))] =
                        std::complex<float>(
                            static_cast<float>(std::cos(angle)),
                            static_cast<float>(std::sin(angle)));
                }
            }
        }
    }
    return table;
}

struct ShiftPass
{
    uint32_t x = 0;
    uint32_t y = 0;
};

struct TileStats
{
    double rms = 0.0;
    double mismatch = 0.0;
    double highlights_norm = 1.0;
    double total_weight = 0.0;
};

struct TileMergeResult
{
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t tw = 0;
    uint32_t th = 0;
    std::vector<double> window;
    std::vector<float> pixels;
};

struct StandardTileContext
{
    const FloatImage& reference;
    const std::vector<FloatImage>& aligned_comparisons;
    const FrequencyMergeParams& params;
    double robustness_norm = 0.0;
    double read_noise = 0.0;
    size_t stack_size = 0;
    /// Per-comparison exposure weight number wn = 1/exposure_scales[idx]
    /// (1.0 when exposure_weighted is false). The reference seed implicitly
    /// carries wn = 1.
    std::vector<float> comp_weight_numbers;
    /// Precomputed 1 / (1 + Sum(wn)). Equals 1/stack_size when not
    /// exposure-weighted, so the legacy normalization is preserved.
    float inv_stack = 0.0f;
};

struct RobustTileContext
{
    const FloatImage& reference;
    const std::vector<FloatImage>& aligned_comparisons;
    const FrequencyMergeParams& params;
    double robustness_norm = 0.0;
    double read_noise = 0.0;
    double max_motion_norm = 0.0;
    size_t stack_size = 0;
};

struct TileBandResult
{
    uint32_t y0 = 0;
    FloatImage out;
    FloatImage norm;
};

TileBandResult MakeBandResult(const FloatImage& reference,
                              uint32_t y0,
                              uint32_t y1)
{
    TileBandResult band;
    band.y0 = y0;
    band.out.width = reference.width;
    band.out.height = y1 - y0;
    band.out.channels = reference.channels;
    band.out.data.assign(static_cast<size_t>(band.out.width) * band.out.height * band.out.channels, 0.0f);
    band.norm.width = reference.width;
    band.norm.height = y1 - y0;
    band.norm.channels = 1;
    band.norm.data.assign(static_cast<size_t>(band.norm.width) * band.norm.height, 0.0f);
    return band;
}

void AccumulateTileResultToBand(const TileMergeResult& tile_result,
                                TileBandResult& band)
{
    const uint32_t ch = band.out.channels;
    const uint32_t w = band.out.width;
    const uint32_t by_offset = band.y0;
    const uint32_t bnd_h = band.out.height;

    for (uint32_t yy = 0; yy < tile_result.th; ++yy)
    {
        const uint32_t gy = tile_result.y0 + yy;
        if (gy < by_offset || gy >= by_offset + bnd_h) continue;
        const uint32_t by = gy - by_offset;
        const double* win = tile_result.window.data() + yy * tile_result.tw;
        const float* px = tile_result.pixels.data() + static_cast<size_t>(yy) * tile_result.tw * ch;
        float* bo = band.out.data.data() + static_cast<size_t>(by) * w * ch;
        float* bn = band.norm.data.data() + static_cast<size_t>(by) * w;
        for (uint32_t xx = 0; xx < tile_result.tw; ++xx)
        {
            const float wv = static_cast<float>(win[xx]);
            for (uint32_t c = 0; c < ch; ++c)
                bo[(tile_result.x0 + xx) * ch + c] += px[xx * ch + c];
            bn[tile_result.x0 + xx] += wv;
        }
    }
}

void MergeBandIntoImage(const TileBandResult& band,
                        FloatImage& out,
                        FloatImage& norm)
{
    const uint32_t ch = out.channels;
    const uint32_t w = band.out.width;
    const int64_t h = band.out.height;
    const uint32_t gy0 = band.y0;
    const uint32_t out_w = out.width;

    #pragma omp parallel for schedule(static)
    for (int64_t y = 0; y < h; ++y)
    {
        const float* bo = band.out.data.data() + y * w * ch;
        const float* bn = band.norm.data.data() + y * w;
        float* oo = out.data.data() + static_cast<size_t>(gy0 + y) * out_w * ch;
        float* on = norm.data.data() + static_cast<size_t>(gy0 + y) * out_w;
        for (uint32_t x = 0; x < w; ++x)
        {
            for (uint32_t c = 0; c < ch; ++c)
                oo[x * ch + c] += bo[x * ch + c];
            on[x] += bn[x];
        }
    }
}

TileStats ComputeTileStats(const FloatImage& reference,
                          const FloatImage& comparison,
                          uint32_t x0,
                          uint32_t y0,
                          size_t tw,
                          size_t th,
                          double exposure_factor,
                          const FrequencyMergeParams& params)
{
    TileStats stats;

    for (uint32_t c = 0; c < reference.channels; ++c)
    {
        double sum_sq = 0.0;
        for (size_t yy = 0; yy < th; ++yy)
        {
            for (size_t xx = 0; xx < tw; ++xx)
            {
                const double v = std::max(0.0f,
                    reference.At(x0 + static_cast<uint32_t>(xx),
                                 y0 + static_cast<uint32_t>(yy), c));
                sum_sq += v * v;
            }
        }
        stats.rms += FrequencyConstants::kRmsScale * std::sqrt(sum_sq) /
            static_cast<double>(FrequencyConstants::kWienerTileSize);
    }
    stats.rms /= std::max<uint32_t>(1, reference.channels);

    const int tile = static_cast<int>(FrequencyConstants::kWienerTileSize);
    const int x_center = static_cast<int>(x0);
    const int y_center = static_cast<int>(y0);
    const int x_start = std::max(0, x_center - tile / 2);
    const int y_start = std::max(0, y_center - tile / 2);
    const int x_end = std::min(static_cast<int>(reference.width), x_center + tile * 3 / 2);
    const int y_end = std::min(static_cast<int>(reference.height), y_center + tile * 3 / 2);
    const int x_shift = -(x_center - tile / 2);
    const int y_shift = -(y_center - tile / 2);

    double diff_sum = 0.0;
    double diff_weight = 0.0;
    for (int y = y_start; y < y_end; ++y)
    {
        for (int x = x_start; x < x_end; ++x)
        {
            const double w = ModifiedMismatchWindow(x + x_shift, tile) *
                             ModifiedMismatchWindow(y + y_shift, tile);
            double abs_diff_mean = 0.0;
            for (uint32_t c = 0; c < reference.channels; ++c)
            {
                abs_diff_mean += std::abs(
                    reference.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c) -
                    comparison.At(static_cast<uint32_t>(x), static_cast<uint32_t>(y), c));
            }
            abs_diff_mean /= std::max<uint32_t>(1, reference.channels);
            diff_sum += w * abs_diff_mean;
            diff_weight += w;
        }
    }
    const double mean_diff = diff_weight > 0.0 ? diff_sum / diff_weight : 0.0;
    const double mismatch_denom = std::sqrt(
        0.5 * stats.rms + 0.5 * stats.rms / std::max(1.0, exposure_factor) + 1.0);
    stats.mismatch = mean_diff / std::max(1e-9, mismatch_denom);

    double clipped = 0.0;
    for (size_t yy = 0; yy < th; ++yy)
    {
        for (size_t xx = 0; xx < tw; ++xx)
        {
            double pixel_max = 0.0;
            for (uint32_t c = 0; c < comparison.channels; ++c)
            {
                pixel_max = std::max(pixel_max,
                    static_cast<double>(comparison.At(
                        x0 + static_cast<uint32_t>(xx),
                        y0 + static_cast<uint32_t>(yy), c)));
            }
            // aligned comparison tiles are already normalized to reference
            // exposure; recover their original brightness before estimating
            // clipped-highlight risk.
            pixel_max = pixel_max * exposure_factor + params.black_level;
            clipped += ClampDouble(
                (pixel_max / std::max(1.0f, params.white_level) - 0.50) / 0.49,
                0.0, 1.0);
        }
    }
    clipped /= std::max<size_t>(1, tw * th);
    stats.highlights_norm = ClampDouble(
        (1.0 - clipped) * (1.0 - clipped),
        0.04 / std::min(std::max(1.0, exposure_factor), 4.0),
        1.0);

    return stats;
}

struct StandardTileBuffers
{
    std::vector<std::complex<float>> ref_spectra;
    std::vector<std::complex<float>> merged_spectra;
    std::vector<std::complex<float>> comp_spectra;
};

thread_local StandardTileBuffers tls_standard;

static void Fft2DAdaptiveF(std::complex<float>* data, size_t w, size_t h, bool inverse)
{
    if (w == 8 && h == 8)
    {
        float* raw = reinterpret_cast<float*>(data);
        if (inverse) Fft2D8x8Backward(raw);
        else Fft2D8x8Forward(raw);
        return;
    }
    std::vector<std::complex<double>> tmp(w * h);
    for (size_t i = 0; i < w * h; ++i)
        tmp[i] = static_cast<std::complex<double>>(data[i]);
    Fft2D(tmp.data(), w, h, inverse);
    for (size_t i = 0; i < w * h; ++i)
        data[i] = static_cast<std::complex<float>>(tmp[i]);
}

void ComputeStandardTileResult(const StandardTileContext& ctx,
                                        uint32_t x0,
                                        uint32_t y0,
                                        TileMergeResult& result)
{
    auto& buf = tls_standard;

    result.x0 = x0;
    result.y0 = y0;

    const uint32_t tile = static_cast<uint32_t>(FrequencyConstants::kWienerTileSize);
    const size_t tw = std::min<uint32_t>(tile, ctx.reference.width - x0);
    const size_t th = std::min<uint32_t>(tile, ctx.reference.height - y0);
    const size_t n = tw * th;
    result.tw = static_cast<uint32_t>(tw);
    result.th = static_cast<uint32_t>(th);
    result.window.resize(n, 1.0);
    result.pixels.assign(static_cast<size_t>(tw) * th * ctx.reference.channels, 0.0f);

    for (size_t yy = 0; yy < th; ++yy)
    {
        const double wy = RaisedCosineWindow(yy, th);
        for (size_t xx = 0; xx < tw; ++xx)
        {
            result.window[yy * tw + xx] = wy * RaisedCosineWindow(xx, tw);
        }
    }

    const size_t stride = n;
    buf.ref_spectra.resize(ctx.reference.channels * n);
    buf.merged_spectra.resize(ctx.reference.channels * n);
    auto& ref_spectra = buf.ref_spectra;
    auto& merged_spectra = buf.merged_spectra;

    double rms = 0.0;
    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        std::complex<float>* dst = ref_spectra.data() + c * stride;
        double sum_sq_c = 0.0;
        for (size_t yy = 0; yy < th; ++yy)
        {
            for (size_t xx = 0; xx < tw; ++xx)
            {
                const double sample = ctx.reference.At(
                    x0 + static_cast<uint32_t>(xx),
                    y0 + static_cast<uint32_t>(yy), c);
                dst[yy * tw + xx] = static_cast<float>(sample);
                const double v = std::max(0.0, sample);
                sum_sq_c += v * v;
            }
        }
        rms += FrequencyConstants::kRmsScale * std::sqrt(sum_sq_c) /
            static_cast<double>(FrequencyConstants::kWienerTileSize);
        for (size_t k = 0; k < n; ++k)
        {
            dst[k] *= static_cast<float>(result.window[k]);
        }
        Fft2DAdaptiveF(dst, tw, th, false);
        std::copy_n(dst, n, merged_spectra.data() + c * stride);
    }
    rms /= std::max<uint32_t>(1, ctx.reference.channels);

    const double noise_norm =
        (rms + ctx.read_noise) * static_cast<double>(tile * tile) * ctx.robustness_norm;

    for (size_t comp_idx = 0; comp_idx < ctx.aligned_comparisons.size(); ++comp_idx)
    {
        const FloatImage& comp_img = ctx.aligned_comparisons[comp_idx];
        // Exposure weight number for this comparison frame (1.0 for the
        // uniform / non-bracketed path). Applied only to the spectral-blend
        // accumulation below; the reference seed stays at wn = 1.
        const float wn = ctx.comp_weight_numbers.empty() ? 1.0f
            : ctx.comp_weight_numbers[comp_idx];
        buf.comp_spectra.resize(ctx.reference.channels * n);
        auto& comp_spectra = buf.comp_spectra;
        for (uint32_t c = 0; c < ctx.reference.channels; ++c)
        {
            std::complex<float>* dst = comp_spectra.data() + c * stride;
            for (size_t yy = 0; yy < th; ++yy)
            {
                for (size_t xx = 0; xx < tw; ++xx)
                {
                    dst[yy * tw + xx] = comp_img.At(
                        x0 + static_cast<uint32_t>(xx),
                        y0 + static_cast<uint32_t>(yy), c);
                }
            }
            for (size_t k = 0; k < n; ++k)
            {
                dst[k] *= static_cast<float>(result.window[k]);
            }
            Fft2DAdaptiveF(dst, tw, th, false);
        }

        const auto* ref_fp = ref_spectra.data();
        const auto* comp_fp = comp_spectra.data();

        const double kPi = 3.14159265358979323846;
        float best_diff = std::numeric_limits<float>::max();
        double best_sx = 0.0;
        double best_sy = 0.0;
        int best_shift_idx = 0;
        const int grid = FrequencyConstants::kFourierSearchGrid;
        const double range = FrequencyConstants::kFourierSearchRange;
        const int half = grid / 2;
        const bool use_phase_table =
            (tw == static_cast<size_t>(FrequencyConstants::kWienerTileSize) &&
             th == static_cast<size_t>(FrequencyConstants::kWienerTileSize));
        const auto* phase_table = use_phase_table
            ? &GetCachedPhaseTable(grid, FrequencyConstants::kWienerTileSize, range)
            : nullptr;
        int shift_idx = 0;
        for (int iy = -half; iy <= half; ++iy)
        {
            for (int ix = -half; ix <= half; ++ix)
            {
                const double sx = range * static_cast<double>(ix) /
                    static_cast<double>(half);
                const double sy = range * static_cast<double>(iy) /
                    static_cast<double>(half);
                float diff_sum = 0.0f;
                if (use_phase_table)
                {
                    for (size_t fy = 0; fy < th; ++fy)
                    {
                        for (size_t fx = 0; fx < tw; ++fx)
                        {
                            const auto phase = phase_table->At(shift_idx, fy, fx);
                            const size_t k = fy * tw + fx;
                            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                            {
                                const auto d = ref_fp[c * stride + k] -
                                    comp_fp[c * stride + k] * phase;
                                diff_sum += d.real() * d.real() + d.imag() * d.imag();
                            }
                        }
                    }
                }
                else
                {
                    for (size_t fy = 0; fy < th; ++fy)
                    {
                        for (size_t fx = 0; fx < tw; ++fx)
                        {
                            const std::complex<float> phase(
                                static_cast<float>(std::cos(-2.0 * kPi *
                                    (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                     static_cast<double>(fy) * sy / static_cast<double>(th)))),
                                static_cast<float>(std::sin(-2.0 * kPi *
                                    (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                     static_cast<double>(fy) * sy / static_cast<double>(th)))));
                            const size_t k = fy * tw + fx;
                            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                            {
                                const auto d = ref_fp[c * stride + k] -
                                    comp_fp[c * stride + k] * phase;
                                diff_sum += d.real() * d.real() + d.imag() * d.imag();
                            }
                        }
                    }
                }
                if (diff_sum < best_diff)
                {
                    best_diff = diff_sum;
                    best_sx = sx;
                    best_sy = sy;
                    best_shift_idx = shift_idx;
                }
                ++shift_idx;
            }
        }

        for (size_t fy = 0; fy < th; ++fy)
        {
            for (size_t fx = 0; fx < tw; ++fx)
            {
                const size_t k = fy * tw + fx;
                const std::complex<float> phase = use_phase_table
                    ? phase_table->At(best_shift_idx, fy, fx)
                    : std::complex<float>(
                        static_cast<float>(std::cos(-2.0 * kPi *
                            (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                             static_cast<double>(fy) * best_sy / static_cast<double>(th)))),
                        static_cast<float>(std::sin(-2.0 * kPi *
                            (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                             static_cast<double>(fy) * best_sy / static_cast<double>(th)))));
                double noise_term = std::max(1e-9, noise_norm);
                double w_c[8];
                std::complex<float> shifted[8];
                for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                {
                    shifted[c] = comp_spectra[c * stride + k] * phase;
                    const float dr = ref_spectra[c * stride + k].real() - shifted[c].real();
                    const float di = ref_spectra[c * stride + k].imag() - shifted[c].imag();
                    double d2 = static_cast<double>(dr * dr + di * di);
                    w_c[c] = d2 / (d2 + noise_term);
                }

                double weight = 0.0;
                if (ctx.reference.channels >= 4)
                {
                    double wmin = w_c[0], wmax = w_c[0], wsum = w_c[0];
                    for (uint32_t i = 1; i < ctx.reference.channels; ++i)
                    {
                        wmin = std::min(wmin, w_c[i]);
                        wmax = std::max(wmax, w_c[i]);
                        wsum += w_c[i];
                    }
                    weight = 0.5 * (wsum - wmin - wmax);
                }
                else
                {
                    for (uint32_t i = 0; i < ctx.reference.channels; ++i) weight += w_c[i];
                    weight /= static_cast<double>(std::max<uint32_t>(1, ctx.reference.channels));
                }
                weight = ClampDouble(weight, 0.0, 1.0);
                
                for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                {
                    if (ctx.params.exposure_weighted)
                    {
                        // wn augments the existing Wiener trust `weight` (which is
                        // preserved unchanged); it scales this comparison frame's
                        // whole blend contribution.
                        merged_spectra[c * stride + k] += wn *
                            (static_cast<float>(1.0 - weight) * shifted[c] +
                             static_cast<float>(weight) * ref_spectra[c * stride + k]);
                    }
                    else
                    {
                        // Legacy path: expression kept verbatim so -ffast-math
                        // FMA contraction is bit-identical for non-bracketed
                        // bursts (wn == 1 here, but the compiler cannot see that
                        // through the wn*() wrapper above).
                        merged_spectra[c * stride + k] +=
                            static_cast<float>(1.0 - weight) * shifted[c] +
                            static_cast<float>(weight) * ref_spectra[c * stride + k];
                    }
                }
            }
        }
    }

    // EV-weighted normalization (== 1/stack_size when not exposure-weighted).
    const float inv_stack = ctx.inv_stack;
    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        std::complex<float>* src = merged_spectra.data() + c * stride;
        Fft2DAdaptiveF(src, tw, th, true);
        for (size_t yy = 0; yy < th; ++yy)
        {
            for (size_t xx = 0; xx < tw; ++xx)
            {
                const size_t k = yy * tw + xx;
                const size_t out_idx = (yy * tw + xx) * ctx.reference.channels + c;
                result.pixels[out_idx] = src[k].real() * inv_stack;
            }
        }
    }
}

struct CompTileStats
{
    TileStats stats;
    double exposure_factor = 1.0;
};

struct RobustTileBuffers
{
    std::vector<std::complex<float>> ref_spectra;
    std::vector<std::complex<float>> merged_spectra;
    std::vector<std::complex<float>> comp_spectra;
    std::vector<CompTileStats> comp_stats;
};

thread_local RobustTileBuffers tls_robust;

void ComputeRobustTileResult(const RobustTileContext& ctx,
                                        uint32_t x0,
                                        uint32_t y0,
                                        TileMergeResult& result)
{
    auto& buf = tls_robust;

    result.x0 = x0;
    result.y0 = y0;

    const uint32_t tile = static_cast<uint32_t>(FrequencyConstants::kWienerTileSize);
    const size_t tw = std::min<uint32_t>(tile, ctx.reference.width - x0);
    const size_t th = std::min<uint32_t>(tile, ctx.reference.height - y0);
    const size_t n = tw * th;
    result.tw = static_cast<uint32_t>(tw);
    result.th = static_cast<uint32_t>(th);
    result.window.resize(n, 1.0);
    result.pixels.assign(static_cast<size_t>(tw) * th * ctx.reference.channels, 0.0f);

    for (size_t yy = 0; yy < th; ++yy)
    {
        const double wy = RaisedCosineWindow(yy, th);
        for (size_t xx = 0; xx < tw; ++xx)
        {
            result.window[yy * tw + xx] = wy * RaisedCosineWindow(xx, tw);
        }
    }

    const size_t stride = n;
    buf.ref_spectra.resize(ctx.reference.channels * n);
    buf.merged_spectra.resize(ctx.reference.channels * n);
    auto& ref_spectra = buf.ref_spectra;
    auto& merged_spectra = buf.merged_spectra;

    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        std::complex<float>* dst = ref_spectra.data() + c * stride;
        for (size_t yy = 0; yy < th; ++yy)
        {
            for (size_t xx = 0; xx < tw; ++xx)
            {
                dst[yy * tw + xx] = ctx.reference.At(
                    x0 + static_cast<uint32_t>(xx),
                    y0 + static_cast<uint32_t>(yy), c);
            }
        }
        for (size_t k = 0; k < n; ++k)
        {
            dst[k] *= static_cast<float>(result.window[k]);
        }
        Fft2DAdaptiveF(dst, tw, th, false);
        std::copy_n(dst, n, merged_spectra.data() + c * stride);
    }

    struct CompTileStats
    {
        TileStats stats;
        double exposure_factor = 1.0;
    };
    buf.comp_stats.resize(ctx.aligned_comparisons.size());
    auto& comp_stats = buf.comp_stats;
    for (size_t comp_idx = 0; comp_idx < ctx.aligned_comparisons.size(); ++comp_idx)
    {
        const double exposure_factor =
            (ctx.params.exposure_scales != nullptr && comp_idx < static_cast<size_t>(ctx.params.num_scales))
            ? 1.0 / std::max(1e-6f, ctx.params.exposure_scales[comp_idx])
            : 1.0;
        comp_stats[comp_idx].exposure_factor = exposure_factor;
        {
            ProfileScope scope("time.merge.wiener_tile_stats");
            comp_stats[comp_idx].stats = ComputeTileStats(ctx.reference,
                                                           ctx.aligned_comparisons[comp_idx],
                                                           x0,
                                                           y0,
                                                           tw,
                                                           th,
                                                           exposure_factor,
                                                           ctx.params);
        }
    }

    const double mean_mismatch = comp_stats.empty() ? 0.0 : [&]()
    {
        double sum = 0.0;
        for (const auto& item : comp_stats) sum += item.stats.mismatch;
        return sum / static_cast<double>(comp_stats.size());
    }();
    const double mismatch_scale = FrequencyConstants::kMismatchTargetMean /
        std::max(1e-12, mean_mismatch);
    for (auto& item : comp_stats)
    {
        item.stats.mismatch = ClampDouble(item.stats.mismatch * mismatch_scale,
                                          0.0, 1.0);
    }

    const double noise_norm =
        (comp_stats.empty() ? 0.0 : comp_stats[0].stats.rms) + ctx.read_noise;

    {
        ProfileScope scope("time.merge.wiener_comp_merge");
        for (size_t comp_idx = 0; comp_idx < ctx.aligned_comparisons.size(); ++comp_idx)
        {
            const FloatImage& comp_img = ctx.aligned_comparisons[comp_idx];
            const auto& tile_stats = comp_stats[comp_idx];
            std::vector<std::complex<float>>& comp_spectra = buf.comp_spectra;
            comp_spectra.resize(ctx.reference.channels * n);
            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
            {
                std::complex<float>* dst = comp_spectra.data() + c * stride;
                for (size_t yy = 0; yy < th; ++yy)
                {
                    for (size_t xx = 0; xx < tw; ++xx)
                    {
                        dst[yy * tw + xx] = comp_img.At(
                            x0 + static_cast<uint32_t>(xx),
                            y0 + static_cast<uint32_t>(yy), c);
                    }
                }
                for (size_t k = 0; k < n; ++k)
                {
                    dst[k] *= static_cast<float>(result.window[k]);
                }
                Fft2DAdaptiveF(dst, tw, th, false);
            }

            const auto* ref_fp = ref_spectra.data();
            const auto* comp_fp = comp_spectra.data();

            const double kPi = 3.14159265358979323846;
            float best_diff = std::numeric_limits<float>::max();
            double best_sx = 0.0;
            double best_sy = 0.0;
            int best_shift_idx = 0;
            const int grid = FrequencyConstants::kFourierSearchGrid;
            const double range = FrequencyConstants::kFourierSearchRange;
            const int half = grid / 2;
            const bool use_phase_table =
                (tw == static_cast<size_t>(FrequencyConstants::kWienerTileSize) &&
                 th == static_cast<size_t>(FrequencyConstants::kWienerTileSize));
            const auto* phase_table = use_phase_table
                ? &GetCachedPhaseTable(grid, FrequencyConstants::kWienerTileSize, range)
                : nullptr;
            int shift_idx = 0;
            for (int iy = -half; iy <= half; ++iy)
            {
                for (int ix = -half; ix <= half; ++ix)
                {
                    const double sx = range * static_cast<double>(ix) /
                        static_cast<double>(half);
                    const double sy = range * static_cast<double>(iy) /
                        static_cast<double>(half);
                    float diff_sum = 0.0f;
                    if (use_phase_table)
                    {
                        for (size_t fy = 0; fy < th; ++fy)
                        {
                            for (size_t fx = 0; fx < tw; ++fx)
                            {
                                const auto phase = phase_table->At(shift_idx, fy, fx);
                                const size_t k = fy * tw + fx;
                                for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                                {
                                    const auto d = ref_fp[c * stride + k] -
                                        comp_fp[c * stride + k] * phase;
                                    diff_sum += d.real() * d.real() + d.imag() * d.imag();
                                }
                            }
                        }
                    }
                    else
                    {
                        for (size_t fy = 0; fy < th; ++fy)
                        {
                            for (size_t fx = 0; fx < tw; ++fx)
                            {
                                const std::complex<float> phase(
                                    static_cast<float>(std::cos(-2.0 * kPi *
                                        (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                         static_cast<double>(fy) * sy / static_cast<double>(th)))),
                                    static_cast<float>(std::sin(-2.0 * kPi *
                                        (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                         static_cast<double>(fy) * sy / static_cast<double>(th)))));
                                const size_t k = fy * tw + fx;
                                for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                                {
                                    const auto d = ref_fp[c * stride + k] -
                                        comp_fp[c * stride + k] * phase;
                                    diff_sum += d.real() * d.real() + d.imag() * d.imag();
                                }
                            }
                        }
                    }
                    if (diff_sum < best_diff)
                    {
                        best_diff = diff_sum;
                        best_sx = sx;
                        best_sy = sy;
                        best_shift_idx = shift_idx;
                    }
                    ++shift_idx;
                }
            }

            const double mismatch_weight = ClampDouble(
                1.0 - 10.0 * (tile_stats.stats.mismatch - 0.2), 0.0, 1.0);
            const double motion_norm = ClampDouble(
                ctx.max_motion_norm - (tile_stats.stats.mismatch - 0.02) *
                (ctx.max_motion_norm - 1.0) / 0.15,
                1.0, ctx.max_motion_norm);
            const double motion_norm_exposure =
                std::min(4.0, tile_stats.exposure_factor) * std::sqrt(motion_norm);

            for (size_t fy = 0; fy < th; ++fy)
            {
                for (size_t fx = 0; fx < tw; ++fx)
                {
                    const size_t k = fy * tw + fx;
                    const std::complex<float> phase = use_phase_table
                        ? phase_table->At(best_shift_idx, fy, fx)
                        : std::complex<float>(
                            static_cast<float>(std::cos(-2.0 * kPi *
                                (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                                 static_cast<double>(fy) * best_sy / static_cast<double>(th)))),
                            static_cast<float>(std::sin(-2.0 * kPi *
                                (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                                 static_cast<double>(fy) * best_sy / static_cast<double>(th)))));
                    float ref_mag_sum = 0.0f;
                    float shifted_mag_sum = 0.0f;
                    float w_c[8];
                    std::complex<float> shifted[8];
                    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                    {
                        shifted[c] = comp_spectra[c * stride + k] * phase;
                        w_c[c] = std::norm(ref_spectra[c * stride + k] - shifted[c]);
                        ref_mag_sum += std::abs(ref_spectra[c * stride + k]);
                        shifted_mag_sum += std::abs(shifted[c]);
                    }
                    double magnitude_norm = 1.0;
                    if (fx + fy > 0 && tile_stats.stats.mismatch < 0.3 &&
                        std::abs(tile_stats.exposure_factor - 1.0) < 1e-3)
                    {
                        const double ratio = shifted_mag_sum /
                            std::max(1e-12, static_cast<double>(ref_mag_sum));
                        magnitude_norm = mismatch_weight *
                            ClampDouble(ratio * ratio * ratio * ratio, 0.5, 3.0);
                    }
                    const double noise_term = 
                        magnitude_norm * motion_norm_exposure *
                        (noise_norm * static_cast<double>(tile * tile) * ctx.robustness_norm) *
                        tile_stats.stats.highlights_norm;
                    
                    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                    {
                        w_c[c] = w_c[c] / (w_c[c] + std::max(1e-9f, static_cast<float>(noise_term)));
                    }

                    float weight = 0.0f;
                    if (ctx.reference.channels >= 4)
                    {
                        float wmin = w_c[0], wmax = w_c[0], wsum = w_c[0];
                        for (uint32_t i = 1; i < ctx.reference.channels; ++i)
                        {
                            wmin = std::min(wmin, w_c[i]);
                            wmax = std::max(wmax, w_c[i]);
                            wsum += w_c[i];
                        }
                        weight = 0.5f * (wsum - wmin - wmax);
                    }
                    else
                    {
                        for (uint32_t i = 0; i < ctx.reference.channels; ++i) weight += w_c[i];
                        weight /= static_cast<float>(std::max<uint32_t>(1, ctx.reference.channels));
                    }
                    weight = std::clamp(weight, 0.0f, 1.0f);

                    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                    {
                        merged_spectra[c * stride + k] +=
                            (1.0f - weight) * shifted[c] + weight * ref_spectra[c * stride + k];
                    }
                }
            }
        }
    }

    double total_mismatch = 0.0;
    for (const auto& item : comp_stats) total_mismatch += item.stats.mismatch;
    const double mismatch_avg = comp_stats.empty() ? 0.0 :
        total_mismatch / static_cast<double>(comp_stats.size());


    double magnitude_zero = 0.0;
    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        magnitude_zero += std::abs(merged_spectra[c * stride]);
    }

    for (size_t fy = 0; fy < th; ++fy)
    {
        for (size_t fx = 0; fx < tw; ++fx)
        {
            if (fx + fy == 0 || mismatch_avg >= 0.3) continue;
            const size_t k = fy * tw + fx;

            double magnitude = 0.0;
            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
            {
                magnitude += std::abs(merged_spectra[c * stride + k]);
            }

            const double mismatch_weight = ClampDouble(
                1.0 - 10.0 * (mismatch_avg - 0.2), 0.0, 1.0);
            const double deconv_weight = mismatch_weight * ClampDouble(
                1.25 - 25.0 * magnitude / std::max(1e-12, magnitude_zero), 0.0, 1.0);
            static const double cw[8] =
            {0.00, 0.02, 0.04, 0.08, 0.04, 0.08, 0.04, 0.02};

            const float w_factor = static_cast<float>(
                (1.0 + deconv_weight * cw[std::min<size_t>(fx, 7)]) *
                (1.0 + deconv_weight * cw[std::min<size_t>(fy, 7)]));

            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
            {
                merged_spectra[c * stride + k] *= w_factor;
            }
        }
    }

    {
    const float inv_stack = 1.0f / static_cast<float>(ctx.stack_size);
        for (uint32_t c = 0; c < ctx.reference.channels; ++c)
        {
            std::complex<float>* ms = merged_spectra.data() + c * stride;
            Fft2DAdaptiveF(ms, tw, th, true);
            for (size_t yy = 0; yy < th; ++yy)
            {
                for (size_t xx = 0; xx < tw; ++xx)
                {
                    const size_t k = yy * tw + xx;
                    const size_t out_idx = (yy * tw + xx) * ctx.reference.channels + c;
                    result.pixels[out_idx] = ms[k].real() * inv_stack;
                }
            }
        }
    }
}

void ReduceTileBorderArtifacts(FloatImage& image,
                               const FloatImage& reference,
                               uint32_t tile,
                               float black_level)
{
    (void)reference;
    (void)tile;
    (void)black_level;

    const float min_value = -1.0f;
    float* dst = image.data.data();
    const int64_t n = static_cast<int64_t>(image.data.size());

    #pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < n; ++i)
    {
        if (dst[i] < min_value) dst[i] = min_value;
    }
}

FloatImage WienerFftMergeRobust(const FloatImage& reference,
                                const std::vector<FloatImage>& aligned_comparisons,
                                const FrequencyMergeParams& params)
{
    ProfileScope scope("time.merge.wiener_robust_total");
    if (aligned_comparisons.empty()) return reference;

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    // Track how many passes actually contribute to each pixel. Border pixels
    // (first/last stride rows and columns) are only covered by a subset of
    // the kWienerShiftPasses shift-offset passes. Dividing by the total pass
    // count instead of the actual contributor count halves the output at
    // borders, producing dark edge artifacts.
    FloatImage pass_count;
    pass_count.width = reference.width;
    pass_count.height = reference.height;
    pass_count.channels = 1;
    pass_count.data.assign(static_cast<size_t>(reference.width) * reference.height, 0.0f);

    const uint32_t tile = static_cast<uint32_t>(FrequencyConstants::kWienerTileSize);
    const uint32_t stride = std::max<uint32_t>(1, tile / 2);
    const double robustness_rev = 0.5 *
        (FrequencyConstants::kRobustnessRevOffset - static_cast<double>(static_cast<int>(params.noise_reduction + 0.5f)));
    const double robustness_norm = std::pow(2.0, -robustness_rev + FrequencyConstants::kRobustnessNormBase);
    const double read_noise = std::pow(std::pow(2.0, -robustness_rev + FrequencyConstants::kReadNoiseBase), FrequencyConstants::kReadNoiseExp);
    const double max_motion_norm = std::max(1.0, std::pow(1.3, 11.0 - robustness_rev));
    const RobustTileContext ctx{reference, aligned_comparisons, params,
                                robustness_norm, read_noise, max_motion_norm,
                                aligned_comparisons.size() + 1};

    const ShiftPass passes[FrequencyConstants::kWienerShiftPasses] =
    {
        {0, 0},
        {stride, 0},
        {0, stride},
        {stride, stride},
    };

    FloatImage pass_out;
    pass_out.width = reference.width;
    pass_out.height = reference.height;
    pass_out.channels = reference.channels;
    pass_out.data.resize(reference.data.size(), 0.0f);

    FloatImage pass_norm;
    pass_norm.width = reference.width;
    pass_norm.height = reference.height;
    pass_norm.channels = 1;
    pass_norm.data.resize(static_cast<size_t>(reference.width) * reference.height, 0.0f);

    for (const auto& pass : passes)
    {
        std::fill(pass_out.data.begin(), pass_out.data.end(), 0.0f);
        std::fill(pass_norm.data.begin(), pass_norm.data.end(), 0.0f);

        std::vector<uint32_t> y_coords;
        for (uint32_t y0 = pass.y; y0 < reference.height; y0 += stride)
        {
            y_coords.push_back(y0);
        }

        const size_t band_count = RecommendedBandCount(static_cast<uint32_t>(y_coords.size()), kBandCountPerThread, kBandCountMax);
        std::vector<TileBandResult> bands(band_count);
        ParallelFor(band_count, 1, [&](size_t b0, size_t b1)
        {
            for (size_t b = b0; b < b1; ++b)
            {
                const size_t row_begin = (y_coords.size() * b) / band_count;
                const size_t row_end = (y_coords.size() * (b + 1)) / band_count;
                if (row_begin >= row_end)
                {
                    bands[b] = MakeBandResult(reference, 0, 0);
                    continue;
                }
                const uint32_t band_y0 = y_coords[row_begin];
                const uint32_t band_y1 = std::min<uint32_t>(reference.height,
                    y_coords[row_end - 1] + tile);
                TileBandResult band = MakeBandResult(reference, band_y0, band_y1);
                TileMergeResult tile_result;
                for (size_t row = row_begin; row < row_end; ++row)
                {
                    const uint32_t y0 = y_coords[row];
                    for (uint32_t x0 = pass.x; x0 < reference.width; x0 += stride)
                    {
                        ComputeRobustTileResult(ctx, x0, y0, tile_result);
                        AccumulateTileResultToBand(tile_result, band);
                    }
                }
                bands[b] = std::move(band);
            }
        }, "freq_merge_robust");

        for (const auto& band : bands)
        {
            MergeBandIntoImage(band, pass_out, pass_norm);
        }

        {
            float* po = pass_out.data.data();
            const float* pn = pass_norm.data.data();
            const uint32_t ch = pass_out.channels;
            const int64_t npix = static_cast<int64_t>(pass_out.width) * pass_out.height;

            #pragma omp parallel for schedule(static)
            for (int64_t i = 0; i < npix; ++i)
            {
                const float w = std::max(1e-6f, pn[i]);
                for (uint32_t c = 0; c < ch; ++c)
                    po[i * ch + c] /= w;
            }
        }

        ReduceTileBorderArtifacts(pass_out, reference, tile, params.black_level);

        {
            const float* po = pass_out.data.data();
            const float* pn = pass_norm.data.data();
            float* o = out.data.data();
            float* pc = pass_count.data.data();
            const uint32_t ch = out.channels;
            const int64_t npix = static_cast<int64_t>(out.width) * out.height;

            #pragma omp parallel for schedule(static)
            for (int64_t i = 0; i < npix; ++i)
            {
                if (pn[i] > 1e-6f)
                    pc[i] += 1.0f;
                for (uint32_t c = 0; c < ch; ++c)
                    o[i * ch + c] += po[i * ch + c];
            }
        }
    }

    {
        float* o = out.data.data();
        const float* pc = pass_count.data.data();
        const uint32_t ch = out.channels;
        const int64_t npix = static_cast<int64_t>(out.width) * out.height;

        #pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < npix; ++i)
        {
            const float cnt = std::max(1.0f, pc[i]);
            for (uint32_t c = 0; c < ch; ++c)
                o[i * ch + c] /= cnt;
        }
    }

    return out;
}

FloatImage WienerFftMerge(const FloatImage& reference,
                                const std::vector<FloatImage>& aligned_comparisons,
                                const FrequencyMergeParams& params)
{
    ProfileScope scope("time.merge.wiener_standard_total");
    if (aligned_comparisons.empty()) return reference;

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    FloatImage norm;
    norm.width = reference.width;
    norm.height = reference.height;
    norm.channels = 1;
    norm.data.resize(static_cast<size_t>(reference.width) * reference.height, 0.0f);

    const uint32_t tile = static_cast<uint32_t>(FrequencyConstants::kWienerTileSize);
    const uint32_t stride = std::max<uint32_t>(1, tile / 2);
    const double robustness_rev = 0.5 *
        (FrequencyConstants::kRobustnessRevOffset - static_cast<double>(static_cast<int>(params.noise_reduction + 0.5f)));
    const double robustness_norm = std::pow(2.0, -robustness_rev + FrequencyConstants::kRobustnessNormBase);
    const double read_noise = std::pow(std::pow(2.0, -robustness_rev + FrequencyConstants::kReadNoiseBase), FrequencyConstants::kReadNoiseExp);

    // Exposure-bracketing weight numbers + EV-weighted normalization. wn[idx]
    // stays 1.0 for the uniform / non-bracketed path, so weight_acc collapses to
    // the frame count and inv_stack == 1/stack_size -- the legacy behaviour.
    // Accumulate in float (not double) so that, when every wn == 1.0, weight_acc
    // is bit-identical to the integer stack_size and inv_stack reproduces the
    // legacy `1.0f / stack_size` exactly (no 1-ULP drift on uniform bursts).
    std::vector<float> comp_wn(aligned_comparisons.size(), 1.0f);
    if (params.exposure_weighted && params.exposure_scales)
    {
        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
        {
            if (idx < params.num_scales && params.exposure_scales[idx] > 0.0f)
                comp_wn[idx] = 1.0f / params.exposure_scales[idx];
        }
    }
    float weight_acc = 1.0f;  // reference seed contributes wn = 1
    for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
        weight_acc += comp_wn[idx];
    const float inv_stack = 1.0f / weight_acc;

    const StandardTileContext ctx{reference, aligned_comparisons, params,
                                robustness_norm, read_noise,
                                aligned_comparisons.size() + 1,
                                std::move(comp_wn), inv_stack};

    std::vector<uint32_t> y_coords;
    for (uint32_t y0 = 0; y0 < reference.height; y0 += stride)
    {
        y_coords.push_back(y0);
    }

    const size_t band_count = std::min<size_t>(y_coords.size(), std::max<size_t>(1, ParallelismHint() * 2));
    std::vector<TileBandResult> bands(band_count);
    ParallelFor(band_count, 1, [&](size_t b0, size_t b1)
    {
        for (size_t b = b0; b < b1; ++b)
        {
            const size_t row_begin = (y_coords.size() * b) / band_count;
            const size_t row_end = (y_coords.size() * (b + 1)) / band_count;
            if (row_begin >= row_end)
            {
                bands[b] = MakeBandResult(reference, 0, 0);
                continue;
            }
            const uint32_t band_y0 = y_coords[row_begin];
            const uint32_t band_y1 = std::min<uint32_t>(reference.height,
                y_coords[row_end - 1] + tile);
            TileBandResult band = MakeBandResult(reference, band_y0, band_y1);
            TileMergeResult tile_result;
            for (size_t row = row_begin; row < row_end; ++row)
            {
                const uint32_t y0 = y_coords[row];
                for (uint32_t x0 = 0; x0 < reference.width; x0 += stride)
                {
                    ComputeStandardTileResult(ctx, x0, y0, tile_result);
                    AccumulateTileResultToBand(tile_result, band);
                }
            }
            bands[b] = std::move(band);
        }
    }, "freq_merge_standard" /* named tag for profiler */);

    for (const auto& band : bands)
    {
        MergeBandIntoImage(band, out, norm);
    }

    {
        float* o = out.data.data();
        const float* nrm = norm.data.data();
        const uint32_t ch = out.channels;
        const int64_t npix = static_cast<int64_t>(out.width) * out.height;

        #pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < npix; ++i)
        {
            const float w = std::max(1e-6f, nrm[i]);
            for (uint32_t c = 0; c < ch; ++c)
                o[i * ch + c] /= w;
        }
    }

    return out;
}

} // namespace

FloatImage FrequencyMerge(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const FrequencyMergeParams& params)
{
    ProfileScope scope("time.merge.frequency_total");
    if (params.mode == FrequencyMode::WienerFft)
    {
        return WienerFftMerge(reference, aligned_comparisons, params);
    }

    if (params.mode == FrequencyMode::WienerFftRobust)
    {
        // WienerFftRobust retains its own Swift-style exposure handling
        // (per-tile noise-term scaling, highlights_norm, motion_norm_exposure)
        // and therefore intentionally IGNORES params.exposure_weighted. Only
        // the Laplacian and standard-Wiener paths consume the wn augmentation.
        if (params.white_level < 0.0f || params.black_level < 0.0f)
        {
            throw std::runtime_error(
                "FrequencyMerge(WienerFftRobust) requires valid white_level and black_level; "
                "the caller must populate FrequencyMergeParams from RAW metadata.");
        }
        return WienerFftMergeRobust(reference, aligned_comparisons, params);
    }

    const int blur_radius = std::max(1, params.tile_size / FrequencyConstants::kLaplacianBlurDiv);
    FloatImage ref_low = BoxBlur(reference, blur_radius);
    std::vector<FloatImage> comp_low;
    comp_low.reserve(aligned_comparisons.size());
    for (const auto& img : aligned_comparisons)
    {
        comp_low.push_back(BoxBlur(img, blur_radius));
    }

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    // Exposure-bracketing weight numbers for the low-frequency average. The
    // high-frequency max-abs selection below is intentionally left untouched
    // (it is not a weighted average). wn[idx]==1 for the uniform / non-bracketed
    // path, so the legacy equal-weight low-frequency average is preserved.
    std::vector<float> cmp_wn(aligned_comparisons.size(), 1.0f);
    if (params.exposure_weighted && params.exposure_scales)
    {
        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
        {
            if (idx < params.num_scales && params.exposure_scales[idx] > 0.0f)
                cmp_wn[idx] = 1.0f / params.exposure_scales[idx];
        }
    }

    for (size_t i = 0; i < out.data.size(); ++i)
    {
        float low_sum = ref_low.data[i];
        float low_weight = 1.0f;
        float high = reference.data[i] - ref_low.data[i];

        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
        {
            float comp_high = aligned_comparisons[idx].data[i] - comp_low[idx].data[i];
            // EV-weight the low-frequency accumulation; reference seed is wn=1.
            low_sum += comp_low[idx].data[i] * cmp_wn[idx];
            low_weight += cmp_wn[idx];
            if (std::abs(comp_high) > std::abs(high)) high = comp_high;
        }

        out.data[i] = low_sum / low_weight + high;
    }

    return out;
}

} // namespace burstmerge
