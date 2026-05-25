#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace burstmerge
{
namespace io
{
namespace
{

std::string LowerExt(const std::string& path)
{
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

} // namespace

std::unique_ptr<ImageDecoder> SelectDecoder(const std::string& path)
{
    std::string ext = LowerExt(path);

    if (ext == ".dng")
    {
        extern std::unique_ptr<ImageDecoder> CreateDngDecoder();
        return CreateDngDecoder();
    }
    if (ext == ".jpg" || ext == ".jpeg")
    {
        extern std::unique_ptr<ImageDecoder> CreateJpegDecoder();
        return CreateJpegDecoder();
    }
    if (ext == ".png")
    {
        extern std::unique_ptr<ImageDecoder> CreatePngDecoder();
        return CreatePngDecoder();
    }
    if (ext == ".bmp")
    {
        extern std::unique_ptr<ImageDecoder> CreateBmpDecoder();
        return CreateBmpDecoder();
    }
    if (ext == ".tif" || ext == ".tiff")
    {
        extern std::unique_ptr<ImageDecoder> CreateTiffDecoder();
        return CreateTiffDecoder();
    }

    return nullptr;
}

DecodedImage ReadImage(const std::string& path)
{
    auto decoder = SelectDecoder(path);
    if (!decoder)
    {
        throw std::runtime_error("Unsupported file format: " + path);
    }
    return decoder->Decode(path);
}

} // namespace io
} // namespace burstmerge
