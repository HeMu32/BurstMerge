#include "burstmerge/internal/align/align_pyramid.h"

#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/align/align_common.h"

namespace burstmerge
{

std::vector<FloatImage> BuildPyramidSingle(const FloatImage& img, int32_t tile_size)
{
    const uint32_t half = static_cast<uint32_t>(ResolveAlignTile(tile_size)) / 2;

    auto level_tiles = [half](uint32_t w, uint32_t h) -> uint32_t
    {
        if (w < half || h < half) return 0;
        uint32_t nx = (w + half - 1) / half - 1;
        uint32_t ny = (h + half - 1) / half - 1;
        return nx * ny;
    };

    const uint32_t kMinTiles = static_cast<uint32_t>(AlignConstants::kMinCoarseTiles);
    const uint32_t kMaxTiles = static_cast<uint32_t>(AlignConstants::kMaxCoarseTiles);

    std::vector<FloatImage> pyr = {img};

    if (img.width >= half * 2 && img.height >= half * 2 &&
        level_tiles(img.width / 2, img.height / 2) >= kMinTiles)
    {
        pyr.push_back(Downsample2x(img));

        FloatImage blur_tmp, blur_result, next;

        while (true)
        {
            const auto& prev = pyr.back();
            if (prev.width < half * 2 || prev.height < half * 2) break;

            uint32_t nw = prev.width / 2;
            uint32_t nh = prev.height / 2;
            if (nw < half || nh < half) break;

            uint32_t cur_tiles = level_tiles(prev.width, prev.height);
            if (cur_tiles <= kMaxTiles) break;

            uint32_t ntiles = level_tiles(nw, nh);
            if (ntiles < kMinTiles) break;

            BinomialBlur5Tap(prev, blur_result, blur_tmp);
            Downsample2x(blur_result, next);
            pyr.push_back(std::move(next));
        }
    }

    return pyr;
}

void BuildPyramid(const FloatImage& ref,
                  const FloatImage& cmp,
                  std::vector<FloatImage>& ref_pyr,
                  std::vector<FloatImage>& cmp_pyr,
                  int32_t tile_size)
{
    ref_pyr = BuildPyramidSingle(ref, tile_size);
    cmp_pyr = BuildPyramidSingle(cmp, tile_size);
}

} // namespace burstmerge
