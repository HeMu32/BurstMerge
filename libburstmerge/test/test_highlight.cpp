#include "burstmerge/internal/core/pipeline_frame.h"
#include "burstmerge/internal/core/float_image.h"
#include "burstmerge/internal/io/dng_io.h"

#include <cmath>
#include <cstdint>
#include <iostream>
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

static bool Approx(float a, float b, float eps = 0.01f)
{
    return std::fabs(a - b) <= eps;
}

static burstmerge::FloatImage MakePlaneImage(uint32_t w, uint32_t h, uint32_t ch, float fill = 500.0f)
{
    burstmerge::FloatImage img;
    img.width = w;
    img.height = h;
    img.channels = ch;
    img.data.assign(static_cast<size_t>(w) * h * ch, fill);
    return img;
}

static burstmerge::RawImage MakeRawMeta(uint32_t wl, float bl,
                                         float cf0, float cf1, float cf2,
                                         uint32_t mosaic_width,
                                         const uint16_t pattern[4])
{
    burstmerge::RawImage ri;
    ri.metadata.white_level = wl;
    for (int i = 0; i < 4; ++i) ri.metadata.black_level[i] = bl;
    ri.metadata.color_factors[0] = cf0;
    ri.metadata.color_factors[1] = cf1;
    ri.metadata.color_factors[2] = cf2;
    ri.metadata.color_factors[3] = cf1;
    ri.metadata.mosaic_pattern_width = mosaic_width;
    if (pattern)
        for (int i = 0; i < 4; ++i) ri.metadata.mosaic_pattern[i] = pattern[i];
    ri.metadata.ev_value = 1.0f;
    ri.metadata.exposure_bias = 0.0f;
    return ri;
}

static void TestBayerRGGB()
{
    std::cout << "[test] Bayer RGGB highlight recovery..." << std::endl;

    const uint16_t rggb[4] = {0, 1, 1, 2};
    const float wl = 1000.0f;

    burstmerge::FloatImage img = MakePlaneImage(3, 3, 4, 500.0f);

    // Super-pixel (1,1): R=995 (clipped), G1=850 (bright), G2=500 (not bright), B=991 (clipped)
    img.At(1, 1, 0) = 995.0f;
    img.At(1, 1, 1) = 850.0f;
    img.At(1, 1, 2) = 500.0f;
    img.At(1, 1, 3) = 991.0f;
    // Right neighbor (2,1) c0 (R)
    img.At(2, 1, 0) = 995.0f;
    // Above neighbor (1,0) c3 (B)
    img.At(1, 0, 3) = 991.0f;

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    // Expected G1: ratio=0.85, extrapolated=((995+995)+(991+991))/4=993.0
    // t=0.15, weight=0.9-4.5*0.15=0.225, max(993,850)=993
    // new = 0.225*993 + 0.775*850 = 882.175
    float g1 = imgs[0].At(1, 1, 1);
    CHECK(Approx(g1, 882.175f, 0.1f),
          "G1 extrapolated to ~882 (got " + std::to_string(g1) + ")");

    // G2 should be unchanged (ratio 0.5 <= 0.8)
    CHECK(Approx(imgs[0].At(1, 1, 2), 500.0f),
          "G2 unchanged (not bright)");

    // R and B should be unchanged
    CHECK(Approx(imgs[0].At(1, 1, 0), 995.0f), "R unchanged");
    CHECK(Approx(imgs[0].At(1, 1, 3), 991.0f), "B unchanged");

    // Non-recovery super-pixels should be unchanged
    CHECK(Approx(imgs[0].At(0, 0, 0), 500.0f), "Baseline pixel unchanged");

    std::cout << "  Bayer RGGB OK" << std::endl;
}

static void TestBayerRGGB_NoBright()
{
    std::cout << "[test] Bayer RGGB no bright green..." << std::endl;

    const uint16_t rggb[4] = {0, 1, 1, 2};
    burstmerge::FloatImage img = MakePlaneImage(3, 3, 4, 500.0f);
    img.At(1, 1, 0) = 995.0f;  // R clipped
    img.At(1, 1, 1) = 700.0f;  // G1 ratio=0.7 <= 0.8 → not bright
    img.At(1, 1, 3) = 991.0f;  // B clipped

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    CHECK(Approx(imgs[0].At(1, 1, 1), 700.0f),
          "G1 unchanged when not bright (got " + std::to_string(imgs[0].At(1, 1, 1)) + ")");

    std::cout << "  No bright green OK" << std::endl;
}

static void TestBayerRGGB_NoClip()
{
    std::cout << "[test] Bayer RGGB no clipped neighbour..." << std::endl;

    const uint16_t rggb[4] = {0, 1, 1, 2};
    burstmerge::FloatImage img = MakePlaneImage(3, 3, 4, 500.0f);
    img.At(1, 1, 0) = 980.0f;  // R ratio=0.98 <= 0.99 → not clipped
    img.At(1, 1, 1) = 850.0f;  // G1 bright
    img.At(1, 1, 3) = 980.0f;  // B not clipped

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    CHECK(Approx(imgs[0].At(1, 1, 1), 850.0f),
          "G1 unchanged when no R/B clipped");

    std::cout << "  No clipped neighbour OK" << std::endl;
}

static void TestBayerBGGR_Factors()
{
    std::cout << "[test] Bayer BGGR factor assignment..." << std::endl;

    const uint16_t bggr[4] = {2, 1, 1, 0};  // c0=B, c1=G1, c2=G2, c3=R
    // Non-uniform factors to verify pattern lookup
    // factor_r = 0.4, factor_b = 0.6
    // For BGGR: c0(B) → factor 0.6, c3(R) → factor 0.4

    burstmerge::FloatImage img = MakePlaneImage(3, 3, 4, 500.0f);

    // c0=B=600 (ratio 0.6 > 0.99*0.6=0.594 → clipped), c3=R=400 (ratio 0.4 > 0.99*0.4=0.396 → clipped)
    img.At(1, 1, 0) = 600.0f;  // B
    img.At(1, 1, 1) = 850.0f;  // G1 (bright)
    img.At(1, 1, 2) = 500.0f;  // G2 (not bright)
    img.At(1, 1, 3) = 400.0f;  // R
    // Right neighbor (2,1) c0 (B) = 600
    img.At(2, 1, 0) = 600.0f;
    // Above neighbor (1,0) c3 (R) = 400
    img.At(1, 0, 3) = 400.0f;

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 0.4f, 1.0f, 0.6f, 2, bggr);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    // For G1 (c1, g_px=1, g_py=0): h_ch=0(B), v_ch=3(R)
    // factor_h = factor_for_ch[0] = 0.6 (B), factor_v = factor_for_ch[3] = 0.4 (R)
    // extrapolated = ((600+600)/0.6 + (400+400)/0.4) / 4 = (2000+2000)/4 = 1000.0
    // weight=0.225, max(1000, 850)=1000
    // new = 0.225*1000 + 0.775*850 = 883.75
    float g1 = imgs[0].At(1, 1, 1);
    CHECK(Approx(g1, 883.75f, 0.1f),
          "BGGR G1 extrapolated to ~884 (got " + std::to_string(g1) + ")");

    // If factors were swapped (wrong assignment):
    // extrapolated = ((600+600)/0.4 + (400+400)/0.6) / 4 = (3000+1333)/4 = 1083.3
    // → result would be ~962, clearly different from 884
    CHECK(!Approx(g1, 962.0f, 5.0f),
          "BGGR result differs from swapped-factor result");

    std::cout << "  Bayer BGGR factors OK" << std::endl;
}

static void TestBayerEdgePixel()
{
    std::cout << "[test] Bayer edge super-pixel..." << std::endl;

    const uint16_t rggb[4] = {0, 1, 1, 2};
    burstmerge::FloatImage img = MakePlaneImage(3, 3, 4, 500.0f);

    // Corner super-pixel (0,0): no left, no above
    img.At(0, 0, 0) = 995.0f;  // R (clipped)
    img.At(0, 0, 1) = 850.0f;  // G1 (bright)
    img.At(0, 0, 2) = 500.0f;  // G2 (not bright)
    img.At(0, 0, 3) = 991.0f;  // B (clipped)
    // Right neighbor (1,0) c0 (R) = 995
    img.At(1, 0, 0) = 995.0f;
    // No above neighbor (oy=0)

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    // G1 at (0,0): g_px=1 → right neighbor (1,0) c0 available, no left
    //              g_py=0 → no above neighbor
    // pixel_count = 3 (2 same + 1 right)
    // extrapolated = ((995+995)/1.0 + (991+0)/1.0) / 3 = 2981/3 = 993.667
    // weight=0.225, max(993.667, 850)=993.667
    // new = 0.225*993.667 + 0.775*850 = 882.325
    float g1 = imgs[0].At(0, 0, 1);
    CHECK(Approx(g1, 882.325f, 0.1f),
          "Edge G1 extrapolated with missing neighbor (got " + std::to_string(g1) + ")");

    std::cout << "  Edge super-pixel OK" << std::endl;
}

static void TestLinearRAW()
{
    std::cout << "[test] LinearRAW highlight recovery..." << std::endl;

    burstmerge::FloatImage img = MakePlaneImage(3, 3, 3, 500.0f);

    // Pixel (1,1): R=995 (clipped), G=850 (bright), B=991 (clipped)
    img.At(1, 1, 0) = 995.0f;
    img.At(1, 1, 1) = 850.0f;
    img.At(1, 1, 2) = 991.0f;

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 0, nullptr);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    // extrapolated = (995/1.0 + 991/1.0) / 2 = 993.0
    // weight=0.225, max(993, 850)=993
    // new = 0.225*993 + 0.775*850 = 882.175
    float g = imgs[0].At(1, 1, 1);
    CHECK(Approx(g, 882.175f, 0.1f),
          "LinearRAW G extrapolated to ~882 (got " + std::to_string(g) + ")");

    // R and B unchanged
    CHECK(Approx(imgs[0].At(1, 1, 0), 995.0f), "LinearRAW R unchanged");
    CHECK(Approx(imgs[0].At(1, 1, 2), 991.0f), "LinearRAW B unchanged");

    // Non-recovery pixel unchanged
    CHECK(Approx(imgs[0].At(0, 0, 1), 500.0f), "LinearRAW baseline unchanged");

    std::cout << "  LinearRAW OK" << std::endl;
}

static void TestLinearRAW_NoBright()
{
    std::cout << "[test] LinearRAW no bright green..." << std::endl;

    burstmerge::FloatImage img = MakePlaneImage(3, 3, 3, 500.0f);
    img.At(1, 1, 0) = 995.0f;
    img.At(1, 1, 1) = 700.0f;  // ratio 0.7 <= 0.8
    img.At(1, 1, 2) = 991.0f;

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 0, nullptr);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    CHECK(Approx(imgs[0].At(1, 1, 1), 700.0f),
          "LinearRAW G unchanged when not bright");

    std::cout << "  LinearRAW no bright OK" << std::endl;
}

static void TestMultiFrame()
{
    std::cout << "[test] multi-frame highlight recovery..." << std::endl;

    const uint16_t rggb[4] = {0, 1, 1, 2};

    // Two frames: ref (0) and comparison (1) with 2× exposure scale
    burstmerge::FloatImage img0 = MakePlaneImage(3, 3, 4, 500.0f);
    img0.At(1, 1, 0) = 995.0f;
    img0.At(1, 1, 1) = 850.0f;
    img0.At(1, 1, 3) = 991.0f;
    img0.At(2, 1, 0) = 995.0f;
    img0.At(1, 0, 3) = 991.0f;

    // Comparison frame: same scene but values halved (darker exposure)
    // After normalization (×2), values match ref. So the effective_range
    // should account for the scale and ratios should match.
    burstmerge::FloatImage img1 = MakePlaneImage(3, 3, 4, 250.0f);
    img1.At(1, 1, 0) = 497.5f;
    img1.At(1, 1, 1) = 425.0f;
    img1.At(1, 1, 3) = 495.5f;
    img1.At(2, 1, 0) = 497.5f;
    img1.At(1, 0, 3) = 495.5f;

    std::vector<burstmerge::FloatImage> imgs = {img0, img1};

    auto ri0 = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    ri0.metadata.ev_value = 2.0f;
    auto ri1 = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    ri1.metadata.ev_value = 1.0f;  // darker → ev_scale = 2.0

    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri0));
    raws.push_back(std::move(ri1));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    // Frame 0 (reference): same as single-frame test → G1 ≈ 882.175
    CHECK(Approx(imgs[0].At(1, 1, 1), 882.175f, 0.1f),
          "Ref frame G1 recovered");

    // Frame 1 (comparison, ev_scale=2.0):
    // effective_range = 1000 * 2.0 = 2000
    // G1 ratio = 425.0 / 2000 = 0.2125 ≤ 0.8 → not bright → unchanged
    CHECK(Approx(imgs[1].At(1, 1, 1), 425.0f),
          "Dark comp frame G1 unchanged (ratio below threshold)");

    std::cout << "  Multi-frame OK" << std::endl;
}

static void TestAllModerate()
{
    std::cout << "[test] all moderate values (no-op)..." << std::endl;

    const uint16_t rggb[4] = {0, 1, 1, 2};
    burstmerge::FloatImage img = MakePlaneImage(4, 4, 4, 300.0f);

    std::vector<burstmerge::FloatImage> imgs = {img};
    auto ri = MakeRawMeta(1000, 0.0f, 1.0f, 1.0f, 1.0f, 2, rggb);
    std::vector<burstmerge::RawImage> raws;
    raws.push_back(std::move(ri));

    burstmerge::RecoverHighlights(imgs, raws, 0);

    bool all_unchanged = true;
    for (float v : imgs[0].data)
    {
        if (!Approx(v, 300.0f)) { all_unchanged = false; break; }
    }
    CHECK(all_unchanged, "All pixels unchanged when no highlights present");

    std::cout << "  All moderate OK" << std::endl;
}

static void TestConstantsMatch()
{
    std::cout << "[test] HighlightRecoveryParams constants..." << std::endl;

    CHECK(Approx(burstmerge::HighlightRecoveryParams::kBrightRatioThreshold, 0.8f),
          "kBrightRatioThreshold == 0.8");
    CHECK(Approx(burstmerge::HighlightRecoveryParams::kNeighbourClipRatio, 0.99f),
          "kNeighbourClipRatio == 0.99");
    CHECK(Approx(burstmerge::HighlightRecoveryParams::kExtrapolationWeightBase, 0.9f),
          "kExtrapolationWeightBase == 0.9");
    CHECK(Approx(burstmerge::HighlightRecoveryParams::kExtrapolationWeightSlope, 4.5f),
          "kExtrapolationWeightSlope == 4.5");
    CHECK(Approx(burstmerge::HighlightRecoveryParams::kWeightClampRange, 0.2f),
          "kWeightClampRange == 0.2");

    std::cout << "  Constants OK" << std::endl;
}

int main()
{
    TestConstantsMatch();
    TestBayerRGGB();
    TestBayerRGGB_NoBright();
    TestBayerRGGB_NoClip();
    TestBayerBGGR_Factors();
    TestBayerEdgePixel();
    TestLinearRAW();
    TestLinearRAW_NoBright();
    TestMultiFrame();
    TestAllModerate();

    std::cout << "\n================================" << std::endl;
    std::cout << "Highlight recovery: " << g_tests << " checks, "
              << g_failed << " failed" << std::endl;
    std::cout << "================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
