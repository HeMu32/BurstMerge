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

float ComputeRobustness(float noise_reduction)
{
    return std::max(PipelineConstants::kRobustnessMin,
                    noise_reduction / PipelineConstants::kRobustnessDiv);
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

} // namespace burstmerge
