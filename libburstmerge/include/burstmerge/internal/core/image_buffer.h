#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>

namespace burstmerge
{

enum class MemoryLocation
{ Host, Device };
enum class PixelFormat
{ R8_Uint, R16_Uint, R16_Uint_RGB, R32_Float, RGBA32_Float };
enum class BufferType
{ Staging, DeviceLocal };

struct HostBuffer
{
    std::byte*  data        = nullptr;
    size_t      size        = 0;
    PixelFormat format      = PixelFormat::R16_Uint;
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    uint32_t    row_stride  = 0;

    ~HostBuffer()
    { delete[] data; data = nullptr; }
    HostBuffer() = default;
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&& other) noexcept
        : data(other.data), size(other.size), format(other.format),
          width(other.width), height(other.height), row_stride(other.row_stride)
    {
        other.data = nullptr;
        other.size = 0;
        other.width = 0;
        other.height = 0;
        other.row_stride = 0;
    }
    HostBuffer& operator=(HostBuffer&& other) noexcept
    {
        if (this != &other)
        {
            delete[] data;
            data = other.data; other.data = nullptr;
            size = other.size; other.size = 0;
            format = other.format;
            width = other.width; other.width = 0;
            height = other.height; other.height = 0;
            row_stride = other.row_stride; other.row_stride = 0;
        }
        return *this;
    }
};

struct DeviceBuffer
{
    uint64_t       handle      = 0;
    BufferType     type        = BufferType::DeviceLocal;
    PixelFormat    format      = PixelFormat::R32_Float;
    uint32_t       width       = 0;
    uint32_t       height      = 0;
    uint32_t       depth       = 1;
    MemoryLocation location    = MemoryLocation::Device;
};

} // namespace burstmerge
