#include "burstmerge/internal/align/align_pyramid.h"

#include "burstmerge/internal/align/align.h"
#include "burstmerge/internal/align/align_common.h"

namespace burstmerge
{

void BuildPyramid(const FloatImage& ref,
                  const FloatImage& cmp,
                  std::vector<FloatImage>& ref_pyr,
                  std::vector<FloatImage>& cmp_pyr,
                  int32_t tile_size)
{
    const uint32_t half = static_cast<uint32_t>(ResolveAlignTile(tile_size)) / 2;

    auto level_tiles = [half](uint32_t w, uint32_t h) -> uint32_t
    {
        if (w < half || h < half) return 0;
        uint32_t nx = (w + half - 1) / half - 1;
        uint32_t ny = (h + half - 1) / half - 1;
        return nx * ny;
    };

    ref_pyr = {ref};
    cmp_pyr = {cmp};

    const uint32_t kMinTiles = static_cast<uint32_t>(AlignConstants::kMinCoarseTiles);
    const uint32_t kMaxTiles = static_cast<uint32_t>(AlignConstants::kMaxCoarseTiles);

    if (ref.width >= half * 2 && ref.height >= half * 2 &&
        level_tiles(ref.width / 2, ref.height / 2) >= kMinTiles)
    {
        ref_pyr.push_back(Downsample2x(ref));
        cmp_pyr.push_back(Downsample2x(cmp));

        FloatImage blur_tmp, blur_result, next;

        while (true)
        {
            const auto& prev_ref = ref_pyr.back();
            const auto& prev_cmp = cmp_pyr.back();
            if (prev_ref.width < half * 2 || prev_ref.height < half * 2) break;

            uint32_t nw = prev_ref.width / 2;
            uint32_t nh = prev_ref.height / 2;
            if (nw < half || nh < half) break;

            uint32_t cur_tiles = level_tiles(prev_ref.width, prev_ref.height);
            if (cur_tiles <= kMaxTiles) break;

            uint32_t ntiles = level_tiles(nw, nh);
            if (ntiles < kMinTiles) break;

            BinomialBlur5Tap(prev_ref, blur_result, blur_tmp);
            Downsample2x(blur_result, next);
            ref_pyr.push_back(std::move(next));

            BinomialBlur5Tap(prev_cmp, blur_result, blur_tmp);
            Downsample2x(blur_result, next);
            cmp_pyr.push_back(std::move(next));
        }
    }
}

} // namespace burstmerge
