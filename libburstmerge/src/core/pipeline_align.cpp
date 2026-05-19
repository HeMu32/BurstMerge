#include "burstmerge/internal/core/pipeline_align.h"

#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <limits>

namespace burstmerge
{
namespace
{

// Progress callback shim kept local to the pipeline alignment partition.

void Report(const PipelineOrchestrator::ProgressFn& progress,
            float percent,
            const std::string& stage)
{
    if (progress) progress(percent, stage);
}

} // namespace

std::vector<FloatImage> BuildAlignedComparisons(const std::vector<FloatImage>& float_images,
                                                const std::vector<RawImage>& raw_images,
                                                size_t ref_idx,
                                                const Settings& settings,
                                                uint32_t cfa_period,
                                                const PipelineOrchestrator::ProgressFn& progress)
{
    // Pipeline-facing adapter around alignment: choose guides, dispatch to the
    // alignment code, and organize chaining for bracketed stacks.
    std::vector<FloatImage> aligned;
    aligned.reserve(float_images.size() > 0 ? float_images.size() - 1 : 0);

    AlignParams params;
    params.tile_size = settings.tile_size;
    params.search_distance = settings.search_distance;
    params.mode = settings.alignment_mode;
    params.cfa_period = (float_images[0].channels > 1)
        ? 1u : std::max<uint32_t>(1, cfa_period);

    if (settings.alignment_mode == AlignmentMode::DenseTile)
    {
        std::fprintf(stderr,
            "[WARN] Advanced dense alignment is experimental and not fully implemented yet. Results may be unstable.\n");
    }

    auto align_and_warp = [&](const FloatImage& guide_ref,
                              const FloatImage& source,
                              size_t progress_idx,
                              size_t total_count) -> FloatImage
    {
        const FloatImage gray_ref = ConvertPlanesToGrayscale(guide_ref);
        const FloatImage gray_src = ConvertPlanesToGrayscale(source);

        Report(progress,
               PipelineConstants::kProgressAlignStart + PipelineConstants::kProgressAlignRange *
                   static_cast<float>(progress_idx) / static_cast<float>(std::max<size_t>(1, total_count)),
               "Aligning frame " + std::to_string(progress_idx + 1) + "/" + std::to_string(total_count));
        AlignmentResult ar = EstimateTranslation(gray_ref, gray_src, params);

        Report(progress,
               PipelineConstants::kProgressWarpStart + PipelineConstants::kProgressWarpRange *
                   static_cast<float>(progress_idx) / static_cast<float>(std::max<size_t>(1, total_count)),
               "Warping frame " + std::to_string(progress_idx + 1) + "/" + std::to_string(total_count));
        return WarpAligned(source, ar);
    };

    bool has_exposure = false;
    float min_exp = std::numeric_limits<float>::max();
    float max_exp = 0.0f;
    std::vector<std::pair<float, size_t>> exposure_order;
    exposure_order.reserve(raw_images.size());
    for (size_t i = 0; i < raw_images.size(); ++i)
    {
        float v = raw_images[i].metadata.iso_exposure_time;
        if (v > 0.0f)
        {
            has_exposure = true;
            min_exp = std::min(min_exp, v);
            max_exp = std::max(max_exp, v);
            exposure_order.push_back({v, i});
        }
    }

    const bool use_transmission = settings.alignment_mode == AlignmentMode::DenseTile &&
                                  has_exposure && !exposure_order.empty() &&
                                  max_exp > min_exp * std::pow(2.0f, PipelineConstants::kBracketTransmissionFallbackEv);

    if (!use_transmission)
    {
        const FloatImage& ref = float_images[ref_idx];
        const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
        aligned.resize(total);
        ParallelFor(float_images.size(), 1, [&](size_t i0, size_t i1)
        {
            for (size_t i = i0; i < i1; ++i)
            {
                if (i == ref_idx) continue;
                const size_t out_idx = (i < ref_idx) ? i : (i - 1);
                aligned[out_idx] = align_and_warp(ref, float_images[i], out_idx, total);
            }
        });
        return aligned;
    }

    std::sort(exposure_order.begin(), exposure_order.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });

    const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
    size_t root_pos = 0;
    for (size_t pos = 0; pos < exposure_order.size(); ++pos)
    {
        if (exposure_order[pos].second == ref_idx)
        {
            root_pos = pos;
            break;
        }
    }
    const size_t root_idx = exposure_order[root_pos].second;
    std::vector<FloatImage> aligned_to_root(float_images.size());
    std::vector<uint8_t> has_aligned(float_images.size(), 0);
    aligned_to_root[root_idx] = float_images[root_idx];
    has_aligned[root_idx] = 1;

    size_t processed = 0;

    for (size_t pos = root_pos; pos > 0; --pos)
    {
        size_t parent_idx = exposure_order[pos].second;
        size_t child_idx = exposure_order[pos - 1].second;
        const FloatImage& parent_ref = has_aligned[parent_idx]
            ? aligned_to_root[parent_idx] : float_images[parent_idx];
        FloatImage child_aligned = align_and_warp(parent_ref, float_images[child_idx], processed, total);
        aligned_to_root[child_idx] = std::move(child_aligned);
        has_aligned[child_idx] = 1;
        ++processed;
    }

    for (size_t pos = root_pos + 1; pos < exposure_order.size(); ++pos)
    {
        size_t parent_idx = exposure_order[pos - 1].second;
        size_t child_idx = exposure_order[pos].second;
        const FloatImage& parent_ref = has_aligned[parent_idx]
            ? aligned_to_root[parent_idx] : float_images[parent_idx];
        FloatImage child_aligned = align_and_warp(parent_ref, float_images[child_idx], processed, total);
        aligned_to_root[child_idx] = std::move(child_aligned);
        has_aligned[child_idx] = 1;
        ++processed;
    }

    for (size_t i = 0; i < float_images.size(); ++i)
    {
        if (i == ref_idx) continue;
        if (has_aligned[i]) aligned.push_back(std::move(aligned_to_root[i]));
        else aligned.push_back(align_and_warp(float_images[ref_idx], float_images[i], processed, total));
    }
    return aligned;
}

} // namespace burstmerge
