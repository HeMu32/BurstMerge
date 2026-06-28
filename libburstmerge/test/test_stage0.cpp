#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <new>
#include <cmath>

#include "burstmerge/api.h"
#include "burstmerge/api_c.h"
#include "burstmerge/internal/io/dng_io.h"
#include "burstmerge/internal/core/types.h"
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/sub_graph.h"
#include "burstmerge/internal/compute/compute_backend.h"
#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/merge/spatial.h"
#include "burstmerge/internal/merge/frequency.h"
#include "burstmerge/internal/denoise/temporal.h"
#include "burstmerge/internal/exposure/exposure.h"
#include "burstmerge/internal/core/pipeline_frame.h"

static int g_tests = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    g_tests++; \
    if (!(cond)) \
    { \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << msg << std::endl; \
        g_failed++; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg)
#define CHECK_NE(a, b, msg) CHECK((a) != (b), msg)

// ============================================================
// 1. Internal type tests
// ============================================================
static void test_types()
{
    std::cout << "[test] internal types..." << std::endl;

    // TileInfo defaults
    burstmerge::TileInfo ti
    {};
    CHECK_EQ(ti.tile_x, 0u, "TileInfo.tile_x default");
    CHECK_EQ(ti.width, 0u, "TileInfo.width default");

    // Settings (public API) defaults
    burstmerge::Settings s
    {};
    CHECK_EQ(s.tile_size, 32, "Settings.tile_size default");
    CHECK_EQ(s.search_distance, 64, "Settings.search_distance default");
    CHECK_EQ(s.noise_reduction, 13.0f, "Settings.noise_reduction default");
    CHECK(s.merge_algo == burstmerge::MergeAlgorithm::Spatial,
           "Settings.merge_algo default Spatial");
    CHECK(s.exposure_mode == burstmerge::ExposureMode::Off,
           "Settings.exposure_mode default Off");

    // RawMetadata defaults
    burstmerge::RawMetadata rm
    {};
    CHECK_EQ(rm.width, 0u, "RawMetadata.width default");
    CHECK_EQ(rm.white_level, 65535u, "RawMetadata.white_level default");
    CHECK_EQ(rm.color_factors[0], 1.0f, "RawMetadata.color_factors[0] default");
    CHECK_EQ(rm.mosaic_pattern_width, 2u, "RawMetadata.mosaic_pattern_width default");
    CHECK_EQ(rm.dng_negative, nullptr, "RawMetadata.dng_negative default null");
    CHECK(rm.dng_pixel_type == burstmerge::DngPixelType::Uint16,
           "RawMetadata.dng_pixel_type default Uint16");

    std::cout << "  Internal types OK" << std::endl;
}

// ============================================================
// 2. HostBuffer tests
// ============================================================
static void test_hostbuffer()
{
    std::cout << "[test] HostBuffer..." << std::endl;

    // Default construction
    burstmerge::HostBuffer hb
    {};
    CHECK_EQ(hb.data, nullptr, "HostBuffer default data is null");
    CHECK_EQ(hb.size, 0u, "HostBuffer default size is 0");

    // Allocation
    hb.width = 100;
    hb.height = 50;
    hb.format = burstmerge::PixelFormat::R16_Uint;
    hb.row_stride = 100 * 2;
    hb.size = static_cast<size_t>(hb.row_stride) * hb.height;
    hb.data = new std::byte[hb.size]();
    CHECK_NE(hb.data, nullptr, "HostBuffer allocated data not null");

    // Fill with pattern
    for (size_t i = 0; i < hb.size; i++)
    {
        hb.data[i] = static_cast<std::byte>(i & 0xFF);
    }

    // Move semantics
    burstmerge::HostBuffer hb2(std::move(hb));
    CHECK_EQ(hb.data, nullptr, "HostBuffer moved-from data is null");
    CHECK_EQ(hb.size, 0u, "HostBuffer moved-from size is 0");
    CHECK_EQ(hb.width, 0u, "HostBuffer moved-from width reset");
    CHECK_EQ(hb.height, 0u, "HostBuffer moved-from height reset");
    CHECK_EQ(hb.row_stride, 0u, "HostBuffer moved-from row_stride reset");
    CHECK_NE(hb2.data, nullptr, "HostBuffer moved-to data not null");
    CHECK_EQ(hb2.width, 100u, "HostBuffer moved-to width");
    CHECK_EQ(hb2.height, 50u, "HostBuffer moved-to height");

    // Verify moved data integrity
    for (size_t i = 0; i < hb2.size && i < 10; i++)
    {
        CHECK_EQ(static_cast<int>(hb2.data[i]), static_cast<int>(i & 0xFF),
                 "HostBuffer moved data integrity");
    }

    // Move assign
    burstmerge::HostBuffer hb3
    {};
    hb3 = std::move(hb2);
    CHECK_EQ(hb2.data, nullptr, "HostBuffer move-assign source nullified");
    CHECK_NE(hb3.data, nullptr, "HostBuffer move-assign dest has data");

    // Verify moved-from is in valid empty state
    CHECK_EQ(hb2.width, 0u, "HostBuffer moved-from verify empty width");
    CHECK_EQ(hb2.height, 0u, "HostBuffer moved-from verify empty height");
    CHECK_EQ(hb2.row_stride, 0u, "HostBuffer moved-from verify empty stride");
    CHECK_EQ(hb2.size, 0u, "HostBuffer moved-from verify empty size");

    std::cout << "  HostBuffer OK" << std::endl;
}

// ============================================================
// 3. DeviceBuffer tests
// ============================================================
static void test_devicebuffer()
{
    std::cout << "[test] DeviceBuffer..." << std::endl;

    burstmerge::DeviceBuffer db
    {};
    CHECK_EQ(db.handle, 0u, "DeviceBuffer.handle default");
    CHECK_EQ(db.width, 0u, "DeviceBuffer.width default");
    CHECK_EQ(db.height, 0u, "DeviceBuffer.height default");
    CHECK_EQ(db.depth, 1u, "DeviceBuffer.depth default");
    CHECK_EQ(db.location, burstmerge::MemoryLocation::Device,
             "DeviceBuffer.location default");

    db.handle = 42;
    db.width = 1920;
    db.height = 1080;
    CHECK_EQ(db.handle, 42u, "DeviceBuffer.handle set");
    CHECK_EQ(db.width, 1920u, "DeviceBuffer.width set");

    std::cout << "  DeviceBuffer OK" << std::endl;
}

// ============================================================
// 4. DngReader tests
// ============================================================
static void test_dng_reader()
{
    std::cout << "[test] DngReader..." << std::endl;

    std::string sample_path = std::string(TEST_DATA_DIR)
        + "/libburstmerge/test/samples/X1M5_Wide.dng";

    FILE* f = fopen(sample_path.c_str(), "rb");
    CHECK_NE(f, nullptr, "Sample DNG file exists");
    if (!f)
    {
        std::cout << "  SKIP: sample file not found" << std::endl;
        return;
    }
    fclose(f);

    burstmerge::DngReader reader(sample_path.c_str());
    auto image = reader.Read();

    CHECK(image.metadata.width > 0, "DNG width > 0");
    CHECK(image.metadata.height > 0, "DNG height > 0");
    CHECK(image.metadata.white_level > 0, "DNG white_level > 0");
    CHECK_NE(image.metadata.dng_negative, nullptr,
             "DNG dng_negative not null");
    CHECK(image.metadata.exposure_bias >= -16.0f && image.metadata.exposure_bias <= 16.0f,
          "DNG exposure_bias in sane range");
    CHECK(image.metadata.ev_value > 0.0f,
          "DNG ev_value > 0");

    std::cout << "  DNG: " << image.metadata.width << "x"
              << image.metadata.height
              << " white=" << image.metadata.white_level
              << " pattern=" << image.metadata.mosaic_pattern_width
              << " type=" << static_cast<int>(image.metadata.dng_pixel_type)
              << std::endl;

    CHECK_NE(image.pixels.data, nullptr, "DNG pixel data not null");
    CHECK_EQ(image.pixels.width, image.metadata.width, "DNG pixel width matches");
    CHECK_EQ(image.pixels.height, image.metadata.height, "DNG pixel height matches");
    CHECK(image.pixels.size > 0, "DNG pixel data size > 0");

    bool has_data = false;
    for (size_t i = 0; i < image.pixels.size && i < 1000; i++)
    {
        if (image.pixels.data[i] != std::byte
        {0}) {
            has_data = true;
            break;
        }
    }
    CHECK(has_data, "DNG pixel data contains non-zero values");

    switch (image.metadata.dng_pixel_type)
    {
        case burstmerge::DngPixelType::Uint8:
            CHECK(image.pixels.format == burstmerge::PixelFormat::R8_Uint,
                  "Uint8 DNG maps to R8_Uint");
            break;
        case burstmerge::DngPixelType::Uint16:
            CHECK(image.pixels.format == burstmerge::PixelFormat::R16_Uint,
                  "Uint16 DNG maps to R16_Uint");
            break;
        case burstmerge::DngPixelType::Float32:
            CHECK(image.pixels.format == burstmerge::PixelFormat::R32_Float,
                  "Float32 DNG maps to R32_Float");
            break;
        default:
            break;
    }

    bool has_pattern = false;
    for (int i = 0; i < 36; i++)
    {
        if (image.metadata.mosaic_pattern[i] != 0)
        {
            has_pattern = true;
            break;
        }
    }
    CHECK(has_pattern, "DNG mosaic pattern present");

    for (int c = 0; c < 4; c++)
    {
        CHECK(image.metadata.black_level[c] >= 0,
              "DNG black_level[" + std::to_string(c) + "] >= 0");
    }

    std::cout << "  DngReader OK" << std::endl;
}

// ============================================================
// 5. DngWriter round-trip test
// ============================================================
static void test_dng_writer_roundtrip()
{
    std::cout << "[test] DngWriter round-trip..." << std::endl;

    std::string sample_path = std::string(TEST_DATA_DIR)
        + "/libburstmerge/test/samples/X1M5_Wide.dng";
    std::string out_path = std::string(TEST_DATA_DIR)
        + "/build/test_roundtrip.dng";

    FILE* f = fopen(sample_path.c_str(), "rb");
    if (!f)
    {
        std::cout << "  SKIP: sample file not found" << std::endl;
        return;
    }
    fclose(f);

    // Read original
    burstmerge::DngReader reader(sample_path.c_str());
    auto original = reader.Read();

    CHECK_NE(original.metadata.dng_negative, nullptr,
             "Original has dng_negative");
    if (!original.metadata.dng_negative) return;

    // Write (DngWriter takes ownership of dng_negative)
    {
        burstmerge::DngWriter writer(original.metadata.dng_negative);
        writer.Write(out_path.c_str(), original);
    }
    // After this, original.metadata.dng_negative is null (moved by DngWriter)

    // Verify output file exists and has content
    f = fopen(out_path.c_str(), "rb");
    CHECK_NE(f, nullptr, "Output DNG file exists");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long out_size = ftell(f);
        fclose(f);
        CHECK(out_size > 1000, "Output DNG size > 1KB");

        // Read back the written file
        burstmerge::DngReader reader2(out_path.c_str());
        auto reread = reader2.Read();
        CHECK_EQ(reread.metadata.width, original.metadata.width,
                 "Round-trip width matches");
        CHECK_EQ(reread.metadata.height, original.metadata.height,
                 "Round-trip height matches");
        CHECK_EQ(reread.metadata.white_level, original.metadata.white_level,
                 "Round-trip white_level matches");
        CHECK(reread.metadata.dng_pixel_type == original.metadata.dng_pixel_type,
                "Round-trip pixel type matches");
        CHECK(reread.pixels.format == original.pixels.format,
                "Round-trip host pixel format matches");

        remove(out_path.c_str());
    }

    std::cout << "  DngWriter round-trip OK" << std::endl;
}

// ============================================================
// 6. C API tests
// ============================================================
static void test_c_api()
{
    std::cout << "[test] C API..." << std::endl;

    BM_Context ctx = BM_Create(0);
    CHECK_NE(ctx, nullptr, "BM_Create returns non-null");

    BM_SetTileSize(ctx, 64);
    BM_SetSearchDistance(ctx, 128);
    BM_SetNoiseReduction(ctx, 5.0f);
    BM_SetExposureMode(ctx, BM_EXPOSURE_LINEAR);
    BM_SetMergeAlgorithm(ctx, BM_ALGO_SPATIAL);
    BM_SetExposureStops(ctx, 1.5f);
    BM_AddImage(ctx, (std::string(TEST_DATA_DIR) + "/libburstmerge/test/samples/X1M5_Wide.dng").c_str());

    int result = BM_Process(ctx, (std::string(TEST_DATA_DIR) + "/build/test_c_api_output.dng").c_str());
    CHECK_EQ(result, 1, "BM_Process returns success");

    BM_Destroy(ctx);

    // Null context safety
    BM_Destroy(nullptr);
    BM_AddImage(nullptr, "test.dng");
    BM_SetTileSize(nullptr, 32);
    int null_result = BM_Process(nullptr, ".");
    CHECK_EQ(null_result, 0, "BM_Process(nullptr) returns 0");
    const char* err = BM_GetLastError(nullptr);
    CHECK_EQ(err[0], '\0', "BM_GetLastError(nullptr) returns empty");

    // Constants
    CHECK_EQ(BM_BACKEND_CPU, 0, "BM_BACKEND_CPU is 0");
    CHECK_EQ(BM_BACKEND_VULKAN, 1, "BM_BACKEND_VULKAN is 1");
    CHECK_EQ(BM_EXPOSURE_OFF, 0, "BM_EXPOSURE_OFF is 0");
    CHECK_EQ(BM_EXPOSURE_LINEAR, 1, "BM_EXPOSURE_LINEAR is 1");
    CHECK_EQ(BM_EXPOSURE_CURVE, 2, "BM_EXPOSURE_CURVE is 2");
    CHECK_EQ(BM_ALGO_SPATIAL, 0, "BM_ALGO_SPATIAL is 0");
    CHECK_EQ(BM_ALGO_FREQUENCY, 1, "BM_ALGO_FREQUENCY is 1");

    std::cout << "  C API OK" << std::endl;
}

static void test_progress_callback()
{
    std::cout << "[test] progress callback..." << std::endl;

    struct ProgressState
    {
        int calls = 0;
        float first = -1.0f;
        float last = -1.0f;
        std::string last_stage;
    } state;

    auto callback = [](float percent, const char* stage, void* user)
    {
        auto* s = static_cast<ProgressState*>(user);
        if (s->calls == 0)
        {
            s->first = percent;
        }
        s->last = percent;
        s->last_stage = stage ? stage : "";
        s->calls++;
    };

    BM_Context ctx = BM_Create(BM_BACKEND_CPU);
    CHECK_NE(ctx, nullptr, "BM_Create for progress test returns non-null");
    BM_SetProgressCallback(ctx, callback, &state);
    BM_AddImage(ctx, (std::string(TEST_DATA_DIR) + "/libburstmerge/test/samples/X1M5_Wide.dng").c_str());
    int result = BM_Process(ctx, (std::string(TEST_DATA_DIR) + "/build/test_progress_output.dng").c_str());
    CHECK_EQ(result, 1, "BM_Process with progress callback succeeds");
    CHECK(state.calls >= 2, "Progress callback invoked at least twice");
    CHECK(state.first == 0.0f, "Progress callback starts at 0");
    CHECK(state.last == 1.0f, "Progress callback ends at 1");
    CHECK(!state.last_stage.empty(), "Progress callback stage not empty");
    BM_Destroy(ctx);

    std::cout << "  Progress callback OK" << std::endl;
}

// ============================================================
// 7. Edge case tests
// ============================================================
static void test_edge_cases()
{
    std::cout << "[test] edge cases..." << std::endl;

    // DngReader with non-existent file (must not crash)
    {
        burstmerge::DngReader reader("/nonexistent/path.dng");
        try
        {
            auto image = reader.Read();
            CHECK_EQ(image.pixels.data, nullptr, "Nonexistent file returns empty pixels");
            CHECK_EQ(image.metadata.dng_negative, nullptr,
                     "Nonexistent file has no negative");
        } catch (const std::exception& e)
        {
            std::cout << "  (non-existent file threw: " << e.what() << ")" << std::endl;
        } catch (...)
        {
            std::cout << "  (non-existent file threw unknown)" << std::endl;
        }
    }

    // DngWriter with null holder (singular ownership: must pass a variable)
    {
        burstmerge::io::DngNegativeHolder* null_holder = nullptr;
        try
        {
            burstmerge::DngWriter writer(null_holder);
            burstmerge::RawImage dummy
            {};
            writer.Write("output.dng", dummy);
        } catch (const std::exception& e)
        {
            std::cout << "  (null writer threw: " << e.what() << ")" << std::endl;
        } catch (...)
        {
            std::cout << "  (null writer threw unknown)" << std::endl;
        }
    }

    // HostBuffer empty move + moved-from state fully reset
    {
        burstmerge::HostBuffer empty
        {};
        burstmerge::HostBuffer moved(std::move(empty));
        CHECK_EQ(moved.data, nullptr, "Empty HostBuffer move produces null data");
        CHECK_EQ(moved.size, 0u, "Empty HostBuffer move produces 0 size");
    }

    // RawMetadata empty move
    {
        burstmerge::RawMetadata rm1
        {};
        rm1.width = 100;
        rm1.height = 200;
        burstmerge::RawMetadata rm2(std::move(rm1));
        CHECK_EQ(rm2.width, 100u, "RawMetadata move preserves width");
        CHECK_EQ(rm1.width, 0u, "RawMetadata moved-from nulled width");
        CHECK_EQ(rm2.height, 200u, "RawMetadata move preserves height");
    }

    // DngPixelType helpers
    {
        CHECK_EQ(burstmerge::DngPixelTypeSize(burstmerge::DngPixelType::Uint8), 1u,
                 "DngPixelTypeSize Uint8");
        CHECK_EQ(burstmerge::DngPixelTypeSize(burstmerge::DngPixelType::Uint16), 2u,
                 "DngPixelTypeSize Uint16");
        CHECK_EQ(burstmerge::DngPixelTypeSize(burstmerge::DngPixelType::Float32), 4u,
                 "DngPixelTypeSize Float32");
    }

    std::cout << "  Edge cases OK" << std::endl;
}

// ============================================================
// 8. SubGraph type tests
// ============================================================
static void test_subgraph()
{
    std::cout << "[test] SubGraph types..." << std::endl;

    burstmerge::SubGraphNode sgn
    {};
    CHECK_EQ(sgn.input_handles.size(), 0u, "SubGraphNode empty inputs");
    CHECK_EQ(sgn.output_handles.size(), 0u, "SubGraphNode empty outputs");

    sgn.type = burstmerge::NodeType::Blur;
    sgn.input_handles.push_back(1);
    sgn.input_handles.push_back(2);
    CHECK_EQ(sgn.input_handles.size(), 2u, "SubGraphNode inputs after push");
    sgn.output_handles.push_back(3);
    CHECK_EQ(sgn.output_handles.size(), 1u, "SubGraphNode outputs after push");

    CHECK_EQ(static_cast<int>(burstmerge::NodeType::PrepareTexture), 0,
             "NodeType enum starts at 0");

    std::cout << "  SubGraph types OK" << std::endl;
}

// ============================================================
// 9. Module parameter struct tests
// ============================================================
static void test_module_params()
{
    std::cout << "[test] module params..." << std::endl;

    burstmerge::AlignParams ap
    {};
    CHECK_EQ(ap.tile_size, burstmerge::AlignConstants::kDefaultTileSize, "AlignParams.tile_size default");
    CHECK_EQ(ap.search_distance, burstmerge::AlignConstants::kDefaultSearchDistance, "AlignParams.search_distance default");

    burstmerge::SpatialMergeParams sp
    {};
    CHECK_EQ(sp.noise_reduction, 13.0f, "SpatialMergeParams.noise_reduction default");

    burstmerge::FrequencyMergeParams fp
    {};
    CHECK_EQ(fp.tile_size, 32, "FrequencyMergeParams.tile_size default");

    burstmerge::TemporalDenoiseParams tp
    {};
    CHECK_EQ(tp.strength, 23.0f, "TemporalDenoiseParams.strength default");

    burstmerge::ExposureParams ep
    {};
    CHECK_EQ(static_cast<int>(ep.mode), 0, "ExposureParams.mode default(Off)");
    CHECK_EQ(ep.stops, 0.0f, "ExposureParams.stops default");

    std::cout << "  Module params OK" << std::endl;
}

static burstmerge::FloatImage make_float_image(uint32_t width, uint32_t height, uint32_t channels, float value = 0.0f)
{
    burstmerge::FloatImage img;
    img.width = width;
    img.height = height;
    img.channels = channels;
    img.data.assign(static_cast<size_t>(width) * height * channels, value);
    return img;
}

// Regression: Wiener frequency merge must not create dark blocks or strong
// color bias around clipped highlights when comparison exposure differs.
static void test_frequency_wiener_highlights()
{
    std::cout << "[test] frequency wiener highlights..." << std::endl;

    const uint32_t width = 16;
    const uint32_t height = 16;
    const uint32_t channels = 4;
    const float black = 64.0f;
    const float white = 1023.0f;

    burstmerge::FloatImage reference = make_float_image(width, height, channels, black + 32.0f);
    burstmerge::FloatImage comparison = make_float_image(width, height, channels, black + 32.0f);

    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const bool in_tube = (x >= 6 && x <= 9);
            const bool near_tube = (x >= 5 && x <= 10);
            for (uint32_t c = 0; c < channels; ++c)
            {
                float ref_v = black + 32.0f;
                if (near_tube) ref_v = black + 220.0f;
                if (in_tube) ref_v = white - 8.0f;
                reference.At(x, y, c) = ref_v;

                float comp_v = ref_v;
                if (near_tube) comp_v = black + (ref_v - black) * 1.10f;
                if (in_tube)
                {
                    // R/B clip harder than G to emulate purple highlight risk.
                    static const float clip_vals[4] = {white, white - 96.0f, white - 88.0f, white};
                    comp_v = clip_vals[c];
                }
                comparison.At(x, y, c) = std::min(comp_v, white);
            }
        }
    }

    const float exposure_scale = 1.0f / 1.23f; // comparison about +0.3 EV before normalization
    for (float& v : comparison.data)
    {
        v = (v - black) * exposure_scale + black;
    }

    // Apply highlight recovery before frequency merge. Work in black-subtracted
    // space (matching post-NormalizeFrames state), then restore black.
    {
        std::vector<burstmerge::FloatImage> rec_imgs = {reference, comparison};
        for (auto& im : rec_imgs)
            for (float& v : im.data) v -= black;

        const uint16_t rggb[4] = {0, 1, 1, 2};
        auto mk = [&](float ev) {
            burstmerge::RawImage ri;
            ri.metadata.white_level = static_cast<uint32_t>(white);
            ri.metadata.mosaic_pattern_width = 2;
            for (int i = 0; i < 4; ++i)
            {
                ri.metadata.mosaic_pattern[i] = rggb[i];
                ri.metadata.black_level[i] = black;
            }
            ri.metadata.ev_value = ev;
            return ri;
        };
        std::vector<burstmerge::RawImage> rec_raws;
        rec_raws.push_back(mk(1.0f));
        rec_raws.push_back(mk(1.0f / exposure_scale));
        burstmerge::RecoverHighlights(rec_imgs, rec_raws, 0);

        reference = std::move(rec_imgs[0]);
        comparison = std::move(rec_imgs[1]);
        for (float& v : reference.data) v += black;
        for (float& v : comparison.data) v += black;
    }

    burstmerge::FrequencyMergeParams params{};
    params.mode = burstmerge::FrequencyMode::WienerFftRobust;
    params.noise_reduction = 13.0f;
    params.white_level = white;
    params.black_level = black;
    params.num_scales = 1;
    params.exposure_scales = &exposure_scale;

    burstmerge::FloatImage out = burstmerge::FrequencyMerge(reference, {comparison}, params);

    float min_edge = 1e9f;
    float max_edge = -1e9f;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 5; x <= 10; ++x)
        {
            for (uint32_t c = 0; c < channels; ++c)
            {
                float v = out.At(x, y, c);
                min_edge = std::min(min_edge, v);
                max_edge = std::max(max_edge, v);
            }
        }
    }

    float center_vals[4];
    for (uint32_t c = 0; c < channels; ++c)
    {
        center_vals[c] = out.At(7, 8, c);
    }
    float center_min = *std::min_element(center_vals, center_vals + 4);
    float center_max = *std::max_element(center_vals, center_vals + 4);

    CHECK(min_edge >= black + 80.0f,
          "Wiener robust highlight edge avoids dark block (min=" + std::to_string((int)min_edge) + ")");
    CHECK(center_min >= white * 0.80f,
          "Wiener robust highlight center stays bright (min=" + std::to_string((int)center_min) + ")");
    CHECK(center_max - center_min <= 140.0f,
          "Wiener robust highlight center avoids strong magenta bias (spread=" + std::to_string((int)(center_max - center_min)) + ")");

    std::cout << "  highlight edge min/max: " << (int)min_edge << "/" << (int)max_edge
              << " center: " << (int)center_vals[0] << " " << (int)center_vals[1]
              << " " << (int)center_vals[2] << " " << (int)center_vals[3] << std::endl;
}

// ============================================================
// Main
// ============================================================

// TemporalMedian: per-pixel median across reference + comparisons.
// Verifies odd/even count handling and outlier rejection.
static void test_temporal_median()
{
    std::cout << "[test] temporal median..." << std::endl;

    const uint32_t width = 4;
    const uint32_t height = 4;
    const uint32_t channels = 1;

    // Build 5 frames with values 10, 20, 30, 40, 50 at every pixel
    // (odd count).  Median should be 30.
    burstmerge::FloatImage ref = make_float_image(width, height, channels, 10.0f);
    std::vector<burstmerge::FloatImage> comps;
    static const float odd_vals[4] = {20.0f, 30.0f, 40.0f, 50.0f};
    for (float v : odd_vals) comps.push_back(make_float_image(width, height, channels, v));

    burstmerge::TemporalDenoiseParams params{};
    burstmerge::FloatImage out_odd = burstmerge::TemporalMedian(ref, comps, params);
    bool odd_ok = true;
    for (float v : out_odd.data) if (v != 30.0f) odd_ok = false;
    CHECK(odd_ok, "TemporalMedian odd count returns middle value");

    // Even count: drop the last comparison (4 frames: 10,20,30,40).
    // Median = (20+30)/2 = 25.
    std::vector<burstmerge::FloatImage> comps_even(comps.begin(), comps.begin() + 3);
    burstmerge::FloatImage out_even = burstmerge::TemporalMedian(ref, comps_even, params);
    bool even_ok = true;
    for (float v : out_even.data) if (v != 25.0f) even_ok = false;
    CHECK(even_ok, "TemporalMedian even count averages two middle values");

    // Outlier rejection: introduce a huge spike in one comparison frame.
    // With 5 frames total (10,20,30,40,5000), median is still 30.
    comps[3].data[0] = 5000.0f;
    burstmerge::FloatImage out_outlier = burstmerge::TemporalMedian(ref, comps, params);
    CHECK(out_outlier.data[0] == 30.0f,
          "TemporalMedian rejects outlier spike (got "
          + std::to_string(out_outlier.data[0]) + ")");

    // Single frame (no comparisons): output equals reference.
    std::vector<burstmerge::FloatImage> empty;
    burstmerge::FloatImage out_single = burstmerge::TemporalMedian(ref, empty, params);
    bool single_ok = (out_single.data == ref.data);
    CHECK(single_ok, "TemporalMedian with no comparisons returns reference");

    // MergeAlgorithm enum coverage: TemporalMedian must be distinct.
    burstmerge::Settings s{};
    s.merge_algo = burstmerge::MergeAlgorithm::TemporalMedian;
    CHECK(s.merge_algo != burstmerge::MergeAlgorithm::TemporalAverage,
          "MergeAlgorithm::TemporalMedian distinct from TemporalAverage");
    CHECK(s.merge_algo != burstmerge::MergeAlgorithm::Spatial,
          "MergeAlgorithm::TemporalMedian distinct from Spatial");

    // C ABI constants must map back to the enum.
    CHECK(static_cast<burstmerge::MergeAlgorithm>(BM_ALGO_TEMPORAL_MEDIAN)
            == burstmerge::MergeAlgorithm::TemporalMedian,
          "BM_ALGO_TEMPORAL_MEDIAN maps to TemporalMedian");

    std::cout << "  TemporalMedian OK" << std::endl;
}

// Clip detection + highlight bypass: comparisons with clipped values are
// excluded from the median (prevents Frankenstein pixel color casts), and
// reference values at/near clipping are preserved (keeps the signal for RAW
// converter highlight recovery). This variant runs WITHOUT highlight recovery.
static void test_temporal_median_clip_no_recovery()
{
    std::cout << "[test] temporal median clip + bypass..." << std::endl;

    const uint32_t width = 4;
    const uint32_t height = 4;
    const uint32_t channels = 4;

    burstmerge::FloatImage ref = make_float_image(width, height, channels, 100.0f);
    ref.data[0] = 1010.0f;  // pixel 0, ch0: above clip_threshold for bypass test

    burstmerge::FloatImage comp1 = make_float_image(width, height, channels, 120.0f);
    burstmerge::FloatImage comp2 = make_float_image(width, height, channels, 120.0f);
    comp2.data[0] = 1010.0f;  // clipped → should be rejected by clip detection
    burstmerge::FloatImage comp3 = make_float_image(width, height, channels, 110.0f);

    burstmerge::TemporalDenoiseParams params{};
    params.white_level = 1023.0f;
    params.black_level = 0.0f;
    // clip_threshold = 1023 * 0.98 = 1002.5

    std::vector<burstmerge::FloatImage> comps = {comp1, comp2, comp3};
    burstmerge::FloatImage out = burstmerge::TemporalMedian(ref, comps, params);

    // Pixel 0, ch0: ref=1010 ≥ threshold → bypass → output = 1010
    CHECK(out.At(0, 0, 0) == 1010.0f,
          "TemporalMedian bypasses clipped reference channel (got "
          + std::to_string(out.At(0, 0, 0)) + ")");

    // Pixel 0, ch1: ref=100, comp2 clipped (ch0=1010) → comp2 rejected.
    // Remaining: [100(ref), 120(comp1), 110(comp3)] → median = 110
    CHECK(out.At(0, 0, 1) == 110.0f,
          "TemporalMedian rejects clipped comparison (ch1 got "
          + std::to_string(out.At(0, 0, 1)) + ")");

    // Pixel 1: no clipping. All 4 values [100, 120, 120, 110] → median = 115
    CHECK(out.At(1, 0, 0) == 115.0f,
          "TemporalMedian normal median at non-clipped pixel (got "
          + std::to_string(out.At(1, 0, 0)) + ")");

    // Bracketed: comp value moderate but /exposure_scale exceeds threshold.
    burstmerge::FloatImage comp_br = make_float_image(width, height, channels, 600.0f);
    float exp_scale = 0.5f;  // 600/0.5 = 1200 ≥ 1002.5 → clipped
    params.num_scales = 1;
    params.exposure_scales = &exp_scale;

    std::vector<burstmerge::FloatImage> comps_br = {comp_br};
    burstmerge::FloatImage out_br = burstmerge::TemporalMedian(ref, comps_br, params);
    CHECK(out_br.At(0, 0, 0) == 1010.0f,
          "TemporalMedian bypass still works with exposure_scale (got "
          + std::to_string(out_br.At(0, 0, 0)) + ")");
    CHECK(out_br.At(0, 0, 1) == 100.0f,
          "TemporalMedian rejects exposure-scaled clipped comparison (got "
          + std::to_string(out_br.At(0, 0, 1)) + ")");

    std::cout << "  TemporalMedian clip + bypass OK (no recovery)" << std::endl;
}

// Same scenario as above but WITH highlight recovery applied first.
// Verifies that RecoverHighlights + TemporalMedian clip detection interact
// correctly: recovered green values don't break the bypass/reject logic.
static void test_temporal_median_clip_with_recovery()
{
    std::cout << "[test] temporal median clip + bypass (with recovery)..." << std::endl;

    const uint32_t width = 4;
    const uint32_t height = 4;
    const uint32_t channels = 4;
    const float white = 1023.0f;

    // Reference: R+B clipped at (0,0), G1 bright → recovery will raise G1
    burstmerge::FloatImage ref = make_float_image(width, height, channels, 100.0f);
    ref.At(0, 0, 0) = 1020.0f;  // R clipped
    ref.At(0, 0, 1) = 850.0f;   // G1 bright (ratio 0.83 > 0.8)
    ref.At(0, 0, 3) = 1020.0f;  // B clipped
    ref.At(1, 0, 0) = 1020.0f;  // right neighbour R (for G1 extrapolation)

    burstmerge::FloatImage comp1 = make_float_image(width, height, channels, 120.0f);
    burstmerge::FloatImage comp2 = make_float_image(width, height, channels, 120.0f);
    comp2.At(0, 0, 0) = 1020.0f;  // clipped → should be rejected
    burstmerge::FloatImage comp3 = make_float_image(width, height, channels, 110.0f);

    // Apply highlight recovery
    {
        std::vector<burstmerge::FloatImage> rec_imgs = {ref, comp1, comp2, comp3};
        const uint16_t rggb[4] = {0, 1, 1, 2};
        auto mk = [&]() {
            burstmerge::RawImage ri;
            ri.metadata.white_level = static_cast<uint32_t>(white);
            ri.metadata.mosaic_pattern_width = 2;
            for (int i = 0; i < 4; ++i) ri.metadata.mosaic_pattern[i] = rggb[i];
            ri.metadata.ev_value = 1.0f;
            return ri;
        };
        std::vector<burstmerge::RawImage> rec_raws;
        for (int i = 0; i < 4; ++i) rec_raws.push_back(mk());
        burstmerge::RecoverHighlights(rec_imgs, rec_raws, 0);
        ref = std::move(rec_imgs[0]);
        comp1 = std::move(rec_imgs[1]);
        comp2 = std::move(rec_imgs[2]);
        comp3 = std::move(rec_imgs[3]);
    }

    // Recovery should have raised G1 at (0,0) from 850 towards ~874
    float recovered_g1 = ref.At(0, 0, 1);
    CHECK(recovered_g1 > 860.0f,
          "Recovery raised ref G1 (got " + std::to_string(recovered_g1) + ")");

    burstmerge::TemporalDenoiseParams params{};
    params.white_level = white;
    params.black_level = 0.0f;

    std::vector<burstmerge::FloatImage> comps = {comp1, comp2, comp3};
    burstmerge::FloatImage out = burstmerge::TemporalMedian(ref, comps, params);

    // Pixel 0, ch0: ref=1020 ≥ threshold → bypass → output = 1020
    CHECK(out.At(0, 0, 0) == 1020.0f,
          "With recovery: clipped ref ch0 still bypassed (got "
          + std::to_string(out.At(0, 0, 0)) + ")");

    // Pixel 0, ch1: ref=~874, comp2 rejected (clipped ch0).
    // Remaining: [~874(ref), 120(comp1), 110(comp3)] → median = 120
    // (Without recovery it was 110 — recovery changed the median by raising ref G1.)
    CHECK(out.At(0, 0, 1) == 120.0f,
          "With recovery: median reflects raised ref G1 (got "
          + std::to_string(out.At(0, 0, 1)) + ")");

    // Pixel 1, ch0: ref=1020 (set as neighbour, also clipped) → bypass
    CHECK(out.At(1, 0, 0) == 1020.0f,
          "With recovery: neighbour pixel also bypassed (got "
          + std::to_string(out.At(1, 0, 0)) + ")");

    // Pixel 2: no clipping anywhere. [100, 120, 120, 110] → median = 115
    CHECK(out.At(2, 0, 0) == 115.0f,
          "With recovery: normal pixel unaffected (got "
          + std::to_string(out.At(2, 0, 0)) + ")");

    std::cout << "  TemporalMedian clip + bypass OK (with recovery)" << std::endl;
}

// Exercises the unified bracketing classifier (ClassifyExposureSequence /
// IsBracketedSequence / SelectExposureRefIndex) across the three EV-spread
// regimes: uniform, bracketed-only (1.4x..2.0x), and wide bracket (>2.0x).
static void test_exposure_classification()
{
    std::cout << "[test] exposure classification..." << std::endl;

    auto mk = [](float ev) {
        burstmerge::RawImage ri;
        ri.metadata.ev_value = ev;
        return ri;
    };

    // Uniform burst: same EV everywhere -> not bracketed, no chaining, middle ref.
    {
        std::vector<burstmerge::RawImage> imgs;
        for (int i = 0; i < 3; ++i) imgs.push_back(mk(1.0f));
        auto ec = burstmerge::ClassifyExposureSequence(imgs);
        CHECK(ec.has_exposure, "uniform: has_exposure");
        CHECK(!ec.is_bracketed, "uniform: not bracketed");
        CHECK(!ec.needs_chained_alignment, "uniform: no chained alignment");
        CHECK(!burstmerge::IsBracketedSequence(imgs), "uniform: IsBracketedSequence false");
        CHECK_EQ(burstmerge::SelectExposureRefIndex(imgs, ec), imgs.size() / 2,
                 "uniform: middle frame is reference");
    }

    // Narrow bracket (ratio 1.5x: within 1.4x..2.0x band): bracketed, but the
    // spread is not yet wide enough to trigger chained alignment.
    {
        std::vector<burstmerge::RawImage> imgs;
        imgs.push_back(mk(1.0f));
        imgs.push_back(mk(1.25f));
        imgs.push_back(mk(1.5f));
        auto ec = burstmerge::ClassifyExposureSequence(imgs);
        CHECK(ec.is_bracketed, "narrow bracket: bracketed");
        CHECK(!ec.needs_chained_alignment, "narrow bracket: no chained alignment");
        CHECK_EQ(ec.exposure_order.front().second, 0u,
                 "narrow bracket: order front is darkest (ev=1.0, idx 0)");
        CHECK_EQ(burstmerge::SelectExposureRefIndex(imgs, ec), 0u,
                 "narrow bracket: darkest frame is reference");
    }

    // Wide bracket (ratio 16x: >2.0x): both bracketed and chained alignment.
    {
        std::vector<burstmerge::RawImage> imgs;
        imgs.push_back(mk(1.0f));
        imgs.push_back(mk(0.25f));   // darkest
        imgs.push_back(mk(4.0f));
        auto ec = burstmerge::ClassifyExposureSequence(imgs);
        CHECK(ec.is_bracketed, "wide bracket: bracketed");
        CHECK(ec.needs_chained_alignment, "wide bracket: chained alignment");
        CHECK_EQ(burstmerge::SelectExposureRefIndex(imgs, ec), 1u,
                 "wide bracket: darkest frame (idx 1) is reference");
    }

    // No EV data at all -> not bracketed, empty order.
    {
        std::vector<burstmerge::RawImage> imgs;
        imgs.push_back(mk(0.0f));
        imgs.push_back(mk(0.0f));
        auto ec = burstmerge::ClassifyExposureSequence(imgs);
        CHECK(!ec.has_exposure, "no EV: not has_exposure");
        CHECK(!ec.is_bracketed, "no EV: not bracketed");
        CHECK(ec.exposure_order.empty(), "no EV: empty exposure_order");
    }

    std::cout << "  Exposure classification OK" << std::endl;
}

// Verifies the exposure-weight-number (wn) augmentation of SpatialMerge:
//  - uniform-burst invariance (wn == 1 -> bit-identical to legacy path),
//  - EV weight-number domination in shadows (brighter frame pulls the average),
//  - the clip gate zeroes clipped comparisons despite a large wn.
// All cases use robustness == 0 so the per-pixel robustness weight w == 1 and
// the only differentiator is wn, making the expected averages exact.
static void test_spatial_exposure_weighting()
{
    std::cout << "[test] spatial exposure weighting..." << std::endl;

    const uint32_t w = 8, h = 8, ch = 1;

    // --- Uniform-burst invariance: with every exposure_scale == 1.0, wn == 1
    //     everywhere, so exposure_weighted must reproduce the legacy output. ---
    {
        burstmerge::FloatImage ref = make_float_image(w, h, ch, 100.0f);
        burstmerge::FloatImage compA = make_float_image(w, h, ch, 130.0f);
        burstmerge::FloatImage compB = make_float_image(w, h, ch, 90.0f);
        std::vector<burstmerge::FloatImage> comps = {compA, compB};

        float scales[2] = {1.0f, 1.0f};
        burstmerge::SpatialMergeParams p_off{};
        p_off.robustness = 0.0f;            // -> per-pixel weight w == 1
        p_off.noise_floor = 100.0f;
        p_off.highlight_threshold = 1e6f;   // never enter highlight branch
        p_off.guide_block_size = 1;
        p_off.num_scales = 2;
        p_off.exposure_scales = scales;
        p_off.exposure_weighted = false;

        burstmerge::SpatialMergeParams p_on = p_off;
        p_on.exposure_weighted = true;      // wn == 1 -> must equal p_off

        burstmerge::FloatImage out_off = burstmerge::SpatialMerge(ref, comps, p_off);
        burstmerge::FloatImage out_on  = burstmerge::SpatialMerge(ref, comps, p_on);

        bool identical = true;
        for (size_t i = 0; i < out_off.data.size(); ++i)
            if (out_off.data[i] != out_on.data[i]) { identical = false; break; }
        CHECK(identical,
              "uniform scales: exposure_weighted=true bit-identical to false");
    }

    // --- EV weight-number domination: a brighter comparison (larger wn) pulls
    //     the shadow average toward its value. ---
    {
        // ref=100; compA is a darker frame (scale 2.0 -> wn 0.5) value 90;
        // compB is a brighter frame (scale 0.5 -> wn 2.0) value 110.
        burstmerge::FloatImage ref = make_float_image(w, h, ch, 100.0f);
        burstmerge::FloatImage compA = make_float_image(w, h, ch, 90.0f);
        burstmerge::FloatImage compB = make_float_image(w, h, ch, 110.0f);
        std::vector<burstmerge::FloatImage> comps = {compA, compB};

        float scales[2] = {2.0f, 0.5f};
        burstmerge::SpatialMergeParams p{};
        p.robustness = 0.0f;
        p.noise_floor = 100.0f;
        p.highlight_threshold = 1e6f;
        p.guide_block_size = 1;
        p.num_scales = 2;
        p.exposure_scales = scales;

        // Legacy equal-weight average: (100 + 90 + 110)/3 = 100.0
        p.exposure_weighted = false;
        burstmerge::FloatImage out_eq = burstmerge::SpatialMerge(ref, comps, p);
        CHECK(std::fabs(out_eq.data[0] - 100.0f) < 0.01f,
              "EV weighting off -> equal-weight average 100 (got " +
              std::to_string(out_eq.data[0]) + ")");

        // wn-weighted average: (100*1 + 90*0.5 + 110*2.0)/(1+0.5+2.0) ~ 104.286
        p.exposure_weighted = true;
        burstmerge::FloatImage out_wn = burstmerge::SpatialMerge(ref, comps, p);
        const float expected = (100.0f + 90.0f * 0.5f + 110.0f * 2.0f) /
                               (1.0f + 0.5f + 2.0f);
        CHECK(std::fabs(out_wn.data[0] - expected) < 0.01f,
              "EV weighting on -> wn-weighted average (got " +
              std::to_string(out_wn.data[0]) + " want " + std::to_string(expected) + ")");
        CHECK(expected > 100.0f,
              "brighter frame (wn=2) pulls shadow average above 100");
    }

    // --- Clip gate is the clamp: a clipped bright comparison contributes 0
    //     despite a large wn, so it cannot drag the highlight down. ---
    {
        // ref=950 (highlight); bright comp scale 0.0625 (wn=16) value 800,
        // whose pre-normalization value 800/0.0625 = 12800 exceeds clip_threshold.
        burstmerge::FloatImage ref = make_float_image(w, h, ch, 950.0f);
        burstmerge::FloatImage comp = make_float_image(w, h, ch, 800.0f);
        std::vector<burstmerge::FloatImage> comps = {comp};

        float scales[1] = {0.0625f};
        burstmerge::SpatialMergeParams p{};
        p.robustness = 0.0f;
        p.noise_floor = 100.0f;
        p.highlight_threshold = 1e6f;
        p.clip_threshold = 1000.0f;         // enable gate; 12800 >= 1000 -> clipped
        p.guide_block_size = 1;
        p.num_scales = 1;
        p.exposure_scales = scales;
        p.exposure_weighted = true;         // wn = 16, but clipped -> weight 0

        burstmerge::FloatImage out = burstmerge::SpatialMerge(ref, comps, p);
        // Rejected bright comp -> output == reference (950). Without the clip
        // gate it would be (950 + 800*16)/17 ~ 809, confirming rejection.
        CHECK(std::fabs(out.data[0] - 950.0f) < 0.01f,
              "clipped bright frame rejected despite wn=16 (got " +
              std::to_string(out.data[0]) + ")");
    }

    std::cout << "  Spatial exposure weighting OK" << std::endl;
}

int main()
{
    test_types();
    test_hostbuffer();
    test_devicebuffer();
    test_dng_reader();
    test_dng_writer_roundtrip();
    test_c_api();
    test_progress_callback();
    test_edge_cases();
    test_subgraph();
    test_module_params();
    test_frequency_wiener_highlights();
    test_temporal_median();
    test_temporal_median_clip_no_recovery();
    test_temporal_median_clip_with_recovery();
    test_exposure_classification();
    test_spatial_exposure_weighting();

    std::cout << "\n================================" << std::endl;
    std::cout << "Stage 0: " << g_tests << " checks, "
              << g_failed << " failed" << std::endl;
    std::cout << "================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
