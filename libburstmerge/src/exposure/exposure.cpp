#include "burstmerge/internal/exposure/exposure.h"

#include <algorithm>
#include <cmath>

namespace burstmerge
{

namespace
{

FloatImage LargeBlur(const FloatImage& src, int radius)
{
    return BoxBlur(BoxBlur(src, radius), radius);
}

float MeanBlack(const ExposureParams& params)
{
    float sum = 0.0f;
    int n = 0;
    for (float v : params.black_level)
    {
        if (v > 0.0f)
        { sum += v; ++n; }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

float MinBlack(const ExposureParams& params)
{
    float out = 1e30f;
    bool any = false;
    for (float v : params.black_level)
    {
        if (v > 0.0f)
        { out = std::min(out, v); any = true; }
    }
    return any ? out : 0.0f;
}

float BlackAt(const ExposureParams& params, uint32_t x, uint32_t y)
{
    uint32_t p = std::max<uint32_t>(1, params.mosaic_pattern_width);
    uint32_t idx = (y % p) * p + (x % p);
    if (idx < 4 && params.black_level[idx] > 0.0f) return params.black_level[idx];
    return MeanBlack(params);
}

void ApplyLocalReinhard(FloatImage& image, uint32_t white_level, const ExposureParams& params)
{
    const float white = static_cast<float>(white_level);
    if (white <= 0.0f) return;

    const float black_mean = MeanBlack(params);
    const float black_min = MinBlack(params);
    const float max_value = std::max(black_min + 1.0f, MaxValue(image));
    float linear_gain = (white - black_min) / (max_value - black_min);
    linear_gain = std::max(1.0f, std::min(ExposureConstants::kMaxGain, ExposureConstants::kLinearGainFactor * linear_gain));
    float gain_stops = std::max(ExposureConstants::kMinGainStops, std::min(ExposureConstants::kMaxGainStops, params.stops - std::log2(linear_gain)));
    float gain0 = std::pow(2.0f, gain_stops - ExposureConstants::kGainStopsFalloff * std::max(0.0f, gain_stops - ExposureConstants::kGainStopsFalloffThreshold));
    float gain1 = std::pow(2.0f, gain_stops / ExposureConstants::kGain1Divisor);

    // For plane-image pipeline, compute a single luminance guide per pixel (max across channels)
    // to prevent per-channel tone mapping divergence in gradient regions.
    FloatImage guide = LargeBlur(image, params.mosaic_pattern_width > 1
        ? static_cast<int>(ExposureConstants::kLocalBlurRadius)
        : static_cast<int>(ExposureConstants::kGlobalBlurRadius));
    FloatImage luminance_guide;
    luminance_guide.width = image.width;
    luminance_guide.height = image.height;
    luminance_guide.channels = 1;
    luminance_guide.data.resize(static_cast<size_t>(image.width) * image.height, 0.0f);
    for (uint32_t y = 0; y < image.height; ++y)
    {
        for (uint32_t x = 0; x < image.width; ++x)
        {
            float max_lum = 0.0f;
            for (uint32_t c = 0; c < image.channels; ++c)
            {
                max_lum = std::max(max_lum, guide.At(x, y, c));
            }
            luminance_guide.At(x, y, 0) = max_lum;
        }
    }

    for (uint32_t y = 0; y < image.height; ++y)
    {
        for (uint32_t x = 0; x < image.width; ++x)
        {
            float rescale = std::max(1.0f, white - black_min);
            float lum_raw = luminance_guide.At(x, y, 0);
            float luminance = std::max(ExposureConstants::kReinhardLuminanceMin,
                std::min(1.0f, (lum_raw - black_mean) / (rescale * std::max(ExposureConstants::kColorFactorMin, params.color_factor_mean))));

            float luminance_after0 = linear_gain * gain0 * luminance;
            float luminance_after1 = linear_gain * gain1 * luminance;
            luminance_after0 = luminance_after0 * (ExposureConstants::kReinhardShadowMid + luminance_after0 / (gain0 * gain0)) /
                (ExposureConstants::kReinhardShadowMid + luminance_after0);
            float luminance_max = gain1 * (ExposureConstants::kReinhardHighlightMid + gain1 / (gain1 * gain1)) /
                (ExposureConstants::kReinhardHighlightMid + gain1);
            luminance_after1 = luminance_after1 * (ExposureConstants::kReinhardHighlightMid + luminance_after1 / (gain1 * gain1)) /
                ((ExposureConstants::kReinhardHighlightMid + luminance_after1) * std::max(ExposureConstants::kColorFactorMin, luminance_max));
            float weight = std::max(0.0f, std::min(1.0f, gain_stops * ExposureConstants::kWeightStopsFactor));
            float scale = ((1.0f - weight) * luminance_after0 + weight * luminance_after1) / luminance;

            for (uint32_t c = 0; c < image.channels; ++c)
            {
                float black = image.channels == 1 ? BlackAt(params, x, y) : black_mean;
                float pixel = std::max(0.0f, std::min(1.0f, (image.At(x, y, c) - black) / rescale));
                image.At(x, y, c) = std::max(0.0f, std::min(white, pixel * scale * rescale + black));
            }
        }
    }
}

} // namespace

void ApplyExposure(FloatImage& image, uint32_t white_level, const ExposureParams& params)
{
    if (params.mode == ExposureMode::Off || params.stops == 0.0f) return;

    const float gain = std::pow(2.0f, params.stops);
    const float white = static_cast<float>(white_level);

    if (params.mode == ExposureMode::Linear)
    {
        for (float& v : image.data) v = std::max(0.0f, v * gain);
        float mx = MaxValue(image);
        if (mx > white && mx > 0.0f)
        {
            float scale = white / mx;
            for (float& v : image.data) v *= scale;
        }
        return;
    }

    if (params.mode == ExposureMode::Curve)
    {
        if (params.curve_mode == ExposureCurveMode::LocalReinhard)
        {
            ApplyLocalReinhard(image, white_level, params);
            return;
        }
        for (float& v : image.data)
        {
            float x = std::max(0.0f, v * gain);
            v = white * (x / (x + white));
        }
    }
}

} // namespace burstmerge
