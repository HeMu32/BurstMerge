#include "burstmerge/internal/core/pipeline_frame.h"

#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace burstmerge
{

// Frame statistics and normalization helpers used by the orchestrator but not
// part of orchestration control flow itself.

bool IsCompatibleForAverage(const RawImage& a, const RawImage& b)
{
    return a.pixels.width == b.pixels.width &&
           a.pixels.height == b.pixels.height &&
           a.pixels.format == b.pixels.format &&
           a.pixels.row_stride == b.pixels.row_stride;
}

// Map the user-facing noise-reduction knob to the internal spatial-merge
// robustness scalar.
//
// Direction is critical and was previously inverted here. The spatial weight
// formula `w = 1 / (1 + (diff/noise_floor)^2 * robustness)` means LARGER
// robustness => LESS averaging => LESS denoising. So if robustness grows with
// nr (as the old `nr / 13` mapping did), turning UP noise-reduction actually
// reduces denoising -- the opposite of what the user expects.
//
// The reference implementation (hdr-plus-swift, spatial.swift:21-22) inverts
// the relationship with an exponential decay, replicated here exactly. `nr` is
// rounded the same way Swift's `Int(noise_reduction + 0.5)` rounds (half-up
// for the positive values nr always takes). Intermediate math is done in
// double to match frequency.cpp's robustness path; the pow() result is then
// narrowed to float. The kRobustnessMin floor keeps a small residual rejection
// for extreme nr (see pipeline.h for the rationale).
float ComputeRobustness(float noise_reduction)
{
    const int nr_int = static_cast<int>(std::floor(noise_reduction + 0.5f));
    const double rev = static_cast<double>(PipelineConstants::kRobustnessRevHalf) *
                       (static_cast<double>(PipelineConstants::kRobustnessRevOffset) -
                        static_cast<double>(nr_int));
    const double r = static_cast<double>(PipelineConstants::kRobustnessBase) *
                         std::pow(static_cast<double>(PipelineConstants::kRobustnessExpBase), rev) -
                     static_cast<double>(PipelineConstants::kRobustnessSubtract);
    return std::max(PipelineConstants::kRobustnessMin, static_cast<float>(r));
}

float EstimateNoiseFloor(const FloatImage& image, uint32_t guide_block_size)
{
    ProfileScope scope("time.pipeline.estimate_noise_floor");
    if (image.data.empty()) return 8.0f;

    const int blur_radius = 2;
    const FloatImage blurred = BoxBlur(image, blur_radius);
    const uint32_t step = std::max<uint32_t>(1, guide_block_size);

    const uint32_t sample_rows = (image.height + step - 1) / step;
    const uint32_t grain_rows = std::max<uint32_t>(1,
        RecommendedImageRowGrain(image.width, image.channels, kRowGrainMinPixels / 2, kRowGrainMinRows) / std::max<uint32_t>(1, step));
    std::vector<double> partial_sum_sq(sample_rows, 0.0);
    std::vector<uint64_t> partial_count(sample_rows, 0);
    ParallelForRows(image.height, grain_rows, [&](uint32_t y0, uint32_t y1)
    {
        uint32_t sample_idx = y0 / step;
        for (uint32_t y = y0; y < y1; y += step, ++sample_idx)
        {
            double local_sum_sq = 0.0;
            uint64_t local_count = 0;
            for (uint32_t x = 0; x < image.width; x += step)
            {
                size_t idx = (static_cast<size_t>(y) * image.width + x) * image.channels;
                for (uint32_t c = 0; c < image.channels; ++c)
                {
                    float d = image.data[idx + c] - blurred.data[idx + c];
                    local_sum_sq += static_cast<double>(d) * static_cast<double>(d);
                    ++local_count;
                }
            }
            partial_sum_sq[sample_idx] = local_sum_sq;
            partial_count[sample_idx] = local_count;
        }
    }, "estimate_noise" /* named tag for profiler */);

    double sum_sq = std::accumulate(partial_sum_sq.begin(), partial_sum_sq.end(), 0.0);
    uint64_t count = std::accumulate(partial_count.begin(), partial_count.end(), uint64_t(0));

    if (count == 0) return 8.0f;
    float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
    return std::max(PipelineConstants::kNoiseFloorMin, rms);
}

float MeanBlackLevel(const RawMetadata& meta)
{
    float sum = 0.0f;
    int n = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (meta.black_level[i] > 0.0f)
        { sum += meta.black_level[i]; ++n; }
    }
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}

void NormalizeFrames(std::vector<FloatImage>& float_images,
                     const std::vector<RawImage>& raw_images,
                     size_t ref_idx)
{
    ProfileScope scope("time.pipeline.normalize_frames");
    float ref_ev = raw_images[ref_idx].metadata.ev_value;

    ParallelFor(float_images.size(), 1, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i)
        {
            const auto& meta = raw_images[i].metadata;
            FloatImage& img = float_images[i];

            float bl = MeanBlackLevel(meta);
            if (bl > 1.0f)
            {
                ParallelFor(img.data.size(), 1u << 16, [&](size_t p0, size_t p1)
                {
                    for (size_t p = p0; p < p1; ++p) img.data[p] -= bl;
                }, "normalize_black" /* named tag for profiler */);
            }

            if (i == ref_idx) continue;

            float comp_ev = meta.ev_value;
            if (ref_ev > 0.0f && comp_ev > 0.0f)
            {
                float scale = (ref_ev / comp_ev) *
                              std::pow(2.0f,
                                       raw_images[ref_idx].metadata.exposure_bias - meta.exposure_bias);
                if (std::abs(scale - 1.0f) > 0.001f)
                {
                    ParallelFor(img.data.size(), 1u << 16, [&](size_t p0, size_t p1)
                    {
                        for (size_t p = p0; p < p1; ++p) img.data[p] *= scale;
                    }, "normalize_exposure" /* named tag for profiler */);
                }
            }
        }
    }, "normalize_frame" /* named tag for profiler */);
}

std::vector<FloatImage> BuildFloatImages(const std::vector<RawImage>& images)
{
    ProfileScope scope("time.pipeline.build_float_images");
    std::vector<FloatImage> out;
    out.reserve(images.size());
    for (const auto& img : images)
    {
        FloatImage fi = HostBufferToFloatImage(img.pixels);
        if (img.metadata.mosaic_pattern_width > 1 && fi.channels == 1)
        {
            fi = ConvertMosaicToPlaneImage(fi, img.metadata.mosaic_pattern_width);
        }
        out.push_back(std::move(fi));
    }
    return out;
}

size_t SelectExposureRefIndex(const std::vector<RawImage>& images)
{
    if (images.empty()) return 0;

    bool has_exposure = false;
    float min_exp = std::numeric_limits<float>::max();
    float max_exp = 0.0f;
    for (const auto& img : images)
    {
        float v = img.metadata.ev_value;
        if (v > 0.0f)
        {
            has_exposure = true;
            min_exp = std::min(min_exp, v);
            max_exp = std::max(max_exp, v);
        }
    }

    if (has_exposure && max_exp > min_exp * 1.25f)
    {
        std::vector<std::pair<float, size_t>> exposure_order;
        exposure_order.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            float v = images[i].metadata.ev_value;
            if (v > 0.0f) exposure_order.push_back({v, i});
        }
        if (!exposure_order.empty())
        {
            std::sort(exposure_order.begin(), exposure_order.end(),
                      [](const auto& a, const auto& b)
                      { return a.first < b.first; });
            return exposure_order.front().second;
        }
    }

    return images.size() / 2;
}

FloatImage DecodedImageToFloatImage(const io::DecodedImage& img)
{
    FloatImage fi;
    fi.width = img.info.width;
    fi.height = img.info.height;
    fi.channels = img.info.pix_fmt & 0xFF;
    fi.data = img.pixels;
    return fi;
}

std::vector<FloatImage> BuildRgbImages(const std::vector<io::DecodedImage>& images)
{
    std::vector<FloatImage> out;
    out.reserve(images.size());
    for (const auto& img : images)
    {
        out.push_back(DecodedImageToFloatImage(img));
    }
    return out;
}

// --- Highlight recovery ---

namespace {

float BlendGreenExtrapolation(float original, float extrapolated, float ratio)
{
    float t = 1.0f - ratio;
    if (t < 0.0f) t = 0.0f;
    if (t > HighlightRecoveryParams::kWeightClampRange)
        t = HighlightRecoveryParams::kWeightClampRange;
    float weight = HighlightRecoveryParams::kExtrapolationWeightBase -
                   HighlightRecoveryParams::kExtrapolationWeightSlope * t;
    float max_val = extrapolated > original ? extrapolated : original;
    return weight * max_val + (1.0f - weight) * original;
}

void RecoverHighlightsBayer(FloatImage& img,
                            float effective_range,
                            const float factor_for_ch[4],
                            const std::array<uint16_t, 36>& mosaic_pattern)
{
    const uint32_t W = img.width;
    const uint32_t H = img.height;
    const float inv_range = 1.0f / effective_range;

    ParallelForRows(H,
        RecommendedImageRowGrain(W, 4, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t oy = y_begin; oy < y_end; ++oy)
        {
            for (uint32_t ox = 0; ox < W; ++ox)
            {
                for (int g_ch = 0; g_ch < 4; ++g_ch)
                {
                    if (mosaic_pattern[g_ch] != 1) continue;

                    const int g_px = g_ch % 2;
                    const int g_py = g_ch / 2;
                    const int h_ch = g_py * 2 + (1 - g_px);
                    const int v_ch = (1 - g_py) * 2 + g_px;

                    float pv_g = img.At(ox, oy, g_ch);
                    float ratio_g = pv_g * inv_range;
                    if (ratio_g <= HighlightRecoveryParams::kBrightRatioThreshold)
                        continue;

                    float pv_h_same = img.At(ox, oy, h_ch);
                    float pv_v_same = img.At(ox, oy, v_ch);
                    float ratio_h_same = pv_h_same * inv_range;
                    float ratio_v_same = pv_v_same * inv_range;

                    float pv_h_adj = 0.0f;
                    float pv_v_adj = 0.0f;
                    float ratio_h_adj = 0.0f;
                    float ratio_v_adj = 0.0f;
                    float pixel_count = 2.0f;

                    if (g_px == 1)
                    {
                        if (ox + 1 < W)
                        {
                            pv_h_adj = img.At(ox + 1, oy, h_ch);
                            ratio_h_adj = pv_h_adj * inv_range;
                            pixel_count += 1.0f;
                        }
                    }
                    else
                    {
                        if (ox > 0)
                        {
                            pv_h_adj = img.At(ox - 1, oy, h_ch);
                            ratio_h_adj = pv_h_adj * inv_range;
                            pixel_count += 1.0f;
                        }
                    }

                    if (g_py == 0)
                    {
                        if (oy > 0)
                        {
                            pv_v_adj = img.At(ox, oy - 1, v_ch);
                            ratio_v_adj = pv_v_adj * inv_range;
                            pixel_count += 1.0f;
                        }
                    }
                    else
                    {
                        if (oy + 1 < H)
                        {
                            pv_v_adj = img.At(ox, oy + 1, v_ch);
                            ratio_v_adj = pv_v_adj * inv_range;
                            pixel_count += 1.0f;
                        }
                    }

                    float clip_h = HighlightRecoveryParams::kNeighbourClipRatio * factor_for_ch[h_ch];
                    float clip_v = HighlightRecoveryParams::kNeighbourClipRatio * factor_for_ch[v_ch];

                    if (!(ratio_h_same > clip_h || ratio_v_same > clip_v ||
                          ratio_h_adj > clip_h || ratio_v_adj > clip_v))
                        continue;

                    float extrapolated =
                        ((pv_h_same + pv_h_adj) / factor_for_ch[h_ch] +
                         (pv_v_same + pv_v_adj) / factor_for_ch[v_ch]) / pixel_count;

                    img.At(ox, oy, g_ch) =
                        BlendGreenExtrapolation(pv_g, extrapolated, ratio_g);
                }
            }
        }
    }, "recover_highlights_bayer");
}

void RecoverHighlightsLinear(FloatImage& img, float effective_range,
                              float factor_r, float factor_b)
{
    const uint32_t W = img.width;
    const uint32_t H = img.height;
    const float inv_range = 1.0f / effective_range;
    const float clip_r = HighlightRecoveryParams::kNeighbourClipRatio * factor_r;
    const float clip_b = HighlightRecoveryParams::kNeighbourClipRatio * factor_b;

    ParallelForRows(H,
        RecommendedImageRowGrain(W, 3, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < W; ++x)
            {
                float pv_g = img.At(x, y, 1);
                float ratio_g = pv_g * inv_range;
                if (ratio_g <= HighlightRecoveryParams::kBrightRatioThreshold)
                    continue;

                float pv_r = img.At(x, y, 0);
                float pv_b = img.At(x, y, 2);
                float ratio_r = pv_r * inv_range;
                float ratio_b = pv_b * inv_range;

                if (!(ratio_r > clip_r || ratio_b > clip_b))
                    continue;

                float extrapolated = (pv_r / factor_r + pv_b / factor_b) * 0.5f;
                img.At(x, y, 1) =
                    BlendGreenExtrapolation(pv_g, extrapolated, ratio_g);
            }
        }
    }, "recover_highlights_linear");
}

void RecoverHighlightsMosaic(FloatImage& img, uint32_t period,
                              float effective_range,
                              float factor_r, float factor_b,
                              const std::array<uint16_t, 36>& mosaic_pattern)
{
    const uint32_t W = img.width;
    const uint32_t H = img.height;
    const uint32_t total_ch = period * period;
    const float inv_range = 1.0f / effective_range;
    const float clip_r = HighlightRecoveryParams::kNeighbourClipRatio * factor_r;
    const float clip_b = HighlightRecoveryParams::kNeighbourClipRatio * factor_b;

    int r_channels[36];
    int b_channels[36];
    int g_channels[36];
    int n_r = 0, n_b = 0, n_g = 0;
    for (uint32_t c = 0; c < total_ch && c < 36; ++c)
    {
        uint16_t color = mosaic_pattern[c];
        if (color == 0) r_channels[n_r++] = static_cast<int>(c);
        else if (color == 2) b_channels[n_b++] = static_cast<int>(c);
        else if (color == 1) g_channels[n_g++] = static_cast<int>(c);
    }
    if (n_g == 0 || (n_r == 0 && n_b == 0)) return;

    ParallelForRows(H,
        RecommendedImageRowGrain(W, total_ch, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t oy = y_begin; oy < y_end; ++oy)
        {
            for (uint32_t ox = 0; ox < W; ++ox)
            {
                float r_vals[36];
                float b_vals[36];
                float r_ratio_max = 0.0f;
                float b_ratio_max = 0.0f;
                for (int j = 0; j < n_r; ++j)
                {
                    r_vals[j] = img.At(ox, oy, r_channels[j]);
                    float r = r_vals[j] * inv_range;
                    if (r > r_ratio_max) r_ratio_max = r;
                }
                for (int j = 0; j < n_b; ++j)
                {
                    b_vals[j] = img.At(ox, oy, b_channels[j]);
                    float b = b_vals[j] * inv_range;
                    if (b > b_ratio_max) b_ratio_max = b;
                }

                if (!(r_ratio_max > clip_r || b_ratio_max > clip_b))
                    continue;

                for (int gi = 0; gi < n_g; ++gi)
                {
                    int g_ch = g_channels[gi];
                    float pv_g = img.At(ox, oy, g_ch);
                    float ratio_g = pv_g * inv_range;
                    if (ratio_g <= HighlightRecoveryParams::kBrightRatioThreshold)
                        continue;

                    float sum = 0.0f;
                    int count = 0;
                    for (int j = 0; j < n_r; ++j)
                    {
                        sum += r_vals[j] / factor_r;
                        ++count;
                    }
                    for (int j = 0; j < n_b; ++j)
                    {
                        sum += b_vals[j] / factor_b;
                        ++count;
                    }
                    if (count == 0) continue;
                    float extrapolated = sum / static_cast<float>(count);

                    img.At(ox, oy, g_ch) =
                        BlendGreenExtrapolation(pv_g, extrapolated, ratio_g);
                }
            }
        }
    }, "recover_highlights_mosaic");
}

} // anonymous namespace

void RecoverHighlights(std::vector<FloatImage>& float_images,
                       const std::vector<RawImage>& raw_images,
                       size_t ref_idx)
{
    ProfileScope scope("time.pipeline.highlight_recovery");
    if (float_images.empty() || float_images.size() != raw_images.size()) return;
    if (ref_idx >= raw_images.size()) return;

    const float ref_ev = raw_images[ref_idx].metadata.ev_value;
    const float ref_bias = raw_images[ref_idx].metadata.exposure_bias;

    ParallelFor(float_images.size(), 1, [&](size_t i0, size_t i1)
    {
        for (size_t i = i0; i < i1; ++i)
        {
            FloatImage& img = float_images[i];
            const RawMetadata& meta = raw_images[i].metadata;

            float black_mean = MeanBlackLevel(meta);
            float wl = static_cast<float>(meta.white_level);
            float dyn_range = wl - black_mean;
            if (dyn_range <= 0.0f) continue;

            float ev_scale = 1.0f;
            if (i != ref_idx)
            {
                float comp_ev = meta.ev_value;
                if (ref_ev > 0.0f && comp_ev > 0.0f)
                {
                    ev_scale = (ref_ev / comp_ev) *
                               std::pow(2.0f, ref_bias - meta.exposure_bias);
                }
            }
            float effective_range = dyn_range * ev_scale;
            if (effective_range <= 0.0f) continue;

            float cf_g = meta.color_factors[1] > 0.0f ? meta.color_factors[1] : 1.0f;
            float factor_r = meta.color_factors[0] > 0.0f ? meta.color_factors[0] / cf_g : 1.0f;
            float factor_b = meta.color_factors[2] > 0.0f ? meta.color_factors[2] / cf_g : 1.0f;

            uint32_t period = meta.mosaic_pattern_width;
            uint32_t ch = img.channels;

            if (period == 2 && ch == 4)
            {
                float factor_for_ch[4];
                for (int c = 0; c < 4; ++c)
                {
                    uint16_t color = meta.mosaic_pattern[static_cast<size_t>(c)];
                    if (color == 0) factor_for_ch[c] = factor_r;
                    else if (color == 2) factor_for_ch[c] = factor_b;
                    else factor_for_ch[c] = 1.0f;
                }
                RecoverHighlightsBayer(img, effective_range, factor_for_ch,
                                       meta.mosaic_pattern);
            }
            else if (period == 0 && ch == 3)
            {
                RecoverHighlightsLinear(img, effective_range, factor_r, factor_b);
            }
            else if (period >= 3 && ch == period * period)
            {
                RecoverHighlightsMosaic(img, period, effective_range,
                                        factor_r, factor_b, meta.mosaic_pattern);
            }
        }
    }, "recover_highlights");
}

} // namespace burstmerge
