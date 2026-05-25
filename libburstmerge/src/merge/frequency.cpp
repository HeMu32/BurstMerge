#include "burstmerge/internal/merge/frequency.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <complex>
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
    std::vector<std::complex<double>> values;

    size_t Index(int shift_idx, size_t fy, size_t fx) const
    {
        return (static_cast<size_t>(shift_idx) * static_cast<size_t>(tile) + fy) *
            static_cast<size_t>(tile) + fx;
    }

    const std::complex<double>& At(int shift_idx, size_t fy, size_t fx) const
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
                        std::complex<double>(1.0, 0.0));

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
                        std::complex<double>(std::cos(angle), std::sin(angle));
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
    for (uint32_t yy = 0; yy < tile_result.th; ++yy)
    {
        const uint32_t gy = tile_result.y0 + yy;
        if (gy < band.y0 || gy >= band.y0 + band.out.height) continue;
        const uint32_t by = gy - band.y0;
        for (uint32_t xx = 0; xx < tile_result.tw; ++xx)
        {
            const size_t k = static_cast<size_t>(yy) * tile_result.tw + xx;
            const double w = tile_result.window[k];
            for (uint32_t c = 0; c < band.out.channels; ++c)
            {
                const size_t src_idx = (k * band.out.channels) + c;
                band.out.At(tile_result.x0 + xx, by, c) += tile_result.pixels[src_idx];
            }
            band.norm.At(tile_result.x0 + xx, by, 0) += static_cast<float>(w);
        }
    }
}

void MergeBandIntoImage(const TileBandResult& band,
                        FloatImage& out,
                        FloatImage& norm)
{
    for (uint32_t y = 0; y < band.out.height; ++y)
    {
        const uint32_t gy = band.y0 + y;
        for (uint32_t x = 0; x < band.out.width; ++x)
        {
            for (uint32_t c = 0; c < band.out.channels; ++c)
            {
                out.At(x, gy, c) += band.out.At(x, y, c);
            }
            norm.At(x, gy, 0) += band.norm.At(x, y, 0);
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
            std::max<size_t>(1, tw * th);
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

    if (exposure_factor > 1.001)
    {
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
                pixel_max = (pixel_max - params.black_level) * exposure_factor + params.black_level;
                clipped += ClampDouble(
                    (pixel_max / std::max(1.0f, params.white_level) - 0.50) / 0.49,
                    0.0, 1.0);
            }
        }
        clipped /= std::max<size_t>(1, tw * th);
        stats.highlights_norm = ClampDouble(
            (1.0 - clipped) * (1.0 - clipped),
            0.04 / std::min(exposure_factor, 4.0),
            1.0);
    }

    return stats;
}

TileMergeResult ComputeStandardTileResult(const StandardTileContext& ctx,
                                        uint32_t x0,
                                        uint32_t y0)
{
    TileMergeResult result;
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
    std::vector<std::complex<double>> ref_spectra(ctx.reference.channels * n);
    std::vector<std::complex<double>> merged_spectra(ctx.reference.channels * n);

    double rms = 0.0;
    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        std::complex<double>* dst = ref_spectra.data() + c * stride;
        for (size_t yy = 0; yy < th; ++yy)
        {
            for (size_t xx = 0; xx < tw; ++xx)
            {
                const double sample = ctx.reference.At(
                    x0 + static_cast<uint32_t>(xx),
                    y0 + static_cast<uint32_t>(yy), c);
                dst[yy * tw + xx] = sample;
                rms += std::max(0.0, sample) * std::max(0.0, sample);
            }
        }
        Fft2D(dst, tw, th, false);
        std::copy_n(dst, n, merged_spectra.data() + c * stride);
    }
    rms = FrequencyConstants::kRmsScale * std::sqrt(rms) /
        std::max<size_t>(1, n * ctx.reference.channels);
    const double noise_norm =
        (rms + ctx.read_noise) * static_cast<double>(tile * tile) * ctx.robustness_norm;

    for (const auto& comp_img : ctx.aligned_comparisons)
    {
        std::vector<std::complex<double>> comp_spectra(ctx.reference.channels * n);
        for (uint32_t c = 0; c < ctx.reference.channels; ++c)
        {
            std::complex<double>* dst = comp_spectra.data() + c * stride;
            for (size_t yy = 0; yy < th; ++yy)
            {
                for (size_t xx = 0; xx < tw; ++xx)
                {
                    dst[yy * tw + xx] = comp_img.At(
                        x0 + static_cast<uint32_t>(xx),
                        y0 + static_cast<uint32_t>(yy), c);
                }
            }
            Fft2D(dst, tw, th, false);
        }

        const double kPi = 3.14159265358979323846;
        double best_diff = 1e300;
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
                double diff_sum = 0.0;
                for (size_t fy = 0; fy < th; ++fy)
                {
                    for (size_t fx = 0; fx < tw; ++fx)
                    {
                        const size_t k = fy * tw + fx;
                        const std::complex<double> phase = use_phase_table
                            ? phase_table->At(shift_idx, fy, fx)
                            : std::complex<double>(
                                std::cos(-2.0 * kPi *
                                    (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                     static_cast<double>(fy) * sy / static_cast<double>(th))),
                                std::sin(-2.0 * kPi *
                                    (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                     static_cast<double>(fy) * sy / static_cast<double>(th))));
                        for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                        {
                            const auto d = ref_spectra[c * stride + k] -
                                comp_spectra[c * stride + k] * phase;
                            diff_sum += std::norm(d);
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
                const std::complex<double> phase = use_phase_table
                    ? phase_table->At(best_shift_idx, fy, fx)
                    : std::complex<double>(
                        std::cos(-2.0 * kPi *
                            (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                             static_cast<double>(fy) * best_sy / static_cast<double>(th))),
                        std::sin(-2.0 * kPi *
                            (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                             static_cast<double>(fy) * best_sy / static_cast<double>(th))));
                double d2_mean = 0.0;
                std::vector<std::complex<double>> shifted(ctx.reference.channels);
                for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                {
                    shifted[c] = comp_spectra[c * stride + k] * phase;
                    d2_mean += std::norm(ref_spectra[c * stride + k] - shifted[c]);
                }
                d2_mean /= static_cast<double>(ctx.reference.channels);
                double weight = d2_mean / (d2_mean + std::max(1e-9, noise_norm));
                if (k == 0) weight = 0.0;
                for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                {
                    merged_spectra[c * stride + k] +=
                        (1.0 - weight) * shifted[c] + weight * ref_spectra[c * stride + k];
                }
            }
        }
    }

    const double inv_stack = 1.0 / static_cast<double>(ctx.stack_size);
    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        std::complex<double>* src = merged_spectra.data() + c * stride;
        Fft2D(src, tw, th, true);
        for (size_t yy = 0; yy < th; ++yy)
        {
            for (size_t xx = 0; xx < tw; ++xx)
            {
                const size_t k = yy * tw + xx;
                const size_t out_idx = (yy * tw + xx) * ctx.reference.channels + c;
                result.pixels[out_idx] =
                    static_cast<float>(src[k].real() * inv_stack * result.window[k]);
            }
        }
    }

    return result;
}

TileMergeResult ComputeRobustTileResult(const RobustTileContext& ctx,
                                        uint32_t x0,
                                        uint32_t y0)
{
    TileMergeResult result;
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
    std::vector<std::complex<double>> ref_spectra(ctx.reference.channels * n);
    std::vector<std::complex<double>> merged_spectra(ctx.reference.channels * n);

    {
        ProfileScope scope("time.merge.wiener_ref_fft");
        for (uint32_t c = 0; c < ctx.reference.channels; ++c)
        {
            std::complex<double>* dst = ref_spectra.data() + c * stride;
            for (size_t yy = 0; yy < th; ++yy)
            {
                for (size_t xx = 0; xx < tw; ++xx)
                {
                    const double v = std::max(0.0,
                        static_cast<double>(ctx.reference.At(
                            x0 + static_cast<uint32_t>(xx),
                            y0 + static_cast<uint32_t>(yy), c)));
                    dst[yy * tw + xx] = v;
                }
            }
            Fft2D(dst, tw, th, false);
            std::copy_n(dst, n, merged_spectra.data() + c * stride);
        }
    }

    struct CompTileStats
    {
        TileStats stats;
        double exposure_factor = 1.0;
    };
    std::vector<CompTileStats> comp_stats(ctx.aligned_comparisons.size());
    for (size_t comp_idx = 0; comp_idx < ctx.aligned_comparisons.size(); ++comp_idx)
    {
        const double exposure_factor =
            (ctx.params.exposure_scales != nullptr && comp_idx < static_cast<size_t>(ctx.params.num_scales))
            ? std::max(1e-6f, ctx.params.exposure_scales[comp_idx])
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
            std::vector<std::complex<double>> comp_spectra(ctx.reference.channels * n);
            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
            {
                std::complex<double>* dst = comp_spectra.data() + c * stride;
                for (size_t yy = 0; yy < th; ++yy)
                {
                    for (size_t xx = 0; xx < tw; ++xx)
                    {
                        dst[yy * tw + xx] = comp_img.At(
                            x0 + static_cast<uint32_t>(xx),
                            y0 + static_cast<uint32_t>(yy), c);
                    }
                }
                Fft2D(dst, tw, th, false);
            }

            const double kPi = 3.14159265358979323846;
            double best_diff = 1e300;
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
                    double diff_sum = 0.0;
                    for (size_t fy = 0; fy < th; ++fy)
                    {
                        for (size_t fx = 0; fx < tw; ++fx)
                        {
                            const std::complex<double> phase = use_phase_table
                                ? phase_table->At(shift_idx, fy, fx)
                                : std::complex<double>(
                                    std::cos(-2.0 * kPi *
                                        (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                         static_cast<double>(fy) * sy / static_cast<double>(th))),
                                    std::sin(-2.0 * kPi *
                                        (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                         static_cast<double>(fy) * sy / static_cast<double>(th))));
                            for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                            {
                                const auto d = ref_spectra[c * stride + fy * tw + fx] -
                                    comp_spectra[c * stride + fy * tw + fx] * phase;
                                diff_sum += std::norm(d);
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
                    const std::complex<double> phase = use_phase_table
                        ? phase_table->At(best_shift_idx, fy, fx)
                        : std::complex<double>(
                            std::cos(-2.0 * kPi *
                                (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                                 static_cast<double>(fy) * best_sy / static_cast<double>(th))),
                            std::sin(-2.0 * kPi *
                                (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                                 static_cast<double>(fy) * best_sy / static_cast<double>(th))));
                    double d2_mean = 0.0;
                    double ref_mag_sum = 0.0;
                    double shifted_mag_sum = 0.0;
                    std::complex<double> shifted[4];
                    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                    {
                        shifted[c] = comp_spectra[c * stride + k] * phase;
                        d2_mean += std::norm(ref_spectra[c * stride + k] - shifted[c]);
                        ref_mag_sum += std::abs(ref_spectra[c * stride + k]);
                        shifted_mag_sum += std::abs(shifted[c]);
                    }
                    d2_mean /= static_cast<double>(ctx.reference.channels);
                    double magnitude_norm = 1.0;
                    if (fx + fy > 0 && tile_stats.stats.mismatch < 0.3 &&
                        std::abs(tile_stats.exposure_factor - 1.0) < 1e-3)
                    {
                        const double ratio = shifted_mag_sum /
                            std::max(1e-12, ref_mag_sum);
                        magnitude_norm = mismatch_weight *
                            ClampDouble(ratio * ratio * ratio * ratio, 0.5, 3.0);
                    }
                    const double denom = d2_mean +
                        magnitude_norm * motion_norm_exposure *
                        (noise_norm * static_cast<double>(tile * tile) * ctx.robustness_norm) *
                        tile_stats.stats.highlights_norm;
                    double weight = d2_mean / std::max(1e-9, denom);
                    if (k == 0) weight = 0.0;
                    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
                    {
                        merged_spectra[c * stride + k] +=
                            (1.0 - weight) * shifted[c] + weight * ref_spectra[c * stride + k];
                    }
                }
            }
        }
    }

    double total_mismatch = 0.0;
    for (const auto& item : comp_stats) total_mismatch += item.stats.mismatch;
    const double mismatch_avg = comp_stats.empty() ? 0.0 :
        total_mismatch / static_cast<double>(comp_stats.size());

    for (uint32_t c = 0; c < ctx.reference.channels; ++c)
    {
        std::complex<double>* ms = merged_spectra.data() + c * stride;
        const double magnitude_zero = std::abs(ms[0]);
        for (size_t fy = 0; fy < th; ++fy)
        {
            for (size_t fx = 0; fx < tw; ++fx)
            {
                if (fx + fy == 0 || mismatch_avg >= 0.3) continue;
                const size_t k = fy * tw + fx;
                const double magnitude = std::abs(ms[k]);
                const double mismatch_weight = ClampDouble(
                    1.0 - 10.0 * (mismatch_avg - 0.2), 0.0, 1.0);
                const double deconv_weight = mismatch_weight * ClampDouble(
                    1.25 - 25.0 * magnitude / std::max(1e-12, magnitude_zero), 0.0, 1.0);
                static const double cw[8] =
                {0.00, 0.02, 0.04, 0.08, 0.04, 0.08, 0.04, 0.02};
                ms[k] *=
                    (1.0 + deconv_weight * cw[std::min<size_t>(fx, 7)]) *
                    (1.0 + deconv_weight * cw[std::min<size_t>(fy, 7)]);
            }
        }
    }

    {
        ProfileScope scope("time.merge.wiener_ifft");
        const double inv_stack = 1.0 / static_cast<double>(ctx.stack_size);
        for (uint32_t c = 0; c < ctx.reference.channels; ++c)
        {
            std::complex<double>* ms = merged_spectra.data() + c * stride;
            Fft2D(ms, tw, th, true);
            for (size_t yy = 0; yy < th; ++yy)
            {
                for (size_t xx = 0; xx < tw; ++xx)
                {
                    const size_t k = yy * tw + xx;
                    const size_t out_idx = (yy * tw + xx) * ctx.reference.channels + c;
                    result.pixels[out_idx] =
                        static_cast<float>(ms[k].real() * inv_stack * result.window[k]);
                }
            }
        }
    }

    return result;
}

void ReduceTileBorderArtifacts(FloatImage& image,
                               const FloatImage& reference,
                               uint32_t tile,
                               float black_level)
{
    (void)reference;
    (void)tile;

    // In our by-plane CPU path, overlap-add already suppresses most tile seams.
    // Re-blending every internal 8x8 tile border with the reference creates a
    // visible grid after plane->mosaic reconstruction, so keep only the safe
    // negative-value clamp from the reference implementation.
    const float min_value = black_level - 1.0f;
    for (uint32_t y = 0; y < image.height; ++y)
    {
        for (uint32_t x = 0; x < image.width; ++x)
        {
            for (uint32_t c = 0; c < image.channels; ++c)
            {
                float& dst = image.At(x, y, c);
                dst = std::max(dst, min_value);
            }
        }
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

    for (const auto& pass : passes)
    {
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
                for (size_t row = row_begin; row < row_end; ++row)
                {
                    const uint32_t y0 = y_coords[row];
                    for (uint32_t x0 = pass.x; x0 < reference.width; x0 += stride)
                    {
                        TileMergeResult tile_result = ComputeRobustTileResult(ctx, x0, y0);
                        AccumulateTileResultToBand(tile_result, band);
                    }
                }
                bands[b] = std::move(band);
            }
        }, "freq_merge_robust" /* named tag for profiler */);

        for (const auto& band : bands)
        {
            MergeBandIntoImage(band, pass_out, pass_norm);
        }

        for (uint32_t y = 0; y < pass_out.height; ++y)
        {
            for (uint32_t x = 0; x < pass_out.width; ++x)
            {
                const float w = std::max(1e-6f, pass_norm.At(x, y, 0));
                for (uint32_t c = 0; c < pass_out.channels; ++c)
                {
                    pass_out.At(x, y, c) /= w;
                }
            }
        }

        ReduceTileBorderArtifacts(pass_out, reference, tile, params.black_level);
        for (size_t i = 0; i < out.data.size(); ++i)
        {
            out.data[i] += pass_out.data[i] /
                static_cast<float>(FrequencyConstants::kWienerShiftPasses);
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
    const StandardTileContext ctx{reference, aligned_comparisons, params,
                                robustness_norm, read_noise,
                                aligned_comparisons.size() + 1};

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
            for (size_t row = row_begin; row < row_end; ++row)
            {
                const uint32_t y0 = y_coords[row];
                for (uint32_t x0 = 0; x0 < reference.width; x0 += stride)
                {
                    TileMergeResult tile_result = ComputeStandardTileResult(ctx, x0, y0);
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

    for (uint32_t y = 0; y < out.height; ++y)
    {
        for (uint32_t x = 0; x < out.width; ++x)
        {
            const float w = std::max(1e-6f, norm.At(x, y, 0));
            for (uint32_t c = 0; c < out.channels; ++c)
            {
                out.At(x, y, c) /= w;
            }
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

    for (size_t i = 0; i < out.data.size(); ++i)
    {
        float low_sum = ref_low.data[i];
        float low_weight = 1.0f;
        float high = reference.data[i] - ref_low.data[i];

        for (size_t idx = 0; idx < aligned_comparisons.size(); ++idx)
        {
            float comp_high = aligned_comparisons[idx].data[i] - comp_low[idx].data[i];
            low_sum += comp_low[idx].data[i];
            low_weight += 1.0f;
            if (std::abs(comp_high) > std::abs(high)) high = comp_high;
        }

        out.data[i] = low_sum / low_weight + high;
    }

    return out;
}

} // namespace burstmerge
