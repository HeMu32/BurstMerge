#ifndef BURSTMERGE_INTERNAL_CORE_DITHER_H
#define BURSTMERGE_INTERNAL_CORE_DITHER_H

#include <cmath>
#include <cstdint>

namespace burstmerge
{

struct FloatImage;

// PCG hash — deterministic white-noise dither with zero spatial periodicity.
// No masks, no tiling, no visible grid at any scale.
//
// GPU equivalent (GLSL):
//   uint PcgHash(uint v) {
//       uint state = v * 747796405u + 2891336453u;
//       uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
//       return (word >> 22u) ^ word;
//   }
inline uint32_t PcgHash(uint32_t v)
{
    uint32_t state = v * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

inline float Hash01(uint32_t x, uint32_t y)
{
    uint32_t h = PcgHash(x * 1973u + y * 9277u + 479001u);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Triangular-PDF dither in [-amplitude, +amplitude) LSB.
// amplitude = 1.0 yields the theoretically correct full ±1 LSB TPDF that
// decorrelates first and second moments of the quantization error.
//
// GPU equivalent (GLSL):
//   float d1 = float(PcgHash(x * 1973u + y * 9277u + 479001u) >> 8)
//              * (1.0 / 16777216.0);
//   float d2 = float(PcgHash((x+311u) * 1973u + (y+757u) * 9277u + 479001u) >> 8)
//              * (1.0 / 16777216.0);
//   return (d1 + d2 - 1.0) * amplitude;
inline float DitherTPDF(float fx, float fy, float amplitude)
{
    uint32_t x = static_cast<uint32_t>(fx);
    uint32_t y = static_cast<uint32_t>(fy);
    float d1 = Hash01(x, y);
    float d2 = Hash01(x + 311u, y + 757u);
    return (d1 + d2 - 1.0f) * amplitude;
}

// Apply TPDF dither to every pixel of *img in-place.
//
// The dither is based on spatial (x, y) coordinates so that:
//   - Mosaic (1-channel) data: every pixel gets an independent value,
//     different Bayer phases in the same 2x2 block are naturally
//     decorrelated because their coordinates differ.
//   - Multi-channel (RGB) data: all channels at the same pixel position
//     share the same dither value, producing luminance noise rather
//     than chroma noise (more natural-looking).
//
// `amplitude` is the dither amplitude in LSB of the output uint16 container
// (i.e. in the same units as the already-scaled float-image values).
// amplitude <= 0 is a no-op.
//
// Call this AFTER bit-depth scaling and BEFORE uint16 quantisation
// (FloatImageToUint16HostBuffer).
void ApplyQuantizationDither(FloatImage& img, float amplitude);

} // namespace burstmerge

#endif // BURSTMERGE_INTERNAL_CORE_DITHER_H
