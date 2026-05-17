#include "burstmerge/internal/merge/frequency.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

extern "C"
{
#include "pocketfft.h"
}

namespace burstmerge
{
namespace
{

void Fft2D(std::vector<std::complex<double>>& data, size_t w, size_t h, bool inverse)
{
    if (w == 0 || h == 0) return;
    cfft_plan row_plan = make_cfft_plan(w);
    cfft_plan col_plan = make_cfft_plan(h);
    std::vector<double> scratch(2 * std::max(w, h), 0.0);

    for (size_t y = 0; y < h; ++y)
    {
        for (size_t x = 0; x < w; ++x)
        {
            auto v = data[y * w + x];
            scratch[2 * x] = v.real();
            scratch[2 * x + 1] = v.imag();
        }
        if (inverse) cfft_backward(row_plan, scratch.data(), 1.0);
        else cfft_forward(row_plan, scratch.data(), 1.0);
        for (size_t x = 0; x < w; ++x)
        {
            data[y * w + x] =
            {scratch[2 * x], scratch[2 * x + 1]};
        }
    }

    for (size_t x = 0; x < w; ++x)
    {
        for (size_t y = 0; y < h; ++y)
        {
            auto v = data[y * w + x];
            scratch[2 * y] = v.real();
            scratch[2 * y + 1] = v.imag();
        }
        if (inverse) cfft_backward(col_plan, scratch.data(), 1.0 / static_cast<double>(w * h));
        else cfft_forward(col_plan, scratch.data(), 1.0);
        for (size_t y = 0; y < h; ++y)
        {
            data[y * w + x] =
            {scratch[2 * y], scratch[2 * y + 1]};
        }
    }

    destroy_cfft_plan(row_plan);
    destroy_cfft_plan(col_plan);
}

FloatImage WienerFftMerge(const FloatImage& reference,
                          const std::vector<FloatImage>& aligned_comparisons,
                          const FrequencyMergeParams& params)
                          {
    if (aligned_comparisons.empty()) return reference;

    FloatImage out;
    out.width = reference.width;
    out.height = reference.height;
    out.channels = reference.channels;
    out.data.resize(reference.data.size(), 0.0f);

    std::vector<const FloatImage*> stack;
    stack.reserve(aligned_comparisons.size() + 1);
    stack.push_back(&reference);
    for (const auto& img : aligned_comparisons) stack.push_back(&img);

    const uint32_t tile = static_cast<uint32_t>(std::max<int32_t>(FrequencyConstants::kMinTileSize, params.tile_size));
    const double robustness_rev = 0.5 *
        (FrequencyConstants::kRobustnessRevOffset - static_cast<double>(static_cast<int>(params.noise_reduction + 0.5f)));
    const double robustness_norm = std::pow(2.0, -robustness_rev + FrequencyConstants::kRobustnessNormBase);
    const double read_noise = std::pow(std::pow(2.0, -robustness_rev + FrequencyConstants::kReadNoiseBase), FrequencyConstants::kReadNoiseExp);

    for (uint32_t y0 = 0; y0 < reference.height; y0 += tile)
    {
        for (uint32_t x0 = 0; x0 < reference.width; x0 += tile)
        {
            const size_t tw = std::min<uint32_t>(tile, reference.width - x0);
            const size_t th = std::min<uint32_t>(tile, reference.height - y0);
            const size_t n = tw * th;

            for (uint32_t c = 0; c < reference.channels; ++c)
            {
                std::vector<std::complex<double>> ref_spectrum(n);
                for (size_t yy = 0; yy < th; ++yy)
                {
                    for (size_t xx = 0; xx < tw; ++xx)
                    {
                        ref_spectrum[yy * tw + xx] = reference.At(x0 + static_cast<uint32_t>(xx),
                                                                   y0 + static_cast<uint32_t>(yy), c);
                    }
                }
                Fft2D(ref_spectrum, tw, th, false);
                std::vector<std::complex<double>> merged = ref_spectrum;

                double rms = 0.0;
                for (size_t yy = 0; yy < th; ++yy)
                {
                    for (size_t xx = 0; xx < tw; ++xx)
                    {
                        double v = std::max(0.0f, reference.At(x0 + static_cast<uint32_t>(xx), y0 + static_cast<uint32_t>(yy), c));
                        rms += v * v;
                    }
                }
                rms = FrequencyConstants::kRmsScale * std::sqrt(rms) / std::max<size_t>(1, n);
                double noise_norm = (rms + read_noise) * static_cast<double>(tile * tile) * robustness_norm;

                for (const auto& comp_img : aligned_comparisons)
                {
                    std::vector<std::complex<double>> comp_spectrum(n);
                    const FloatImage& img = comp_img;
                    for (size_t yy = 0; yy < th; ++yy)
                    {
                        for (size_t xx = 0; xx < tw; ++xx)
                        {
                            comp_spectrum[yy * tw + xx] = img.At(x0 + static_cast<uint32_t>(xx),
                                                                  y0 + static_cast<uint32_t>(yy), c);
                        }
                    }
                    Fft2D(comp_spectrum, tw, th, false);

                    const double kPi = 3.14159265358979323846;
                    double best_diff = 1e300;
                    double best_sx = 0.0;
                    double best_sy = 0.0;
                    const int grid = FrequencyConstants::kFourierSearchGrid;
                    const double range = FrequencyConstants::kFourierSearchRange;
                    const int half = grid / 2;
                    for (int iy = -half; iy <= half; ++iy)
                    {
                        for (int ix = -half; ix <= half; ++ix)
                        {
                            double sx = range * static_cast<double>(ix) / static_cast<double>(half);
                            double sy = range * static_cast<double>(iy) / static_cast<double>(half);
                            double diff_sum = 0.0;
                            for (size_t fy = 0; fy < th; ++fy)
                            {
                                for (size_t fx = 0; fx < tw; ++fx)
                                {
                                    double angle = -2.0 * kPi * (static_cast<double>(fx) * sx / static_cast<double>(tw) +
                                                                  static_cast<double>(fy) * sy / static_cast<double>(th));
                                    std::complex<double> phase(std::cos(angle), std::sin(angle));
                                    auto d = ref_spectrum[fy * tw + fx] - comp_spectrum[fy * tw + fx] * phase;
                                    diff_sum += std::norm(d);
                                }
                            }
                            if (diff_sum < best_diff)
                            {
                                best_diff = diff_sum;
                                best_sx = sx;
                                best_sy = sy;
                            }
                        }
                    }

                    for (size_t fy = 0; fy < th; ++fy)
                    {
                        for (size_t fx = 0; fx < tw; ++fx)
                        {
                            size_t k = fy * tw + fx;
                            double angle = -2.0 * kPi * (static_cast<double>(fx) * best_sx / static_cast<double>(tw) +
                                                          static_cast<double>(fy) * best_sy / static_cast<double>(th));
                            std::complex<double> shifted = comp_spectrum[k] * std::complex<double>(std::cos(angle), std::sin(angle));
                            double d2 = std::norm(ref_spectrum[k] - shifted);
                            double weight = d2 / (d2 + std::max(1e-9, noise_norm));
                            if (k == 0) weight = 0.0;
                            merged[k] += (1.0 - weight) * shifted + weight * ref_spectrum[k];
                        }
                    }
                }

                Fft2D(merged, tw, th, true);
                double inv_stack = 1.0 / static_cast<double>(stack.size());
                for (size_t yy = 0; yy < th; ++yy)
                {
                    for (size_t xx = 0; xx < tw; ++xx)
                    {
                        out.At(x0 + static_cast<uint32_t>(xx), y0 + static_cast<uint32_t>(yy), c) =
                            static_cast<float>(merged[yy * tw + xx].real() * inv_stack);
                    }
                }
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
    if (params.mode == FrequencyMode::WienerFft)
    {
        return WienerFftMerge(reference, aligned_comparisons, params);
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
