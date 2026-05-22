#include "burstmerge/internal/io/image_writer.h"

#include <stdexcept>

namespace burstmerge
{
namespace io
{

// Factory creation functions declared in each writer's file
extern std::unique_ptr<ImageWriter> CreateDngWriter();
extern std::unique_ptr<ImageWriter> CreateJpegWriter();
extern std::unique_ptr<ImageWriter> CreatePngWriter();
extern std::unique_ptr<ImageWriter> CreateBmpWriter();
extern std::unique_ptr<ImageWriter> CreateTiffWriter();

std::unique_ptr<ImageWriter> SelectWriter(OutputFormat fmt)
{
    switch (fmt)
    {
        case OutputFormat::DNG:  return CreateDngWriter();
        case OutputFormat::JPEG: return CreateJpegWriter();
        case OutputFormat::PNG:  return CreatePngWriter();
        case OutputFormat::BMP:  return CreateBmpWriter();
        case OutputFormat::TIFF: return CreateTiffWriter();
        default: return nullptr;
    }
}

static OutputFormat InferFormat(bool all_raw, const Settings& settings)
{
    if (settings.output_format != OutputFormat::Auto)
    {
        return settings.output_format;
    }

    if (all_raw) return OutputFormat::DNG;
    return OutputFormat::PNG;
}

static uint32_t AdjustBitDepth(OutputFormat fmt, uint32_t bd)
{
    switch (fmt)
    {
        case OutputFormat::JPEG: return 8;
        case OutputFormat::BMP:  return 8;
        case OutputFormat::PNG:
        case OutputFormat::TIFF:
            return (bd <= 8) ? 8 : 16;
        default:
            return bd;
    }
}

void WriteImage(const std::string& path,
                const FloatImage& merged,
                const std::vector<DecodedImage>& inputs,
                const Settings& settings)
{
    bool all_raw = true;
    for (const auto& img : inputs)
    {
        if (!img.info.is_raw)
        {
            all_raw = false;
            break;
        }
    }

    OutputFormat fmt = InferFormat(all_raw, settings);

    if (fmt == OutputFormat::DNG && !all_raw)
    {
        throw std::runtime_error("Cannot output DNG for non-RAW inputs");
    }

    auto writer = SelectWriter(fmt);
    if (!writer)
    {
        throw std::runtime_error("No writer available for selected output format");
    }

    uint32_t bd = settings.bit_depth;
    if (bd < 8) bd = 8;
    if (bd > 16) bd = 16;

    bd = AdjustBitDepth(fmt, bd);

    WriteParams params;
    params.format = fmt;
    params.bit_depth = bd;

    writer->Write(path, merged, params);
}

} // namespace io
} // namespace burstmerge
