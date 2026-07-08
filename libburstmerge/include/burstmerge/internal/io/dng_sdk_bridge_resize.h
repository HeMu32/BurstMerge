#pragma once

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
