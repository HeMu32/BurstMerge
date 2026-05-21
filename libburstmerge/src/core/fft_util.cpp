#include "burstmerge/internal/core/fft_util.h"

#include <map>

namespace burstmerge
{

cfft_plan GetCachedCfftPlan(size_t n)
{
    thread_local std::map<size_t, cfft_plan> cache;
    auto it = cache.find(n);
    if (it != cache.end()) return it->second;
    cfft_plan plan = make_cfft_plan(n);
    cache.emplace(n, plan);
    return plan;
}

void Fft2D(std::vector<std::complex<double>>& data, size_t w, size_t h, bool inverse)
{
    Fft2D(data.data(), w, h, inverse);
}

void Fft2D(std::complex<double>* data, size_t w, size_t h, bool inverse)
{
    if (w == 0 || h == 0) return;
    cfft_plan row_plan = GetCachedCfftPlan(w);
    cfft_plan col_plan = GetCachedCfftPlan(h);

    for (size_t y = 0; y < h; ++y)
    {
        double* row = reinterpret_cast<double*>(&data[y * w]);
        if (inverse) cfft_backward(row_plan, row, 1.0);
        else cfft_forward(row_plan, row, 1.0);
    }

    thread_local std::vector<double> scratch;
    const size_t needed = 2 * std::max(w, h);
    if (scratch.size() < needed) scratch.resize(needed);
    for (size_t x = 0; x < w; ++x)
    {
        for (size_t y = 0; y < h; ++y)
        {
            scratch[2 * y] = data[y * w + x].real();
            scratch[2 * y + 1] = data[y * w + x].imag();
        }
        if (inverse) cfft_backward(col_plan, scratch.data(), 1.0 / static_cast<double>(w * h));
        else cfft_forward(col_plan, scratch.data(), 1.0);
        for (size_t y = 0; y < h; ++y)
        {
            data[y * w + x] =
            {scratch[2 * y], scratch[2 * y + 1]};
        }
    }
}

} // namespace burstmerge
