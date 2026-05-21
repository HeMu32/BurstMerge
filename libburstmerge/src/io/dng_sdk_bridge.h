#pragma once
#include <string>
#include <cstdint>
#include <memory>

// Forward-declare DNG SDK types to minimize header pollution
struct dng_host;
struct dng_negative;

namespace burstmerge
{
namespace io
{

struct DngHostHolder
{
    std::unique_ptr<dng_host, void(*)(dng_host*)> host;

    DngHostHolder();
    ~DngHostHolder();
    DngHostHolder(const DngHostHolder&) = delete;
    DngHostHolder& operator=(const DngHostHolder&) = delete;
    DngHostHolder(DngHostHolder&&) = delete;
    DngHostHolder& operator=(DngHostHolder&&) = delete;

    dng_host& get()
    { return *host; }
};

struct DngNegativeHolder
{
    std::unique_ptr<DngHostHolder> host_holder;
    dng_negative* negative = nullptr;

    DngNegativeHolder();
    ~DngNegativeHolder();
    DngNegativeHolder(const DngNegativeHolder&) = delete;
    DngNegativeHolder& operator=(const DngNegativeHolder&) = delete;
    DngNegativeHolder(DngNegativeHolder&&) = delete;
    DngNegativeHolder& operator=(DngNegativeHolder&&) = delete;
};

// UTF-8 -> UTF-16 conversion (Windows only)
#ifdef _WIN32
std::wstring Utf8ToWide(const char* utf8);
inline std::wstring Utf8ToWide(const std::string& utf8)
{
    return Utf8ToWide(utf8.c_str());
}
#endif

// DNG negative holder lifecycle
DngNegativeHolder* CreateNegativeHolder();
void DestroyNegativeHolder(DngNegativeHolder* holder);

// Accessors
inline dng_negative* GetNegative(DngNegativeHolder* holder)
{
    return holder ? holder->negative : nullptr;
}
inline dng_host& GetHost(DngNegativeHolder* holder)
{
    return holder->host_holder->get();
}
inline void SetNegative(DngNegativeHolder* holder, dng_negative* neg)
{
    if (holder) holder->negative = neg;
}

// Raw image data transfer
void ReadRawImageFromNegative(DngNegativeHolder* holder, void* pixels,
                               uint32_t width, uint32_t height,
                               uint32_t planes, uint32_t pixelType, size_t pitch = 0);
void WriteRawImageToNegative(DngNegativeHolder* holder,
                              const void* pixels, uint32_t width, uint32_t height,
                              uint32_t planes, uint32_t pixelType, size_t pitch = 0);

// Metadata extraction
void ExtractRawMetadata(DngNegativeHolder* holder,
                          uint32_t& width, uint32_t& height,
                          uint32_t& white_level, float black_level[4],
                          uint32_t& pattern_width, uint16_t* mosaic_pattern,
                          float color_factors[4]);

void ExtractExposureMetadata(DngNegativeHolder* holder,
                             float& exposure_bias,
                             float& iso_exposure_time);

// Override the output DNG's white/black level to match the desired bit depth.
void SetDngWhiteLevel(DngNegativeHolder* holder, uint32_t white_level);
void SetDngBlackLevel(DngNegativeHolder* holder, const float black_level[4]);

} // namespace io
} // namespace burstmerge
