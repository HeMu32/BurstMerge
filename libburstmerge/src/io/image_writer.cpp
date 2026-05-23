#include "burstmerge/internal/io/image_writer.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
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

const char* OutputFormatToString(OutputFormat fmt)
{
    switch (fmt)
    {
        case OutputFormat::PNG:  return "PNG";
        case OutputFormat::JPEG: return "JPEG";
        case OutputFormat::BMP:  return "BMP";
        case OutputFormat::TIFF: return "TIFF";
        case OutputFormat::DNG:  return "DNG";
        default:                 return "Auto";
    }
}

OutputFormat InferOutputFormat(const Settings& settings, bool all_raw)
{
    if (settings.output_format != OutputFormat::Auto)
    {
        return settings.output_format;
    }

    if (all_raw) return OutputFormat::DNG;
    return OutputFormat::PNG;
}

OutputFormat InferFormatFromExtension(const std::string& path,
                                      OutputFormat fallback)
{
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".png")          return OutputFormat::PNG;
    if (ext == ".jpg" || ext == ".jpeg") return OutputFormat::JPEG;
    if (ext == ".bmp")          return OutputFormat::BMP;
    if (ext == ".tif" || ext == ".tiff") return OutputFormat::TIFF;
    if (ext == ".dng")          return OutputFormat::DNG;
    return fallback;
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

    // Use the output path extension to determine the format, matching the
    // pipeline's resolution logic in pipeline.cpp.  This keeps the actual
    // writer in sync with the file extension — e.g. --output-format auto
    // -o out.jpg chooses JPEG here, not the bare all_raw fallback.
    OutputFormat fmt = InferFormatFromExtension(path, InferOutputFormat(settings, all_raw));

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

    uint32_t adjusted = AdjustBitDepth(fmt, bd);
    if (adjusted != bd)
    {
        std::fprintf(stderr, "Warning: bit depth %u not supported for %s output, using %u\n",
            bd, OutputFormatToString(fmt), adjusted);
    }
    bd = adjusted;

    float wl = 255.0f;
    for (const auto& img : inputs)
    {
        if (img.info.white_level > 1.0f)
        {
            wl = img.info.white_level;
            break;
        }
    }

    WriteParams params;
    params.format = fmt;
    params.bit_depth = bd;
    params.white_level = wl;

    writer->Write(path, merged, params);
}

} // namespace io
} // namespace burstmerge
