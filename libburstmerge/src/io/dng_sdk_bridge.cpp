#include "dng_sdk_bridge.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "dng_host.h"
#include "dng_negative.h"
#include "dng_image.h"
#include "dng_pixel_buffer.h"
#include "dng_simple_image.h"
#include "dng_mosaic_info.h"
#include "dng_linearization_info.h"
#include "dng_shared.h"
#include "dng_exif.h"
#include "dng_tag_values.h"
#include "dng_sdk_limits.h"

#include <cstring>

#include "burstmerge/internal/io/dng_io.h"

namespace burstmerge {
namespace io {

#ifdef _WIN32
std::wstring Utf8ToWide(const char* utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf.data(), len);
    buf.pop_back();
    return buf;
}
#endif

DngHostHolder::DngHostHolder()
    : host(new dng_host(), [](dng_host* p) { delete p; })
{}

DngHostHolder::~DngHostHolder() = default;

DngNegativeHolder::DngNegativeHolder()
    : host_holder(std::make_unique<DngHostHolder>())
{}

DngNegativeHolder::~DngNegativeHolder() {
    if (negative) {
        delete negative;
        negative = nullptr;
    }
}

DngNegativeHolder* CreateNegativeHolder() {
    return new DngNegativeHolder();
}

void DestroyNegativeHolder(DngNegativeHolder* holder) {
    delete holder;
}

static uint32_t DngPixelSize(uint32_t tagType) {
    switch (tagType) {
        case ttByte:   return 1;
        case ttShort:  return 2;
        case ttSShort: return 2;
        case ttLong:   return 4;
        case ttFloat:  return 4;
        default:       return 2;
    }
}

void ReadRawImageFromNegative(DngNegativeHolder* holder, void* pixels,
                               uint32_t width, uint32_t height,
                               uint32_t planes, uint32_t pixelType, size_t pitch)
{
    if (!holder || !holder->negative) return;

    dng_negative& neg = *holder->negative;
    const dng_image* raw = nullptr;

    if (neg.Stage1Image()) {
        raw = neg.Stage1Image();
    } else {
        try { raw = &neg.RawImage(); }
        catch (...) { return; }
    }

    if (!raw) return;

    uint32_t pixelSize = DngPixelSize(pixelType);
    if (pitch == 0) pitch = static_cast<size_t>(width) * pixelSize;

    dng_pixel_buffer pb;
    pb.fArea = dng_rect(0, 0, height, width);
    pb.fPlane = 0;
    pb.fPlanes = planes;
    pb.fRowStep = static_cast<int32_t>(pitch / pixelSize);
    pb.fColStep = static_cast<int32_t>(planes);
    pb.fPlaneStep = 1;
    pb.fPixelType = pixelType;
    pb.fPixelSize = pixelSize;
    pb.fData = pixels;

    raw->Get(pb);
}

void WriteRawImageToNegative(DngNegativeHolder* holder,
                              const void* pixels, uint32_t width, uint32_t height,
                              uint32_t planes, uint32_t pixelType, size_t pitch)
{
    if (!holder || !holder->negative) return;

    dng_negative& neg = *holder->negative;
    dng_host& host = holder->host_holder->get();

    uint32_t pixelSize = DngPixelSize(pixelType);
    if (pitch == 0) pitch = static_cast<size_t>(width) * pixelSize;

    AutoPtr<dng_image> newImage(
        host.Make_dng_image(dng_rect(0, 0, height, width), planes, pixelType));

    dng_pixel_buffer pb;
    pb.fArea = dng_rect(0, 0, height, width);
    pb.fPlane = 0;
    pb.fPlanes = planes;
    pb.fRowStep = static_cast<int32_t>(pitch / pixelSize);
    pb.fColStep = static_cast<int32_t>(planes);
    pb.fPlaneStep = 1;
    pb.fPixelType = pixelType;
    pb.fPixelSize = pixelSize;
    pb.fData = const_cast<void*>(pixels);

    newImage->Put(pb);

    AutoPtr<dng_image> imagePtr(newImage.Release());
    neg.SetStage1Image(imagePtr);
    neg.ClearRawImage();
    neg.ClearRawLossyCompressedImage();
    neg.ClearRawImageDigest();
}

void ExtractRawMetadata(DngNegativeHolder* holder,
                          uint32_t& width, uint32_t& height,
                          uint32_t& white_level, float black_level[4],
                          uint32_t& pattern_width, uint16_t* mosaic_pattern,
                          float color_factors[4])
{
    if (!holder || !holder->negative) return;

    dng_negative& neg = *holder->negative;

    width = 0; height = 0;
    if (neg.Stage1Image()) {
        width = neg.Stage1Image()->Width();
        height = neg.Stage1Image()->Height();
    } else {
        try {
            const dng_image& raw = neg.RawImage();
            width = raw.Width();
            height = raw.Height();
        } catch (...) {}
    }

    white_level = neg.WhiteLevel(0);

    for (int i = 0; i < 4; i++) black_level[i] = 0.0f;
    if (neg.GetLinearizationInfo()) {
        const auto& linfo = *neg.GetLinearizationInfo();
        for (uint32_t c = 0; c < 4 && c < kMaxColorPlanes; c++)
            black_level[c] = static_cast<float>(linfo.fBlackLevel[0][0][c]);
    }

    pattern_width = 2;
    memset(mosaic_pattern, 0, 36 * sizeof(uint16_t));
    if (neg.GetMosaicInfo()) {
        const auto& minfo = *neg.GetMosaicInfo();
        pattern_width = static_cast<uint32_t>(minfo.fCFAPatternSize.h);
        uint32_t max_i = static_cast<uint32_t>(minfo.fCFAPatternSize.h * minfo.fCFAPatternSize.v);
        for (uint32_t i = 0; i < 36 && i < max_i; i++) {
            uint32_t row = i / static_cast<uint32_t>(minfo.fCFAPatternSize.h);
            uint32_t col = i % static_cast<uint32_t>(minfo.fCFAPatternSize.h);
            mosaic_pattern[i] = minfo.fCFAPattern[row][col];
        }
    }

    for (uint32_t c = 0; c < 4 && c < neg.ColorChannels(); c++) {
        double ab = neg.AnalogBalance(c);
        color_factors[c] = (ab != 0.0) ? static_cast<float>(1.0 / ab) : 1.0f;
    }
    for (uint32_t c = neg.ColorChannels(); c < 4; c++)
        color_factors[c] = 1.0f;
}

void ExtractExposureMetadata(DngNegativeHolder* holder,
                             float& exposure_bias,
                             float& iso_exposure_time)
{
    exposure_bias = 0.0f;
    iso_exposure_time = 0.0f;

    if (!holder || !holder->negative) return;

    dng_negative& neg = *holder->negative;
    exposure_bias = static_cast<float>(neg.BaselineExposure());

    const dng_exif* exif = neg.GetExif();
    if (!exif) return;

    real64 exposure_time = exif->fExposureTime.IsValid() ? exif->fExposureTime.As_real64() : 0.0;
    uint32 iso = exif->fISOSpeed;
    if (iso == 0) {
        iso = exif->fISOSpeedRatings[0];
    }
    if (iso == 0 && exif->fStandardOutputSensitivity != 0) {
        iso = exif->fStandardOutputSensitivity;
    }

    if (exposure_time > 0.0 && iso > 0) {
        iso_exposure_time = static_cast<float>(exposure_time * static_cast<real64>(iso));
    }
}

} // namespace io

// ============================================================
// RawMetadata lifecycle (defined here where DngNegativeHolder is complete)
// ============================================================

RawMetadata::~RawMetadata() {
    if (dng_negative) {
        io::DestroyNegativeHolder(dng_negative);
        dng_negative = nullptr;
    }
}

RawMetadata::RawMetadata(RawMetadata&& other) noexcept
    : width(other.width), height(other.height),
      mosaic_pattern_width(other.mosaic_pattern_width),
      mosaic_pattern(std::move(other.mosaic_pattern)),
      white_level(other.white_level),
      exposure_bias(other.exposure_bias),
      iso_exposure_time(other.iso_exposure_time),
      dng_pixel_type(other.dng_pixel_type),
      dng_negative(other.dng_negative)
{
    for (int i = 0; i < 4; i++) {
        black_level[i] = other.black_level[i];
        color_factors[i] = other.color_factors[i];
    }
    other.dng_negative = nullptr;
    other.width = 0;
    other.height = 0;
}

RawMetadata& RawMetadata::operator=(RawMetadata&& other) noexcept {
    if (this != &other) {
        if (dng_negative) io::DestroyNegativeHolder(dng_negative);

        width = other.width; other.width = 0;
        height = other.height; other.height = 0;
        mosaic_pattern_width = other.mosaic_pattern_width;
        mosaic_pattern = std::move(other.mosaic_pattern);
        white_level = other.white_level;
        exposure_bias = other.exposure_bias;
        iso_exposure_time = other.iso_exposure_time;
        dng_pixel_type = other.dng_pixel_type;
        dng_negative = other.dng_negative; other.dng_negative = nullptr;
        for (int i = 0; i < 4; i++) {
            black_level[i] = other.black_level[i];
            color_factors[i] = other.color_factors[i];
        }
    }
    return *this;
}

} // namespace burstmerge
