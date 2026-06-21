// test_align_gpu: unit test comparing CPU vs GPU alignment TILE MOTION directly
// (not image output). For each alignment mode, runs EstimateTranslation (CPU) and
// GpuEstimateTranslation (GPU) on the same grayscale plane images and reports the
// per-tile shift difference. Acceptance: max |dx|, |dy| <= 2 px on every tile.
//
// Usage: test_align_gpu <img0> <img1> [img2 ...]   (RAW/DNG; >=2 frames)
// Decodes the first two frames, builds gray plane images, compares alignment.

#include "burstmerge/api.h"
#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/core/gpu_pipeline.h"
#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/pipeline_io.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/dng_io.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), buf.size());
    return buf;
}

int g_checks = 0, g_failed = 0;
#define CHECK(c, m) do { ++g_checks; if(!(c)){ std::cerr << "  FAIL [" << __LINE__ << "]: " << m << std::endl; ++g_failed; } } while(0)

void CompareTileMotion(const char* tag, const burstmerge::AlignmentResult& cpu,
                       const burstmerge::AlignmentResult& gpu)
{
    std::printf("[%s] CPU geom: tiles=%ux%u tile_size=%d spacing=%d shift=(%d,%d)\n",
                tag, cpu.tiles_x, cpu.tiles_y, cpu.tile_size, cpu.tile_spacing, cpu.shift_x, cpu.shift_y);
    std::printf("[%s] GPU geom: tiles=%ux%u tile_size=%d spacing=%d shift=(%d,%d)\n",
                tag, gpu.tiles_x, gpu.tiles_y, gpu.tile_size, gpu.tile_spacing, gpu.shift_x, gpu.shift_y);
    CHECK(cpu.tiles_x == gpu.tiles_x && cpu.tiles_y == gpu.tiles_y,
          std::string(tag) + ": tile geometry mismatch");
    if (cpu.tiles_x != gpu.tiles_x || cpu.tiles_y != gpu.tiles_y) return;

    int max_dx = 0, max_dy = 0;
    double sum_dx = 0, sum_dy = 0;
    int over2 = 0, over5 = 0;
    size_t n = cpu.tile_shift_x.size();
    int worst_x = 0, worst_y = 0, worst_tx = 0, worst_ty = 0;
    for (size_t i = 0; i < n; ++i)
    {
        int dx = std::abs(int(cpu.tile_shift_x[i]) - int(gpu.tile_shift_x[i]));
        int dy = std::abs(int(cpu.tile_shift_y[i]) - int(gpu.tile_shift_y[i]));
        sum_dx += dx; sum_dy += dy;
        if (dx > max_dx || (dx == max_dx && dy > max_dy)) { max_dx = dx; max_dy = dy; worst_x = int(cpu.tile_shift_x[i]); worst_y = int(cpu.tile_shift_y[i]); worst_tx = int(i % cpu.tiles_x); worst_ty = int(i / cpu.tiles_x); }
        else { if (dy > max_dy) { max_dy = dy; } }
        if (dx > 2 || dy > 2) ++over2;
        if (dx > 5 || dy > 5) ++over5;
    }
    std::printf("[%s] tiles=%zu  max|d|=(%d,%d)  mean=(%.3f,%.3f)  >2px:%d (%.1f%%)  >5px:%d\n",
                tag, n, max_dx, max_dy, sum_dx / std::max<size_t>(1, n), sum_dy / std::max<size_t>(1, n),
                over2, 100.0 * over2 / std::max<size_t>(1, n), over5);
    std::printf("[%s] worst tile: CPU shift=(%d,%d) at tile(%d,%d)\n", tag, worst_x, worst_y, worst_tx, worst_ty);
    // dump field ranges (CPU and GPU) to spot garbage
    {
        int cmin=0,cmax=0,gmin=0,gmax=0; bool first=true;
        for (size_t i=0;i<cpu.tile_shift_x.size();++i){int v=cpu.tile_shift_x[i]; if(first){cmin=cmax=v;first=false;}else{if(v<cmin)cmin=v;if(v>cmax)cmax=v;}}
        first=true;
        for (size_t i=0;i<gpu.tile_shift_x.size();++i){int v=gpu.tile_shift_x[i]; if(first){gmin=gmax=v;first=false;}else{if(v<gmin)gmin=v;if(v>gmax)gmax=v;}}
        std::printf("[%s] field X range: CPU[%d..%d] GPU[%d..%d]\n", tag, cmin, cmax, gmin, gmax);
    }
    // dump first divergent tiles
    int shown = 0;
    for (size_t i = 0; i < n && shown < 8; ++i)
    {
        int dx = int(cpu.tile_shift_x[i]) - int(gpu.tile_shift_x[i]);
        int dy = int(cpu.tile_shift_y[i]) - int(gpu.tile_shift_y[i]);
        if (dx != 0 || dy != 0)
        {
            int tx = int(i % cpu.tiles_x), ty = int(i / cpu.tiles_x);
            std::printf("[%s] div tile(%d,%d): CPU=(%d,%d) GPU=(%d,%d)\n", tag, tx, ty,
                int(cpu.tile_shift_x[i]), int(cpu.tile_shift_y[i]),
                int(gpu.tile_shift_x[i]), int(gpu.tile_shift_y[i]));
            ++shown;
        }
    }
    CHECK(max_dx <= 2 && max_dy <= 2, std::string(tag) + ": max tile-motion diff > 2px");
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: test_align_gpu <img0> <img1> [more...]\n";
        return 2;
    }
    try
    {
        std::vector<std::string> inputs;
        for (int i = 1; i < argc; ++i) inputs.push_back(argv[i]);
        if (inputs.size() < 2) { std::cerr << "need >=2 inputs\n"; return 2; }

        std::string convert_dir;
        std::printf("[step] PrepareDngInputs...\n"); std::fflush(stdout);
        std::vector<std::string> dng_paths = burstmerge::PrepareDngInputs(inputs, ".",
            [](float, const std::string&){}, convert_dir);
        if (dng_paths.size() < 2) throw std::runtime_error("decoding yielded < 2 frames");
        std::printf("[step] read+decode...\n"); std::fflush(stdout);
        auto buf0 = ReadFile(dng_paths[0]);
        auto buf1 = ReadFile(dng_paths[1]);
        burstmerge::RawImage r0 = burstmerge::ReadDngFromBuffer(buf0.data(), uint32_t(buf0.size()));
        burstmerge::RawImage r1 = burstmerge::ReadDngFromBuffer(buf1.data(), uint32_t(buf1.size()));
        std::printf("[step] build planes %ux%u / %ux%u\n", r0.pixels.width, r0.pixels.height, r1.pixels.width, r1.pixels.height); std::fflush(stdout);

        std::vector<burstmerge::RawImage> raws;
        raws.push_back(std::move(r0));
        raws.push_back(std::move(r1));
        std::vector<burstmerge::FloatImage> planes = burstmerge::BuildFloatImages(raws);
        std::printf("[step] grayscale...\n"); std::fflush(stdout);
        burstmerge::FloatImage g0 = burstmerge::ConvertPlanesToGrayscale(planes[0]);
        burstmerge::FloatImage g1 = burstmerge::ConvertPlanesToGrayscale(planes[1]);
        std::printf("gray plane: %ux%u x %u ch\n", g0.width, g0.height, g0.channels); std::fflush(stdout);

        burstmerge::AlignParams params;
        params.tile_size = 32;
        params.search_distance = 64;
        params.cfa_period = 1;
        params.align_gamma = 1.0f;
        params.smooth_tile_field = false;

        for (auto mode : {burstmerge::AlignmentMode::Standard, burstmerge::AlignmentMode::DenseTile})
        {
            params.mode = mode;
            const char* tag = (mode == burstmerge::AlignmentMode::DenseTile) ? "dense" : "standard";
            std::printf("\n=== %s ===\n", tag); std::fflush(stdout);
            std::printf("[step] CPU EstimateTranslation...\n"); std::fflush(stdout);
            burstmerge::AlignmentResult cpu = burstmerge::EstimateTranslation(g0, g1, params);
            std::printf("[step] GPU GpuEstimateTranslation...\n"); std::fflush(stdout);
            burstmerge::AlignmentResult gpu = burstmerge::GpuEstimateTranslation(g0, g1, params);
            std::printf("[step] compare...\n"); std::fflush(stdout);
            CompareTileMotion(tag, cpu, gpu);
        }

        // cleanup convert dir
        if (!convert_dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(convert_dir, ec);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "EXCEPTION: " << e.what() << std::endl;
        return 1;
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
