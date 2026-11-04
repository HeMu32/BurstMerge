#pragma once

// Optional raw-resize feature; see image_resize.h for the feature-macro
// contract. Guarded out entirely when the raw_resize tools are not built.
#ifndef BURSTMERGE_HAVE_RAW_RESIZE

#else

#include <cstdint>

namespace burstmerge
{
struct DngNegativeHolder;

namespace io
{

void SetDngDimensions(DngNegativeHolder* holder, uint32_t width, uint32_t height);
void ClearDngOriginalSizes(DngNegativeHolder* holder);
void ClearDngCameraHints(DngNegativeHolder* holder);

} // namespace io
} // namespace burstmerge

#endif // BURSTMERGE_HAVE_RAW_RESIZE
