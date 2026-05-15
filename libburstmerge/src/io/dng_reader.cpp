#include "burstmerge/internal/io/dng_io.h"
#include "dng_sdk_bridge.h"

#include "dng_info.h"
#include "dng_file_stream.h"
#include "dng_tag_values.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace burstmerge {

struct DngReaderImpl {
    std::string filepath;
    explicit DngReaderImpl(const char* path) : filepath(path) {}
};

DngReader::DngReader(const char* path)
    : impl_(new DngReaderImpl(path))
{}

DngReader::~DngReader() {
    delete static_cast<DngReaderImpl*>(impl_);
}

RawImage DngReader::Read() {
    auto* p = static_cast<DngReaderImpl*>(impl_);

    // Create holder early so RAII cleanup applies even if exceptions are thrown
    io::DngNegativeHolder* holder = io::CreateNegativeHolder();

    RawImage result;
    result.metadata.dng_negative = holder;

    try {
        dng_host& host = io::GetHost(holder);

#ifdef _WIN32
        std::wstring wpath = io::Utf8ToWide(p->filepath);
        dng_file_stream stream(wpath.c_str(), false);
#else
        dng_file_stream stream(p->filepath.c_str(), false);
#endif

        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);

        dng_negative* negative_ptr = host.Make_dng_negative();
        if (!negative_ptr)
            throw std::runtime_error("DngReader: Make_dng_negative failed");

        // Assign to holder immediately so RAII cleanup works even if Parse throws
        io::SetNegative(holder, negative_ptr);

        dng_negative& negative = *negative_ptr;
        negative.Parse(host, stream, info);
        negative.PostParse(host, stream, info);
        negative.ReadStage1Image(host, stream, info);

        // Use bridge helper for metadata
        uint32_t pat_w = 2;
        io::ExtractRawMetadata(holder,
            result.metadata.width, result.metadata.height,
            result.metadata.white_level, result.metadata.black_level,
            pat_w, result.metadata.mosaic_pattern.data(),
            result.metadata.color_factors);
        result.metadata.mosaic_pattern_width = pat_w;
        io::ExtractExposureMetadata(holder,
            result.metadata.exposure_bias,
            result.metadata.iso_exposure_time);

        // Determine the actual DNG pixel type for the output buffer
        const dng_image* raw = negative.Stage1Image();
        if (!raw) {
            try { raw = &negative.RawImage(); }
            catch (...) {}
        }

        uint32_t pixelType = raw ? static_cast<uint32_t>(raw->PixelType()) : static_cast<uint32_t>(ttShort);
        uint32_t planes    = raw ? raw->Planes() : 1;

        switch (pixelType) {
            case ttByte:  result.metadata.dng_pixel_type = DngPixelType::Uint8;  break;
            case ttShort: result.metadata.dng_pixel_type = DngPixelType::Uint16; break;
            case ttFloat: result.metadata.dng_pixel_type = DngPixelType::Float32; break;
            default:      result.metadata.dng_pixel_type = DngPixelType::Uint16; break;
        }

        uint32_t pixelSize = DngPixelTypeSize(result.metadata.dng_pixel_type);

        // Allocate and read pixel data
        result.pixels.width  = result.metadata.width;
        result.pixels.height = result.metadata.height;
        result.pixels.format = DngPixelTypeToFormat(result.metadata.dng_pixel_type);
        result.pixels.row_stride = result.metadata.width * planes * pixelSize;
        result.pixels.size = static_cast<size_t>(result.pixels.row_stride) * result.metadata.height;
        result.pixels.data = new std::byte[result.pixels.size]();

        if (raw) {
            dng_pixel_buffer pb;
            pb.fArea = dng_rect(0, 0, result.metadata.height, result.metadata.width);
            pb.fPlane = 0;
            pb.fPlanes = planes;
            pb.fRowStep = static_cast<int32_t>(result.pixels.row_stride / pixelSize);
            pb.fColStep = static_cast<int32_t>(planes);
            pb.fPlaneStep = 1;
            pb.fPixelType = pixelType;
            pb.fPixelSize = pixelSize;
            pb.fData = result.pixels.data;
            raw->Get(pb);
        }

        return result;

    } catch (...) {
        // result.metadata owns holder via dng_negative, will be cleaned up in ~RawMetadata()
        // But we need to clear it so the RawImage destructor doesn't double-free
        // Actually, since we're throwing, result is being destroyed,
        // and its metadata destructor will clean up. This is correct.
        throw;
    }
}

} // namespace burstmerge
