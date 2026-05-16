#include "burstmerge/internal/io/dng_io.h"
#include "dng_sdk_bridge.h"

#include "dng_image_writer.h"
#include "dng_file_stream.h"
#include "dng_tag_values.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace burstmerge {

struct DngWriterImpl {
    io::DngNegativeHolder* holder = nullptr;

    explicit DngWriterImpl(io::DngNegativeHolder* h)
        : holder(h)
    {}

    ~DngWriterImpl() {
        if (holder) io::DestroyNegativeHolder(holder);
    }
};

// Takes ownership via reference: sets source pointer to nullptr
DngWriter::DngWriter(io::DngNegativeHolder*& ref_negative)
    : impl_(new DngWriterImpl(ref_negative))
{
    ref_negative = nullptr;
}

DngWriter::~DngWriter() {
    delete static_cast<DngWriterImpl*>(impl_);
}

static bool GetDngType(const HostBuffer& buf, uint32_t& outPixelType, uint32_t& outPixelSize) {
    switch (buf.format) {
        case PixelFormat::R16_Uint:
            // DNG has no separate types for 12/14/16-bit; all use ttShort.
            // The effective bit depth is conveyed by the white_level metadata
            // tag, which the writer receives from RawMetadata::white_level.
            outPixelType = ttShort; outPixelSize = 2; return true;
        case PixelFormat::R32_Float:  outPixelType = ttFloat; outPixelSize = 4; return true;
        case PixelFormat::RGBA32_Float: outPixelType = ttFloat; outPixelSize = 4; return true;
        default: return false;
    }
}

static uint32_t GetPlanes(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::RGBA32_Float: return 4;
        default: return 1;
    }
}

void DngWriter::Write(const char* out_path, const RawImage& image) {
    auto* p = static_cast<DngWriterImpl*>(impl_);
    if (!p->holder || !io::GetNegative(p->holder))
        throw std::runtime_error("DngWriter: no valid DNG negative");

    uint32_t pixelType = ttShort;
    uint32_t pixelSize = 2;
    if (!GetDngType(image.pixels, pixelType, pixelSize))
        throw std::runtime_error("DngWriter: unsupported pixel format");

    uint32_t planes = GetPlanes(image.pixels.format);
    size_t rowStride = image.pixels.row_stride;
    if (rowStride == 0)
        rowStride = static_cast<size_t>(image.pixels.width) * planes * pixelSize;

    io::WriteRawImageToNegative(
        p->holder,
        image.pixels.data,
        image.pixels.width,
        image.pixels.height,
        planes,
        pixelType,
        rowStride
    );

    dng_host& host = io::GetHost(p->holder);
    dng_negative& neg = *io::GetNegative(p->holder);
    dng_image_writer writer;

#ifdef _WIN32
    std::wstring wpath = io::Utf8ToWide(out_path);
    dng_file_stream stream(wpath.c_str(), true);
#else
    dng_file_stream stream(out_path, true);
#endif

    writer.WriteDNG(host, stream, neg);
    stream.Flush();
}

} // namespace burstmerge
