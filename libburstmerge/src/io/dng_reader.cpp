#include "burstmerge/internal/io/dng_io.h"
#include "dng_sdk_bridge.h"

#include "dng_info.h"
#include "dng_file_stream.h"
#include "dng_stream.h"
#include "dng_tag_values.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace burstmerge
{

// ---------------------------------------------------------------
// Shared core: decode a DNG from an already-opened stream.
// The caller provides the stream (file-backed or memory-backed);
// this function creates its own DngNegativeHolder and dng_host.
// ---------------------------------------------------------------
namespace {

static RawImage RawReadDngFromStream(dng_stream& stream)
{
    io::DngNegativeHolder* holder = io::CreateNegativeHolder();
    RawImage result;
    result.metadata.dng_negative = holder;

    dng_host& host = io::GetHost(holder);

    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);

    dng_negative* negative_ptr = host.Make_dng_negative();
    if (!negative_ptr)
        throw std::runtime_error("DngReader: Make_dng_negative failed");

    io::SetNegative(holder, negative_ptr);
    dng_negative& negative = *negative_ptr;
    negative.Parse(host, stream, info);
    negative.PostParse(host, stream, info);
    negative.ReadStage1Image(host, stream, info);

    // Enhanced-Image DNGs (e.g. Adobe Camera Raw "NR" output) carry a second
    // full-resolution SubIFD flagged sfEnhancedImage holding a pre-denoised
    // LinearRaw (typically JPEG XL compressed). With the default host
    // (SaveDNGVersion == dngVersion_None, SaveLinearDNG == false) the SDK
    // runs ReadEnhancedImage in "linear-only" mode: it decodes the enhanced
    // image into fStage3Image, then clears MosaicInfo / LinearizationInfo /
    // OpcodeLists / fEnhanceParams so the negative describes a pure linear
    // DNG. We then drop the Bayer Stage1 we just read so the existing
    // image-selection logic (Stage1Image() ?? RawImage()) and metadata
    // extraction consistently reach fStage3Image via the RawImage() fallback,
    // and the existing is_linear_rgb path routes the data through the
    // LinearRaw pipeline. Without this, fEnhanceParams stays populated while
    // fStage3Image is NULL and the DNG writer dereferences it (NULL access).
    if (info.fEnhancedIndex != -1 && !host.IgnoreEnhanced())
    {
        negative.ReadEnhancedImage(host, stream, info);
        negative.ClearStage1Image();
    }

    // Metadata extraction
    uint32_t pat_w = 2;
    io::ExtractRawMetadata(holder,
        result.metadata.width, result.metadata.height,
        result.metadata.white_level, result.metadata.black_level,
        pat_w, result.metadata.mosaic_pattern.data(),
        result.metadata.color_factors);
    result.metadata.mosaic_pattern_width = pat_w;
    io::ExtractExposureMetadata(holder,
        result.metadata.exposure_bias,
        result.metadata.ev_value);

    // Pixel type
    const dng_image* raw = negative.Stage1Image();
    if (!raw)
    {
        try
        { raw = &negative.RawImage(); }
        catch (...)
        {}
    }

    uint32_t pixelType = raw ? static_cast<uint32_t>(raw->PixelType()) : static_cast<uint32_t>(ttShort);
    uint32_t planes    = raw ? raw->Planes() : 1;

    switch (pixelType)
    {
        case ttByte:  result.metadata.dng_pixel_type = DngPixelType::Uint8;  break;
        case ttShort: result.metadata.dng_pixel_type = DngPixelType::Uint16; break;
        case ttFloat: result.metadata.dng_pixel_type = DngPixelType::Float32; break;
        default:      result.metadata.dng_pixel_type = DngPixelType::Uint16; break;
    }

    uint32_t pixelSize = DngPixelTypeSize(result.metadata.dng_pixel_type);

    // LinearRaw (demosaiced RGB) DNGs — e.g. DxO DeepPRIME output, or the
    // sfEnhancedImage SubIFD of Adobe Camera Raw "NR" DNGs (read above via
    // ReadEnhancedImage) — carry 3 interleaved samples per pixel
    // (PhotometricInterpretation 34892, no CFA). Tag them as R16_Uint_RGB so
    // the channel count survives into the pipeline; everything else stays
    // single-channel.
    const bool is_linear_rgb =
        (planes == 3) && (result.metadata.dng_pixel_type == DngPixelType::Uint16);

    // Allocate and read pixel data
    result.pixels.width  = result.metadata.width;
    result.pixels.height = result.metadata.height;
    result.pixels.format = is_linear_rgb
        ? PixelFormat::R16_Uint_RGB
        : DngPixelTypeToFormat(result.metadata.dng_pixel_type);
    result.pixels.row_stride = result.metadata.width * planes * pixelSize;
    result.pixels.size = static_cast<size_t>(result.pixels.row_stride) * result.metadata.height;
    result.pixels.data = new std::byte[result.pixels.size]();

    if (raw)
    {
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
}

} // anonymous namespace

// ---------------------------------------------------------------
// File-backed read (original path)
// ---------------------------------------------------------------
struct DngReaderImpl
{
    std::string filepath;
    explicit DngReaderImpl(const char* path) : filepath(path)
    {}
};

DngReader::DngReader(const char* path)
    : impl_(std::make_unique<DngReaderImpl>(path))
{}

DngReader::~DngReader() = default;

RawImage DngReader::Read()
{
    auto* p = impl_.get();
#ifdef _WIN32
    std::wstring wpath = io::Utf8ToWide(p->filepath);
    dng_file_stream stream(wpath.c_str(), false);
#else
    dng_file_stream stream(p->filepath.c_str(), false);
#endif
    return RawReadDngFromStream(stream);
}

// ---------------------------------------------------------------
// Memory-backed decode (I/O and decode separated)
// ---------------------------------------------------------------
RawImage ReadDngFromBuffer(const void* data, uint32_t size)
{
    dng_stream stream(data, size);
    return RawReadDngFromStream(stream);
}

} // namespace burstmerge
