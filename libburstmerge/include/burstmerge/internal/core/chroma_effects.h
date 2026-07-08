#pragma once

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
// Control mechanism: a fixed set of compile-time #define macros governs
// enablement and parameters. There is NO CLI surface; the resize utility is
// built with whatever macro values were chosen at build time. The single
// place to edit these values is the `#define` block below this comment —
// there is no longer a CMake option/cache surface for chroma. The defaults
// keep the effect fully disabled (bit-identical output) until the user edits
// the `#define`s in this header.
//
//   EFFECT_CA_Enabled          (0/1)  Master switch (LaCA + LoCA).
//   EFFECT_LaCA_Color          LaCAColor_XXX               (see enum below)
//   EFFECT_LaCA_Width          (>=0, percent of image diagonal)  corner
//                              displacement of the selected channel.
//   EFFECT_LoCA_Color          LoCAColor_XXX               (see enum below)
//   EFFECT_LoCA_Strength       (>=0)  Sensitivity multiplier on the
//                              normalised Sobel edge magnitude. With a
//                              maximum-magnitude edge, Strength ≈ 1.0 will
//                              push the channel to saturation.
//   EFFECT_LoCA_Width          (>=0, percent of image diagonal)  maximum
//                              full width of the fringing band around an
//                              edge; the band extends half of this to either
//                              side of the edge (box diffusion radius).
//   EFFECT_LoCA_MinSensi       (>=0)  raw detection values strictly below
//                              this threshold are floored to zero BEFORE the
//                              Strength multiplier is applied. This means
//                              MinSensi (a floor on raw edge response) and
//                              Strength (a post-gate amplification) act as
//                              independent knobs: setting Strength very low
//                              does NOT raise the MinSensi bar, and setting
//                              Strength very high does NOT lower it.
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

// ----------------------------------------------------------------------------
// EFFECT CONFIGURATION — single point of truth.
// ----------------------------------------------------------------------------
// All chromatic-effect knobs live HERE in the header. The previous CMake-driven
// `BURSTMERGE_CHROMA_*` cache/option machinery has been removed; this header
// is now the only place to enable or tune the effect. To turn the effect on,
// edit the `#define` lines below so that:
//   * EFFECT_CA_Enabled      becomes 1 (master switch)
//   * EFFECT_LaCA_Color      becomes one of LaCAColor_Red/Green/Blue/Cyan/Magenta/Yellow
//   * EFFECT_LaCA_Width      becomes a non-zero percent of the image diagonal
//                            (the corner displacement of the selected channel)
//   * EFFECT_LoCA_Color      becomes one of LoCAColor_Red/Green/Blue/Cyan/Magenta/Yellow
//   * EFFECT_LoCA_Strength   becomes > 0 (1.0 ≈ saturate channel on a maximal-axis edge)
//   * EFFECT_LoCA_Width      becomes > 0 (percent of image diagonal; fringing band width)
//   * EFFECT_LoCA_MinSensi   stays or grows (raw-detection floor, applied BEFORE Strength)
//
// Each macro still respects `#ifndef` so a compiler command-line `-D` (or a
// pre-include `#define`) takes precedence over the values edited here — that
// keeps one-off overrides (e.g. A/B comparisons, CI probes) possible without
// editing source.
// ----------------------------------------------------------------------------

#ifndef EFFECT_CA_Enabled
#define EFFECT_CA_Enabled 1          // Master switch (0 = bit-identical no-op)
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
#define EFFECT_LoCA_Width 0.1       // [% of image diagonal; fringing band width]
#endif

#ifndef EFFECT_LoCA_MinSensi
#define EFFECT_LoCA_MinSensi 0.06    // Raw-detection floor (applied BEFORE Strength)
#endif

// ----------------------------------------------------------------------------
// Public entry point.
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
//
// When EFFECT_CA_Enabled is 0 this function is a no-op (early return).
// ----------------------------------------------------------------------------
void ApplyChromaticEffects(FloatImage& img,
                           uint32_t period,
                           const std::array<uint16_t, 36>& mosaic_pattern,
                           float white_level);

} // namespace burstmerge
