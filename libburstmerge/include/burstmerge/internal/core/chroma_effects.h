#pragma once

// Optional raw-resize feature; see image_resize.h for the feature-macro
// contract. Guarded out entirely when the raw_resize tools are not built.
#ifndef BURSTMERGE_HAVE_RAW_RESIZE

#else

// ============================================================================
// chroma_effects: color-fringing (Lo-Fi effect) for the raw_resize utility.
// ----------------------------------------------------------------------------
// This module injects two flavours of colour dispersion as a Lo-Fi effect:
//
//   * LaCA — Lateral (transverse) Chromatic Aberration: a radial rescaling
//     that stretches the selected colour channel(s) outward from the image
//     centre, mimicking lens-element magnification differences per colour.
//     The maximum displacement is reached at the image corner.
//
//   * LoCA — Longitudinal (axial) Chromatic Aberration: an edge-induced
//     colour fringing. 高反差 (high-contrast) edges detected on a grayscale
//     (channel-averaged) image are boosted into the selected colour channel(s)
//     of the surrounding band, mimicking the colour haling around in-focus
//     high-contrast edges when the sensor plane does not coincide for all
//     wavelengths. Out-of-focus (bokeh) edges are deliberately NOT handled —
//     that requires blur/defocus modelling and is far beyond a Lo-Fi aesthetic
//     effect; we only deal with in-focus high-contrast edges here.
//
// Both effects are Lo-Fi aesthetics, NOT rigorous optical-defect simulation.
// The intent is to give the resized RAW a stylistic colour-fringed look, not
// to model the physics of a real lens accurately.
//
// ----------------------------------------------------------------------------
// Control mechanism: the modern entry point
// ApplyChromaticEffects(img, period, mosaic_pattern, white_level, params) takes
// a runtime ChromaEffectsParams. The raw_resize CLI (--chroma / --laca-* /
// --loca-*) and the raw_resize GUI expose these knobs to the user.
// ChromaEffectsParams defaults to enabled=false, so the effect is a true
// bit-identical no-op unless the caller opts in.
//
// A legacy overload (no params argument) reads a fixed set of compile-time
// #define macros for backward compatibility. Its defaults are also disabled
// (EFFECT_CA_Enabled = 0), so the legacy overload is likewise a bit-identical
// no-op until the macros are overridden at build time. Prefer the
// runtime-params entry point for new code.
//
// ----------------------------------------------------------------------------
// Geometry contract: ALL geometric quantities (distances, radii, the diagonal
// used to interpret "percent" knobs) are measured in the INPUT image's pixel
// units. For a Bayer (period==2) mosaic input the unit is mosaic pixels
// (src.width × src.height); the plane domain is only an internal stepping
// stone and never leaks out as multiplication-by-two heuristics — the actual
// period is read from the metadata / image pixels and used for unit
// conversion. For a LinearRaw (3-channel) input the unit is the same as the
// image's width/height (no CFA subdivision).
//
// The ApplyChromaticEffects() entry point is therefore to be called BEFORE
// resizing, on the freshly-decoded FloatImage of the original RAW.
// ============================================================================

#include "burstmerge/internal/core/float_image.h"

#include <cstdint>
#include <array>

namespace burstmerge
{

// ---- Colour-preset enumerations -------------------------------------------
// Six and only six presets: the three primaries plus their two-at-a-time
// pairings. No other combination is accepted.
//
// The same palette applies to LaCA and LoCA; the enums are duplicated by name
// (LaCAColor_* vs LoCAColor_*) so that the two effects can carry
// independent configuration through the macro layer even though the palette
// itself is conceptually shared.

enum LaCAColor
{
    LaCAColor_Red,
    LaCAColor_Green,
    LaCAColor_Blue,
    LaCAColor_Cyan,     // G + B
    LaCAColor_Magenta, // R + B
    LaCAColor_Yellow    // R + G
};

enum LoCAColor
{
    LoCAColor_Red,
    LoCAColor_Green,
    LoCAColor_Blue,
    LoCAColor_Cyan,     // G + B
    LoCAColor_Magenta, // R + B
    LoCAColor_Yellow    // R + G
};

struct ChromaEffectsParams
{
    bool enabled = false;
    LaCAColor laca_color = LaCAColor_Red;
    float laca_width = 0.0f; // [% of image diagonal; corner displacement]

    LoCAColor loca_color = LoCAColor_Magenta;
    float loca_strength = 0.2f;  // Sensitivity (1.0 ≈ saturate on maximal-axis edge)
    float loca_width = 0.05f;    // [% of image diagonal; fringing band width]
    float loca_min_sensi = 0.08f; // Raw-detection floor (applied BEFORE Strength)
};

#ifndef EFFECT_CA_Enabled
#define EFFECT_CA_Enabled 0          // Master switch (0 = bit-identical no-op)
#endif

#ifndef EFFECT_LaCA_Color
#define EFFECT_LaCA_Color LaCAColor_Red
#endif

#ifndef EFFECT_LaCA_Width
#define EFFECT_LaCA_Width 0.0       // [% of image diagonal; corner displacement]
#endif

#ifndef EFFECT_LoCA_Color
#define EFFECT_LoCA_Color LoCAColor_Magenta
#endif

#ifndef EFFECT_LoCA_Strength
#define EFFECT_LoCA_Strength 0.2    // Sensitivity (1.0 ≈ saturate on maximal-axis edge)
#endif

#ifndef EFFECT_LoCA_Width
#define EFFECT_LoCA_Width 0.05       // [% of image diagonal; fringing band width]
#endif

#ifndef EFFECT_LoCA_MinSensi
#define EFFECT_LoCA_MinSensi 0.08    // Raw-detection floor (applied BEFORE Strength)
#endif

// ----------------------------------------------------------------------------
// Public entry points.
//
// img          — the raw FloatImage to modify in place. Must be either a
//                1-channel Bayer mosaic (with mosaic_pattern + period supplied)
//                or a 3-channel LinearRaw RGB image (period <= 1).
// period       — CFA period (1 or 0 for LinearRaw RGB, 2 for typical Bayer).
// mosaic_pattern — CFA colour codes for each channel of the deinterleaved
//                  plane image (Bayer only). Codes: 0=R, 1=G, 2=B (matches the
//                  RawMetadata::mosaic_pattern convention used elsewhere in
//                  this project; see float_image.cpp / pipeline_frame.cpp).
//                  Length must be period*period and indexed as c = py*period + px.
// white_level  — the saturation ceiling of the input image, used to normalise
//                the Sobel edge magnitude and to clamp the boosted channel
//                (for LoCA) so an edge that maxes out saturates the channel.
//                  * For Bayer: sensor_white_level (raw LSB).
//                  * For LinearRaw: same value; channels share one ceiling.
// params       — Runtime parameters for chromatic effects (enabled, colors, widths).
//
// When disabled this function is a no-op (early return).
// ----------------------------------------------------------------------------
void ApplyChromaticEffects(FloatImage& img,
                           uint32_t period,
                           const std::array<uint16_t, 36>& mosaic_pattern,
                           float white_level,
                           const ChromaEffectsParams& params);

// Legacy overload reading compile-time macros / defaults for backward compatibility
void ApplyChromaticEffects(FloatImage& img,
                           uint32_t period,
                           const std::array<uint16_t, 36>& mosaic_pattern,
                           float white_level);

} // namespace burstmerge

#endif // BURSTMERGE_HAVE_RAW_RESIZE
