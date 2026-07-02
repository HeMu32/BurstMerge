#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>
#include "burstmerge/internal/core/image_buffer.h"

namespace burstmerge
{
namespace io
{
struct DngNegativeHolder;
}

// Maps to our internal Dng pixel types.
// DNG SDK has no separate 12-bit pixel type; 12/14/16-bit data all use
// ttShort (16-bit container). Effective bit depth is conveyed via white_level.
enum class DngPixelType : uint32_t
{
    Uint8  = 1,
    Uint16 = 2,
    Int16  = 3,
    Uint32 = 4,
    Float32 = 5,
};

static inline PixelFormat DngPixelTypeToFormat(DngPixelType dpt)
{
    switch (dpt)
    {
        case DngPixelType::Uint8:   return PixelFormat::R8_Uint;
        case DngPixelType::Uint16:  return PixelFormat::R16_Uint;
        case DngPixelType::Float32: return PixelFormat::R32_Float;
        // Int16 maps to R16_Uint (signed → unsigned, OK for non-negative data).
        // Uint32 maps to R32_Float (32-bit int fits in float24 mantissa for <16M range).
        case DngPixelType::Int16:   return PixelFormat::R16_Uint;
        case DngPixelType::Uint32:  return PixelFormat::R32_Float;
        default: return PixelFormat::R16_Uint;
    }
}

static inline uint32_t DngPixelTypeSize(DngPixelType dpt)
{
    switch (dpt)
    {
        case DngPixelType::Uint8:  return 1;
        case DngPixelType::Uint16: return 2;
        case DngPixelType::Int16:  return 2;
        case DngPixelType::Uint32: return 4;
        case DngPixelType::Float32: return 4;
        default: return 2;
    }
}

struct RawMetadata
{
    uint32_t  width               = 0;
    uint32_t  height              = 0;
    uint32_t  mosaic_pattern_width = 2;
    std::array<uint16_t, 36> mosaic_pattern
    {};
    uint32_t  white_level         = 65535;
    float     black_level[4]      =
    {};
    float     color_factors[4]    =
    {1.0f, 1.0f, 1.0f, 1.0f};
    float     exposure_bias       = 0.0f;
    float     ev_value            = 0.0f;
    DngPixelType dng_pixel_type   = DngPixelType::Uint16;

    // Opaque handle to DNG SDK internal negative (lifecycle managed internally)
    // Owned by this struct; destroyed when this struct is destroyed
    io::DngNegativeHolder* dng_negative = nullptr;

    ~RawMetadata();
    RawMetadata() = default;
    RawMetadata(const RawMetadata&) = delete;
    RawMetadata& operator=(const RawMetadata&) = delete;
    RawMetadata(RawMetadata&& other) noexcept;
    RawMetadata& operator=(RawMetadata&& other) noexcept;
};

struct RawImage
{
    RawMetadata  metadata;
    HostBuffer   pixels;
};

// Decode a DNG from an in-memory buffer (splits I/O from decode).
// The data pointer must remain valid for the duration of the call.
RawImage ReadDngFromBuffer(const void* data, uint32_t size);

struct DngReaderImpl;
class DngReader
{
public:
    explicit DngReader(const char* path);
    ~DngReader();

    DngReader(const DngReader&) = delete;
    DngReader& operator=(const DngReader&) = delete;
    DngReader(DngReader&&) = delete;
    DngReader& operator=(DngReader&&) = delete;

    RawImage Read();

private:
    std::unique_ptr<DngReaderImpl> impl_;
};

struct DngWriterImpl;
class DngWriter
{
public:
    // Takes ownership: ref_negative will be set to nullptr after construction
    explicit DngWriter(io::DngNegativeHolder*& ref_negative);
    ~DngWriter();

    DngWriter(const DngWriter&) = delete;
    DngWriter& operator=(const DngWriter&) = delete;
    DngWriter(DngWriter&&) = delete;
    DngWriter& operator=(DngWriter&&) = delete;

    void Write(const char* out_path, const RawImage& image);

private:
    std::unique_ptr<DngWriterImpl> impl_;
};

// DNG Converter (Windows only)
#ifdef _WIN32
bool RunAdobeDngConverter(const std::vector<std::string>& input_files,
                          const std::string& output_dir,
                          std::vector<std::string>& output_files);
#endif

namespace io
{
void SetDngWhiteLevel(DngNegativeHolder* holder, uint32_t white_level);
void SetDngBlackLevel(DngNegativeHolder* holder, const float black_level[4]);
void SetDngBaselineExposure(DngNegativeHolder* holder, double exposure);
void ClearDngMosaicInfo(DngNegativeHolder* holder);
void SetDngDimensions(DngNegativeHolder* holder, uint32_t width, uint32_t height);
void ClearDngOriginalSizes(DngNegativeHolder* holder);
}

} // namespace burstmerge
