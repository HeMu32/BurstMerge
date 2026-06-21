#include "burstmerge/internal/core/gpu_pipeline.h"

#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/compute/vulkan_backend.h"
#include "burstmerge/internal/core/pipeline.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/merge/frequency.h"
#include "burstmerge/internal/merge/spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace burstmerge
{
namespace
{

using vulkan::Binding;
using vulkan::ShaderPC;
using vulkan::VulkanBackend;
using ProgressFn = PipelineOrchestrator::ProgressFn;

void Report(const ProgressFn& p, float pct, const std::string& s) { if (p) p(pct, s); }

float MeanBlack(const RawMetadata& m)
{
    float s = 0.0f; int n = 0;
    for (int i = 0; i < 4; ++i) if (m.black_level[i] > 0.0f) { s += m.black_level[i]; ++n; }
    return n > 0 ? s / float(n) : 0.0f;
}

struct PyrLevel { uint64_t handle; int w; int h; };

std::vector<PyrLevel> BuildGrayPyramid(VulkanBackend& vk, uint64_t gray_h, int gw, int gh, int tile_size)
{
    std::vector<PyrLevel> pyr = {{gray_h, gw, gh}};
    int half = std::max(2, tile_size) / 2;
    auto level_tiles = [&](int w, int h) -> int
    {
        if (w < half || h < half) return 0;
        return ((w + half - 1) / half - 1) * ((h + half - 1) / half - 1);
    };
    const int kMin = AlignConstants::kMinCoarseTiles;
    const int kMax = AlignConstants::kMaxCoarseTiles;

    if (gw >= half * 2 && gh >= half * 2 && level_tiles(gw / 2, gh / 2) >= kMin)
    {
        int w1 = std::max(1, gw / 2), h1 = std::max(1, gh / 2);
        uint64_t l1 = vk.CreateBuffer(w1 * h1);
        float wgt[16] = {6.0f, 4.0f, 1.0f, 0,0,0,0,0,0,0,0,0,0,0,0,0};
        uint64_t ubo = vk.CreateBufferFromFloats(wgt, 16);

        vk.BeginFrame();
        {
            ShaderPC pc{}; pc.w = gw; pc.h = gh; pc.channels = 1; pc.w2 = w1; pc.h2 = h1; pc.channels2 = 1;
            Binding b[2] = {{0, gray_h, 0}, {1, l1, 0}};
            vk.Dispatch("downsample2x", pc, (w1 + 7) / 8, (h1 + 7) / 8, 1, b, 2);
        }
        pyr.push_back({l1, w1, h1});

        std::vector<uint64_t> scratch;
        while (true)
        {
            PyrLevel prev = pyr.back();
            if (prev.w < half * 2 || prev.h < half * 2) break;
            int nw = prev.w / 2, nh = prev.h / 2;
            if (nw < half || nh < half) break;
            if (level_tiles(prev.w, prev.h) <= kMax) break;
            if (level_tiles(nw, nh) < kMin) break;
            uint64_t tmp = vk.CreateBuffer(prev.w * prev.h);
            uint64_t blur = vk.CreateBuffer(prev.w * prev.h);
            uint64_t next = vk.CreateBuffer(nw * nh);
            {
                ShaderPC pc{}; pc.w = prev.w; pc.h = prev.h; pc.channels = 1; pc.i0 = 0; pc.i1 = 2;
                Binding b[3] = {{0, prev.handle, 0}, {1, tmp, 0}, {7, ubo, 0}};
                vk.Dispatch("binomial_sep", pc, (prev.w + 7) / 8, (prev.h + 7) / 8, 1, b, 3);
            }
            {
                ShaderPC pc{}; pc.w = prev.w; pc.h = prev.h; pc.channels = 1; pc.i0 = 1; pc.i1 = 2;
                Binding b[3] = {{0, tmp, 0}, {1, blur, 0}, {7, ubo, 0}};
                vk.Dispatch("binomial_sep", pc, (prev.w + 7) / 8, (prev.h + 7) / 8, 1, b, 3);
            }
            {
                ShaderPC pc{}; pc.w = prev.w; pc.h = prev.h; pc.channels = 1; pc.w2 = nw; pc.h2 = nh; pc.channels2 = 1;
                Binding b[2] = {{0, blur, 0}, {1, next, 0}};
                vk.Dispatch("downsample2x", pc, (nw + 7) / 8, (nh + 7) / 8, 1, b, 2);
            }
            scratch.push_back(tmp);
            scratch.push_back(blur);
            pyr.push_back({next, nw, nh});
        }
        vk.FlushFrame();
        for (auto h : scratch) vk.DestroyBuffer(h);
        vk.DestroyBuffer(ubo);
    }
    return pyr;
}

void BoxBlurGPU(VulkanBackend& vk, uint64_t src, uint64_t dst, int w, int h, int ch, int radius)
{
    ShaderPC pc{}; pc.w = w; pc.h = h; pc.channels = ch; pc.i0 = radius;
    Binding b[2] = {{0, src, 0}, {1, dst, 0}};
    vk.Dispatch("box_blur", pc, (w + 7) / 8, (h + 7) / 8, ch, b, 2);
}

void BinomialBlurGPU(VulkanBackend& vk, uint64_t src, uint64_t dst, int w, int h, int ch)
{
    float wgt[16] = {601080390.0f, 565722720.0f, 471435600.0f, 347373600.0f,
                     225792840.0f, 129024480.0f, 64512240.0f, 28048800.0f, 10518300.0f,
                     0,0,0,0,0,0,0};
    uint64_t ubo = vk.CreateBufferFromFloats(wgt, 16);
    uint64_t tmp = vk.CreateBuffer(size_t(w) * h * ch);
    ShaderPC pch{}; pch.w = w; pch.h = h; pch.channels = ch; pch.i0 = 0; pch.i1 = 5;
    Binding bh[3] = {{0, src, 0}, {1, tmp, 0}, {7, ubo, 0}};
    vk.Dispatch("binomial_sep", pch, (w + 7) / 8, (h + 7) / 8, ch, bh, 3);
    ShaderPC pcv{}; pcv.w = w; pcv.h = h; pcv.channels = ch; pcv.i0 = 1; pcv.i1 = 5;
    Binding bv[3] = {{0, tmp, 0}, {1, dst, 0}, {7, ubo, 0}};
    vk.Dispatch("binomial_sep", pcv, (w + 7) / 8, (h + 7) / 8, ch, bv, 3);
    vk.DestroyBuffer(tmp); vk.DestroyBuffer(ubo);
}

float EstimateNoiseFloorGPU(VulkanBackend& vk, uint64_t plane_h, int w, int h, int ch, uint32_t step)
{
    uint64_t blurred = vk.CreateBuffer(w * h * ch);
    vk.BeginFrame();
    BoxBlurGPU(vk, plane_h, blurred, w, h, ch, 2);
    vk.FlushFrame();
    int gw = (w + int(step) - 1) / int(step);
    int gh = (h + int(step) - 1) / int(step);
    uint64_t scratch = vk.CreateBuffer(gw * gh);
    uint64_t partials = vk.CreateBuffer(4096);
    uint64_t result = vk.CreateHostBuffer(1);
    vk.BeginFrame();
    {
        ShaderPC pc{}; pc.w = w; pc.h = h; pc.channels = ch; pc.i0 = 0; pc.i1 = int(step); pc.i3 = gw * gh;
        Binding b[3] = {{0, plane_h, 0}, {1, blurred, 0}, {2, scratch, 0}};
        vk.Dispatch("extract", pc, gw, gh, 1, b, 3);
    }
    int n = gw * gh; int numWg = (n + 255) / 256;
    {
        ShaderPC pc{}; pc.i0 = n; pc.i1 = 0; pc.i2 = 0;
        Binding b[2] = {{0, scratch, 0}, {1, partials, 0}};
        vk.Dispatch("reduce_scalar", pc, numWg, 1, 1, b, 2);
    }
    {
        ShaderPC pc{}; pc.i0 = numWg; pc.i1 = 0; pc.i2 = 1; pc.i3 = n * ch;
        Binding b[3] = {{0, partials, 0}, {1, partials, 0}, {2, result, 0}};
        vk.Dispatch("reduce_scalar", pc, 1, 1, 1, b, 3);
    }
    vk.FlushFrame();
    float sumsq = 0.0f;
    vk.DownloadFloats(result, &sumsq, 1);
    return std::max(PipelineConstants::kNoiseFloorMin, std::sqrt(sumsq));
}

float EstimateLinearNoiseGPU(VulkanBackend& vk, uint64_t plane_h, uint64_t blurred_h,
                             int w, int h, int ch, uint32_t step)
{
    int gw = (w + int(step) - 1) / int(step);
    int gh = (h + int(step) - 1) / int(step);
    int n = gw * gh;
    uint64_t scratch = vk.CreateBuffer(std::max(1, n));
    uint64_t partials = vk.CreateBuffer(4096);
    uint64_t result = vk.CreateHostBuffer(1);
    vk.BeginFrame();
    {
        ShaderPC pc{}; pc.w = w; pc.h = h; pc.channels = ch; pc.i0 = 1; pc.i1 = int(step); pc.i3 = n;
        Binding b[3] = {{0, plane_h, 0}, {1, blurred_h, 0}, {2, scratch, 0}};
        vk.Dispatch("extract", pc, gw, gh, 1, b, 3);
    }
    int numWg = (n + 255) / 256;
    {
        ShaderPC pc{}; pc.i0 = n; pc.i1 = 0; pc.i2 = 0;
        Binding b[2] = {{0, scratch, 0}, {1, partials, 0}};
        vk.Dispatch("reduce_scalar", pc, numWg, 1, 1, b, 2);
    }
    {
        ShaderPC pc{}; pc.i0 = numWg; pc.i1 = 0; pc.i2 = 1; pc.i3 = n;
        Binding b[3] = {{0, partials, 0}, {1, partials, 0}, {2, result, 0}};
        vk.Dispatch("reduce_scalar", pc, 1, 1, 1, b, 3);
    }
    vk.FlushFrame();
    float sum_abs = 0.0f;
    vk.DownloadFloats(result, &sum_abs, 1);
    vk.DestroyBuffer(scratch); vk.DestroyBuffer(partials); vk.DestroyBuffer(result);
    return std::max(SpatialConstants::kNoiseFloorMin, sum_abs);
}

float ReduceMaxAbsGPU(VulkanBackend& vk, uint64_t img, int w, int h, int ch)
{
    uint64_t scratch = vk.CreateBuffer(w * h);
    uint64_t partials = vk.CreateBuffer(4096);
    uint64_t result = vk.CreateHostBuffer(1);
    vk.BeginFrame();
    {
        ShaderPC pc{}; pc.w = w; pc.h = h; pc.channels = ch; pc.i0 = 2; pc.i1 = 1; pc.i3 = w * h;
        Binding b[2] = {{0, img, 0}, {2, scratch, 0}};
        vk.Dispatch("extract", pc, w, h, 1, b, 2);
    }
    int n = w * h; int numWg = (n + 255) / 256;
    {
        ShaderPC pc{}; pc.i0 = n; pc.i1 = 1; pc.i2 = 0;
        Binding b[2] = {{0, scratch, 0}, {1, partials, 0}};
        vk.Dispatch("reduce_scalar", pc, numWg, 1, 1, b, 2);
    }
    {
        ShaderPC pc{}; pc.i0 = numWg; pc.i1 = 1; pc.i2 = 1;
        Binding b[3] = {{0, partials, 0}, {1, partials, 0}, {2, result, 0}};
        vk.Dispatch("reduce_scalar", pc, 1, 1, 1, b, 3);
    }
    vk.FlushFrame();
    float mx = 0.0f;
    vk.DownloadFloats(result, &mx, 1);
    vk.DestroyBuffer(scratch); vk.DestroyBuffer(partials); vk.DestroyBuffer(result);
    return mx;
}

// Spatial merge on GPU. Returns merged plane buffer handle.
uint64_t SpatialMergeGPU(VulkanBackend& vk, uint64_t ref_plane,
                         const std::vector<uint64_t>& aligned,
                         const std::vector<RawImage>& images, size_t ref_idx,
                         const Settings& settings, int pw, int ph, int ch,
                         float mean_bl,
                         const std::vector<size_t>& comp_orig,
                         const std::vector<float>& comp_scale)
{
    const bool linear = (settings.spatial_mode == SpatialMergeMode::Linear);
    uint64_t ref_blur = vk.CreateBuffer(pw * ph * ch);
    uint64_t acc = vk.CreateBuffer(pw * ph * ch);
    uint64_t wsum = vk.CreateBuffer(pw * ph);

    vk.BeginFrame();
    if (linear)
        BinomialBlurGPU(vk, ref_plane, ref_blur, pw, ph, ch);
    else
        BoxBlurGPU(vk, ref_plane, ref_blur, pw, ph, ch, 2);
    // acc = ref
    {
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch;
        Binding b[2] = {{0, ref_plane, 0}, {1, acc, 0}};
        vk.Dispatch("copy", pc, (size_t(pw) * ph * ch + 255) / 256, 1, 1, b, 2);
    }
    vk.FlushFrame();
    vk.FillFloat(wsum, 1.0f);

    uint32_t guide_block = std::max<uint32_t>(1, images[ref_idx].metadata.mosaic_pattern_width >= 2
        ? images[ref_idx].metadata.mosaic_pattern_width : 2);
    float robustness = ComputeRobustness(settings.noise_reduction);
    float noise_floor;
    if (linear)
    {
        noise_floor = EstimateLinearNoiseGPU(vk, ref_plane, ref_blur, pw, ph, ch, std::max<uint32_t>(1, guide_block));
    }
    else
    {
        float est_noise = EstimateNoiseFloorGPU(vk, ref_plane, pw, ph, ch, std::max<uint32_t>(1, guide_block));
        float formula_noise = std::max(PipelineConstants::kNoiseFloorMin, settings.noise_reduction * PipelineConstants::kNoiseFormulaMul);
        noise_floor = std::min(est_noise, formula_noise);
    }
    float wl = float(images[ref_idx].metadata.white_level);
    float highlight_threshold = (wl - mean_bl) * PipelineConstants::kHighlightFactor;
    float clip_threshold = (wl - mean_bl) * PipelineConstants::kClipFactor;
    float min_cmp_w = linear ? SpatialConstants::kLinearMinComparisonWeight : SpatialConstants::kStandardMinComparisonWeight;

    uint64_t ref_guide_map = 0;
    if (!linear)
    {
        ref_guide_map = vk.CreateBuffer(pw * ph);
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch; pc.i0 = int(guide_block);
        Binding b[2] = {{0, ref_plane, 0}, {1, ref_guide_map, 0}};
        vk.BeginFrame();
        vk.Dispatch("block_mean_guide", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 2);
        vk.FlushFrame();
    }

    for (size_t k = 0; k < aligned.size(); ++k)
    {
        uint64_t cmp_blur = vk.CreateBuffer(pw * ph * ch);
        uint64_t cmp_guide = 0;
        vk.BeginFrame();
        if (linear)
        {
            BinomialBlurGPU(vk, aligned[k], cmp_blur, pw, ph, ch);
            cmp_guide = cmp_blur;
        }
        else
        {
            BoxBlurGPU(vk, aligned[k], cmp_blur, pw, ph, ch, 2);
        }
        if (!linear)
        {
            cmp_guide = vk.CreateBuffer(pw * ph);
            ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch; pc.i0 = int(guide_block);
            Binding b[2] = {{0, aligned[k], 0}, {1, cmp_guide, 0}};
            vk.Dispatch("block_mean_guide", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 2);
        }
        if (ch == 1)
        {
            uint64_t rg = linear ? ref_blur : ref_guide_map;
            uint64_t cg = linear ? cmp_blur : cmp_guide;
            ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = 1; pc.i4 = linear ? 1 : 0;
            pc.f0 = robustness; pc.f1 = noise_floor; pc.f2 = highlight_threshold;
            pc.f3 = clip_threshold; pc.f4 = min_cmp_w; pc.f5 = comp_scale[k];
            Binding b[5] = {{0, acc, 0}, {1, wsum, 0}, {2, rg, 0}, {3, aligned[k], 0}, {4, cg, 0}};
            vk.Dispatch("spatial_acc_1ch", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 5);
        }
        else
        {
            ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch; pc.i4 = linear ? 1 : 0;
            pc.f0 = robustness; pc.f1 = noise_floor; pc.f2 = highlight_threshold;
            pc.f3 = clip_threshold; pc.f4 = min_cmp_w; pc.f5 = comp_scale[k];
            Binding b[6] = {{0, acc, 0}, {1, wsum, 0}, {2, ref_plane, 0}, {3, ref_blur, 0},
                            {4, aligned[k], 0}, {5, cmp_blur, 0}};
            vk.Dispatch("spatial_acc_multi", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 6);
        }
        vk.FlushFrame();
        vk.DestroyBuffer(cmp_blur);
        if (cmp_guide && cmp_guide != cmp_blur) vk.DestroyBuffer(cmp_guide);
    }
    if (ref_guide_map) vk.DestroyBuffer(ref_guide_map);
    vk.DestroyBuffer(ref_blur);

    uint64_t merged = vk.CreateBuffer(pw * ph * ch);
    // normalize acc -> merged
    {
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch;
        Binding b[3] = {{0, acc, 0}, {1, wsum, 0}, {2, merged, 0}};
        // normalize_div operates in place; use acc then copy. Simpler: run on acc->merged.
        (void)b;
    }
    // Run normalize on acc in place, then copy to merged.
    {
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch;
        Binding bn[2] = {{0, acc, 0}, {1, wsum, 0}};
        vk.BeginFrame();
        vk.Dispatch("normalize_div", pc, (pw + 7) / 8, (ph + 7) / 8, 1, bn, 2);
        ShaderPC pcc{}; pcc.w = pw; pcc.h = ph; pcc.channels = ch;
        Binding bc[2] = {{0, acc, 0}, {1, merged, 0}};
        vk.Dispatch("copy", pcc, (size_t(pw) * ph * ch + 255) / 256, 1, 1, bc, 2);
        vk.FlushFrame();
    }
    vk.DestroyBuffer(acc); vk.DestroyBuffer(wsum);
    return merged;
}

uint64_t FrequencyMergeGPU(VulkanBackend& vk, uint64_t ref_plane,
                           const std::vector<uint64_t>& aligned,
                           const std::vector<RawImage>& images, size_t ref_idx,
                           const Settings& settings, int pw, int ph, int ch,
                           float mean_bl, uint64_t merged_out)
{
    (void)images; (void)mean_bl;
    const int tile = FrequencyConstants::kWienerTileSize; // 8
    const int stride = std::max(1, tile / 2);
    size_t ff = size_t(pw) * ph * ch;

    if (settings.frequency_mode == FrequencyMode::WienerFftRobust)
    {
        throw std::runtime_error(
            "GPU backend does not implement WienerFftRobust (uses standard WienerFft). "
            "Use --backend cpu for WienerFftRobust, or --frequency-mode wiener-fft for the GPU path.");
    }

    if (settings.frequency_mode == FrequencyMode::Laplacian)
    {
        int blur_radius = std::max(1, settings.tile_size / FrequencyConstants::kLaplacianBlurDiv);
        uint64_t ref_low = vk.CreateBuffer(ff);
        uint64_t cmps = vk.CreateBuffer(ff * aligned.size());
        uint64_t cmpsl = vk.CreateBuffer(ff * aligned.size());
        std::vector<uint64_t> cmp_lows(aligned.size());
        vk.BeginFrame();
        BoxBlurGPU(vk, ref_plane, ref_low, pw, ph, ch, blur_radius);
        for (size_t k = 0; k < aligned.size(); ++k)
        {
            cmp_lows[k] = vk.CreateBuffer(ff);
            BoxBlurGPU(vk, aligned[k], cmp_lows[k], pw, ph, ch, blur_radius);
            vk.CopyBufferRegion(cmps, uint32_t(k * ff), aligned[k], uint32_t(ff));
            vk.CopyBufferRegion(cmpsl, uint32_t(k * ff), cmp_lows[k], uint32_t(ff));
        }
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = ch;
        pc.i0 = int(aligned.size()); pc.i1 = int(ff);
        Binding b[5] = {{0, ref_plane, 0}, {1, ref_low, 0}, {2, cmps, 0}, {3, cmpsl, 0}, {4, merged_out, 0}};
        vk.Dispatch("freq_laplacian", pc, (uint32_t(ff) + 255) / 256, 1, 1, b, 5);
        vk.FlushFrame();
        for (auto cl : cmp_lows) vk.DestroyBuffer(cl);
        vk.DestroyBuffer(ref_low); vk.DestroyBuffer(cmps); vk.DestroyBuffer(cmpsl);
        return merged_out;
    }

    // Wiener (standard) per-tile. Pack comparisons on GPU.
    uint64_t cmps = vk.CreateBuffer(ff * aligned.size());
    vk.BeginFrame();
    for (size_t k = 0; k < aligned.size(); ++k)
        vk.CopyBufferRegion(cmps, uint32_t(k * ff), aligned[k], uint32_t(ff));
    vk.FlushFrame();

    float nr = settings.noise_reduction;
    double robustness_rev = 0.5 * (FrequencyConstants::kRobustnessRevOffset - double(int(nr + 0.5f)));
    double robustness_norm = std::pow(2.0, -robustness_rev + FrequencyConstants::kRobustnessNormBase);
    double read_noise = std::pow(std::pow(2.0, -robustness_rev + FrequencyConstants::kReadNoiseBase),
                                 FrequencyConstants::kReadNoiseExp);
    int stack = int(aligned.size()) + 1;

    uint64_t out = vk.CreateBuffer(ff);
    uint64_t norm = vk.CreateBuffer(pw * ph);
    vk.FillFloat(out, 0.0f);
    vk.FillFloat(norm, 0.0f);

    int tiles_x = (pw + stride - 1) / stride;
    int tiles_y = (ph + stride - 1) / stride;

    ShaderPC pc{};
    pc.w = pw; pc.h = ph; pc.channels = ch;
    pc.i0 = stride; pc.i3 = int(aligned.size());
    pc.i4 = stack; pc.i5 = tile; pc.i6 = int(ff);
    pc.f0 = float(robustness_norm); pc.f1 = float(read_noise);
    Binding b[4] = {{0, ref_plane, 0}, {1, cmps, 0}, {2, out, 0}, {3, norm, 0}};
    vk.BeginFrame();
    for (int py = 0; py < 2; ++py)
    {
        for (int px = 0; px < 2; ++px)
        {
            int ptx = std::max(0, (tiles_x - px + 1) / 2);
            int pty = std::max(0, (tiles_y - py + 1) / 2);
            if (ptx == 0 || pty == 0) continue;
            pc.i1 = px;
            pc.i2 = py;
            vk.Dispatch("freq_wiener_tile", pc, ptx, pty, 1, b, 4);
        }
    }
    vk.FlushFrame();
    vk.DestroyBuffer(cmps);

    // normalize out/norm -> merged_out, then clamp floor -1 (ReduceTileBorderArtifacts)
    {
        ShaderPC pn{}; pn.w = pw; pn.h = ph; pn.channels = ch;
        Binding bn[3] = {{0, out, 0}, {1, norm, 0}, {2, merged_out, 0}};
        (void)bn;
    }
    vk.BeginFrame();
    {
        ShaderPC pn{}; pn.w = pw; pn.h = ph; pn.channels = ch;
        Binding bn[2] = {{0, out, 0}, {1, norm, 0}};
        vk.Dispatch("normalize_div", pn, (pw + 7) / 8, (ph + 7) / 8, 1, bn, 2);
        ShaderPC pcc{}; pcc.w = pw; pcc.h = ph; pcc.channels = ch;
        Binding bc[2] = {{0, out, 0}, {1, merged_out, 0}};
        vk.Dispatch("copy", pcc, (size_t(pw) * ph * ch + 255) / 256, 1, 1, bc, 2);
    }
    vk.FlushFrame();
    vk.DestroyBuffer(out); vk.DestroyBuffer(norm);
    return merged_out;
}

// DenseTile alignment on GPU: coarse-to-fine per-level (propagate/correct/search),
// mirroring CPU EstimateDenseTileField. Writes the FINEST-level tile field into
// out_tsx/out_tsy. Buffers must be sized for the finest dense tile count.
void DenseAlignGPU(VulkanBackend& vk,
                   const std::vector<PyrLevel>& ref_pyr,
                   const std::vector<PyrLevel>& cmp_pyr,
                   int pw, int ph, int tile_size, int pyr_n,
                   uint64_t out_tsx, uint64_t out_tsy)
{
    int half_tile = std::max(1, tile_size / 2);
    int ft_x = std::max(1, (pw + half_tile - 1) / half_tile - 1);
    int ft_y = std::max(1, (ph + half_tile - 1) / half_tile - 1);
    uint64_t sc_sx[2] = {vk.CreateBuffer(std::max(1, ft_x * ft_y)),
                         vk.CreateBuffer(std::max(1, ft_x * ft_y))};
    uint64_t sc_sy[2] = {vk.CreateBuffer(std::max(1, ft_x * ft_y)),
                         vk.CreateBuffer(std::max(1, ft_x * ft_y))};

    for (int level = pyr_n - 1; level >= 0; --level)
    {
        int W = ref_pyr[level].w;
        int H = ref_pyr[level].h;
        int tx_L = std::max(1, (W + half_tile - 1) / half_tile - 1);
        int ty_L = std::max(1, (H + half_tile - 1) / half_tile - 1);
        bool is_coarsest = (level == pyr_n - 1);
        int level_scale = is_coarsest ? 0
            : (ref_pyr[level].w / std::max(1, ref_pyr[level + 1].w));
        int weight_ssd = (level > 0) ? 1 : 0;

        uint64_t out_sx = (level == 0) ? out_tsx : sc_sx[level % 2];
        uint64_t out_sy = (level == 0) ? out_tsy : sc_sy[level % 2];
        uint64_t csx = is_coarsest ? sc_sx[0] : sc_sx[(level + 1) % 2];
        uint64_t csy = is_coarsest ? sc_sy[0] : sc_sy[(level + 1) % 2];
        uint64_t cmp_lvl = (level < int(cmp_pyr.size())) ? cmp_pyr[level].handle : cmp_pyr[0].handle;

        ShaderPC pc{};
        pc.w = W; pc.h = H; pc.channels = 1;
        pc.i0 = tile_size; pc.i1 = half_tile; pc.i2 = tx_L; pc.i3 = ty_L;
        pc.i4 = is_coarsest ? 0 : tx_L;  // cur (coarser) tiles — recomputed by shader via ratio; pass coarsest-finer tx
        pc.i5 = is_coarsest ? 0 : ty_L;
        pc.i6 = level_scale; pc.i7 = 1; pc.i8 = is_coarsest ? 1 : 0; pc.i9 = weight_ssd;
        // For correct ratio mapping the shader needs the COARSER level's tile count.
        // Store it in i4/i5 = (coarser tx, ty). Compute coarser tiles:
        if (!is_coarsest)
        {
            int cW = ref_pyr[level + 1].w, cH = ref_pyr[level + 1].h;
            pc.i4 = std::max(1, (cW + half_tile - 1) / half_tile - 1);
            pc.i5 = std::max(1, (cH + half_tile - 1) / half_tile - 1);
        }
        Binding b[6] = {{0, ref_pyr[level].handle, 0}, {1, cmp_lvl, 0},
                        {2, csx, 0}, {3, csy, 0}, {4, out_sx, 0}, {5, out_sy, 0}};
        vk.Dispatch("dense_level", pc, (tx_L + 7) / 8, (ty_L + 7) / 8, 1, b, 6);
    }
    vk.DestroyBuffer(sc_sx[0]); vk.DestroyBuffer(sc_sx[1]);
    vk.DestroyBuffer(sc_sy[0]); vk.DestroyBuffer(sc_sy[1]);
}

} // namespace

bool GpuVulkanAvailable()
{
    VulkanBackend vk;
    return vk.Initialize();
}

AlignmentResult GpuEstimateTranslation(const FloatImage& ref_gray,
                                       const FloatImage& cmp_gray,
                                       const AlignParams& params)
{
    AlignmentResult out;
    if (ref_gray.width != cmp_gray.width || ref_gray.height != cmp_gray.height || ref_gray.channels != 1)
        return out;
    VulkanBackend vk;
    if (!vk.Initialize()) return out;

    int gw = int(ref_gray.width);
    int gh = int(ref_gray.height);
    uint64_t ref_h = vk.CreateBufferFromFloats(ref_gray.data.data(), uint32_t(ref_gray.data.size()));
    uint64_t cmp_h = vk.CreateBufferFromFloats(cmp_gray.data.data(), uint32_t(cmp_gray.data.size()));

    int tile_size = ResolveAlignTile(params.tile_size);
    auto ref_pyr = BuildGrayPyramid(vk, ref_h, gw, gh, tile_size);
    auto cmp_pyr = BuildGrayPyramid(vk, cmp_h, gw, gh, tile_size);
    int pyr_n = int(ref_pyr.size());

    uint64_t align_state = vk.CreateHostBuffer(2);
    uint64_t global_shift = vk.CreateHostBuffer(2);
    uint64_t cand_global = vk.CreateBuffer(128 * 128);
    int z[2] = {0, 0};
    vk.UploadFloats(align_state, reinterpret_cast<float*>(z), 2);
    vk.UploadFloats(global_shift, reinterpret_cast<float*>(z), 2);

    int search_distance = std::max(1, int(params.search_distance));
    int local_radius = std::max(1, std::min(int(AlignConstants::kDenseLocalRadius),
                                            search_distance / int(AlignConstants::kRefineLocalRadiusDiv)));
    bool dense = (params.mode == AlignmentMode::DenseTile);
    int half_tile = std::max(1, tile_size / 2);
    int tiles_x = dense ? std::max(1, (gw + half_tile - 1) / half_tile - 1) : (gw + tile_size - 1) / tile_size;
    int tiles_y = dense ? std::max(1, (gh + half_tile - 1) / half_tile - 1) : (gh + tile_size - 1) / tile_size;
    int spacing = dense ? half_tile : 0;

    uint64_t tsx = vk.CreateBuffer(std::max<size_t>(1, size_t(tiles_x) * tiles_y));
    uint64_t tsy = vk.CreateBuffer(std::max<size_t>(1, size_t(tiles_x) * tiles_y));

    vk.UploadFloats(align_state, reinterpret_cast<float*>(z), 2);
    vk.BeginFrame();
    for (int level = pyr_n - 1; level >= 0; --level)
    {
        if (level < pyr_n - 1)
        {
            int scale = ref_pyr[level].w / std::max(1, ref_pyr[level + 1].w);
            ShaderPC pc{}; pc.i0 = scale;
            Binding b[1] = {{0, align_state, 0}};
            vk.Dispatch("upscale_seed", pc, 1, 1, 1, b, 1);
        }
        int longest = std::max(ref_pyr[level].w, ref_pyr[level].h);
        int shift = AlignConstants::kSearchFractionShiftBase + (pyr_n - 1 - level);
        int radius = std::max(int(AlignConstants::kMinSearchRadius), longest >> shift);
        int step = std::max(1, 16 >> (level + 1));
        int side = 2 * radius + 1;
        uint64_t cmp_lvl = (level < int(cmp_pyr.size())) ? cmp_pyr[level].handle : cmp_h;
        {
            ShaderPC pc{}; pc.w = ref_pyr[level].w; pc.h = ref_pyr[level].h; pc.channels = 1;
            pc.i0 = radius; pc.i1 = step; pc.i2 = 1;
            Binding b[4] = {{0, ref_pyr[level].handle, 0}, {1, cmp_lvl, 0},
                            {2, align_state, 0}, {3, cand_global, 0}};
            vk.Dispatch("sad_global", pc, side * side, 1, 1, b, 4);
        }
        {
            ShaderPC pc{}; pc.i0 = radius; pc.i2 = 1;
            Binding b[3] = {{0, cand_global, 0}, {1, align_state, 0}, {2, global_shift, 0}};
            vk.Dispatch("select_min", pc, 1, 1, 1, b, 3);
        }
    }
    vk.FlushFrame();

    vk.BeginFrame();
    if (dense)
        DenseAlignGPU(vk, ref_pyr, cmp_pyr, gw, gh, tile_size, pyr_n, tsx, tsy);
    else
    {
        ShaderPC pc{};
        pc.w = gw; pc.h = gh; pc.channels = 1;
        pc.i0 = tile_size; pc.i1 = local_radius; pc.i2 = tiles_x; pc.i3 = tiles_y; pc.i5 = 1;
        int diags = tiles_x + tiles_y - 1;
        for (int d = 0; d < diags; ++d)
        {
            pc.i4 = d;
            Binding b[5] = {{0, ref_h, 0}, {1, cmp_h, 0}, {2, global_shift, 0}, {3, tsx, 0}, {4, tsy, 0}};
            vk.Dispatch("tile_refine_diag", pc, 1, 1, 1, b, 5);
        }
    }
    vk.FlushFrame();

    // download
    int gs[2] = {0, 0};
    vk.DownloadFloats(global_shift, reinterpret_cast<float*>(gs), 2);
    out.shift_x = gs[0];
    out.shift_y = gs[1];
    out.confidence = 1.0f;
    out.tile_size = tile_size;
    out.tile_spacing = spacing;
    out.cfa_period = std::max<uint32_t>(1, params.cfa_period);
    out.tiles_x = uint32_t(tiles_x);
    out.tiles_y = uint32_t(tiles_y);
    out.tile_shift_x.resize(size_t(tiles_x) * tiles_y);
    out.tile_shift_y.resize(size_t(tiles_x) * tiles_y);
    {
        size_t nt = out.tile_shift_x.size();
        std::vector<uint32_t> tmpx(nt), tmpy(nt);
        vk.DownloadUints(tsx, tmpx.data(), uint32_t(nt));
        vk.DownloadUints(tsy, tmpy.data(), uint32_t(nt));
        for (size_t k = 0; k < nt; ++k)
        {
            out.tile_shift_x[k] = static_cast<int16_t>(static_cast<int32_t>(tmpx[k]));
            out.tile_shift_y[k] = static_cast<int16_t>(static_cast<int32_t>(tmpy[k]));
        }
    }

    for (auto& lvl : ref_pyr) if (lvl.handle != ref_h) vk.DestroyBuffer(lvl.handle);
    for (auto& lvl : cmp_pyr) if (lvl.handle != cmp_h) vk.DestroyBuffer(lvl.handle);
    vk.DestroyBuffer(ref_h); vk.DestroyBuffer(cmp_h);
    vk.DestroyBuffer(align_state); vk.DestroyBuffer(global_shift);
    vk.DestroyBuffer(cand_global); vk.DestroyBuffer(tsx); vk.DestroyBuffer(tsy);
    return out;
}

FloatImage GpuRunBurstPipeline(const std::vector<RawImage>& images,
                               size_t ref_idx,
                               const Settings& settings,
                               const ProgressFn& progress)
{
    if (images.empty()) throw std::runtime_error("GPU pipeline: no images");
    VulkanBackend vk;
    if (!vk.Initialize()) throw std::runtime_error("Vulkan init failed: " + vk.LastError());

    const uint32_t W = images[0].pixels.width;
    const uint32_t H = images[0].pixels.height;
    const uint32_t period = std::max<uint32_t>(1, images[ref_idx].metadata.mosaic_pattern_width);
    const uint32_t ch = period * period;
    const int pw = int((W + period - 1) / period);
    const int ph = int((H + period - 1) / period);
    const size_t N = images.size();

    // comparison index tables
    std::vector<size_t> comp_orig;
    std::vector<float> comp_scale;
    float ref_ev = images[ref_idx].metadata.ev_value;
    float ref_bias = images[ref_idx].metadata.exposure_bias;
    for (size_t i = 0; i < N; ++i)
    {
        if (i == ref_idx) continue;
        comp_orig.push_back(i);
        float comp_ev = images[i].metadata.ev_value;
        float s = (ref_ev > 0.0f && images[i].metadata.ev_value > 0.0f)
            ? (ref_ev / images[i].metadata.ev_value) * std::pow(2.0f, ref_bias - images[i].metadata.exposure_bias)
            : 1.0f;
        comp_scale.push_back(s);
        (void)comp_ev;
    }

    Report(progress, 0.55f, "GPU: preparing textures");
    std::vector<uint64_t> plane(N);
    std::vector<float> mean_bl(N);
    std::vector<uint64_t> gray(N);
    { ProfileScope _ps("time.gpu.prepare");
    std::vector<uint64_t> rawbufs;
    rawbufs.reserve(N);
    vk.BeginFrame();
    for (size_t i = 0; i < N; ++i)
    {
        mean_bl[i] = MeanBlack(images[i].metadata);
        const uint16_t* raw = reinterpret_cast<const uint16_t*>(images[i].pixels.data);
        std::vector<float> fraw(size_t(W) * H);
        for (size_t j = 0; j < fraw.size(); ++j) fraw[j] = float(raw[j]);
        rawbufs.push_back(vk.CreateBufferFromFloats(fraw.data(), uint32_t(fraw.size())));
        plane[i] = vk.CreateBuffer(pw * ph * ch);
        ShaderPC pc{};
        pc.w = int(W); pc.h = int(H); pc.channels = 1;
        pc.w2 = pw; pc.h2 = ph; pc.channels2 = int(ch);
        pc.i0 = int(period); pc.f0 = mean_bl[i]; pc.f1 = (i == ref_idx) ? 1.0f : comp_scale[std::find(comp_orig.begin(), comp_orig.end(), i) - comp_orig.begin()];
        Binding b[2] = {{0, rawbufs.back(), 0}, {1, plane[i], 0}};
        vk.Dispatch("prepare_texture", pc, (pw + 7) / 8, (ph + 7) / 8, int(ch), b, 2);
    }
    vk.FlushFrame();
    for (auto r : rawbufs) vk.DestroyBuffer(r);

    vk.BeginFrame();
    for (size_t i = 0; i < N; ++i)
    {
        gray[i] = vk.CreateBuffer(pw * ph);
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = int(ch); pc.w2 = pw; pc.h2 = ph;
        pc.f0 = settings.align_gamma;
        pc.f1 = float(images[ref_idx].metadata.white_level);
        Binding b[2] = {{0, plane[i], 0}, {1, gray[i], 0}};
        vk.Dispatch("to_grayscale", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 2);
    }
    vk.FlushFrame();
    } // prepare

    std::vector<uint64_t> aligned;
    { ProfileScope _ps("time.gpu.align");
    int tile_size = std::max(16, int(settings.tile_size));
    auto ref_pyr = BuildGrayPyramid(vk, gray[ref_idx], pw, ph, tile_size);

    uint64_t align_state = vk.CreateHostBuffer(2);
    uint64_t global_shift = vk.CreateHostBuffer(2);
    int zero[2] = {0, 0};
    vk.UploadFloats(align_state, reinterpret_cast<float*>(zero), 2);
    vk.UploadFloats(global_shift, reinterpret_cast<float*>(zero), 2);

    int search_distance = std::max(1, int(settings.search_distance));
    int local_radius = std::max(1, std::min(int(AlignConstants::kDenseLocalRadius),
                                            search_distance / int(AlignConstants::kRefineLocalRadiusDiv)));
    int num_cand = (2 * local_radius + 1) * (2 * local_radius + 1);
    int tiles_x = (pw + tile_size - 1) / tile_size;  // standard tile count
    int tiles_y = (ph + tile_size - 1) / tile_size;

    bool dense = (settings.alignment_mode == AlignmentMode::DenseTile);
    int half_tile = std::max(1, tile_size / 2);
    int align_tiles_x = dense ? std::max(1, (pw + half_tile - 1) / half_tile - 1) : tiles_x;
    int align_tiles_y = dense ? std::max(1, (ph + half_tile - 1) / half_tile - 1) : tiles_y;
    int align_spacing = dense ? half_tile : tile_size;

    uint64_t cand_global = vk.CreateBuffer(128 * 128);
    uint64_t tsx = vk.CreateBuffer(std::max<size_t>(1, size_t(align_tiles_x) * align_tiles_y));
    uint64_t tsy = vk.CreateBuffer(std::max<size_t>(1, size_t(align_tiles_x) * align_tiles_y));

    aligned.reserve(N > 0 ? N - 1 : 0);
    int pyr_n = int(ref_pyr.size());

    for (size_t i = 0; i < N; ++i)
    {
        if (i == ref_idx) continue;
        Report(progress, 0.6f, "GPU: aligning frame " + std::to_string(i));
        auto cmp_pyr = BuildGrayPyramid(vk, gray[i], pw, ph, tile_size);
        int z2[2] = {0, 0};
        vk.UploadFloats(align_state, reinterpret_cast<float*>(z2), 2);

        vk.BeginFrame();
        for (int level = pyr_n - 1; level >= 0; --level)
        {
            if (level < pyr_n - 1)
            {
                int scale = ref_pyr[level].w / std::max(1, ref_pyr[level + 1].w);
                ShaderPC pc{}; pc.i0 = scale;
                Binding b[1] = {{0, align_state, 0}};
                vk.Dispatch("upscale_seed", pc, 1, 1, 1, b, 1);
            }
            int longest = std::max(ref_pyr[level].w, ref_pyr[level].h);
            int shift = AlignConstants::kSearchFractionShiftBase + (pyr_n - 1 - level);
            int radius = std::max(int(AlignConstants::kMinSearchRadius), longest >> shift);
            int step = std::max(1, 16 >> (level + 1));
            int side = 2 * radius + 1;
            uint64_t cmp_lvl = (level < int(cmp_pyr.size())) ? cmp_pyr[level].handle : gray[i];
            {
                ShaderPC pc{}; pc.w = ref_pyr[level].w; pc.h = ref_pyr[level].h; pc.channels = 1;
                pc.i0 = radius; pc.i1 = step; pc.i2 = 1;
                Binding b[4] = {{0, ref_pyr[level].handle, 0}, {1, cmp_lvl, 0},
                                {2, align_state, 0}, {3, cand_global, 0}};
                vk.Dispatch("sad_global", pc, side * side, 1, 1, b, 4);
            }
            {
                ShaderPC pc{}; pc.i0 = radius; pc.i2 = 1;
                Binding b[3] = {{0, cand_global, 0}, {1, align_state, 0}, {2, global_shift, 0}};
                vk.Dispatch("select_min", pc, 1, 1, 1, b, 3);
            }
        }
        vk.FlushFrame();

        // Tile refinement. Standard path uses the diagonal-wavefront shader
        // (tile_refine_diag) which propagates left/top neighbours like the CPU
        // RefineTileField. Dense path uses per-level propagate/correct/search.
        vk.BeginFrame();
        if (settings.alignment_mode == AlignmentMode::DenseTile)
        {
            DenseAlignGPU(vk, ref_pyr, cmp_pyr, pw, ph, tile_size, pyr_n, tsx, tsy);
        }
        else
        {
            ShaderPC pc{};
            pc.w = pw; pc.h = ph; pc.channels = 1;
            pc.i0 = tile_size; pc.i1 = local_radius; pc.i2 = tiles_x; pc.i3 = tiles_y;
            pc.i5 = 1;
            int diags = tiles_x + tiles_y - 1;
            for (int d = 0; d < diags; ++d)
            {
                pc.i4 = d;
                Binding b[5] = {{0, gray[ref_idx], 0}, {1, gray[i], 0},
                                {2, global_shift, 0}, {3, tsx, 0}, {4, tsy, 0}};
                vk.Dispatch("tile_refine_diag", pc, 1, 1, 1, b, 5);
            }
        }
        uint64_t warped = vk.CreateBuffer(pw * ph * ch);
        {
            ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = int(ch);
            pc.i0 = tile_size; pc.i1 = align_tiles_x; pc.i2 = align_tiles_y;
            pc.i3 = dense ? half_tile : 0;  // tile_spacing (0 = use tile_size)
            pc.i4 = 1;
            Binding b[4] = {{0, plane[i], 0}, {1, warped, 0}, {2, tsx, 0}, {3, tsy, 0}};
            vk.Dispatch("warp_tilefield", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 4);
        }
        vk.FlushFrame();
        for (auto& lvl : cmp_pyr) if (lvl.handle != gray[i]) vk.DestroyBuffer(lvl.handle);
        aligned.push_back(warped);

    }

    vk.DestroyBuffer(align_state); vk.DestroyBuffer(global_shift);
    vk.DestroyBuffer(cand_global); vk.DestroyBuffer(tsx); vk.DestroyBuffer(tsy);
    for (size_t i = 0; i < N; ++i) vk.DestroyBuffer(gray[i]);
    } // align

    Report(progress, PipelineConstants::kProgressMerge, "GPU: merging");
    uint64_t merged = 0;
    { ProfileScope _ps("time.gpu.merge");
    if (settings.merge_algo == MergeAlgorithm::TemporalAverage)
    {
        merged = vk.CreateBuffer(pw * ph * ch);
        uint64_t wsum = vk.CreateBuffer(pw * ph * ch);  // per-element (NHWC), matches CPU
        vk.FillFloat(wsum, 1.0f);
        // acc = ref
        {
            ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = int(ch);
            Binding b[2] = {{0, plane[ref_idx], 0}, {1, merged, 0}};
            vk.BeginFrame(); vk.Dispatch("copy", pc, (size_t(pw) * ph * ch + 255) / 256, 1, 1, b, 2); vk.FlushFrame();
        }
        float wl = float(images[ref_idx].metadata.white_level);
        float bl = mean_bl[ref_idx];
        float range = std::max(1.0f, wl - bl);
        for (size_t k = 0; k < aligned.size(); ++k)
        {
            uint64_t cmp_blur = vk.CreateBuffer(pw * ph * ch);
            vk.BeginFrame();
            BoxBlurGPU(vk, aligned[k], cmp_blur, pw, ph, int(ch), 2);
            vk.FlushFrame();
            vk.BeginFrame();
            {
                ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = int(ch);
                pc.f0 = bl; pc.f1 = range; pc.f2 = comp_scale[k];
                Binding b[4] = {{0, merged, 0}, {1, wsum, 0}, {2, aligned[k], 0}, {3, cmp_blur, 0}};
                vk.Dispatch("temporal_acc_exposure", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 4);
            }
            vk.FlushFrame();
            vk.DestroyBuffer(cmp_blur);
        }
        vk.BeginFrame();
        {
            ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = int(ch); pc.i6 = 1;  // wsum NHWC
            Binding b[2] = {{0, merged, 0}, {1, wsum, 0}};
            vk.Dispatch("normalize_div", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 2);
        }
        vk.FlushFrame();
        vk.DestroyBuffer(wsum);
        for (size_t k = 0; k < aligned.size(); ++k) vk.DestroyBuffer(aligned[k]);
    }
    else if (settings.merge_algo == MergeAlgorithm::TemporalMedian)
    {
        size_t ff = size_t(pw) * ph * ch;
        std::vector<float> packed_host(ff * aligned.size(), 0.0f);
        for (size_t k = 0; k < aligned.size(); ++k)
        {
            std::vector<float> tmp(ff);
            vk.DownloadFloats(aligned[k], tmp.data(), uint32_t(ff));
            std::memcpy(packed_host.data() + k * ff, tmp.data(), ff * sizeof(float));
        }
        uint64_t packed = vk.CreateBufferFromFloats(packed_host.data(), uint32_t(ff * aligned.size()));
        float wl = float(images[ref_idx].metadata.white_level);
        float bl = mean_bl[ref_idx];
        float clip = (wl > bl + 1.0f) ? (wl - bl) * 0.98f : 0.0f;
        std::vector<float> clip_scaled(aligned.size());
        for (size_t k = 0; k < aligned.size(); ++k) clip_scaled[k] = clip * comp_scale[k];
        uint64_t clipbuf = vk.CreateBufferFromFloats(clip_scaled.data(), uint32_t(clip_scaled.size()));
        merged = vk.CreateBuffer(pw * ph * ch);
        ShaderPC pc{}; pc.w = pw; pc.h = ph; pc.channels = int(ch);
        pc.i0 = int(aligned.size()); pc.i1 = int(ff); pc.f0 = clip;
        Binding b[4] = {{0, plane[ref_idx], 0}, {1, merged, 0}, {2, packed, 0}, {3, clipbuf, 0}};
        vk.BeginFrame();
        vk.Dispatch("temporal_median", pc, (pw + 7) / 8, (ph + 7) / 8, 1, b, 4);
        vk.FlushFrame();
        vk.DestroyBuffer(packed); vk.DestroyBuffer(clipbuf);
        for (size_t k = 0; k < aligned.size(); ++k) vk.DestroyBuffer(aligned[k]);
    }
    else if (settings.merge_algo == MergeAlgorithm::Frequency)
    {
        merged = vk.CreateBuffer(pw * ph * ch);
        FrequencyMergeGPU(vk, plane[ref_idx], aligned, images, ref_idx, settings, pw, ph, int(ch), mean_bl[ref_idx], merged);
        for (size_t k = 0; k < aligned.size(); ++k) vk.DestroyBuffer(aligned[k]);
    }
    else // Spatial
    {
        merged = SpatialMergeGPU(vk, plane[ref_idx], aligned, images, ref_idx, settings, pw, ph, int(ch), mean_bl[ref_idx], comp_orig, comp_scale);
        for (size_t k = 0; k < aligned.size(); ++k) vk.DestroyBuffer(aligned[k]);
    }

    for (size_t i = 0; i < N; ++i) vk.DestroyBuffer(plane[i]);
    } // merge

    FloatImage out;
    { ProfileScope _ps("time.gpu.download");
    out.width = uint32_t(pw);
    out.height = uint32_t(ph);
    out.channels = ch;
    out.data.resize(size_t(pw) * ph * ch);
    vk.DownloadFloats(merged, out.data.data(), uint32_t(out.data.size()));
    vk.DestroyBuffer(merged);
    } // download

    Report(progress, PipelineConstants::kProgressExposure, "GPU: merge complete");
    return out;
}

} // namespace burstmerge
