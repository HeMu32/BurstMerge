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
                return 1;
            }
        }

        std::cout << "OK: decoded pixel buffers are identical\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
