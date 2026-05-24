#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <memory>

#include "burstmerge/internal/io/dng_io.h"

namespace burstmerge
{
namespace io
{

// PixelFormat encoding: high 2 bytes = category, low 2 bytes = channel count
// Category 0 (generic RGB-like):
//   low 2 bytes: 1 = gray, 2 = gray+alpha, 3 = RGB, 4 = RGBA
constexpr uint32_t kPixelGray  = 0x00000001;
constexpr uint32_t kPixelGA    = 0x00000002;
constexpr uint32_t kPixelRGB   = 0x00000003;
constexpr uint32_t kPixelRGBA  = 0x00000004;

struct ImageMetadata
{
    uint32_t  width       = 0;
    uint32_t  height      = 0;
    uint32_t  pix_fmt     = kPixelGray;
    uint32_t  bit_depth   = 8;
    bool      is_float    = false;

    float     iso_exposure_time   = 0.0f;
    float     exposure_bias       = 0.0f;
    float     white_level         = 255.0f;
    float     black_level[4]      = {};
    uint32_t  mosaic_pattern_width = 0;
    bool      is_raw              = false;

    std::shared_ptr<DngNegativeHolder> dng_negative;

    std::map<std::string, std::string> tags;
};

struct DecodedImage
{
    ImageMetadata info;
    std::vector<float> pixels;
};

class ImageDecoder
{
public:
    virtual ~ImageDecoder() = default;
    virtual bool CanDecode(const std::string& path) = 0;
    virtual DecodedImage Decode(const std::string& path) = 0;
};

std::unique_ptr<ImageDecoder> SelectDecoder(const std::string& path);
DecodedImage ReadImage(const std::string& path);

} // namespace io
} // namespace burstmerge
