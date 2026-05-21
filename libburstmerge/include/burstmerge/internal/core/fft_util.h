#pragma once

#include <complex>
#include <cstddef>
#include <vector>

extern "C"
{
#include "pocketfft.h"
}

namespace burstmerge
{

cfft_plan GetCachedCfftPlan(size_t n);

void Fft2D(std::vector<std::complex<double>>& data, size_t w, size_t h, bool inverse);
void Fft2D(std::complex<double>* data, size_t w, size_t h, bool inverse);

} // namespace burstmerge
