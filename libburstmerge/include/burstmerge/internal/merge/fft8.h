#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace burstmerge
{

inline void Fft8Forward(float* d)
{
    std::swap(d[2], d[8]);
    std::swap(d[3], d[9]);
    std::swap(d[6], d[12]);
    std::swap(d[7], d[13]);

    for (int i = 0; i < 8; i += 2)
    {
        float tr = d[2*i], ti = d[2*i+1];
        float ur = d[2*(i+1)], ui = d[2*(i+1)+1];
        d[2*i] = tr + ur;
        d[2*i+1] = ti + ui;
        d[2*(i+1)] = tr - ur;
        d[2*(i+1)+1] = ti - ui;
    }

    {
        float tr = d[0], ti = d[1];
        float ur = d[4], ui = d[5];
        d[0] = tr + ur; d[1] = ti + ui;
        d[4] = tr - ur; d[5] = ti - ui;
    }
    {
        float tr = d[2], ti = d[3];
        float ur = d[6], ui = d[7];
        float wr = ui, wi = -ur;
        d[2] = tr + wr; d[3] = ti + wi;
        d[6] = tr - wr; d[7] = ti - wi;
    }
    {
        float tr = d[8], ti = d[9];
        float ur = d[12], ui = d[13];
        d[8] = tr + ur; d[9] = ti + ui;
        d[12] = tr - ur; d[13] = ti - ui;
    }
    {
        float tr = d[10], ti = d[11];
        float ur = d[14], ui = d[15];
        float wr = ui, wi = -ur;
        d[10] = tr + wr; d[11] = ti + wi;
        d[14] = tr - wr; d[15] = ti - wi;
    }

    const float s = 0.70710678118654752f;
    {
        float tr = d[0], ti = d[1];
        float ur = d[8], ui = d[9];
        d[0] = tr + ur; d[1] = ti + ui;
        d[8] = tr - ur; d[9] = ti - ui;
    }
    {
        float tr = d[2], ti = d[3];
        float ur = d[10], ui = d[11];
        float wr = s * (ur + ui);
        float wi = s * (ui - ur);
        d[2] = tr + wr; d[3] = ti + wi;
        d[10] = tr - wr; d[11] = ti - wi;
    }
    {
        float tr = d[4], ti = d[5];
        float ur = d[12], ui = d[13];
        float wr = ui, wi = -ur;
        d[4] = tr + wr; d[5] = ti + wi;
        d[12] = tr - wr; d[13] = ti - wi;
    }
    {
        float tr = d[6], ti = d[7];
        float ur = d[14], ui = d[15];
        float wr = s * (ui - ur);
        float wi = -s * (ur + ui);
        d[6] = tr + wr; d[7] = ti + wi;
        d[14] = tr - wr; d[15] = ti - wi;
    }
}

inline void Fft8Backward(float* d)
{
    std::swap(d[2], d[8]);
    std::swap(d[3], d[9]);
    std::swap(d[6], d[12]);
    std::swap(d[7], d[13]);

    for (int i = 0; i < 8; i += 2)
    {
        float tr = d[2*i], ti = d[2*i+1];
        float ur = d[2*(i+1)], ui = d[2*(i+1)+1];
        d[2*i] = tr + ur;
        d[2*i+1] = ti + ui;
        d[2*(i+1)] = tr - ur;
        d[2*(i+1)+1] = ti - ui;
    }

    {
        float tr = d[0], ti = d[1];
        float ur = d[4], ui = d[5];
        d[0] = tr + ur; d[1] = ti + ui;
        d[4] = tr - ur; d[5] = ti - ui;
    }
    {
        float tr = d[2], ti = d[3];
        float ur = d[6], ui = d[7];
        float wr = -ui, wi = ur;
        d[2] = tr + wr; d[3] = ti + wi;
        d[6] = tr - wr; d[7] = ti - wi;
    }
    {
        float tr = d[8], ti = d[9];
        float ur = d[12], ui = d[13];
        d[8] = tr + ur; d[9] = ti + ui;
        d[12] = tr - ur; d[13] = ti - ui;
    }
    {
        float tr = d[10], ti = d[11];
        float ur = d[14], ui = d[15];
        float wr = -ui, wi = ur;
        d[10] = tr + wr; d[11] = ti + wi;
        d[14] = tr - wr; d[15] = ti - wi;
    }

    const float s = 0.70710678118654752f;
    {
        float tr = d[0], ti = d[1];
        float ur = d[8], ui = d[9];
        d[0] = tr + ur; d[1] = ti + ui;
        d[8] = tr - ur; d[9] = ti - ui;
    }
    {
        float tr = d[2], ti = d[3];
        float ur = d[10], ui = d[11];
        float wr = s * (ur - ui);
        float wi = s * (ur + ui);
        d[2] = tr + wr; d[3] = ti + wi;
        d[10] = tr - wr; d[11] = ti - wi;
    }
    {
        float tr = d[4], ti = d[5];
        float ur = d[12], ui = d[13];
        float wr = -ui, wi = ur;
        d[4] = tr + wr; d[5] = ti + wi;
        d[12] = tr - wr; d[13] = ti - wi;
    }
    {
        float tr = d[6], ti = d[7];
        float ur = d[14], ui = d[15];
        float wr = -s * (ur + ui);
        float wi = s * (ur - ui);
        d[6] = tr + wr; d[7] = ti + wi;
        d[14] = tr - wr; d[15] = ti - wi;
    }

    const float inv8 = 0.125f;
    for (int i = 0; i < 16; ++i) d[i] *= inv8;
}

inline void Fft2D8x8Forward(float* data)
{
    for (int y = 0; y < 8; ++y)
        Fft8Forward(data + y * 16);

    float tmp[16];
    for (int x = 0; x < 8; ++x)
    {
        for (int y = 0; y < 8; ++y)
        {
            tmp[2*y]   = data[y * 16 + 2*x];
            tmp[2*y+1] = data[y * 16 + 2*x + 1];
        }
        Fft8Forward(tmp);
        for (int y = 0; y < 8; ++y)
        {
            data[y * 16 + 2*x]     = tmp[2*y];
            data[y * 16 + 2*x + 1] = tmp[2*y + 1];
        }
    }
}

inline void Fft2D8x8Backward(float* data)
{
    for (int y = 0; y < 8; ++y)
        Fft8Backward(data + y * 16);

    float tmp[16];
    for (int x = 0; x < 8; ++x)
    {
        for (int y = 0; y < 8; ++y)
        {
            tmp[2*y]   = data[y * 16 + 2*x];
            tmp[2*y+1] = data[y * 16 + 2*x + 1];
        }
        Fft8Backward(tmp);
        for (int y = 0; y < 8; ++y)
        {
            data[y * 16 + 2*x]     = tmp[2*y];
            data[y * 16 + 2*x + 1] = tmp[2*y + 1];
        }
    }
}

}
