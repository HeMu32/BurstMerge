// Minimal DNG pixel inspector.
// Compile: g++ -std=c++17 -I../libburstmerge/include -I../3rdparty/cxxopts/include
//   -L../build/libburstmerge -lburstmerge -o dump_dng.exe dump_dng.cpp
// Usage: dump_dng.exe output.dng region.csv
//   region.csv: columns x,y,r,g,b (nearest-neighbor demosaic on 2x2 Bayer blocks)

#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/core/float_image.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s input.dng [pixels.csv]\n", argv[0]);
        return 1;
    }

    const char* path = argv[1];
    bool dump_csv = (argc >= 3);

    try {
        burstmerge::DngReader reader(path);
        auto img = reader.Read();
        auto& meta = img.metadata;
        auto& buf = img.pixels;

        std::fprintf(stderr, "DNG: %s\n", path);
        std::fprintf(stderr, "  dimensions: %u x %u\n", meta.width, meta.height);
        std::fprintf(stderr, "  pattern_width: %u\n", meta.mosaic_pattern_width);
        std::fprintf(stderr, "  white_level: %u\n", meta.white_level);
        std::fprintf(stderr, "  black_level: [%.0f, %.0f, %.0f, %.0f]\n",
            meta.black_level[0], meta.black_level[1],
            meta.black_level[2], meta.black_level[3]);
        std::fprintf(stderr, "  pixel_format: %d\n", (int)buf.format);
        std::fprintf(stderr, "  planes: %u\n",
            (buf.format == burstmerge::PixelFormat::RGBA32_Float) ? 4u : 1u);

        if (meta.width == 0 || meta.height == 0 || !buf.data) {
            std::fprintf(stderr, "ERROR: no pixel data\n");
            return 1;
        }

        // Read pixels as float
        burstmerge::FloatImage fi = burstmerge::HostBufferToFloatImage(buf);

        // If the image is in plane format (channels = pattern_width^2), convert to mosaic
        uint32_t pw = meta.mosaic_pattern_width;
        if (pw > 1 && fi.channels == pw * pw) {
            std::fprintf(stderr, "  detected plane format, converting to mosaic...\n");
            uint32_t orig_w = meta.width;
            uint32_t orig_h = meta.height;
            fi = burstmerge::ConvertPlaneImageToMosaic(fi, orig_w, orig_h, pw);
        }

        // Compute stats
        double sum = 0.0;
        double sum_sq = 0.0;
        float min_v = +1e30f, max_v = -1e30f;
        uint64_t n = 0;
        uint64_t n_clipped = 0;
        float hi = static_cast<float>(meta.white_level);

        for (auto& v : fi.data) {
            sum += v;
            sum_sq += static_cast<double>(v) * static_cast<double>(v);
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
            if (v < 0.0f || v > hi) ++n_clipped;
            ++n;
        }

        double mean = sum / static_cast<double>(n);
        double variance = sum_sq / static_cast<double>(n) - mean * mean;

        std::fprintf(stderr, "\n  pixel stats:\n");
        std::fprintf(stderr, "    count: %llu\n", (unsigned long long)n);
        std::fprintf(stderr, "    range: [%.1f, %.1f]\n", min_v, max_v);
        std::fprintf(stderr, "    mean: %.2f\n", mean);
        std::fprintf(stderr, "    stddev: %.2f\n", std::sqrt(std::max(0.0, variance)));
        std::fprintf(stderr, "    clipped (out of [0, %.0f]): %llu / %llu\n",
            hi, (unsigned long long)n_clipped, (unsigned long long)n);

        // If using 14-bit sensor, also check normalized stats
        float effective_white = hi;
        if (effective_white > 0) {
            std::fprintf(stderr, "    normalized mean (of white_level): %.4f\n", mean / effective_white);
            std::fprintf(stderr, "    normalized max: %.4f\n", max_v / effective_white);
        }

        // Print per-channel stats if plane format
        if (fi.channels > 1) {
            for (uint32_t c = 0; c < fi.channels; ++c) {
                double ch_sum = 0, ch_sum_sq = 0;
                float ch_min = +1e30f, ch_max = -1e30f;
                uint64_t ch_n = 0;
                for (uint32_t y = 0; y < fi.height; ++y) {
                    for (uint32_t x = 0; x < fi.width; ++x) {
                        float v = fi.At(x, y, c);
                        ch_sum += v;
                        ch_sum_sq += static_cast<double>(v) * v;
                        ch_min = std::min(ch_min, v);
                        ch_max = std::max(ch_max, v);
                        ++ch_n;
                    }
                }
                double ch_mean = ch_sum / static_cast<double>(ch_n);
                double ch_var = ch_sum_sq / static_cast<double>(ch_n) - ch_mean * ch_mean;
                std::fprintf(stderr, "    channel[%u]: range=[%.1f, %.1f] mean=%.2f std=%.2f\n",
                    c, ch_min, ch_max, ch_mean, std::sqrt(std::max(0.0, ch_var)));
            }
        }

        // Dump a CSV of a region near the brightest pixels (demosaiced RGBA)
        if (dump_csv) {
            const char* csv_path = argv[2];
            FILE* f = fopen(csv_path, "w");
            if (!f) {
                std::fprintf(stderr, "ERROR: cannot write %s\n", csv_path);
                return 1;
            }
            std::fprintf(f, "x,y,raw_value\n");

            // Find brightest rows (top 10 rows that contain max-value pixels)
            uint32_t search_rows = std::min<uint32_t>(50, fi.height);
            std::vector<float> row_max(fi.height, 0.0f);
            for (uint32_t y = 0; y < fi.height; ++y) {
                for (uint32_t x = 0; x < fi.width; ++x) {
                    row_max[y] = std::max(row_max[y], fi.At(x, y, 0));
                }
            }

            // Pick the row with the max value
            float overall_max = 0;
            uint32_t max_y = 0;
            for (uint32_t y = 0; y < fi.height; ++y) {
                if (row_max[y] > overall_max) {
                    overall_max = row_max[y];
                    max_y = y;
                }
            }

            uint32_t region_y0 = (max_y > 16) ? max_y - 16 : 0;
            uint32_t region_y1 = std::min(fi.height, region_y0 + 64);
            uint32_t region_x0 = 0;
            uint32_t region_x1 = std::min<uint32_t>(64, fi.width);

            std::fprintf(stderr, "\n  dumping region [%u..%u, %u..%u] to %s\n",
                region_x0, region_x1 - 1, region_y0, region_y1 - 1, csv_path);

            for (uint32_t y = region_y0; y < region_y1; ++y) {
                for (uint32_t x = region_x0; x < region_x1; ++x) {
                    std::fprintf(f, "%u,%u,%.0f\n",
                        x, y, fi.At(x, y, 0));
                }
            }
            fclose(f);
            std::fprintf(stderr, "  wrote %u x %u region\n",
                region_x1 - region_x0, region_y1 - region_y0);
        }

        return 0;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
