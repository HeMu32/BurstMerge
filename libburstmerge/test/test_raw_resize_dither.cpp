#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/core/image_resize.h"
#include "burstmerge/internal/io/dng_io.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

static int g_tests = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    ++g_tests; \
    if (!(cond)) { \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << msg << std::endl; \
        ++g_failed; \
    } \
} while(0)

namespace
{

std::string SamplePath()
{
    return std::string(TEST_DATA_DIR) + "/libburstmerge/test/samples/X1M5_Wide.dng";
}

struct SampleStats
{
    uint32_t white = 0;
    uint32_t data_max = 0;
    uint32_t min_gap = 0;
    std::vector<float> values;
};

void ReadSample(const std::string& path, SampleStats& stats)
{
    burstmerge::DngReader reader(path.c_str());
    burstmerge::RawImage img = reader.Read();
    burstmerge::FloatImage fi = burstmerge::HostBufferToFloatImage(img.pixels);

    const uint32_t period = img.metadata.mosaic_pattern_width;
    if (period > 1 && fi.channels == period * period)
        fi = burstmerge::ConvertPlaneImageToMosaic(fi, img.metadata.width, img.metadata.height, period);

    stats.white = img.metadata.white_level;
    stats.values = fi.data;

    std::set<uint32_t> distinct;
    for (float v : fi.data)
    {
        const uint32_t u = static_cast<uint32_t>(v + 0.5f);
        distinct.insert(u);
        stats.data_max = std::max(stats.data_max, u);
    }

    uint32_t gap = 0xFFFFFFFFu;
    uint32_t prev = 0;
    bool first = true;
    for (uint32_t v : distinct)
    {
        if (!first)
        {
            const uint32_t d = v - prev;
            if (d > 0 && d < gap) gap = d;
        }
        prev = v;
        first = false;
    }
    stats.min_gap = (gap == 0xFFFFFFFFu) ? 0u : gap;
}

uint32_t ContainerMax(int bit_depth)
{
    return (bit_depth >= 16) ? 65535u : ((1u << bit_depth) - 1u);
}

std::string OutPath(const std::string& suffix)
{
    return std::string(TEST_BINARY_DIR) + "/test_raw_resize_dither_" + suffix + ".dng";
}

struct DiffStats
{
    double mean_abs = 0.0;
    double max_abs = 0.0;
};

DiffStats MeasureDither(const burstmerge::RawResizeOptions& opts,
                        const std::string& input,
                        const std::string& tag)
{
    burstmerge::RawResizeOptions local = opts;
    local.dither = 0.0f;
    burstmerge::RawResizeResult ra = burstmerge::ProcessRawResize(input, OutPath(tag + "_d0"), local, nullptr);

    local.dither = 1.0f;
    burstmerge::RawResizeResult rb = burstmerge::ProcessRawResize(input, OutPath(tag + "_d1"), local, nullptr);

    DiffStats out;
    CHECK(ra.success, tag + " dither=0 run succeeds: " + ra.error_msg);
    CHECK(rb.success, tag + " dither=1 run succeeds: " + rb.error_msg);
    if (!ra.success || !rb.success) return out;

    SampleStats a;
    SampleStats b;
    ReadSample(OutPath(tag + "_d0"), a);
    ReadSample(OutPath(tag + "_d1"), b);

    double sum_abs = 0.0;
    double max_abs = 0.0;
    const size_t n = std::min(a.values.size(), b.values.size());
    for (size_t i = 0; i < n; ++i)
    {
        const double d = static_cast<double>(b.values[i]) - static_cast<double>(a.values[i]);
        sum_abs += std::fabs(d);
        max_abs = std::max(max_abs, std::fabs(d));
    }
    out.mean_abs = (n > 0) ? sum_abs / static_cast<double>(n) : 0.0;
    out.max_abs = max_abs;
    return out;
}

} // namespace

// raw_resize scales the sensor into the full output container (WhiteLevel = container
// top), which widens the sensor's own quantisation grid to
// bit_scale = container / sensor_white output LSBs (64x for the 10-bit sample).
// --dither-ref selects which grid the amplitude refers to:
//   output - 1 LSB of the output container (default, as documented for --dither)
//   input  - 1 LSB of the sensor grid, i.e. bit_scale output LSBs, which keeps the
//            dither's strength relative to the data grid identical on every RAW.
int main()
{
    const std::string input = SamplePath();
    if (!std::filesystem::exists(input))
    {
        std::cout << "[skip] sample not present: " << input << std::endl;
        return 0;
    }

    SampleStats in;
    ReadSample(input, in);
    CHECK(in.white > 0 && in.white < 65535,
          "sample is a sub-16-bit sensor (WhiteLevel=" + std::to_string(in.white) + ")");

    burstmerge::RawResizeOptions opts;
    opts.scale = 0.5;
    opts.interp = burstmerge::InterpolationMethod::Bicubic;

    for (int bit_depth : { 16, 14, 12 })
    {
        const uint32_t container = ContainerMax(bit_depth);
        const double bit_scale = static_cast<double>(container) / static_cast<double>(in.white);
        const std::string tag = "bit-depth " + std::to_string(bit_depth);

        opts.bit_depth = bit_depth;

        // Container fill (design goal) and the resulting data grid width.
        opts.dither = 0.0f;
        burstmerge::RawResizeResult r = burstmerge::ProcessRawResize(input, OutPath(tag + "_fill"), opts, nullptr);
        CHECK(r.success, tag + " fill run succeeds: " + r.error_msg);
        if (!r.success) continue;

        SampleStats filled;
        ReadSample(OutPath(tag + "_fill"), filled);
        CHECK(r.target_white == container, tag + ": reported target white fills the container");
        CHECK(filled.white == container, tag + ": output WhiteLevel fills the container (" + std::to_string(filled.white) + ")");
        CHECK(filled.data_max <= filled.white, tag + ": output data does not exceed the output WhiteLevel");
        CHECK(filled.min_gap >= 2, tag + ": output data grid is rescaled (min_gap=" + std::to_string(filled.min_gap) + ")");
        CHECK(std::fabs(static_cast<double>(filled.min_gap) - bit_scale) <= 2.0,
              tag + ": grid width matches bit_scale (min_gap=" + std::to_string(filled.min_gap) +
              " vs bit_scale=" + std::to_string(bit_scale) + ")");

        // Default reference: 1 LSB of the output container on every bit depth.
        opts.dither_ref = burstmerge::DitherReference::OutputLsb;
        DiffStats out_ref = MeasureDither(opts, input, tag + "_out");
        std::cout << "  " << tag << ": grid=" << filled.min_gap << " bit_scale=" << bit_scale
                  << " | output-ref mean|d|=" << out_ref.mean_abs << " max|d|=" << out_ref.max_abs
                  << std::endl;
        CHECK(out_ref.max_abs > 0.5 && out_ref.max_abs <= 1.0 + 1e-6,
              tag + ": output-referenced dither is 1 container LSB (max|d|=" + std::to_string(out_ref.max_abs) + ")");
        CHECK(out_ref.mean_abs > 0.15 && out_ref.mean_abs < 0.40,
              tag + ": output-referenced dither amplitude (mean|d|=" + std::to_string(out_ref.mean_abs) + ")");

        // Input reference: 1 LSB of the sensor grid, so the strength relative to the
        // data grid (~0.25 of a TPDF LSB) is identical on every bit depth.
        opts.dither_ref = burstmerge::DitherReference::InputLsb;
        DiffStats in_ref = MeasureDither(opts, input, tag + "_in");
        const double rel = (bit_scale > 0.0) ? in_ref.mean_abs / bit_scale : 0.0;
        std::cout << "  " << tag << ": input-ref  mean|d|=" << in_ref.mean_abs
                  << " max|d|=" << in_ref.max_abs << " mean|d|/bit_scale=" << rel << std::endl;
        CHECK(in_ref.max_abs > bit_scale * 0.5 && in_ref.max_abs <= bit_scale + 1e-6,
              tag + ": input-referenced dither is bit_scale output LSB (max|d|=" + std::to_string(in_ref.max_abs) + ")");
        CHECK(rel > 0.15 && rel < 0.40,
              tag + ": input-referenced dither strength relative to the data grid is stable (mean|d|/bit_scale=" +
              std::to_string(rel) + ")");
    }

    std::cout << "\n================================" << std::endl;
    std::cout << "raw_resize dither: " << g_tests << " checks, " << g_failed << " failed" << std::endl;
    std::cout << "================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
