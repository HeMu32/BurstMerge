#include "burstmerge/internal/core/dither.h"

#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/task_executor.h"

namespace burstmerge
{

void ApplyQuantizationDither(FloatImage& img, float amplitude)
{
    if (amplitude <= 0.0f) return;

    const uint32_t w = img.width;
    const uint32_t ch = img.channels;
    const size_t num_pixels = static_cast<size_t>(w) * img.height;
    const size_t grain = 1u << 16;

    ParallelFor(num_pixels, grain, [&](size_t p0, size_t p1)
    {
        for (size_t p = p0; p < p1; ++p)
        {
            const float x = static_cast<float>(p % w);
            const float y = static_cast<float>(p / w);
            const float dither = DitherTPDF(x, y, amplitude);
            const size_t base = p * ch;
            for (uint32_t c = 0; c < ch; ++c)
                img.data[base + c] += dither;
        }
    }, "dither" /* named tag for profiler */);
}

} // namespace burstmerge
