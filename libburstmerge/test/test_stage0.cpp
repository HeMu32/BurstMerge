#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <new>

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

static int g_tests = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    g_tests++; \
    if (!(cond))
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
    CHECK(image.metadata.iso_exposure_time > 0.0f,
          "DNG iso_exposure_time > 0");

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
    CHECK_EQ(ap.tile_size, 32, "AlignParams.tile_size default");
    CHECK_EQ(ap.search_distance, 64, "AlignParams.search_distance default");

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

// ============================================================
// Main
// ============================================================
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

    std::cout << "\n================================" << std::endl;
    std::cout << "Stage 0: " << g_tests << " checks, "
              << g_failed << " failed" << std::endl;
    std::cout << "================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
