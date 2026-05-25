#pragma once
#include <string>
#include <cstdint>
#include <memory>

#include "burstmerge/api.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/image_decoder.h"

namespace burstmerge
{
namespace io
{

struct WriteParams
{
    OutputFormat format;
    uint32_t     bit_depth;
    float        white_level = 255.0f; // input max value; writer scales to output bit depth
};

class ImageWriter
{
public:
    virtual ~ImageWriter() = default;
    virtual bool CanWrite(OutputFormat fmt, uint32_t bit_depth) = 0;
    virtual void Write(const std::string& path,
                       const FloatImage& image,
                       const WriteParams& params) = 0;
};

std::unique_ptr<ImageWriter> SelectWriter(OutputFormat fmt);

// String name for a concrete output format (never Auto).
const char* OutputFormatToString(OutputFormat fmt);

// Infer the effective output format from settings + input type.
// Returns settings.output_format directly if not Auto;
// otherwise auto-selects DNG (all RAW) or PNG (non-RAW / mixed).
OutputFormat InferOutputFormat(const Settings& settings, bool all_raw);

// Infer a concrete output format from a file path extension.
// Returns fallback if the extension is unknown or missing.
OutputFormat InferFormatFromExtension(const std::string& path,
                                      OutputFormat fallback);

// Helper: write DecodedImage (which may have is_raw metadata) to output.
// For RAW -> non-DNG: writes Bayer mosaic grayscale without demosaic.
// For non-RAW -> output: directly as-is with optional OETF (currently skipped).
void WriteImage(const std::string& path,
                const FloatImage& merged,
                const std::vector<DecodedImage>& inputs,
                const Settings& settings);

} // namespace io
} // namespace burstmerge
