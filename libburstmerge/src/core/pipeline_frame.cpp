#include "burstmerge/internal/core/pipeline_frame.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
    if (image.data.empty()) return 8.0f;

    const int blur_radius = 2;
    const FloatImage blurred = BoxBlur(image, blur_radius);
    const uint32_t step = std::max<uint32_t>(1, guide_block_size);

    double sum_sq = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 0; y < image.height; y += step)
    {
        for (uint32_t x = 0; x < image.width; x += step)
        {
            size_t idx = (static_cast<size_t>(y) * image.width + x) * image.channels;
            for (uint32_t c = 0; c < image.channels; ++c)
            {
                float d = image.data[idx + c] - blurred.data[idx + c];
                sum_sq += static_cast<double>(d) * static_cast<double>(d);
                ++count;
            }
        }
    }

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
    float ref_iso = raw_images[ref_idx].metadata.iso_exposure_time;

    for (size_t i = 0; i < float_images.size(); ++i)
    {
        const auto& meta = raw_images[i].metadata;
        FloatImage& img = float_images[i];

        float bl = MeanBlackLevel(meta);
        if (bl > 1.0f)
        {
            for (float& v : img.data) v -= bl;
        }

        if (i == ref_idx) continue;

        float comp_iso = meta.iso_exposure_time;
        if (ref_iso > 0.0f && comp_iso > 0.0f)
        {
            float scale = (ref_iso / comp_iso) *
                          std::pow(2.0f,
                                   raw_images[ref_idx].metadata.exposure_bias - meta.exposure_bias);
            if (std::abs(scale - 1.0f) > 0.001f)
            {
                for (float& v : img.data) v *= scale;
            }
        }
    }
}

std::vector<FloatImage> BuildFloatImages(const std::vector<RawImage>& images)
{
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
        float v = img.metadata.iso_exposure_time;
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
            float v = images[i].metadata.iso_exposure_time;
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

} // namespace burstmerge
