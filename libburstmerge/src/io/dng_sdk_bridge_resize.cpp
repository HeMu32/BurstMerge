#include "burstmerge/internal/io/dng_sdk_bridge_resize.h"

#include "dng_host.h"
#include "dng_negative.h"
#include "dng_shared.h"

#include "burstmerge/internal/io/dng_io.h"
#include "dng_sdk_bridge.h"

namespace burstmerge
{
namespace io
{

void SetDngDimensions(DngNegativeHolder* holder, uint32_t width, uint32_t height)
{
    if (!holder || !holder->negative) return;
    dng_negative& neg = *holder->negative;
    const dng_point old_size = neg.RawImage().Bounds().Size();
    const real64 scale_h = old_size.h > 0 ? static_cast<real64>(width) / old_size.h : 1.0;
    const real64 scale_v = old_size.v > 0 ? static_cast<real64>(height) / old_size.v : 1.0;
    neg.SetDefaultCropSize(neg.DefaultCropSizeH().As_real64() * scale_h,
                           neg.DefaultCropSizeV().As_real64() * scale_v);
    neg.SetDefaultCropOrigin(neg.DefaultCropOriginH().As_real64() * scale_h,
                             neg.DefaultCropOriginV().As_real64() * scale_v);
    neg.SetActiveArea(dng_rect(0, 0, static_cast<int32>(height), static_cast<int32>(width)));
    neg.SetOriginalSizes(dng_point(static_cast<int32>(height), static_cast<int32>(width)));
}

void ClearDngOriginalSizes(DngNegativeHolder* holder)
{
    if (!holder || !holder->negative) return;
    holder->negative->ClearOriginalSizes();
}

void ClearDngOpcodes(DngNegativeHolder* holder)
{
    if (!holder || !holder->negative) return;
    holder->negative->OpcodeList1().Clear();
    holder->negative->OpcodeList2().Clear();
    holder->negative->OpcodeList3().Clear();
}

void ClearDngCameraHints(DngNegativeHolder* holder)
{
    if (!holder || !holder->negative) return;
    dng_negative& neg = *holder->negative;

    dng_urational invalid;
    invalid.Clear();
    neg.SetAntiAliasStrength(invalid);
    neg.SetChromaBlurRadius(invalid);

    neg.SetBaselineSharpness(static_cast<real64>(0.0));
    neg.SetBaselineNoise(static_cast<real64>(1.0));

    neg.SetNoiseProfile(dng_noise_profile());

    neg.SetGreenSplit(static_cast<uint32>(0));
}

} // namespace io
} // namespace burstmerge
