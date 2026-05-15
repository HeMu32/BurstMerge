#include "burstmerge/internal/exposure/exposure.h"

#include <algorithm>
#include <cmath>

namespace burstmerge {

void ApplyExposure(FloatImage& image, uint32_t white_level, const ExposureParams& params) {
    if (params.mode == ExposureMode::Off || params.stops == 0.0f) return;

    const float gain = std::pow(2.0f, params.stops);
    const float white = static_cast<float>(white_level);

    if (params.mode == ExposureMode::Linear) {
        for (float& v : image.data) v = std::max(0.0f, v * gain);
        float mx = MaxValue(image);
        if (mx > white && mx > 0.0f) {
            float scale = white / mx;
            for (float& v : image.data) v *= scale;
        }
        return;
    }

    if (params.mode == ExposureMode::Curve) {
        for (float& v : image.data) {
            float x = std::max(0.0f, v * gain);
            v = white * (x / (x + white));
        }
    }
}

} // namespace burstmerge
