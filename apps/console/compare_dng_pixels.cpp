#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/core/float_image.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: burstmerge_compare <fileA.dng> <fileB.dng>\n";
        return 2;
    }

    try
    {
        burstmerge::DngReader reader_a(argv[1]);
        burstmerge::DngReader reader_b(argv[2]);
        auto a = reader_a.Read();
        auto b = reader_b.Read();

        if (a.metadata.width != b.metadata.width ||
            a.metadata.height != b.metadata.height ||
            a.pixels.format != b.pixels.format ||
            a.pixels.row_stride != b.pixels.row_stride)
        {
            std::cerr << "DIFF: metadata/layout mismatch\n";
            return 1;
        }

        burstmerge::FloatImage fa = burstmerge::HostBufferToFloatImage(a.pixels);
        burstmerge::FloatImage fb = burstmerge::HostBufferToFloatImage(b.pixels);
        if (fa.channels != fb.channels || fa.data.size() != fb.data.size())
        {
            std::cerr << "DIFF: float image shape mismatch\n";
            return 1;
        }

        for (size_t i = 0; i < fa.data.size(); ++i)
        {
            if (fa.data[i] != fb.data[i])
            {
                std::cerr << "DIFF: pixel data mismatch at index " << i
                          << " (" << fa.data[i] << " vs " << fb.data[i] << ")\n";
                // fall through to stats
                break;
            }
        }

        double suma = 0, sumb = 0, mad = 0, mina = 1e30, maxa = -1e30;
        for (size_t i = 0; i < fa.data.size(); ++i)
        {
            suma += fa.data[i];
            sumb += fb.data[i];
            mad += std::fabs(fa.data[i] - fb.data[i]);
            if (fa.data[i] < mina) mina = fa.data[i];
            if (fa.data[i] > maxa) maxa = fa.data[i];
        }
        double n = static_cast<double>(fa.data.size());
        std::cout << "STATS A: mean=" << suma / n << " min=" << mina << " max=" << maxa << "\n";
        std::cout << "STATS B: mean=" << sumb / n << "\n";
        std::cout << "MAD A-B=" << mad / n << "  (rel=" << (mad / n) / (suma / n) << ")\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
