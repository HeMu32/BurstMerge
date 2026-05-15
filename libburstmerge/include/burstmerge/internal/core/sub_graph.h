#pragma once
#include <cstdint>
#include <vector>
#include "burstmerge/internal/core/image_buffer.h"

namespace burstmerge {

enum class NodeType {
    PrepareTexture,
    BuildPyramid,
    ConvertToRGBA,
    ConvertToBayer,
    Blur,
    Copy,
    Crop,
    Upsample,
    FillZeros,
    Accumulate,
    TileAlign,
    Warp,
    ColorDifference,
    ComputeMergeWeight,
    AddWeighted,
    ForwardFFT,
    FreqMerge,
    Deconvolute,
    BackwardFFT,
    ReduceArtifacts,
    CalculateRMS,
    CalculateMismatch,
    NormalizeMismatch,
    CalcHighlightsNorm,
    AddTextureHighlights,
    AddTextureExposure,
    Normalize,
    ExposureLinear,
    ExposureCurve,
    TextureMax,
    TextureMean,
    FloatToUint16,
};

struct SubGraphNode {
    NodeType type;
    std::vector<uint64_t> input_handles;
    std::vector<uint64_t> output_handles;
};

struct SubGraph {
    std::vector<SubGraphNode> nodes;
    std::vector<DeviceBuffer> shared_inputs;
    std::vector<DeviceBuffer> outputs;
};

} // namespace burstmerge
