#include "burstmerge/internal/core/demosaic.h"
#include "burstmerge/internal/core/gpu_pipeline.h"
#include "burstmerge/internal/core/super_resolution.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
int failed = 0;
#define CHECK(c, m) do { if (!(c)) { std::cerr << "FAIL: " << m << '\n'; ++failed; } } while (0)

burstmerge::FloatImage MakePlanes()
{
    burstmerge::FloatImage image;
    image.width = 4;
    image.height = 4;
    image.channels = 4;
    image.data.resize(4 * 4 * 4);
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
        {
            image.At(x, y, 0) = 100.0f;
            image.At(x, y, 1) = 200.0f;
            image.At(x, y, 2) = 200.0f;
            image.At(x, y, 3) = 300.0f;
        }
    return image;
}

burstmerge::FloatImage ShiftQuarterPixel(const burstmerge::FloatImage& source)
{
    burstmerge::FloatImage shifted = source;
    for (uint32_t y = 0; y < source.height; ++y)
    {
        for (uint32_t x = 0; x < source.width; ++x)
        {
            uint32_t x1 = std::min(x + 1, source.width - 1);
            uint32_t y1 = std::min(y + 1, source.height - 1);
            shifted.At(x, y) =
                source.At(x, y) * 0.5625f +
                source.At(x1, y) * 0.1875f +
                source.At(x, y1) * 0.1875f +
                source.At(x1, y1) * 0.0625f;
        }
    }
    return shifted;
}
}

int main()
{
    burstmerge::Settings defaults;
    CHECK(defaults.preprocess_interpolation == burstmerge::PreprocessInterpolation::Off,
          "pre-processing interpolation is disabled by default");
    CHECK(defaults.super_resolution == burstmerge::SuperResolutionMode::Off,
          "super-resolution is disabled by default");

    burstmerge::RawMetadata metadata;
    metadata.width = 8;
    metadata.height = 8;
    metadata.mosaic_pattern_width = 2;
    metadata.mosaic_pattern[0] = 0;
    metadata.mosaic_pattern[1] = 1;
    metadata.mosaic_pattern[2] = 1;
    metadata.mosaic_pattern[3] = 2;

    for (auto method : {burstmerge::PreprocessInterpolation::Nearest,
                        burstmerge::PreprocessInterpolation::Bilinear,
                        burstmerge::PreprocessInterpolation::MalvarHeCutler})
    {
        burstmerge::FloatImage rgb = burstmerge::DemosaicBayer(MakePlanes(), metadata, method);
        CHECK(rgb.width == 8 && rgb.height == 8 && rgb.channels == 3, "demosaic topology");
        CHECK(std::abs(rgb.At(3, 3, 0) - 100.0f) < 0.01f, "constant red");
        CHECK(std::abs(rgb.At(3, 3, 1) - 200.0f) < 0.01f, "constant green");
        CHECK(std::abs(rgb.At(3, 3, 2) - 300.0f) < 0.01f, "constant blue");
    }

    metadata.black_level[0] = 100.0f;
    metadata.black_level[1] = 120.0f;
    metadata.black_level[2] = 120.0f;
    metadata.black_level[3] = 140.0f;
    burstmerge::FloatImage normalized = MakePlanes();
    for (float& value : normalized.data) value = (value - 120.0f) * 0.5f;
    burstmerge::FloatImage corrected = burstmerge::DemosaicBayer(
        normalized, metadata, burstmerge::PreprocessInterpolation::Bilinear, 0.5f);
    CHECK(std::abs(corrected.At(3, 3, 0)) < 0.01f, "scaled red black delta");
    CHECK(std::abs(corrected.At(3, 3, 1) - 40.0f) < 0.01f, "scaled green black delta");
    CHECK(std::abs(corrected.At(3, 3, 2) - 80.0f) < 0.01f, "scaled blue black delta");

    burstmerge::FloatImage reference;
    reference.width = 16;
    reference.height = 16;
    reference.channels = 1;
    reference.data.resize(256);
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x)
            reference.At(x, y) = static_cast<float>(x + y * 2);
    std::vector<burstmerge::FloatImage> comparisons = {reference};
    std::vector<float> scales = {1.0f};
    burstmerge::FloatImage sr = burstmerge::SuperResolve2x(
        reference, comparisons, scales,
        burstmerge::SuperResolutionInterpolation::Bilinear, 0.0f);
    CHECK(sr.width == 32 && sr.height == 32 && sr.channels == 1, "super-resolution topology");
    CHECK(std::abs(sr.At(8, 10) - reference.At(4, 5)) < 0.01f, "direct sample preserved");
    CHECK(std::abs(sr.At(9, 10) - (reference.At(4, 5) + reference.At(5, 5)) * 0.5f) < 0.01f,
          "bilinear fallback sample");
    CHECK(burstmerge::SuperResolutionConstants::kDirectSampleWeight >
           burstmerge::SuperResolutionConstants::kInterpolatedSampleWeight,
           "direct sample has larger fixed weight");

    burstmerge::FloatImage bicubic = burstmerge::SuperResolve2x(
        reference, comparisons, scales,
        burstmerge::SuperResolutionInterpolation::Bicubic, 0.0f);
    CHECK(bicubic.width == sr.width && bicubic.height == sr.height,
          "bicubic super-resolution topology");

    // Green-zipper regression: demosaic a noiseless horizontal black/white
    // Bayer edge. The interpolated green must not create a 2px checkerboard
    // (the classic non-adaptive zipper) at the edge.
    {
        burstmerge::RawMetadata edge_meta;
        edge_meta.width = 32;
        edge_meta.height = 32;
        edge_meta.mosaic_pattern_width = 2;
        edge_meta.mosaic_pattern[0] = 0;
        edge_meta.mosaic_pattern[1] = 1;
        edge_meta.mosaic_pattern[2] = 1;
        edge_meta.mosaic_pattern[3] = 2;
        burstmerge::FloatImage edge_planes;
        edge_planes.width = 16;
        edge_planes.height = 16;
        edge_planes.channels = 4;
        edge_planes.data.resize(16u * 16u * 4u, 1000.0f);
        for (uint32_t y = 8; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                for (uint32_t c = 0; c < 4; ++c)
                    edge_planes.At(x, y, c) = 0.0f;

        for (auto method : {burstmerge::PreprocessInterpolation::Bilinear,
                            burstmerge::PreprocessInterpolation::MalvarHeCutler})
        {
            burstmerge::FloatImage edge = burstmerge::DemosaicBayer(
                edge_planes, edge_meta, method, 1.0f);
            // Row 15 is the last white photosite row; row 16 the first black.
            // Green must be uniform 1000 across row 15 (no 2px alternation).
            float g15 = edge.At(0, 15, 1);
            bool uniform_row = true;
            for (uint32_t x = 0; x < 16; ++x)
                if (std::abs(edge.At(x, 15, 1) - g15) > 0.01f) uniform_row = false;
            CHECK(uniform_row, "demosaic green uniform across horizontal edge (no zipper)");
            CHECK(std::abs(edge.At(0, 14, 1) - 1000.0f) < 0.01f &&
                  std::abs(edge.At(0, 16, 1)) < 0.01f,
                  "demosaic green edge transition clean");
        }
    }

    if (burstmerge::GpuVulkanAvailable())
    {
        burstmerge::FloatImage gpu = burstmerge::GpuSuperResolve2x(
            reference, comparisons, scales,
            burstmerge::SuperResolutionInterpolation::Bilinear, 0.0f);
        CHECK(gpu.width == sr.width && gpu.height == sr.height && gpu.channels == sr.channels,
              "GPU super-resolution topology");
        float max_error = 0.0f;
        for (size_t i = 0; i < sr.data.size(); ++i)
            max_error = std::max(max_error, std::abs(sr.data[i] - gpu.data[i]));
        CHECK(max_error < 0.01f, "GPU super-resolution matches CPU on identity input");

        std::vector<burstmerge::FloatImage> shifted_comparisons = {
            ShiftQuarterPixel(reference)
        };
        burstmerge::FloatImage shifted_cpu = burstmerge::SuperResolve2x(
            reference, shifted_comparisons, scales,
            burstmerge::SuperResolutionInterpolation::Bilinear, 0.0f);
        burstmerge::FloatImage shifted_gpu = burstmerge::GpuSuperResolve2x(
            reference, shifted_comparisons, scales,
            burstmerge::SuperResolutionInterpolation::Bilinear, 0.0f);
        max_error = 0.0f;
        for (size_t i = 0; i < shifted_cpu.data.size(); ++i)
            max_error = std::max(max_error, std::abs(shifted_cpu.data[i] - shifted_gpu.data[i]));
        CHECK(max_error < 0.05f, "GPU residual reconstruction matches CPU at quarter-pixel shift");
    }

    std::cout << "interpolate/superres checks: " << (failed == 0 ? "passed" : "failed") << '\n';
    return failed == 0 ? 0 : 1;
}
