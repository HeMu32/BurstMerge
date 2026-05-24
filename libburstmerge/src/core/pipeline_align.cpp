#include "burstmerge/internal/core/pipeline_align.h"

#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/align/align_common.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace burstmerge
{
namespace
{

constexpr bool kEnableAlignmentGrayDump = false;

inline void ApplyGammaGray(FloatImage& img, float white_level, float gamma)
{
    if (img.channels != 1 || white_level <= 0.0f) return;
    if (std::abs(gamma - 1.0f) < 0.001f) return;

    const float scale = std::pow(white_level, 1.0f - gamma);

    if (std::abs(gamma - 0.5f) < 0.001f)
    {
        for (auto& v : img.data)
        {
            // clamp to [0, white_level]
            float vin = v;
            if (vin <= 0.0f) { v = 0.0f; continue; }
            if (vin >= white_level) { v = white_level; continue; }
            v = scale * std::sqrt(vin);
        }
    }
    else
    {
        for (auto& v : img.data)
        {
            float vin = v;
            if (vin <= 0.0f) { v = 0.0f; continue; }
            if (vin >= white_level) { v = white_level; continue; }
            v = scale * std::pow(vin, gamma);
        }
    }
}

// Progress callback shim kept local to the pipeline alignment partition.

void Report(const PipelineOrchestrator::ProgressFn& progress,
            float percent,
            const std::string& stage)
{
    if (progress) progress(percent, stage);
}

void WriteLe16(std::FILE* f, uint16_t v)
{
    const uint8_t b[2] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu)
    };
    std::fwrite(b, 1, 2, f);
}

void WriteLe32(std::FILE* f, uint32_t v)
{
    const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu)
    };
    std::fwrite(b, 1, 4, f);
}

const char* AlignmentModeTag(AlignmentMode mode)
{
    switch (mode)
    {
        case AlignmentMode::DenseTile: return "dense";
        case AlignmentMode::Frequency: return "freq";
        case AlignmentMode::Standard:
        default: return "standard";
    }
}

void WriteGrayBmpRgba(const char* path,
                     const FloatImage& gray,
                     float white_level,
                     bool transparent_outside,
                     const AlignmentResult* alignment)
{
    if (gray.channels != 1 || gray.width == 0 || gray.height == 0)
    {
        return;
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;

    const uint32_t width = gray.width;
    const uint32_t height = gray.height;
    const uint32_t row_bytes = width * 4u;
    const uint32_t pixel_bytes = row_bytes * height;
    const uint32_t dib_size = 124u;
    const uint32_t file_size = 14u + dib_size + pixel_bytes;
    const uint32_t pixel_offset = 14u + dib_size;

    std::fwrite("BM", 1, 2, f);
    WriteLe32(f, file_size);
    WriteLe16(f, 0);
    WriteLe16(f, 0);
    WriteLe32(f, pixel_offset);

    WriteLe32(f, dib_size);
    WriteLe32(f, width);
    WriteLe32(f, height);
    WriteLe16(f, 1);
    WriteLe16(f, 32);
    WriteLe32(f, 3u);
    WriteLe32(f, pixel_bytes);
    WriteLe32(f, 2835u);
    WriteLe32(f, 2835u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0x00FF0000u);
    WriteLe32(f, 0x0000FF00u);
    WriteLe32(f, 0x000000FFu);
    WriteLe32(f, 0xFF000000u);
    WriteLe32(f, 0x57696E20u);
    for (int i = 0; i < 9; ++i) WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);
    WriteLe32(f, 0u);

    std::vector<uint8_t> row(static_cast<size_t>(row_bytes), 0);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            float sample_value = gray.At(x, static_cast<uint32_t>(y), 0);
            bool covered = true;
            if (transparent_outside && alignment)
            {
                const float spacing = static_cast<float>(alignment->tile_spacing > 0 ? alignment->tile_spacing : alignment->tile_size);
                const float fx = (static_cast<float>(x) + 0.5f) / spacing - 1.0f;
                const float fy = (static_cast<float>(y) + 0.5f) / spacing - 1.0f;

                int x0 = static_cast<int>(std::floor(fx));
                int y0 = static_cast<int>(std::floor(fy));
                const float tx = fx - static_cast<float>(x0);
                const float ty = fy - static_cast<float>(y0);

                auto sample_shift = [&](const std::vector<int16_t>& field, int ix, int iy) -> int
                {
                    ix = std::max(0, std::min(ix, static_cast<int>(alignment->tiles_x) - 1));
                    iy = std::max(0, std::min(iy, static_cast<int>(alignment->tiles_y) - 1));
                    const size_t idx = static_cast<size_t>(iy) * alignment->tiles_x + static_cast<uint32_t>(ix);
                    return SnapToPeriod(static_cast<int>(field[idx]), alignment->cfa_period);
                };

                auto sample_value_at = [&](int sx_shift, int sy_shift) -> float
                {
                    const int sx = static_cast<int>(x) - sx_shift;
                    const int sy = y - sy_shift;
                    if (sx < 0 || sx >= static_cast<int>(width) || sy < 0 || sy >= static_cast<int>(height))
                    {
                        covered = false;
                        return 0.0f;
                    }
                    return gray.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), 0);
                };

                const int dx00 = sample_shift(alignment->tile_shift_x, x0, y0);
                const int dx10 = sample_shift(alignment->tile_shift_x, x0 + 1, y0);
                const int dx01 = sample_shift(alignment->tile_shift_x, x0, y0 + 1);
                const int dx11 = sample_shift(alignment->tile_shift_x, x0 + 1, y0 + 1);
                const int dy00 = sample_shift(alignment->tile_shift_y, x0, y0);
                const int dy10 = sample_shift(alignment->tile_shift_y, x0 + 1, y0);
                const int dy01 = sample_shift(alignment->tile_shift_y, x0, y0 + 1);
                const int dy11 = sample_shift(alignment->tile_shift_y, x0 + 1, y0 + 1);
                const float w00 = (1.0f - tx) * (1.0f - ty);
                const float w10 = tx * (1.0f - ty);
                const float w01 = (1.0f - tx) * ty;
                const float w11 = tx * ty;

                sample_value = w00 * sample_value_at(dx00, dy00) +
                               w10 * sample_value_at(dx10, dy10) +
                               w01 * sample_value_at(dx01, dy01) +
                               w11 * sample_value_at(dx11, dy11);
            }

            const size_t base = static_cast<size_t>(x) * 4u;
            if (!covered)
            {
                row[base + 0] = 0;
                row[base + 1] = 0;
                row[base + 2] = 0;
                row[base + 3] = 0;
                continue;
            }

            float v = sample_value;
            v = std::max(0.0f, std::min(1.0f, v / std::max(1.0f, white_level)));
            const uint8_t g = static_cast<uint8_t>(std::lround(v * 255.0f));
            row[base + 0] = g;
            row[base + 1] = g;
            row[base + 2] = g;
            row[base + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

void DumpWarpedGrayBmp(const FloatImage& gray_src,
                      const AlignmentResult& alignment,
                      size_t burst_size,
                      size_t frame_idx,
                      const char* mode_tag,
                      bool is_reference,
                      float white_level)
{
    if (!kEnableAlignmentGrayDump)
    {
        return;
    }

    if (gray_src.channels != 1 || gray_src.width == 0 || gray_src.height == 0)
    {
        return;
    }

    char fname[128];
    std::snprintf(fname, sizeof(fname), "R:\\aligned_gray_%s_%zuburst_frame%02zu%s.bmp",
                  mode_tag, burst_size, frame_idx, is_reference ? "_ref" : "");
    WriteGrayBmpRgba(fname, gray_src, white_level, !is_reference, is_reference ? nullptr : &alignment);
    std::fprintf(stderr, "[DIAG] Saved warped gray BMP: %s (%ux%u)\n", fname, gray_src.width, gray_src.height);
}

} // namespace

std::vector<FloatImage> BuildAlignedComparisons(const std::vector<FloatImage>& float_images,
                                                const std::vector<RawImage>& raw_images,
                                                size_t ref_idx,
                                                const Settings& settings,
                                                uint32_t cfa_period,
                                                const PipelineOrchestrator::ProgressFn& progress)
{
    ProfileScope scope("time.pipeline.build_aligned_comparisons");
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
    params.align_gamma = settings.align_gamma;
    params.smooth_tile_field = settings.smooth_tile_field;

    const float wl = static_cast<float>(raw_images[ref_idx].metadata.white_level);
    FloatImage gray_ref_full = ConvertPlanesToGrayscale(float_images[ref_idx]);
    ApplyGammaGray(gray_ref_full, wl, settings.align_gamma);
    DumpWarpedGrayBmp(gray_ref_full, AlignmentResult{}, float_images.size(), ref_idx, AlignmentModeTag(params.mode), true, wl);
    std::vector<FloatImage> gray_inputs;
    gray_inputs.reserve(float_images.size());
    for (const auto& img : float_images)
    {
        gray_inputs.push_back(ConvertPlanesToGrayscale(img));
        ApplyGammaGray(gray_inputs.back(), wl, settings.align_gamma);
    }

    auto align_and_warp_pregrays = [&](const FloatImage& gray_ref,
                                       const FloatImage& gray_src,
                                       const FloatImage& source,
                                       size_t source_idx,
                                       size_t progress_idx,
                                       size_t total_count) -> FloatImage
    {
        Report(progress,
               PipelineConstants::kProgressAlignStart + PipelineConstants::kProgressAlignRange *
                   static_cast<float>(progress_idx) / static_cast<float>(std::max<size_t>(1, total_count)),
               "Aligning frame " + std::to_string(progress_idx + 1) + "/" + std::to_string(total_count));
        AlignmentResult ar = EstimateTranslation(gray_ref, gray_src, params);

        DumpWarpedGrayBmp(gray_src, ar, float_images.size(), source_idx, AlignmentModeTag(params.mode), false, wl);

        Report(progress,
               PipelineConstants::kProgressWarpStart + PipelineConstants::kProgressWarpRange *
                   static_cast<float>(progress_idx) / static_cast<float>(std::max<size_t>(1, total_count)),
               "Warping frame " + std::to_string(progress_idx + 1) + "/" + std::to_string(total_count));
        return WarpAligned(source, ar);
    };

    auto align_and_warp = [&](const FloatImage& guide_ref,
                              const FloatImage& source,
                              size_t source_idx,
                              size_t progress_idx,
                              size_t total_count) -> FloatImage
    {
        FloatImage gr = ConvertPlanesToGrayscale(guide_ref);
        FloatImage gs = ConvertPlanesToGrayscale(source);
        ApplyGammaGray(gr, wl, settings.align_gamma);
        ApplyGammaGray(gs, wl, settings.align_gamma);
        return align_and_warp_pregrays(gr, gs, source, source_idx, progress_idx, total_count);
    };

    bool has_exposure = false;
    float min_exp = std::numeric_limits<float>::max();
    float max_exp = 0.0f;
    std::vector<std::pair<float, size_t>> exposure_order;
    exposure_order.reserve(raw_images.size());
    for (size_t i = 0; i < raw_images.size(); ++i)
    {
        float v = raw_images[i].metadata.ev_value;
        if (v > 0.0f)
        {
            has_exposure = true;
            min_exp = std::min(min_exp, v);
            max_exp = std::max(max_exp, v);
            exposure_order.push_back({v, i});
        }
    }

    const bool use_transmission = has_exposure && !exposure_order.empty() &&
                                  max_exp > min_exp * std::pow(2.0f, PipelineConstants::kBracketTransmissionFallbackEv);
                                  // Enable chained alignment for dense mode + bracketed stacks

    if (!use_transmission)
    {
        std::fprintf(stderr, "[DEBUG] BuildAlignedComparisons: fixed-reference alignment (ref=#%zu)\n", ref_idx);
        const size_t total = float_images.size() > 0 ? float_images.size() - 1 : 0;
        aligned.clear();
        aligned.reserve(total);
        size_t processed = 0;
        for (size_t i = 0; i < float_images.size(); ++i)
        {
            if (i == ref_idx) continue;
            aligned.push_back(align_and_warp_pregrays(gray_ref_full,
                                                      gray_inputs[i],
                                                      float_images[i],
                                                      i,
                                                      processed,
                                                      total));
            ++processed;
        }
        return aligned;
    }

    std::fprintf(stderr, "[DEBUG] BuildAlignedComparisons: chained alignment (ref=#%zu, %zu frames)\n",
        ref_idx, exposure_order.size());

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
        std::fprintf(stderr, "[DEBUG] Chained align: frame #%zu (Ev=%.2f) -> parent #%zu (Ev=%.2f)\n",
            child_idx, exposure_order[pos - 1].first,
            parent_idx, exposure_order[pos].first);
        const FloatImage& parent_ref = has_aligned[parent_idx]
            ? aligned_to_root[parent_idx] : float_images[parent_idx];
        FloatImage child_aligned = align_and_warp(parent_ref, float_images[child_idx], child_idx, processed, total);
        aligned_to_root[child_idx] = std::move(child_aligned);
        has_aligned[child_idx] = 1;
        ++processed;
    }

    for (size_t pos = root_pos + 1; pos < exposure_order.size(); ++pos)
    {
        size_t parent_idx = exposure_order[pos - 1].second;
        size_t child_idx = exposure_order[pos].second;
        std::fprintf(stderr, "[DEBUG] Chained align: frame #%zu (Ev=%.2f) -> parent #%zu (Ev=%.2f)\n",
            child_idx, exposure_order[pos].first,
            parent_idx, exposure_order[pos - 1].first);
        const FloatImage& parent_ref = has_aligned[parent_idx]
            ? aligned_to_root[parent_idx] : float_images[parent_idx];
        FloatImage child_aligned = align_and_warp(parent_ref, float_images[child_idx], child_idx, processed, total);
        aligned_to_root[child_idx] = std::move(child_aligned);
        has_aligned[child_idx] = 1;
        ++processed;
    }

    for (size_t i = 0; i < float_images.size(); ++i)
    {
        if (i == ref_idx) continue;
        if (has_aligned[i]) aligned.push_back(std::move(aligned_to_root[i]));
        else aligned.push_back(align_and_warp(float_images[ref_idx], float_images[i], i, processed, total));
    }
    return aligned;
}

} // namespace burstmerge
