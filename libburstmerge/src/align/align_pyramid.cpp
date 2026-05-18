#include "burstmerge/internal/align/align_pyramid.h"

#include "burstmerge/internal/align/align.h"

namespace burstmerge
{

void BuildPyramid(const FloatImage& ref,
                  const FloatImage& cmp,
                  std::vector<FloatImage>& ref_pyr,
                  std::vector<FloatImage>& cmp_pyr)
{
    // Keep pyramid policy isolated here so geometry changes do not require
    // editing mode-specific estimator files.
    const uint32_t half = static_cast<uint32_t>(AlignConstants::kDefaultTileSize) / 2;

    auto level_tiles = [half](uint32_t w, uint32_t h) -> uint32_t
    {
        uint32_t nx = std::max(1u, (w + half - 1) / half) - 1;
        uint32_t ny = std::max(1u, (h + half - 1) / half) - 1;
        return nx * ny;
    };

    ref_pyr = {ref};
    cmp_pyr = {cmp};

    if (level_tiles(ref.width / 2, ref.height / 2) >=
        static_cast<uint32_t>(AlignConstants::kMinCoarseTiles))
    {
        ref_pyr.push_back(Downsample2x(ref));
        cmp_pyr.push_back(Downsample2x(cmp));

        while (ref_pyr.back().width > 1 && ref_pyr.back().height > 1)
        {
            uint32_t w = ref_pyr.back().width;
            uint32_t h = ref_pyr.back().height;
            if (level_tiles(w, h) <=
                static_cast<uint32_t>(AlignConstants::kMaxCoarseTiles))
                break;
            uint32_t w4 = w / 4;
            uint32_t h4 = h / 4;
            if (w4 == 0 || h4 == 0) break;
            if (level_tiles(w4, h4) <
                static_cast<uint32_t>(AlignConstants::kMinCoarseTiles))
                break;
            auto r_half = Downsample2x(ref_pyr.back());
            auto c_half = Downsample2x(cmp_pyr.back());
            ref_pyr.push_back(Downsample2x(r_half));
            cmp_pyr.push_back(Downsample2x(c_half));
        }
    }
}

} // namespace burstmerge
