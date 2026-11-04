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
    neg.SetDefaultCropSize(width, height);
    neg.SetDefaultCropOrigin(0, 0);
    neg.SetActiveArea(dng_rect(0, 0, static_cast<int32>(height), static_cast<int32>(width)));
}

void ClearDngOriginalSizes(DngNegativeHolder* holder)
{
    if (!holder || !holder->negative) return;
    holder->negative->ClearOriginalSizes();
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
