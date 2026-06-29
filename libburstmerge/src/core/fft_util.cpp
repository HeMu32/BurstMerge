#include "burstmerge/internal/core/fft_util.h"

#include <map>

namespace burstmerge
{

cfft_plan GetCachedCfftPlan(size_t n)
{
    thread_local cfft_plan plans[16] = {};
    if (n < 16)
    {
        if (!plans[n]) plans[n] = make_cfft_plan(n);
        return plans[n];
    }
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

    if (w <= 8 && h <= 8)
    {
        double scratch[16];
        const double scale = inverse ? 1.0 / static_cast<double>(w * h) : 1.0;
        for (size_t x = 0; x < w; ++x)
        {
            for (size_t y = 0; y < h; ++y)
            {
                scratch[2 * y] = data[y * w + x].real();
                scratch[2 * y + 1] = data[y * w + x].imag();
            }
            if (inverse) cfft_backward(col_plan, scratch, scale);
            else cfft_forward(col_plan, scratch, 1.0);
            for (size_t y = 0; y < h; ++y)
            {
                data[y * w + x] = {scratch[2 * y], scratch[2 * y + 1]};
            }
        }
    }
    else
    {
        thread_local std::vector<double> vs;
        const size_t needed = 2 * std::max(w, h);
        if (vs.size() < needed) vs.resize(needed);
        for (size_t x = 0; x < w; ++x)
        {
            for (size_t y = 0; y < h; ++y)
            {
                vs[2 * y] = data[y * w + x].real();
                vs[2 * y + 1] = data[y * w + x].imag();
            }
            if (inverse) cfft_backward(col_plan, vs.data(), 1.0 / static_cast<double>(w * h));
            else cfft_forward(col_plan, vs.data(), 1.0);
            for (size_t y = 0; y < h; ++y)
            {
                data[y * w + x] = {vs[2 * y], vs[2 * y + 1]};
            }
        }
    }
}

} // namespace burstmerge
