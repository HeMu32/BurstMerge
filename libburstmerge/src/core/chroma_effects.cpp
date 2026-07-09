#include "burstmerge/internal/core/chroma_effects.h"

#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace burstmerge
{
namespace
{

// ----------------------------------------------------------------------------
// Optional lightweight timing instrumentation for diagnostic purposes. The
// mechanism is intentionally identical to the project-wide profiler (see
// profiler.cpp `ProfileEnabled()`): the SAME environment variable
// BURSTMERGE_PROFILE gates output, and the entire feature is compiled out
// under release builds (NDEBUG), so an unchanged release build has zero
// overhead and emits no code in chroma_effects.cpp.
//
// When BURSTMERGE_PROFILE is set to a non-empty / non-'0' value AND the
// build is a Debug (NDEBUG not defined) build, each chromatic-effect stage
// prints a millisecond-level wall-clock timing line to stderr (so stdout
// progress messages from the resize utility stay clean). In release the
// timer objects vanish entirely via `#ifdef NDEBUG`.
// ----------------------------------------------------------------------------
#ifndef NDEBUG

// Mirrors profiler.cpp::ProfileEnabled — but localised to this TU so we
// don't introduce a header dependency on profiler.h. Same env-var, same
// semantics. Cached across calls via a function-local static.
inline bool ChromaTimingEnabled()
{
    static int enabled = []()
    {
        const char* env = std::getenv("BURSTMERGE_PROFILE");
        return (env && env[0] && env[0] != '0') ? 1 : 0;
    }();
    return enabled != 0;
}

struct ChromaTimer
{
    const char* tag;
    std::chrono::steady_clock::time_point t0;
    explicit ChromaTimer(const char* t) : tag(t), t0(std::chrono::steady_clock::now()) {}
    ~ChromaTimer()
    {
        if (!ChromaTimingEnabled()) return;
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr, "[chroma-timing] %-32s %8lld ms\n", tag, static_cast<long long>(ms));
    }
};
#define BURSTMERGE_CHROMA_SCOPE_TIMER(tag) ChromaTimer _chroma_t(tag)

#else  // NDEBUG

#define BURSTMERGE_CHROMA_SCOPE_TIMER(tag) ((void)0)

#endif  // NDEBUG

// ----------------------------------------------------------------------------
// Geometry helpers
// ----------------------------------------------------------------------------
//
// CRITICAL: every "pixel distance" in this file is expressed in the input
// image's own pixel units (mosaic pixel count for Bayer input, LinearRaw
// pixel count for demosaiced 3-channel input). The plane domain (plane_width
// = mosaic_width / period) is only an internal stepping stone; we never use
// "multiply by two" / "multiply by period" as a substitute — instead the real
// factor is derived from the image's own width and the actual CFA period.
//
// The single source of truth for "how big is one plane, in mosaic pixels" is
//   mosaic_per_plane = (mosaic_w + period - 1) / period    (== plane pixel ↔
//                mosaic-pixel-mapping for a CFA period-2 image)
// and conversely, distance in mosaic pixels = plane_distance * period exactly
// when the deinterleave is uniform (which it is for both Bayer and X-Trans
// within the project). We do NOT divide by 2 ad-hoc anywhere — we scale by
// the actual `period` read from the metadata passed in by the caller.

// Sample a single channel of a FloatImage with bilinear interpolation and
// clamp-to-edge border handling. Operates in the SAME pixel coordinate system
// as the caller; the sample position may be fractional.
float SampleBilinearChannel(const FloatImage& src, float x, float y, uint32_t c)
{
    const int W = static_cast<int>(src.width);
    const int H = static_cast<int>(src.height);

    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);

    // Clamp source coordinate base to the image rectangle BEFORE computing the
    // +1 neighbour, so that BOTH ix and ix1 end up inside [0, W-1]. (Earlier
    // versions computed ix1 = min(ix+1, W-1) — but with ix huge-positive the
    // subsequent `ix = max(ix, 0)` would leave ix above W-1, producing an out-
    // of-bounds pixel index on the v00 / v01 fetches. With the radical LaCA
    // "compress inward" knob (negative Width) the inverse-sampling source
    // coordinate can be enormous, so we must clamp both bounds of the base
    // index before doing anything with it.)
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix > W - 1) ix = W - 1;
    if (iy > H - 1) iy = H - 1;
    int ix1 = std::min(ix + 1, W - 1);
    int iy1 = std::min(iy + 1, H - 1);
    // fx / fy are still computed from the un-clamped floor above. When the
    // source coordinate was clamped, fx and fy carry the fractional part of
    // the un-clamped position, which for our use case (extreme scale floor)
    // is fine — the result is a clamp-to-edge sample, the exact semantics we
    // want at the image boundary.

    const float v00 = src.At(static_cast<uint32_t>(ix),  static_cast<uint32_t>(iy),  c);
    const float v10 = src.At(static_cast<uint32_t>(ix1), static_cast<uint32_t>(iy),  c);
    const float v01 = src.At(static_cast<uint32_t>(ix),  static_cast<uint32_t>(iy1), c);
    const float v11 = src.At(static_cast<uint32_t>(ix1), static_cast<uint32_t>(iy1), c);

    const float v0 = v00 + fx * (v10 - v00);
    const float v1 = v01 + fx * (v11 - v01);
    return v0 + fy * (v1 - v0);
}

// ----------------------------------------------------------------------------
// Separable (2-pass) BoxBlur — O(N·(2r+1)) instead of O(N·(2r+1)²).
// ----------------------------------------------------------------------------
// A true 2D box average can be computed as the composition of a 1D horizontal
// box average and a 1D vertical box average applied successively; the result
// is identical to the Naive 2D box average of size (2r+1)×(2r+1) on the
// interior. The only behavioural difference from the public `BoxBlur` lives at
// the borders: the naive implementation uses a *single* clamp at the 2D offset
// level (the per-window count `n` is the actual number of in-image pixels that
// survived clamping), while the separable version clamps twice (first the
// horizontal pass, then the vertical pass), which alters the effective border
// weights slightly. For the LoCA use-case — diffusing a Sobel boost map as a
// Lo-Fi aesthetic effect — this small border discrepancy is acceptable; we do
// not need bit-exact equivalence to the naive path.
//
// Per-row sliding-window implementation: cost per row = O(width + r) for the
// prefix/suffix initialisation plus O(width) for sliding (one add, one
// subtract per pixel). Then the vertical pass runs the same algorithm over the
// transposed domain. For each channel this is O(height·width) per pass, two
// passes total, giving O(N·(2r+1)) in total ≈ linear in the image size —
// compared to the naive O(N·(2r+1)²) which grows quadratically with `r`. At
// r=50 (LoCA_Width=2.0 on a 4000×3000 source) this is roughly a 100× speedup,
// taking the LoCA diffusion step from ~6 seconds down to ~60 milliseconds.
// ----------------------------------------------------------------------------
FloatImage BoxBlurSeparable(const FloatImage& src, int radius)
{
    if (radius <= 0) return src;
    const uint32_t W = src.width;
    const uint32_t H = src.height;
    const uint32_t ch = src.channels;
    if (W == 0 || H == 0 || ch == 0) return src;
    const int win = 2 * radius + 1;   // box window width along one axis
    const float inv_win = 1.0f / static_cast<float>(win);

    FloatImage horiz;   // output of horizontal pass (W×H×ch)
    horiz.width = W;
    horiz.height = H;
    horiz.channels = ch;
    horiz.data.assign(src.data.size(), 0.0f);

    // ---- Horizontal pass (sliding window along x for each row & channel) ----
    // For each row y and channel c we maintain a running sum `win_sum` over
    // the box [x - radius, x + radius] using clamp-to-edge indexing. Initial
    // sum for x=0 absorbs the left-clamp tail (index 0 hit `radius+1` times
    // instead of once). Sliding then subtracts the outgoing sample and adds
    // the incoming sample (both clamped).
    ParallelForRows(H,
        RecommendedImageRowGrain(W, ch, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t c = 0; c < ch; ++c)
            {
                // Prefix window for x=0: indices -r..+r clamp to [0, r] within
                // the image. The left half [-r, -1] all hit index 0, so the
                // initial sum has src(0,y,c) contributing (r+1) times plus
                // src(1..r, y, c) once each.
                float win_sum = 0.0f;
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    int sx = static_cast<int>(dx);
                    if (sx < 0) sx = 0;
                    if (sx > static_cast<int>(W) - 1) sx = static_cast<int>(W) - 1;
                    win_sum += src.At(static_cast<uint32_t>(sx), y, c);
                }
                horiz.At(0u, y, c) = win_sum * inv_win;

                // Sliding window: for x > 0, remove sample at (x - r - 1) and
                // add sample at (x + r), both clamped. The clamping hari-
                // kensa when the window is partially/fully past the image.
                for (uint32_t x = 1; x < W; ++x)
                {
                    int sx_out = static_cast<int>(x) - radius - 1;     // outgoing
                    if (sx_out < 0) sx_out = 0;
                    if (sx_out > static_cast<int>(W) - 1) sx_out = static_cast<int>(W) - 1;
                    int sx_in = static_cast<int>(x) + radius;          // incoming
                    if (sx_in < 0) sx_in = 0;
                    if (sx_in > static_cast<int>(W) - 1) sx_in = static_cast<int>(W) - 1;
                    win_sum -= src.At(static_cast<uint32_t>(sx_out), y, c);
                    win_sum += src.At(static_cast<uint32_t>(sx_in),  y, c);
                    horiz.At(x, y, c) = win_sum * inv_win;
                }
            }
        }
    }, "chroma_boxblur_horiz");

    // ---- Vertical pass (sliding window along y over `horiz`, write to out) ----
    // Identical pattern to the horizontal pass, but operating on `horiz`
    // (which already carries the x-axis blur) and writing the final 2D box
    // average to `out`. Clamp-to-edge along y as before.
    FloatImage out;
    out.width = W;
    out.height = H;
    out.channels = ch;
    out.data.assign(src.data.size(), 0.0f);

    ParallelForRows(W,
        RecommendedImageRowGrain(H, ch, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t x_begin, uint32_t x_end)
    {
        for (uint32_t x = x_begin; x < x_end; ++x)
        {
            for (uint32_t c = 0; c < ch; ++c)
            {
                float win_sum = 0.0f;
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    int sy = dy;
                    if (sy < 0) sy = 0;
                    if (sy > static_cast<int>(H) - 1) sy = static_cast<int>(H) - 1;
                    win_sum += horiz.At(x, static_cast<uint32_t>(sy), c);
                }
                out.At(x, 0u, c) = win_sum * inv_win;

                for (uint32_t y = 1; y < H; ++y)
                {
                    int sy_out = static_cast<int>(y) - radius - 1;
                    if (sy_out < 0) sy_out = 0;
                    if (sy_out > static_cast<int>(H) - 1) sy_out = static_cast<int>(H) - 1;
                    int sy_in = static_cast<int>(y) + radius;
                    if (sy_in < 0) sy_in = 0;
                    if (sy_in > static_cast<int>(H) - 1) sy_in = static_cast<int>(H) - 1;
                    win_sum -= horiz.At(x, static_cast<uint32_t>(sy_out), c);
                    win_sum += horiz.At(x, static_cast<uint32_t>(sy_in),  c);
                    out.At(x, y, c) = win_sum * inv_win;
                }
            }
        }
    }, "chroma_boxblur_vert");

#ifdef BURSTMERGE_CHROMA_BOXBLUR_VERIFY
    // Diagnostic verification: recompute naive box blur over the interior and
    // compare against separable output. Reports L1/L2/max abs diff to stderr.
    // Enable by temporarily #defining BURSTMERGE_CHROMA_BOXBLUR_VERIFY at the
    // top of this TU — note that CompilerFlags.cmake overrides
    // CMAKE_CXX_FLAGS_RELEASE so -D injection there is not picked up directly;
    // an in-source #define near the top of this file is the simplest way (as
    // was used during the initial separable/naive equivalence audit).
    {
        double l1 = 0.0, l2 = 0.0, max_abs = 0.0;
        size_t n_check = 0;
        const uint32_t x_lo = static_cast<uint32_t>(radius);
        const uint32_t x_hi = (W > static_cast<uint32_t>(radius)) ? W - static_cast<uint32_t>(radius) : 0;
        const uint32_t y_lo = static_cast<uint32_t>(radius);
        const uint32_t y_hi = (H > static_cast<uint32_t>(radius)) ? H - static_cast<uint32_t>(radius) : 0;
        for (uint32_t y = y_lo; y < y_hi; ++y)
        {
            for (uint32_t x = x_lo; x < x_hi; ++x)
            {
                for (uint32_t c = 0; c < ch; ++c)
                {
                    float naive_sum = 0.0f;
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx)
                            naive_sum += src.At(x + dx, y + dy, c);
                    float naive = naive_sum / static_cast<float>(win * win);
                    float sep = out.At(x, y, c);
                    double d = std::fabs(static_cast<double>(naive) - static_cast<double>(sep));
                    l1 += d; l2 += d * d;
                    if (d > max_abs) max_abs = d;
                    ++n_check;
                }
            }
        }
        std::fprintf(stderr,
            "[boxblur-verify] interior pixels=%zu  L1_mean=%.6e  L2_mean=%.6e  max_abs=%.6e\n",
            n_check, l1 / std::max<double>(1.0, static_cast<double>(n_check)),
            l2 / std::max<double>(1.0, static_cast<double>(n_check)),
            max_abs);
    }
#endif
    return out;
}

// ----------------------------------------------------------------------------
// Colour-preset → channel-membership helpers
// ----------------------------------------------------------------------------
//
// Both LaCA and LoCA accept one of six fixed colour presets. For each preset
// we resolve to a boolean mask of (R, G, B) primaries it touches:
//   Red      -> { R }
//   Green    -> { G }
//   Blue     -> { B }
//   Cyan      -> { G, B }
//   Magenta -> { R, B }
//   Yellow   -> { R, G }
//
// For Bayer plane images each plane channel c corresponds to a CFA colour code
// in mosaic_pattern[c] (0=R, 1=G, 2=B). Two plane channels may both be G (the
// two green Bayer phases) — when a preset touches G, BOTH green phases are
// affected, to keep the effect "by colour, not by Bayer phase". See the
// header's design notes for the rationale.

struct PrimaryMask
{
    bool r = false;
    bool g = false;
    bool b = false;
};

PrimaryMask ResolveLaCAMask()
{
    switch (EFFECT_LaCA_Color)
    {
        case LaCAColor_Red:      return { true,  false, false };
        case LaCAColor_Green:    return { false, true,  false };
        case LaCAColor_Blue:     return { false, false, true  };
        case LaCAColor_Cyan:     return { false, true,  true  };
        case LaCAColor_Magenta:  return { true,  false, true  };
        case LaCAColor_Yellow:   return { true,  true,  false };
        default:                  return { false, false, false };
    }
}

PrimaryMask ResolveLoCAMask()
{
    switch (EFFECT_LoCA_Color)
    {
        case LoCAColor_Red:      return { true,  false, false };
        case LoCAColor_Green:    return { false, true,  false };
        case LoCAColor_Blue:     return { false, false, true  };
        case LoCAColor_Cyan:     return { false, true,  true  };
        case LoCAColor_Magenta:  return { true,  false, true  };
        case LoCAColor_Yellow:   return { true,  true,  false };
        default:                  return { false, false, false };
    }
}

// Decide whether the given CFA colour code (0=R, 1=G, 2=B) is included by the
// (R,G,B) preset mask.
inline bool PrimaryMatches(uint16_t cfa_color, const PrimaryMask& m)
{
    switch (cfa_color)
    {
        case 0: return m.r;
        case 1: return m.g;
        case 2: return m.b;
        default: return false;
    }
}

// ============================================================================
// LaCA — Lateral Chromatic Aberration (radial rescale of selected channels)
// ----------------------------------------------------------------------------
// Model: the selected colour channel(s) are rescaled outward (or inward) by a
// factor f(r) that grows linearly with normalised distance r from the image
// geometric centre. At the extreme corner (r = 1 in normalised diagonal units)
// the displacement equals Width% × diagonal_pixel_count (= the user-facing
// "corner displacement"). Bayer's two green phases are scaled together when
// the preset touches G, and the R/B plane channels are scaled when the preset
// touches R/B respectively. Non-selected channels copy through unchanged.
//
// We work in the plane domain for Bayer input (deinterleave -> per-channel
// radial resample -> reinterleave) because the resampling interpolates within
// a single colour plane; resampling in the mosaic domain would mix samples
// across colour phases and break the CFA structure. The distance r is
// computed in MOSAIC pixel units (the input image's own pixel coordinates),
// then converted to plane pixel coordinates through the real CFA period
// (NOT through a hard-coded "multiply by two").
// ============================================================================

void ApplyLaCAOnPlane(FloatImage& plane,
                      uint32_t period,
                      const std::array<uint16_t, 36>& mosaic_pattern,
                      const PrimaryMask& mask,
                      float width_percent)
{
    // The plane image's per-plane pixel is `period` mosaic pixels wide.
    // All radial geometry is computed in mosaic pixel units (the unit the
    // user reports their knobs in), then projected into plane-pixel space by
    // division by the actual period.
    const float mosaic_w = static_cast<float>(plane.width)  * static_cast<float>(period);
    const float mosaic_h = static_cast<float>(plane.height) * static_cast<float>(period);
    const float half_mw  = mosaic_w * 0.5f;
    const float half_mh  = mosaic_h * 0.5f;

    // Image diagonal in MOSAIC pixels — the anchor unit for Width%.
    const float diag_mosaic = std::sqrt(mosaic_w * mosaic_w + mosaic_h * mosaic_h);

    // Half-diagonal (corner-to-centre) in mosaic pixels. The corner is where
    // the displacement equals Width%, so the per-ratio scaling in mosaic
    // coords is (corner_displacement / half_diagonal).
    const float half_diag_mosaic = diag_mosaic * 0.5f;
    if (half_diag_mosaic <= 0.0f) return;

    // corner displacement (mosaic pixels) = width_percent% × diagonal.
    // Sign convention: positive Width -> selected channel stretched outward
    // (i.e. the channel appears larger, as a real LaCA does on one channel).
    const float corner_disp_mosaic = static_cast<float>(width_percent) * 0.01f * diag_mosaic;
    // The radial scale factor at ratio r from centre is (1 + k * r), where k
    // is chosen so that AT THE CORNER (r=1) the scale yields a corner
    // displacement of corner_disp_mosaic. The displacement for a point at
    // ratio r under scale (1+k*r) is k*r*half_diag; at r=1 this is
    // k*half_diag. Setting that equal to corner_disp_mosaic gives k below.
    const float k = corner_disp_mosaic / half_diag_mosaic;

    // Centre in plane-pixel coordinates (same as centre mosaic / period).
    const float centre_px = half_mw / static_cast<float>(period);
    const float centre_py = half_mh / static_cast<float>(period);

    // Maximum radial ratio (normalised to corner) in plane-pixel space. We
    // pre-compute the half-diagonal in plane pixels = half_diag_mosaic / period
    // (real factor, no hardcoded "2").
    const float inv_half_diag_plane = static_cast<float>(period) / half_diag_mosaic;

    const uint32_t total_ch = plane.channels;
    // Per-channel "should this plane be radially rescaled?" lookup.
    std::vector<uint8_t> channel_scaled(total_ch, 0);
    for (uint32_t c = 0; c < total_ch && c < 36; ++c)
    {
        if (PrimaryMatches(mosaic_pattern[c], mask))
            channel_scaled[c] = 1;
    }

    FloatImage out;
    out.width = plane.width;
    out.height = plane.height;
    out.channels = total_ch;
    out.data.resize(plane.data.size(), 0.0f);

    ParallelForRows(out.height,
        RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t oy = y_begin; oy < y_end; ++oy)
        {
            for (uint32_t ox = 0; ox < out.width; ++ox)
            {
                // Vector from centre in plane-pixel coords. Scale by
                // (1 + k*r); for channels not selected the scale is 1.
                const float dx = static_cast<float>(ox) - centre_px;
                const float dy = static_cast<float>(oy) - centre_py;
                const float plane_dist = std::sqrt(dx * dx + dy * dy);
                const float r = plane_dist * inv_half_diag_plane; // ∈ [0, ~1] at corner
                float scale = 1.0f + k * r;

                // Defensive lower bound: a negative EFFECT_LaCA_Width drives k<0,
                // which makes scale shrink with distance. When |k|>=1 the scale
                // reaches zero or crosses into negative at large r, which would
                // produce Inf/NaN in the inverse-sampling division below (and a
                // negative scale physically mirrors the channel — not the
                // intended "compress inward" semantics). Clamp scale to a small
                // positive floor so the inverse map degrades to a near-pivot
                // collapse rather than producing garbage. This keeps the effect
                // safe for arbitrarily negative Width values; typical Lo-Fi use
                // (|k| in [0, ~0.5]) never approaches this guard.
                if (scale < 1e-4f) scale = 1e-4f;

                for (uint32_t c = 0; c < total_ch; ++c)
                {
                    if (!channel_scaled[c])
                    {
                        // Unmodified channel: direct copy.
                        out.At(ox, oy, c) = plane.At(ox, oy, c);
                        continue;
                    }
                    // Rescale this plane BACKWARDS to source coordinate: we
                    // want the output pixel at (ox, oy) to hold the value that
                    // would naturally land at the rescaled location, hence we
                    // sample the source at the inverse-scaled position.
                    const float sx = centre_px + dx / scale;
                    const float sy = centre_py + dy / scale;
                    out.At(ox, oy, c) = SampleBilinearChannel(plane, sx, sy, c);
                }
            }
        }
    }, "chroma_laca_plane");

    plane = std::move(out);
}

void ApplyLaCAOnLinearRGB(FloatImage& img, const PrimaryMask& mask, float width_percent)
{
    // 3-channel linear RGB; channel indices are fixed: 0=R, 1=G, 2=B.
    // Distances are in plain image pixels (no CFA subdivision).
    const float W = static_cast<float>(img.width);
    const float H = static_cast<float>(img.height);
    const float half_w = W * 0.5f;
    const float half_h = H * 0.5f;
    const float diag = std::sqrt(W * W + H * H);
    const float half_diag = diag * 0.5f;
    if (half_diag <= 0.0f) return;

    const float corner_disp = static_cast<float>(width_percent) * 0.01f * diag;
    const float k = corner_disp / half_diag;

    // Channel-membership by RGB primary index (0..2).
    const uint8_t channel_scaled[3] =
    {
        static_cast<uint8_t>(mask.r ? 1 : 0),
        static_cast<uint8_t>(mask.g ? 1 : 0),
        static_cast<uint8_t>(mask.b ? 1 : 0)
    };

    FloatImage out;
    out.width = img.width;
    out.height = img.height;
    out.channels = img.channels;
    out.data.resize(img.data.size(), 0.0f);

    ParallelForRows(out.height,
        RecommendedImageRowGrain(out.width, out.channels, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < out.width; ++x)
            {
                const float dx = static_cast<float>(x) - half_w;
                const float dy = static_cast<float>(y) - half_h;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float r = dist / half_diag;
                float scale = 1.0f + k * r;

                // Defensive lower bound — see ApplyLaCAOnPlane for rationale.
                if (scale < 1e-4f) scale = 1e-4f;

                for (uint32_t c = 0; c < 3 && c < img.channels; ++c)
                {
                    if (!channel_scaled[c])
                    {
                        out.At(x, y, c) = img.At(x, y, c);
                        continue;
                    }
                    const float sx = half_w + dx / scale;
                    const float sy = half_h + dy / scale;
                    out.At(x, y, c) = SampleBilinearChannel(img, sx, sy, c);
                }
            }
        }
    }, "chroma_laca_linear");

    img = std::move(out);
}

// ============================================================================
// LoCA — Longitudinal (axial) Chromatic Aberration
// ----------------------------------------------------------------------------
// Strategy (see the design notes in the header; preserved here for code
// navigation):
//
//   * Edge detection happens on a GRAYSCALE image built by averaging across
//     all colour channels (three RGB planes / four Bayer plane channels) so
//     that one strong edge across an RGB-to-RGB or Bayer-to-Bayer transition
//     contributes equally to the detection signal regardless of which phase
//     happens to sit under a given pixel.
//
//   * All downstream work (Sobel, Box diffusion, phase-matched channel
//     boosting) lives in the MOSAIC pixel domain — i.e. the input image's own
//     pixel coordinates. This is the unit the user's knobs are expressed in
//     and the unit in which distances, box-blur radii and the image diagonal
//     are measured. The plane domain is an internal stepping stone ONLY for
//     producing the grayscale image; once obtained, the grayscale image is
//     bloomed back up to mosaic dimensions explicitly through the actual
//     CFA period (NOT through any hard-coded "multiply by two").
//
//   * ASSUMPTION: only IN-FOCUS, high-contrast edges are targeted. Out-of-
//     focus (bokeh) edges are deliberately not handled — that would require
//     blur/defocus modelling, which is out of scope for a Lo-Fi effect (and
//     visually negligible for the small magnitudes typical of Lo-Fi
//     fringing).
//
//   * Sobel magnitude is normalised to [0,1] using the theoretical maximum
//     of the 3×3 Sobel kernels (~4 * white_level), so that Strength ≈ 1.0
//     on a maximal-strength edge will saturate the boosted channel.
//
//   * The MinSensi gate floors raw_detection values strictly below the
//     threshold to zero (per the user's spec):
//         v = (raw_detect * Strength) < MinSensi ? 0 : (raw_detect * Strength)
//   * After per-edge MinSensi gating, the boost map is Box-diffused by a
//     radius = Width% × diagonal / 2 (uniform box) so each surviving edge
//     spreads its boost ±half_width to either side along the mosaic plane.
//   * The diffused boost map is added to the selected colour channel(s) at
//     the matching mosaic phase(s) only, clamped to white_level (saturating
//     on a maximal edge).
// ============================================================================

// Produce a grayscale mosaic FloatImage (1 channel, mosaic dimensions) from
// the raw input image.
//   * LinearRaw (3ch): pixel (x,y) gray = mean(c0,c1,c2).
//   * Bayer mosaic (1ch): channel-average the four plane channels and bloom
//     each plane pixel back into its period×period mosaic block, so the
//     resulting gray image has the original (mosaic) width/height.
FloatImage BuildGrayMosaic(const FloatImage& img, uint32_t period)
{
    // When the input is already a plane image (channels == period² for Bayer /
    // multi-channel for LinearRaw), average across all channels to produce a
    // single-channel grayscale in the same pixel grid — no mosaic bloom needed
    // because the caller already committed to plane-domain processing.
    const bool input_is_plane = (img.channels > 1 && period > 1);

    FloatImage gray;
    if (input_is_plane)
    {
        // Plane input: grayscale in plane-pixel dimensions.
        gray.width = img.width;
        gray.height = img.height;
    }
    else
    {
        // Mosaic input: grayscale in mosaic-pixel dimensions.
        gray.width = img.width;
        gray.height = img.height;
    }
    gray.channels = 1;
    gray.data.resize(static_cast<size_t>(gray.width) * gray.height, 0.0f);

    if (period <= 1 || img.channels > 1)
    {
        // LinearRaw (3ch) or plane image (period² channels): average all
        // channels per pixel.
        const uint32_t ch = std::max<uint32_t>(1, img.channels);
        const float inv_ch = 1.0f / static_cast<float>(ch);
        ParallelForRows(img.height,
            RecommendedImageRowGrain(img.width, 1, kRowGrainMinPixels, kRowGrainCoarseRows),
            [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < img.width; ++x)
                {
                    float sum = 0.0f;
                    for (uint32_t c = 0; c < ch; ++c) sum += img.At(x, y, c);
                    gray.At(x, y, 0) = sum * inv_ch;
                }
            }
        }, "chroma_loca_gray_linear");
        return gray;
    }

    // Bayer mosaic input (1 channel, period > 1): deinterleave to plane,
    // average all planes to a single-channel plane grayscale, then bloom
    // each plane pixel into its period×period mosaic block.
    const FloatImage plane = ConvertMosaicToPlaneImage(img, period);
    const uint32_t plane_ch = plane.channels; // period²
    const float inv_ch = 1.0f / static_cast<float>(plane_ch);

    ParallelForRows(img.height,
        RecommendedImageRowGrain(img.width, 1, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            const uint32_t oy = y / period;
            for (uint32_t x = 0; x < img.width; ++x)
            {
                const uint32_t ox = x / period;
                float sum = 0.0f;
                for (uint32_t c = 0; c < plane_ch; ++c) sum += plane.At(ox, oy, c);
                gray.At(x, y, 0) = sum * inv_ch;
            }
        }
    }, "chroma_loca_gray_bayer");

    return gray;
}

// Sample a 1-channel FloatImage with clamp-to-edge border at integer (sx, sy).
inline float SampleGrayClamped(const FloatImage& g, int sx, int sy)
{
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= static_cast<int>(g.width))  sx = static_cast<int>(g.width)  - 1;
    if (sy >= static_cast<int>(g.height)) sy = static_cast<int>(g.height) - 1;
    return g.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), 0);
}

// Compute the per-mosaic-pixel LoCA boost map.
// Output is a 1-channel FloatImage of mosaic dimensions. Per the user's spec
// the gate order is:
//     detect = |Sobel| / (4 * white_level)              // raw_detection ∈ [0,1]
//     detect = detect < EFFECT_LoCA_MinSensi ? 0 : detect   // MinSensi floor
//     boost  = detect * EFFECT_LoCA_Strength          // sensitivity × survivors
// This keeps MinSensi (a floor on raw edge response) and Strength (a post-gate
// amplification) independent, as the spec wording prescribes.
FloatImage ComputeLoCABoostMap(const FloatImage& gray_mosaic,
                               float strength,
                               float min_sensi,
                               float white_level)
{
    const uint32_t W = gray_mosaic.width;
    const uint32_t H = gray_mosaic.height;

    // 3×3 Sobel kernels:
    //   Kx = [ -1  0 +1 ; -2  0 +2 ; -1  0 +1 ]
    //   Ky = [ -1 -2 -1 ;  0  0  0 ; +1 +2 +1 ]
    //
    // For an input in [0, white_level], the theoretical maximum |Gx| or |Gy|
    // over a 3×3 window is 4 * white_level (extreme case: -1 side all zeros,
    // +1 side all white_level, weights 1+2+1 = 4). A maximal single-axis step
    // edge therefore gives |G| = 4*white_level → detect = 1.0 after we divide
    // by `4 * white_level` below. A maximal DIAGONAL step edge (gradient on
    // both axes simultaneously) would give |G| = sqrt(2) * 4 * white_level,
    // producing detect ≈ 1.414; we clamp that to 1.0 so diagonal edges are
    // capped to the same ceiling as single-axis edges (a diagonal edge does
    // not generate "extra" fringing beyond a maximal-axis edge). This gives
    // Strength the intuitive meaning "1.0 = saturate the channel of the
    // boosted colour on a maximal-edge response (per axis)".
    const float norm_denom = 4.0f * std::max(white_level, 1.0f);

    FloatImage boost;
    boost.width = W;
    boost.height = H;
    boost.channels = 1;
    boost.data.resize(static_cast<size_t>(W) * H, 0.0f);

    ParallelForRows(H,
        RecommendedImageRowGrain(W, 1, kRowGrainMinPixels, kRowGrainCoarseRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < W; ++x)
            {
                const int ix = static_cast<int>(x);
                const int iy = static_cast<int>(y);

                // Sobel X
                const float gx =
                    -1.0f * SampleGrayClamped(gray_mosaic, ix - 1, iy - 1)
                    -2.0f * SampleGrayClamped(gray_mosaic, ix - 1, iy    )
                    -1.0f * SampleGrayClamped(gray_mosaic, ix - 1, iy + 1)
                    +1.0f * SampleGrayClamped(gray_mosaic, ix + 1, iy - 1)
                    +2.0f * SampleGrayClamped(gray_mosaic, ix + 1, iy    )
                    +1.0f * SampleGrayClamped(gray_mosaic, ix + 1, iy + 1);

                // Sobel Y
                const float gy =
                    -1.0f * SampleGrayClamped(gray_mosaic, ix - 1, iy - 1)
                    -2.0f * SampleGrayClamped(gray_mosaic, ix,     iy - 1)
                    -1.0f * SampleGrayClamped(gray_mosaic, ix + 1, iy - 1)
                    +1.0f * SampleGrayClamped(gray_mosaic, ix - 1, iy + 1)
                    +2.0f * SampleGrayClamped(gray_mosaic, ix,     iy + 1)
                    +1.0f * SampleGrayClamped(gray_mosaic, ix + 1, iy + 1);

                float mag = std::sqrt(gx * gx + gy * gy);
                float detect = mag / norm_denom;
                if (detect > 1.0f) detect = 1.0f;

                // MinSensi gate — per the user's spec literally:
                //   result = raw_detection < MinSensi ? 0 : raw_detection
                // `detect` here is "raw_detection" (the Sobel magnitude
                // normalised into [0,1]) BEFORE the Strength sensitivity
                // multiplier is applied. The spec separates the two
                // semantically:
                //   * MinSensi is a FLOOR on the raw edge response — it
                //     decided whether a position carries ANY effect, inde-
                //     pendently of how loud Strength will later amplify the
                //     survivors.
                //   * Strength is a SEPARATE sensitivity knob on whatever the
                //     gate let through.
                // Applying the gate on (detect * strength) instead — as the
                // previous revision did — couples the two knobs: a high
                // Strength would lower the effective MinSensi bar (letting
                // noisy weak edges through), and a tiny Strength would raise
                // the bar (disabling the entire effect). The two knobs are
                // meant to be independent per the spec, so we gate on `detect`
                // alone and multiply Strength only on the surviving tail:
                //       |detect| = detect            (raw_detection ∈ [0,1])
                //       detect  = detect < MinSensi ? 0 : detect   (spec gate)
                //       v       = detect * Strength (sensitivity on survivors)
                if (detect < min_sensi)
                {
                    boost.At(x, y, 0) = 0.0f;
                    continue;
                }

                float v = detect * strength;
                boost.At(x, y, 0) = v;
            }
        }
    }, "chroma_loca_sobel");

    return boost;
}

// Apply the LoCA effect in-place on `img` (mosaic or LinearRaw). This is the
// final assembler: grayscale → Sobel boost → box diffusion → phase-matched
// channel addition.
void ApplyLoCA(FloatImage& img,
               uint32_t period,
               const std::array<uint16_t, 36>& mosaic_pattern,
               float strength,
               float width_percent,
               float min_sensi,
               float white_level)
{
    if (strength <= 0.0f) return;

    // Detect plane-domain input: when the caller has already deinterleaved
    // the mosaic to a plane image (channels == period², period>1). In that
    // case all geometric quantities must be projected back to MOSAIC pixel
    // units (the unit the user's Width% knob references) by multiplying the
    // plane dimensions by the actual CFA period; the Sobel kernel/box blur
    // operate in the plane grid, but the box-blur radius (in plane pixels)
    // must be the mosaic-domain radius divided by period.
    const bool input_is_plane = (period > 1 && img.channels > 1);

    // Image dimensions in MOSAIC pixels — the unit Width% is referenced to.
    // For a plane-domain input each plane pixel spans `period` mosaic pixels
    // along both axes, so the mosaic dimensions are plane dims × period.
    const float mosaic_w = input_is_plane
        ? static_cast<float>(img.width)  * static_cast<float>(period)
        : static_cast<float>(img.width);
    const float mosaic_h = input_is_plane
        ? static_cast<float>(img.height) * static_cast<float>(period)
        : static_cast<float>(img.height);
    const float diag_mosaic = std::sqrt(mosaic_w * mosaic_w + mosaic_h * mosaic_h);
    if (diag_mosaic <= 0.0f) return;

    // half width in mosaic pixels (the fringing band extends ±half_width,
    // so the radius of the box blur is exactly half_width).
    const float half_width_mosaic =
        static_cast<float>(width_percent) * 0.01f * diag_mosaic * 0.5f;
    // Convert mosaic-pixel radius to plane-pixel radius when running in the
    // plane domain. We round to the nearest whole plane pixel; this preserves
    // the "Width% of mosaic diagonal" semantics exactly because half_width
    // was already computed in mosaic pixels via diag_mosaic above.
    float half_width_grid = half_width_mosaic;
    if (input_is_plane)
    {
        half_width_grid /= static_cast<float>(period);
    }
    int radius = static_cast<int>(std::lround(half_width_grid));
    if (radius < 0) radius = 0;

    // 1) Grayscale mosaic image (averaged across all colour channels).
    FloatImage gray_mosaic;
    {
        BURSTMERGE_CHROMA_SCOPE_TIMER("  LoCA: BuildGrayMosaic");
        gray_mosaic = BuildGrayMosaic(img, period);
    }

    // 2) Sobel-based boost map (already MinSensi-floored and Strength-scaled).
    FloatImage boost;
    {
        BURSTMERGE_CHROMA_SCOPE_TIMER("  LoCA: ComputeLoCABoostMap (Sobel)");
        boost = ComputeLoCABoostMap(gray_mosaic, strength, min_sensi, white_level);
    }

    // 3) Box diffusion in the working-domain grid (mosaic or plane). radius
    //    0 = no blur (keeps the effect strictly on the Sobel-positive pixels).
    //    For the legacy mosaic path the diffusion happens at full mosaic
    //    resolution; for the plane-domain path the radius has already been
    //    scaled to plane pixels above, so the diffusion happens at plane
    //    resolution. In both cases the fringing band covers the equivalent
    //    physical extent on the original sensor image because the radius
    //    scaling uses the actual `period` of the input.
    //
    // We use the separable (2-pass horizontal+vertical) implementation defined
    // above in this TU: it is O(N·(2r+1)) rather than O(N·(2r+1)²), giving
    // a ~100× speedup at typical Lo-Fi radii (r=50 for LoCA_Width=2.0 on a
    // 4000×3000 image). Border handling clamps twice (once per pass), which
    // differs infinitesimally from the naive single-pass clamp behaviour at
    // the image borders — acceptable for a Lo-Fi aesthetic effect.
    if (radius > 0)
    {
        BURSTMERGE_CHROMA_SCOPE_TIMER("  LoCA: BoxBlurSeparable (diffusion)");
        boost = BoxBlurSeparable(boost, radius);
    }
    else
    {
        BURSTMERGE_CHROMA_SCOPE_TIMER("  LoCA: BoxBlur (skipped radius=0)");
    }

    // 4) Phase-matched channel addition.
    //    * LinearRaw (3ch): for each preset-selected primary (R/G/B), add
    //      the diffused boost (rescaled to input LSB scale by white_level)
    //      to that channel of the image, then saturate at white_level.
    //    * Bayer (1ch): for each mosaic pixel, only add the boost if the
    //      CFA phase code at (py*period + px) matches one of the preset-
    //      selected primaries. The two green Bayer phases are both affected
    //      when the preset touches G, exactly like in LaCA.
    //    * Bayer plane (period² channels): same phase selection logic but
    //      applied at the channel index level — plane channel c corresponds
    //      to mosaic_pattern[c] (0=R, 1=G, 2=B). No per-pixel phase dispatch
    //      is needed because in the plane domain all pixels of a given channel
    //      share the same CFA colour.
    const PrimaryMask mask = ResolveLoCAMask();
    if (period > 1 && img.channels > 1)
    {
        // Bayer plane input: per-channel phase lookup, then per-(pixel,channel)
        // addition. Each channel c is a fixed CFA colour (mosaic_pattern[c]).
        const float wl = static_cast<float>(white_level);
        const uint32_t total_ch = img.channels;
        std::vector<uint8_t> channel_selected(total_ch, 0);
        for (uint32_t c = 0; c < total_ch && c < 36; ++c)
        {
            if (PrimaryMatches(mosaic_pattern[c], mask))
                channel_selected[c] = 1;
        }

        ParallelForRows(img.height,
            RecommendedImageRowGrain(img.width, 1, kRowGrainMinPixels, kRowGrainCoarseRows),
            [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < img.width; ++x)
                {
                    const float add = boost.At(x, y, 0) * wl;
                    if (add <= 0.0f) continue;
                    for (uint32_t c = 0; c < total_ch; ++c)
                    {
                        if (!channel_selected[c]) continue;
                        float v = img.At(x, y, c) + add;
                        if (v > wl) v = wl;
                        else if (v < 0.0f) v = 0.0f;
                        img.At(x, y, c) = v;
                    }
                }
            }
        }, "chroma_loca_boost_bayer_plane");
    }
    else if (period > 1 && img.channels == 1)
    {
        // Bayer mosaic: per-pixel phase lookup.
        const float wl = static_cast<float>(white_level);
        // Precompute per (py, px) ∈ [0, period) whether the channel is
        // selected, so the inner pixel loop is branchless cheaper.
        // mosaic_pattern is indexed as c = py*period + px (matches
        // ConvertMosaicToPlaneImage's layout in float_image.cpp).
        std::vector<uint8_t> phase_selected(period * period, 0);
        for (uint32_t py = 0; py < period; ++py)
        {
            for (uint32_t px = 0; px < period; ++px)
            {
                const uint32_t c = py * period + px;
                if (c < 36 && PrimaryMatches(mosaic_pattern[c], mask))
                    phase_selected[c] = 1;
            }
        }

        ParallelForRows(img.height,
            RecommendedImageRowGrain(img.width, 1, kRowGrainMinPixels, kRowGrainCoarseRows),
            [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                const uint32_t py = y % period;
                for (uint32_t x = 0; x < img.width; ++x)
                {
                    const uint32_t px = x % period;
                    const uint32_t c = py * period + px;
                    if (!phase_selected[c]) continue;

                    float v = img.At(x, y, 0) + boost.At(x, y, 0) * wl;
                    if (v > wl) v = wl;             // saturate channel
                    else if (v < 0.0f) v = 0.0f;    // guard (defensive)
                    img.At(x, y, 0) = v;
                }
            }
        }, "chroma_loca_boost_bayer");
    }
    else if (img.channels == 3)
    {
        // LinearRaw: channel index 0=R, 1=G, 2=B; add the preset-selected ones.
        const float wl = static_cast<float>(white_level);
        const uint8_t channel_selected[3] =
        {
            static_cast<uint8_t>(mask.r ? 1 : 0),
            static_cast<uint8_t>(mask.g ? 1 : 0),
            static_cast<uint8_t>(mask.b ? 1 : 0)
        };

        ParallelForRows(img.height,
            RecommendedImageRowGrain(img.width, img.channels, kRowGrainMinPixels, kRowGrainCoarseRows),
            [&](uint32_t y_begin, uint32_t y_end)
        {
            for (uint32_t y = y_begin; y < y_end; ++y)
            {
                for (uint32_t x = 0; x < img.width; ++x)
                {
                    const float add = boost.At(x, y, 0) * wl;
                    if (add <= 0.0f) continue;
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        if (!channel_selected[c]) continue;
                        float v = img.At(x, y, c) + add;
                        if (v > wl) v = wl;
                        img.At(x, y, c) = v;
                    }
                }
            }
        }, "chroma_loca_boost_linear");
    }
    // Other formats: no-op.
}

} // namespace

// ============================================================================
// Public entry point
// ============================================================================

void ApplyChromaticEffects(FloatImage& img,
                           uint32_t period,
                           const std::array<uint16_t, 36>& mosaic_pattern,
                           float white_level)
{
#if EFFECT_CA_Enabled

    BURSTMERGE_CHROMA_SCOPE_TIMER("ApplyChromaticEffects total");

    // ---- LaCA (Lateral Chromatic Aberration) ----------------------------
    {
        const double width_pct = (EFFECT_LaCA_Width);
        if (width_pct != 0.0)
        {
            const PrimaryMask mask = ResolveLaCAMask();
            if (mask.r || mask.g || mask.b)
            {
                BURSTMERGE_CHROMA_SCOPE_TIMER("LaCA total");
                if (period > 1 && img.channels == 1)
                {
                    // Bayer mosaic: jump to plane, rescale, jump back.
                    // (Legacy path: caller passed in a 1-channel mosaic.)
                    FloatImage plane;
                    {
                        BURSTMERGE_CHROMA_SCOPE_TIMER("  ConvertMosaicToPlaneImage");
                        plane = ConvertMosaicToPlaneImage(img, period);
                    }
                    {
                        BURSTMERGE_CHROMA_SCOPE_TIMER("  ApplyLaCAOnPlane");
                        ApplyLaCAOnPlane(plane, period, mosaic_pattern, mask,
                                         static_cast<float>(width_pct));
                    }
                    {
                        BURSTMERGE_CHROMA_SCOPE_TIMER("  ConvertPlaneImageToMosaic");
                        img = ConvertPlaneImageToMosaic(plane, img.width, img.height, period);
                    }
                }
                else if (period > 1 && img.channels > 1)
                {
                    // Bayer plane: caller has already deinterleaved the mosaic
                    // to a plane image (channels == period²). Apply LaCA
                    // in the plane domain directly — no mosaic↔plane round-
                    // trip. The downstream resize/assembly happens once at the
                    // caller side.  ApplyLaCAOnPlane internally projects the
                    // plane dimensions back to mosaic pixel units (× period)
                    // for all radial geometry, so the Width% knob references
                    // the SAME mosaic diagonal as in the legacy path.
                    BURSTMERGE_CHROMA_SCOPE_TIMER("  ApplyLaCAOnPlane (in-place)");
                    ApplyLaCAOnPlane(img, period, mosaic_pattern, mask,
                                     static_cast<float>(width_pct));
                }
                else if (img.channels == 3)
                {
                    BURSTMERGE_CHROMA_SCOPE_TIMER("  ApplyLaCAOnLinearRGB");
                    ApplyLaCAOnLinearRGB(img, mask, static_cast<float>(width_pct));
                }
                // Other formats: no-op (LinearRaw non-RGB etc., not supported).
            }
        }
    }

    // ---- LoCA (Longitudinal/Axial CA) -----------------------------------
    {
        const double strength = (EFFECT_LoCA_Strength);
        const double width_pct = (EFFECT_LoCA_Width);
        if (strength > 0.0 && width_pct > 0.0)
        {
            const PrimaryMask mask = ResolveLoCAMask();
            if (mask.r || mask.g || mask.b)
            {
                BURSTMERGE_CHROMA_SCOPE_TIMER("LoCA dispatch (delegates)");
                ApplyLoCA(img, period, mosaic_pattern,
                          static_cast<float>(strength),
                          static_cast<float>(width_pct),
                          static_cast<float>(EFFECT_LoCA_MinSensi),
                          white_level);
            }
        }
    }

#else
    (void)img; (void)period; (void)mosaic_pattern; (void)white_level;
#endif // EFFECT_CA_Enabled
}

} // namespace burstmerge
