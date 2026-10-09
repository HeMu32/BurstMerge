#pragma once

// The raw-resize feature is optional: its core modules are only compiled into
// libburstmerge when the raw_resize front-ends are enabled, and the same
// BURSTMERGE_HAVE_RAW_RESIZE macro (PUBLIC on the burstmerge target) is used
// here so the header declarations stay consistent with the compiled objects.
#ifndef BURSTMERGE_HAVE_RAW_RESIZE

#else

#include "burstmerge/internal/core/chroma_effects.h"
#include "burstmerge/internal/core/float_image.h"

#include <cstdint>
#include <functional>
#include <string>

namespace burstmerge
{

FloatImage GaussianBlur(const FloatImage& src, float sigma, int radius = -1);

enum class InterpolationMethod
{
    Bilinear,
    Bicubic,
    AreaAverage,
    GaussianArea,
    HalfSample
};

// Reference grid for the --dither amplitude. The bit-depth rescale that fills the
// output container multiplies the sensor's own quantisation lattice by
// bit_scale = target_white / sensor_white, so the two grids differ whenever the
// sensor is narrower than the container.
enum class DitherReference
{
    OutputLsb, // 1.0 = 1 LSB of the output container (16-bit: 1/65535 of full scale)
    InputLsb   // 1.0 = 1 LSB of the sensor grid (bit_scale output LSBs after the rescale)
};

struct RawResizeOptions
{
    // Geometry / Target dimension
    uint32_t width = 0;
    uint32_t height = 0;
    double scale = 0.0; // If > 0, scale factor is used instead of width/height

    // Algorithm options
    InterpolationMethod interp = InterpolationMethod::Bicubic;
    int bit_depth = 16;
    float pseudo_olpf = 0.0f; // 0 = disabled
    float dither = 0.0f;      // 0 = disabled, 1.0 = ±1 LSB TPDF
    DitherReference dither_ref = DitherReference::OutputLsb;
    bool clear_camera_hints = false; // Clear AA strength, noise profile, sharpness hints

    // Chromatic aberration effects
    ChromaEffectsParams chroma;
};

FloatImage ResizeImage(const FloatImage& src,
                       uint32_t dst_width, uint32_t dst_height,
                       InterpolationMethod method);

// Core processing pipeline function for single RAW resize.
// Can be called by CLI, GUI, or library clients.
struct RawResizeResult
{
    bool success = false;
    std::string error_msg;
    uint32_t src_width = 0;
    uint32_t src_height = 0;
    uint32_t dst_width = 0;
    uint32_t dst_height = 0;
    uint32_t target_white = 0;
    InterpolationMethod effective_interp = InterpolationMethod::Bicubic;
};

using RawResizeProgressCallback = std::function<void(float progress, const std::string& status)>;

RawResizeResult ProcessRawResize(const std::string& input_path,
                                const std::string& output_path,
                                const RawResizeOptions& options,
                                RawResizeProgressCallback progress_cb = nullptr);

} // namespace burstmerge

#endif // BURSTMERGE_HAVE_RAW_RESIZE
