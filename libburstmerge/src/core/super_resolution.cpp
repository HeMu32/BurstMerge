#include "burstmerge/internal/core/super_resolution.h"

#include "burstmerge/internal/core/demosaic.h"
#include "burstmerge/internal/core/profiler.h"
#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace burstmerge
{
namespace
{

struct Shift
{
    float x = 0.0f;
    float y = 0.0f;
};

struct ShiftField
{
    uint32_t tiles_x = 0;
    uint32_t tiles_y = 0;
    std::vector<Shift> values;
};

int ClampCoord(int value, int upper)
{
    return std::max(0, std::min(value, upper - 1));
}

float CubicWeight(float value)
{
    constexpr float a = -0.5f;
    value = std::abs(value);
    if (value <= 1.0f)
        return (a + 2.0f) * value * value * value -
               (a + 3.0f) * value * value + 1.0f;
    if (value < 2.0f)
        return a * value * value * value - 5.0f * a * value * value +
               8.0f * a * value - 4.0f * a;
    return 0.0f;
}

float SampleBilinear(const FloatImage& image, float x, float y, uint32_t channel)
{
    x = std::max(0.0f, std::min(x, static_cast<float>(image.width - 1)));
    y = std::max(0.0f, std::min(y, static_cast<float>(image.height - 1)));
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = std::min(x0 + 1, static_cast<int>(image.width) - 1);
    int y1 = std::min(y0 + 1, static_cast<int>(image.height) - 1);
    float tx = x - static_cast<float>(x0);
    float ty = y - static_cast<float>(y0);
    return (1.0f - ty) * ((1.0f - tx) * image.At(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), channel) +
                          tx * image.At(static_cast<uint32_t>(x1), static_cast<uint32_t>(y0), channel)) +
           ty * ((1.0f - tx) * image.At(static_cast<uint32_t>(x0), static_cast<uint32_t>(y1), channel) +
                 tx * image.At(static_cast<uint32_t>(x1), static_cast<uint32_t>(y1), channel));
}

float SampleBicubic(const FloatImage& image, float x, float y, uint32_t channel)
{
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    float sum = 0.0f;
    float weight_sum = 0.0f;
    for (int j = -1; j <= 2; ++j)
    {
        float wy = CubicWeight(y - static_cast<float>(y0 + j));
        int sy = ClampCoord(y0 + j, static_cast<int>(image.height));
        for (int i = -1; i <= 2; ++i)
        {
            float weight = wy * CubicWeight(x - static_cast<float>(x0 + i));
            int sx = ClampCoord(x0 + i, static_cast<int>(image.width));
            sum += image.At(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), channel) * weight;
            weight_sum += weight;
        }
    }
    return weight_sum != 0.0f ? sum / weight_sum : SampleBilinear(image, x, y, channel);
}

float SampleGray(const FloatImage& image, float x, float y)
{
    float sum = 0.0f;
    for (uint32_t channel = 0; channel < image.channels; ++channel)
        sum += SampleBilinear(image, x, y, channel);
    return sum / static_cast<float>(image.channels);
}

float SolveLocalLinear(double s0,
                       double sx,
                       double sy,
                       double sxx,
                       double sxy,
                       double syy,
                       double sz,
                       double sxz,
                       double syz,
                       float minimum,
                       float maximum)
{
    const double ridge = std::max(1.0e-8, s0 * 1.0e-3);
    double matrix[3][4] = {
        {s0, sx, sy, sz},
        {sx, sxx + ridge, sxy, sxz},
        {sy, sxy, syy + ridge, syz}
    };
    for (int column = 0; column < 3; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row)
        {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
                pivot = row;
        }
        if (std::abs(matrix[pivot][column]) < 1.0e-10)
            return static_cast<float>(sz / std::max(1.0e-12, s0));
        if (pivot != column)
        {
            for (int item = column; item < 4; ++item)
                std::swap(matrix[column][item], matrix[pivot][item]);
        }
        const double divisor = matrix[column][column];
        for (int item = column; item < 4; ++item)
            matrix[column][item] /= divisor;
        for (int row = 0; row < 3; ++row)
        {
            if (row == column) continue;
            const double factor = matrix[row][column];
            for (int item = column; item < 4; ++item)
                matrix[row][item] -= factor * matrix[column][item];
        }
    }
    return std::max(minimum, std::min(maximum, static_cast<float>(matrix[0][3])));
}

Shift SampleBaseShift(const AlignmentResult& alignment, float x, float y)
{
    const bool has_subpixel = !alignment.tile_shift_x_sub.empty() &&
                              !alignment.tile_shift_y_sub.empty() &&
                              alignment.tile_shift_x_sub.size() == alignment.tile_shift_x.size() &&
                              alignment.tile_shift_y_sub.size() == alignment.tile_shift_y.size();
    if (alignment.tile_shift_x.empty() || alignment.tile_shift_y.empty() ||
        alignment.tiles_x == 0 || alignment.tiles_y == 0 || alignment.tile_size <= 0)
    {
        return has_subpixel
            ? Shift{alignment.shift_x_sub, alignment.shift_y_sub}
            : Shift{static_cast<float>(alignment.shift_x), static_cast<float>(alignment.shift_y)};
    }

    const float spacing = static_cast<float>(alignment.tile_spacing > 0
        ? alignment.tile_spacing : alignment.tile_size);
    const float gx = x / spacing - 0.5f;
    const float gy = y / spacing - 0.5f;
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);
    auto at = [&](int ix, int iy) -> Shift
    {
        ix = ClampCoord(ix, static_cast<int>(alignment.tiles_x));
        iy = ClampCoord(iy, static_cast<int>(alignment.tiles_y));
        const size_t index = static_cast<size_t>(iy) * alignment.tiles_x +
                             static_cast<uint32_t>(ix);
        if (has_subpixel)
        {
            return {
                alignment.tile_shift_x_sub[index],
                alignment.tile_shift_y_sub[index]
            };
        }
        return {
            static_cast<float>(alignment.tile_shift_x[index]),
            static_cast<float>(alignment.tile_shift_y[index])
        };
    };
    const Shift a = at(x0, y0), b = at(x0 + 1, y0);
    const Shift c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return {
        (1.0f - ty) * ((1.0f - tx) * a.x + tx * b.x) +
            ty * ((1.0f - tx) * c.x + tx * d.x),
        (1.0f - ty) * ((1.0f - tx) * a.y + tx * b.y) +
            ty * ((1.0f - tx) * c.y + tx * d.y)
    };
}

Shift EstimateResidual(const FloatImage& reference,
                       const FloatImage& comparison,
                       uint32_t x_begin,
                       uint32_t y_begin,
                       uint32_t x_end,
                       uint32_t y_end)
{
    Shift best;
    double best_error = std::numeric_limits<double>::max();
    constexpr int steps = static_cast<int>(
        2.0f * SuperResolutionConstants::kResidualSearchRadius /
        SuperResolutionConstants::kResidualSearchStep + 0.5f);
    const uint32_t stride = SuperResolutionConstants::kResidualSampleStride;
    const int candidate_count = (steps + 1) * (steps + 1);
    const int center = (steps / 2) * (steps + 1) + steps / 2;
    for (int candidate = 0; candidate < candidate_count; ++candidate)
    {
        const int linear = candidate == 0 ? center
            : (candidate <= center ? candidate - 1 : candidate);
        const int yi = linear / (steps + 1);
        const int xi = linear % (steps + 1);
        float dy = -SuperResolutionConstants::kResidualSearchRadius +
                   static_cast<float>(yi) * SuperResolutionConstants::kResidualSearchStep;
        float dx = -SuperResolutionConstants::kResidualSearchRadius +
                   static_cast<float>(xi) * SuperResolutionConstants::kResidualSearchStep;
        double error = 0.0;
        uint64_t count = 0;
        for (uint32_t y = y_begin; y < y_end; y += stride)
        {
            for (uint32_t x = x_begin; x < x_end; x += stride)
            {
                float ref = SampleGray(reference, static_cast<float>(x), static_cast<float>(y));
                float cmp = SampleGray(comparison, static_cast<float>(x) + dx, static_cast<float>(y) + dy);
                error += std::abs(static_cast<double>(ref - cmp));
                ++count;
            }
        }
        if (count > 0) error /= static_cast<double>(count);
        if (error < best_error)
        {
            best_error = error;
            best = {dx, dy};
        }
    }
    return best;
}

ShiftField EstimateResidualField(const FloatImage& reference, const FloatImage& comparison)
{
    ShiftField field;
    const uint32_t tile = SuperResolutionConstants::kResidualTileSize;
    field.tiles_x = (reference.width + tile - 1) / tile;
    field.tiles_y = (reference.height + tile - 1) / tile;
    field.values.resize(static_cast<size_t>(field.tiles_x) * field.tiles_y);
    ParallelFor(field.values.size(), 1, [&](size_t begin, size_t end)
    {
        for (size_t index = begin; index < end; ++index)
        {
            uint32_t tx = static_cast<uint32_t>(index % field.tiles_x);
            uint32_t ty = static_cast<uint32_t>(index / field.tiles_x);
            uint32_t x0 = tx * tile;
            uint32_t y0 = ty * tile;
            uint32_t x1 = std::min(reference.width, x0 + tile);
            uint32_t y1 = std::min(reference.height, y0 + tile);
            field.values[index] = EstimateResidual(reference, comparison, x0, y0, x1, y1);
        }
    }, "superres_residual_tiles");
    return field;
}

Shift SampleShift(const ShiftField& field, float x, float y)
{
    const float tile = static_cast<float>(SuperResolutionConstants::kResidualTileSize);
    float gx = x / tile - 0.5f;
    float gy = y / tile - 0.5f;
    int x0 = static_cast<int>(std::floor(gx));
    int y0 = static_cast<int>(std::floor(gy));
    float tx = gx - static_cast<float>(x0);
    float ty = gy - static_cast<float>(y0);
    auto at = [&](int ix, int iy) -> Shift
    {
        ix = ClampCoord(ix, static_cast<int>(field.tiles_x));
        iy = ClampCoord(iy, static_cast<int>(field.tiles_y));
        return field.values[static_cast<size_t>(iy) * field.tiles_x + static_cast<uint32_t>(ix)];
    };
    Shift a = at(x0, y0), b = at(x0 + 1, y0);
    Shift c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return {
        (1.0f - ty) * ((1.0f - tx) * a.x + tx * b.x) +
            ty * ((1.0f - tx) * c.x + tx * d.x),
        (1.0f - ty) * ((1.0f - tx) * a.y + tx * b.y) +
            ty * ((1.0f - tx) * c.y + tx * d.y)
    };
}

bool IsExact(float coordinate)
{
    return std::abs(coordinate - std::round(coordinate)) <=
           SuperResolutionConstants::kExactSampleEpsilon;
}

} // namespace

FloatImage SuperResolve2xMosaic(const FloatImage& ref_plane,
                                const std::vector<FloatImage>& comp_planes,
                                const std::vector<AlignmentResult>& alignments,
                                const RawMetadata& metadata,
                                PreprocessInterpolation demosaic_method,
                                float clip_threshold)
{
    ProfileScope scope("time.pipeline.super_resolution_mosaic");
    if (ref_plane.channels != 4 || metadata.mosaic_pattern_width != 2)
        throw std::runtime_error("Super-resolution mosaic requires a 2x2 Bayer input");
    if (alignments.size() != comp_planes.size())
        throw std::runtime_error("Super-resolution mosaic alignment count mismatch");

    const uint32_t pw = ref_plane.width;   // W/2
    const uint32_t ph = ref_plane.height;  // H/2
    const uint32_t ow = metadata.width * 2;   // 2W (2x output width)
    const uint32_t oh = metadata.height * 2;  // 2H

    // Reconstruct the 2W x 2H Bayer mosaic (one channel per pixel).
    FloatImage mosaic;
    mosaic.width = ow;
    mosaic.height = oh;
    mosaic.channels = 1;
    mosaic.data.assign(static_cast<size_t>(ow) * oh, 0.0f);

    const float falloff = SuperResolutionConstants::kSampleWeightFalloff;
    AlignmentResult ref_align;  // identity (shift 0, no tiles)

    // Per-source gather: each source is (plane image, alignment). The reference
    // uses the identity alignment.
    const size_t source_count = comp_planes.size() + 1;
    std::vector<const FloatImage*> src_planes;
    std::vector<const AlignmentResult*> src_aligns;
    src_planes.push_back(&ref_plane);
    src_aligns.push_back(&ref_align);
    for (size_t k = 0; k < comp_planes.size(); ++k)
    {
        src_planes.push_back(&comp_planes[k]);
        src_aligns.push_back(&alignments[k]);
    }

    ParallelForRows(oh,
        RecommendedImageRowGrain(ow, 1, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            const float fy = static_cast<float>(y) * 0.5f;
            const int phy = static_cast<int>(y & 1);
            for (uint32_t x = 0; x < ow; ++x)
            {
                const float fx = static_cast<float>(x) * 0.5f;
                const int phx = static_cast<int>(x & 1);
                const int channel = static_cast<int>((y & 1) * 2 + (x & 1));

                double acc = 0.0;
                double wsum = 0.0;
                for (size_t s = 0; s < source_count; ++s)
                {
                    const FloatImage& plane = *src_planes[s];
                    const Shift base = SampleBaseShift(*src_aligns[s], fx, fy);
                    const float px_f = (fx - base.x - static_cast<float>(phx)) * 0.5f;
                    const float py_f = (fy - base.y - static_cast<float>(phy)) * 0.5f;
                    if (px_f < 0.0f || py_f < 0.0f ||
                        px_f >= static_cast<float>(pw) || py_f >= static_cast<float>(ph))
                    {
                        continue;
                    }
                    // Bilinear sample of this channel at the fractional plane
                    // position. (Bicubic overshoots at edges, producing
                    // Bayer-cell ringing; bilinear is monotonic and does not.)
                    const float cx = std::max(0.0f, std::min(px_f, static_cast<float>(pw - 1)));
                    const float cy = std::max(0.0f, std::min(py_f, static_cast<float>(ph - 1)));
                    const int x0 = static_cast<int>(std::floor(cx));
                    const int y0 = static_cast<int>(std::floor(cy));
                    const int x1 = std::min(x0 + 1, static_cast<int>(pw) - 1);
                    const int y1 = std::min(y0 + 1, static_cast<int>(ph) - 1);
                    const float tx = cx - static_cast<float>(x0);
                    const float ty = cy - static_cast<float>(y0);
                    const float v =
                        (1.0f - ty) * ((1.0f - tx) *
                                           plane.At(static_cast<uint32_t>(x0),
                                                    static_cast<uint32_t>(y0),
                                                    static_cast<uint32_t>(channel)) +
                                       tx * plane.At(static_cast<uint32_t>(x1),
                                                     static_cast<uint32_t>(y0),
                                                     static_cast<uint32_t>(channel))) +
                        ty * ((1.0f - tx) * plane.At(static_cast<uint32_t>(x0),
                                                     static_cast<uint32_t>(y1),
                                                     static_cast<uint32_t>(channel)) +
                              tx * plane.At(static_cast<uint32_t>(x1),
                                            static_cast<uint32_t>(y1),
                                            static_cast<uint32_t>(channel)));
                    if (clip_threshold > 0.0f && v >= clip_threshold) continue;
                    const float dx = std::abs(px_f - std::round(px_f));
                    const float dy = std::abs(py_f - std::round(py_f));
                    const float d = std::max(dx, dy);
                    // Sharp weight: only near-native samples (d ~ 0) contribute
                    // meaningfully. Averaging many slightly-misaligned samples
                    // dilutes the sharp native samples and blurs the output
                    // (the sub-pixel alignment on demosaiced data is not accurate
                    // enough to safely average). Keep each HR position dominated
                    // by the best-aligned native sample.
                    const float w = std::exp(-d * d * 26.0f);
                    acc += static_cast<double>(w) * v;
                    wsum += static_cast<double>(w);
                }
                mosaic.At(x, y, 0) = wsum > 0.0 ? static_cast<float>(acc / wsum) : 0.0f;
            }
        }
    }, "superres_mosaic_reconstruct");

    // Convert the 2W x 2H Bayer mosaic back into a W x H x 4 plane image and
    // demosaic once at 2x resolution.
    FloatImage planes;
    planes.width = ow / 2;
    planes.height = oh / 2;
    planes.channels = 4;
    planes.data.assign(static_cast<size_t>(planes.width) * planes.height * 4, 0.0f);
    for (uint32_t py = 0; py < planes.height; ++py)
    {
        for (uint32_t px = 0; px < planes.width; ++px)
        {
            for (int phy2 = 0; phy2 < 2; ++phy2)
            {
                for (int phx2 = 0; phx2 < 2; ++phx2)
                {
                    const int c = phy2 * 2 + phx2;
                    planes.At(px, py, static_cast<uint32_t>(c)) =
                        mosaic.At(2 * px + static_cast<uint32_t>(phx2),
                                  2 * py + static_cast<uint32_t>(phy2), 0);
                }
            }
        }
    }

    RawMetadata meta2;
    meta2.width = ow;
    meta2.height = oh;
    meta2.mosaic_pattern_width = metadata.mosaic_pattern_width;
    meta2.mosaic_pattern = metadata.mosaic_pattern;
    for (int i = 0; i < 4; ++i) meta2.black_level[i] = metadata.black_level[i];
    meta2.white_level = metadata.white_level;
    return DemosaicBayer(planes, meta2, demosaic_method, 1.0f);
}

FloatImage SuperResolve2x(const FloatImage& reference,
                          const std::vector<FloatImage>& comparisons,
                          const std::vector<float>& exposure_scales,
                          SuperResolutionInterpolation interpolation,
                          float clip_threshold,
                          const std::vector<FloatImage>* original_comparisons,
                          const std::vector<AlignmentResult>* alignments)
{
    ProfileScope scope("time.pipeline.super_resolution");
    if (reference.width == 0 || reference.height == 0 || reference.channels == 0)
        throw std::runtime_error("Super-resolution received an empty reference image");
    if (reference.channels > 4)
        throw std::runtime_error("Super-resolution supports at most four channels");
    for (const FloatImage& comparison : comparisons)
    {
        if (comparison.width != reference.width || comparison.height != reference.height ||
            comparison.channels != reference.channels)
            throw std::runtime_error("Super-resolution frame dimensions or channels differ");
    }

    const bool use_original_sources = original_comparisons && alignments &&
                                      original_comparisons->size() == comparisons.size() &&
                                      alignments->size() == comparisons.size();
    const std::vector<FloatImage>& residual_sources = use_original_sources
        ? *original_comparisons : comparisons;
    // A fractional (sub-pixel) base already carries the sub-pixel correction.
    // The integer-warp residual search would double-count that fraction and
    // cancel it, snapping every sample back to the integer grid (which produces
    // the soft / period-2px checkerboard output). So the residual is only used
    // when the alignment base is integer.
    auto has_fractional_base = [&](size_t i) -> bool
    {
        return use_original_sources &&
               !(*alignments)[i].tile_shift_x_sub.empty() &&
               (*alignments)[i].tile_shift_x_sub.size() == (*alignments)[i].tile_shift_x.size();
    };
    std::vector<ShiftField> shifts(residual_sources.size());
    ParallelFor(comparisons.size(), 1, [&](size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; ++i)
        {
            if (has_fractional_base(i))
            {
                shifts[i] = ShiftField{};
                continue;
            }
            if (use_original_sources)
            {
                FloatImage base_warp = WarpAligned(residual_sources[i], (*alignments)[i]);
                shifts[i] = EstimateResidualField(reference, base_warp);
            }
            else
            {
                shifts[i] = EstimateResidualField(reference, residual_sources[i]);
            }
        }
    }, "superres_residual");

    FloatImage output;
    output.width = reference.width * 2;
    output.height = reference.height * 2;
    output.channels = reference.channels;
    output.data.resize(static_cast<size_t>(output.width) * output.height * output.channels);

    ParallelForRows(output.height,
        RecommendedImageRowGrain(output.width, output.channels, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            float fy = static_cast<float>(y) * 0.5f;
            for (uint32_t x = 0; x < output.width; ++x)
            {
                float fx = static_cast<float>(x) * 0.5f;
                const bool ref_exact = IsExact(fx) && IsExact(fy);
                const float ref_weight = ref_exact
                    ? SuperResolutionConstants::kDirectSampleWeight
                    : SuperResolutionConstants::kInterpolatedSampleWeight;
                std::array<float, 4> weighted_sum{};
                std::array<float, 4> weight_sum{};
                for (uint32_t channel = 0; channel < output.channels; ++channel)
                {
                    float ref_value = (interpolation == SuperResolutionInterpolation::Bicubic
                        ? SampleBicubic(reference, fx, fy, channel)
                        : SampleBilinear(reference, fx, fy, channel));
                    weighted_sum[channel] = ref_value * ref_weight;
                    weight_sum[channel] = ref_weight;
                }

                for (size_t i = 0; i < comparisons.size(); ++i)
                {
                    float sx = fx;
                    float sy = fy;
                    if (use_original_sources)
                    {
                        const Shift base = SampleBaseShift((*alignments)[i], fx, fy);
                        if (has_fractional_base(i))
                        {
                            // Fractional base alone; no residual (see above).
                            sx = fx - base.x;
                            sy = fy - base.y;
                        }
                        else
                        {
                            const Shift residual = SampleShift(shifts[i], fx, fy);
                            sx = fx - base.x + residual.x;
                            sy = fy - base.y + residual.y;
                        }
                    }
                    else
                    {
                        const Shift residual = SampleShift(shifts[i], fx, fy);
                        sx = fx + residual.x;
                        sy = fy + residual.y;
                    }
                    bool exact = IsExact(sx) && IsExact(sy) && sx >= 0.0f && sy >= 0.0f &&
                                 sx < static_cast<float>(reference.width) &&
                                 sy < static_cast<float>(reference.height);
                    std::array<float, 4> samples{};
                    float max_value = 0.0f;
                    for (uint32_t channel = 0; channel < output.channels; ++channel)
                    {
                        samples[channel] = exact
                            ? residual_sources[i].At(static_cast<uint32_t>(std::round(sx)),
                                                 static_cast<uint32_t>(std::round(sy)), channel)
                            : (interpolation == SuperResolutionInterpolation::Bicubic
                                ? SampleBicubic(residual_sources[i], sx, sy, channel)
                                : SampleBilinear(residual_sources[i], sx, sy, channel));
                        max_value = std::max(max_value, samples[channel]);
                    }
                    float scale = i < exposure_scales.size() && exposure_scales[i] > 0.0f
                        ? exposure_scales[i] : 1.0f;
                    if (clip_threshold > 0.0f && max_value / scale >= clip_threshold) continue;
                    // Continuous phase-distance weight: w = 1/(1+falloff*d^2) with
                    // d = distance of (sx,sy) to the nearest integer grid point.
                    // Removes the hard direct/interpolated parity boundary that
                    // otherwise produces the period-2px checkerboard.
                    float dx = std::abs(sx - std::round(sx));
                    float dy = std::abs(sy - std::round(sy));
                    float d = std::max(dx, dy);
                    float phase_weight = SuperResolutionConstants::kDirectSampleWeight /
                        (1.0f + SuperResolutionConstants::kSampleWeightFalloff * d * d);
                    float sample_weight = phase_weight / scale;
                    for (uint32_t channel = 0; channel < output.channels; ++channel)
                    {
                        weighted_sum[channel] += samples[channel] * sample_weight;
                        weight_sum[channel] += sample_weight;
                    }
                }
                for (uint32_t channel = 0; channel < output.channels; ++channel)
                {
                    output.At(x, y, channel) = weighted_sum[channel] / weight_sum[channel];
                }
            }
        }
    }, "superres_reconstruct");
    return output;
}

FloatImage SuperResolve2xKernel(const FloatImage& ref_plane,
                                const std::vector<FloatImage>& comp_planes,
                                const std::vector<AlignmentResult>& alignments,
                                const RawMetadata& metadata,
                                float clip_threshold,
                                const FloatImage* reference_guide,
                                const std::vector<FloatImage>* aligned_guides)
{
    ProfileScope scope("time.pipeline.super_resolution_kernel");
    if (ref_plane.channels != 4 || metadata.mosaic_pattern_width != 2)
        throw std::runtime_error("Super-resolution kernel requires a 2x2 Bayer input");
    if (alignments.size() != comp_planes.size())
        throw std::runtime_error("Super-resolution kernel alignment count mismatch");
    const bool use_guides = reference_guide && aligned_guides &&
                            aligned_guides->size() == comp_planes.size() &&
                            reference_guide->channels >= 2;

    const uint32_t pw = ref_plane.width;
    const uint32_t ph = ref_plane.height;
    const uint32_t ow = metadata.width * 2;
    const uint32_t oh = metadata.height * 2;

    // Map each colour (0=R,1=G,2=B) to the plane channels carrying it.
    std::vector<uint8_t> color_ch[3];
    for (uint32_t c = 0; c < 4; ++c)
    {
        uint16_t col = metadata.mosaic_pattern[c];
        if (col < 3) color_ch[col].push_back(static_cast<uint8_t>(c));
    }

    const size_t source_count = comp_planes.size() + 1;
    std::vector<const FloatImage*> src_planes;
    std::vector<const AlignmentResult*> src_aligns;
    AlignmentResult ref_align;
    src_planes.push_back(&ref_plane);
    src_aligns.push_back(&ref_align);
    for (size_t k = 0; k < comp_planes.size(); ++k)
    {
        src_planes.push_back(&comp_planes[k]);
        src_aligns.push_back(&alignments[k]);
    }

    // Luminance (green) and chroma (R/B) use different kernel widths: a narrow
    // kernel on green recovers genuine sub-pixel high-frequency detail (the
    // resolution gain of SR), while the wider kernel on the sparse R/B samples
    // keeps color smooth and fringing low. This decouples perceived sharpness
    // from color cleanliness, which a uniform kernel cannot.
    constexpr double kSigmaLuma = 0.65;
    constexpr double kSigmaChroma = 1.0;
    constexpr double kInv2SigmaSqLuma = 1.0 / (2.0 * kSigmaLuma * kSigmaLuma);
    constexpr double kInv2SigmaSqChroma = 1.0 / (2.0 * kSigmaChroma * kSigmaChroma);
    const float guide_sigma = std::max(16.0f, clip_threshold * 0.015f);
    const double guide_inv = 1.0 /
        (2.0 * static_cast<double>(guide_sigma) * static_cast<double>(guide_sigma));

    FloatImage out;
    out.width = ow;
    out.height = oh;
    out.channels = 3;
    out.data.resize(static_cast<size_t>(ow) * oh * 3, 0.0f);

    ParallelForRows(oh,
        RecommendedImageRowGrain(ow, 3, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            const float fy = static_cast<float>(y) * 0.5f;
            for (uint32_t x = 0; x < ow; ++x)
            {
                const float fx = static_cast<float>(x) * 0.5f;
                const float ref_guide_value = use_guides
                    ? SampleBilinear(*reference_guide, fx, fy, 1) : 0.0f;
                for (int colour = 0; colour < 3; ++colour)
                {
                    double acc = 0.0;
                    double wsum = 0.0;
                    double sx_sum = 0.0;
                    double sy_sum = 0.0;
                    double sxx_sum = 0.0;
                    double sxy_sum = 0.0;
                    double syy_sum = 0.0;
                    double sxz_sum = 0.0;
                    double syz_sum = 0.0;
                    float sample_min = std::numeric_limits<float>::max();
                    float sample_max = std::numeric_limits<float>::lowest();
                    for (size_t s = 0; s < source_count; ++s)
                    {
                        const FloatImage& plane = *src_planes[s];
                        const Shift base = SampleBaseShift(*src_aligns[s], fx, fy);
                        double frame_weight = 1.0;
                        if (use_guides && s != 0)
                        {
                            const FloatImage& guide = (*aligned_guides)[s - 1];
                            const float guide_value = SampleBilinear(guide, fx, fy, 1);
                            const double difference = static_cast<double>(guide_value) -
                                                      static_cast<double>(ref_guide_value);
                            frame_weight = std::exp(-difference * difference * guide_inv);
                        }
                        for (uint8_t p : color_ch[colour])
                        {
                            const int phx = static_cast<int>(p & 1);
                            const int phy = static_cast<int>(p >> 1);
                            // The alignment shift `base` is the displacement to apply
                            // when resampling the comparison into the reference frame:
                            // cmp(x - base) ≈ ref(x). So the comparison's full-res
                            // position corresponding to ref position fx is (fx - base).
                            const float target_cx = fx - base.x;
                            const float target_cy = fy - base.y;
                            const float px0f = (target_cx - static_cast<float>(phx)) * 0.5f;
                            const float py0f = (target_cy - static_cast<float>(phy)) * 0.5f;
                            const int px0 = static_cast<int>(std::floor(px0f));
                            const int py0 = static_cast<int>(std::floor(py0f));
                            for (int oy = 0; oy < 2; ++oy)
                            {
                                const int py = py0 + oy;
                                if (py < 0 || py >= static_cast<int>(ph)) continue;
                                for (int ox = 0; ox < 2; ++ox)
                                {
                                    const int px = px0 + ox;
                                    if (px < 0 || px >= static_cast<int>(pw)) continue;
                                    // Distance in full-res px between the sample's
                                    // actual comparison position and the target.
                                    const double dx = static_cast<double>(2 * px + phx) - static_cast<double>(target_cx);
                                    const double dy = static_cast<double>(2 * py + phy) - static_cast<double>(target_cy);
                                    const double w = frame_weight * std::exp(
                                        -(dx * dx + dy * dy) *
                                        (colour == 1 ? kInv2SigmaSqLuma : kInv2SigmaSqChroma));
                                    if (w < 1e-4) continue;
                                    const float v = plane.At(static_cast<uint32_t>(px),
                                                             static_cast<uint32_t>(py),
                                                             p);
                                    if (clip_threshold > 0.0f && v >= clip_threshold) continue;
                                    acc += w * static_cast<double>(v);
                                    wsum += w;
                                    sx_sum += w * dx;
                                    sy_sum += w * dy;
                                    sxx_sum += w * dx * dx;
                                    sxy_sum += w * dx * dy;
                                    syy_sum += w * dy * dy;
                                    sxz_sum += w * dx * static_cast<double>(v);
                                    syz_sum += w * dy * static_cast<double>(v);
                                    sample_min = std::min(sample_min, v);
                                    sample_max = std::max(sample_max, v);
                                }
                            }
                        }
                    }
                    if (wsum > 1e-12)
                    {
                        const float mean = static_cast<float>(acc / wsum);
                        const float value = SolveLocalLinear(
                            wsum, sx_sum, sy_sum, sxx_sum, sxy_sum, syy_sum,
                            acc, sxz_sum, syz_sum, sample_min, sample_max);
                        out.At(x, y, static_cast<uint32_t>(colour)) =
                            std::isfinite(value) ? value : mean;
                    }
                    else
                    {
                        out.At(x, y, static_cast<uint32_t>(colour)) = 0.0f;
                    }
                }
            }
        }
    }, "superres_kernel");

    FloatImage result = out;
    constexpr float kChromaCenter = 0.0f;
    constexpr float kLumaSharpen = 1.75f;
    constexpr float kBinomial[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    const float chroma_range = std::max(16.0f, clip_threshold * 0.01f);
    const double chroma_range_inv = 1.0 /
        (2.0 * static_cast<double>(chroma_range) * static_cast<double>(chroma_range));
    ParallelForRows(oh,
        RecommendedImageRowGrain(ow, 3, kRowGrainMinPixels, kRowGrainMinRows),
        [&](uint32_t y_begin, uint32_t y_end)
    {
        for (uint32_t y = y_begin; y < y_end; ++y)
        {
            for (uint32_t x = 0; x < ow; ++x)
            {
                const float center_g = out.At(x, y, 1);
                const float center_r_diff = out.At(x, y, 0) - out.At(x, y, 1);
                const float center_b_diff = out.At(x, y, 2) - out.At(x, y, 1);
                float local_min = center_g;
                float local_max = center_g;
                float g_blur = 0.0f;
                double r_sum = 0.0;
                double b_sum = 0.0;
                double wsum = 0.0;
                for (int oy = -2; oy <= 2; ++oy)
                {
                    const uint32_t sy = static_cast<uint32_t>(ClampCoord(
                        static_cast<int>(y) + oy, static_cast<int>(oh)));
                    for (int ox = -2; ox <= 2; ++ox)
                    {
                        const uint32_t sx = static_cast<uint32_t>(ClampCoord(
                            static_cast<int>(x) + ox, static_cast<int>(ow)));
                        const float guide_diff = out.At(sx, sy, 1) - center_g;
                        const double range_weight = std::exp(
                            -static_cast<double>(guide_diff * guide_diff) * chroma_range_inv);
                        const double weight = static_cast<double>(
                            kBinomial[oy + 2] * kBinomial[ox + 2]) * range_weight;
                        const float source_g = out.At(sx, sy, 1);
                        g_blur += static_cast<float>(weight) * source_g;
                        if (std::abs(ox) <= 1 && std::abs(oy) <= 1)
                        {
                            local_min = std::min(local_min, source_g);
                            local_max = std::max(local_max, source_g);
                        }
                        r_sum += weight * static_cast<double>(out.At(sx, sy, 0) - source_g);
                        b_sum += weight * static_cast<double>(out.At(sx, sy, 2) - source_g);
                        wsum += weight;
                    }
                }
                const float normalized_blur = wsum > 1e-12
                    ? g_blur / static_cast<float>(wsum) : center_g;
                const float sharpened_g = std::max(local_min, std::min(local_max,
                    center_g + kLumaSharpen * (center_g - normalized_blur)));
                const float r_diff = wsum > 1e-12
                    ? kChromaCenter * center_r_diff + (1.0f - kChromaCenter) *
                        static_cast<float>(r_sum / wsum)
                    : center_r_diff;
                const float b_diff = wsum > 1e-12
                    ? kChromaCenter * center_b_diff + (1.0f - kChromaCenter) *
                        static_cast<float>(b_sum / wsum)
                    : center_b_diff;
                result.At(x, y, 0) = std::max(0.0f, sharpened_g + r_diff);
                result.At(x, y, 1) = std::max(0.0f, sharpened_g);
                result.At(x, y, 2) = std::max(0.0f, sharpened_g + b_diff);
            }
        }
    }, "superres_kernel_chroma");
    return result;
}

} // namespace burstmerge
