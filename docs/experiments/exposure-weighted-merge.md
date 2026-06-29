# Exposure-Weighted Merge + Bracketing Classifier Unification

Status: **CPU path implemented**; GPU/Vulkan path deferred until the algorithm
stabilises. This note records the design and the resulting CPU/GPU divergence.

## Problem

For a constant-exposure burst, giving every frame equal weight in the merge is
correct. For an **exposure-bracketed** burst it defeats the HDR intent: the
brighter frames (cleaner shadows) should lead the dark regions, and the darker
frames (unclipped highlights) should lead the bright regions. The legacy merge
paths weighted every comparison frame purely by motion-robustness and treated
exposure only as a binary clip-rejection signal.

Separately, the "is this burst bracketed?" question was answered independently
in two places (`SelectExposureRefIndex` and `BuildAlignedComparisons`), each
with its own duplicated min/max-EV loop and its own threshold.

## Design

### 1. Unified bracketing classifier

`ExposureClassification ClassifyExposureSequence(const std::vector<RawImage>&)`
(`pipeline_frame.cpp`) scans every frame's `ev_value` **once** and applies both
thresholds:

| field                       | threshold                            | consumed by                       |
|-----------------------------|--------------------------------------|-----------------------------------|
| `is_bracketed`              | spread > `kBracketEvRatioThreshold` (1.4×, ~+0.49 EV) | reference-frame selection, merge weighting gate |
| `needs_chained_alignment`   | spread > `2^kBracketTransmissionFallbackEv` (2.0×, +1 EV) | chained ("transmission") alignment |

The orchestrator computes it once in `PipelineOrchestrator::Process` and threads
it through `SelectExposureRefIndex`, `BuildAlignedComparisons`, and the merge
param setup — replacing the two previously-duplicated spread loops. There is an
intentional **1.4×..2.0× band** where a burst is bracketed (darkest frame picked
as reference, merge weighting engaged) but still uses fixed-reference alignment.
`exposure_order` is returned pre-sorted so neither consumer re-sorts.

`IsBracketedSequence()` is a thin bool wrapper for callers (tests, CLI) that only
need the gate.

### 2. EV weight-number augmentation (CPU)

For bracketed bursts the orchestrator sets
`SpatialMergeParams::exposure_weighted` / `FrequencyMergeParams::exposure_weighted`
to `true`. Each comparison frame's contribution is then multiplied by an EV
weight number on top of the existing algorithmic weight:

```
wn_i = 1 / exposure_scales[i]      (comparison; brighter frame -> larger wn)
wn_ref = 1                         (merge/exposure reference = darkest frame)
final_comparison_weight = (existing_robustness_weight) * wn_i
```

This **augments** the existing weighting (robustness curve, Wiener shrinkage,
highlight weight, chroma veto, clip gate) — it does not replace any of it.

Per mode:

| Mode                 | change                                                                 |
|----------------------|------------------------------------------------------------------------|
| `SpatialMerge`       | `w` (and `shared_w`) multiplied by `wn[idx]` at the accumulation point |
| `FrequencyMerge::Laplacian` | low-frequency average uses `wn` (high-frequency max-abs unchanged) |
| `FrequencyMerge::WienerFft` | each comparison's spectral blend × `wn`; normalisation `1/(1+Σwn)` |
| `FrequencyMerge::WienerFftRobust` | **unchanged** — keeps its own Swift-style exposure handling |
| `TemporalAverage` / `TemporalMedian` | **unchanged** (semantics preserved)             |
| `ExpBracketAverage` | **always** uses `wn = 1/scale` + hard clip gate (assumes bracketed input) |

The clip gate (`value/scale ≥ clip_threshold → weight 0`) is preserved and acts
as the natural upper bound on `wn`: a bright frame's large `wn` cannot pollute
highlights because its clipped pixels are zeroed — this is "judge clipping by
the pre-recovery white value", since dividing by the normalisation scale recovers
the original saturation.

**No-regression invariant:** for a uniform burst every `exposure_scales[i] ≈ 1`,
so `wn ≡ 1` and every formula collapses bit-for-bit to the legacy path (verified
by `test_stage0`'s uniform-scales invariance check).

### 3. GPU / Vulkan path — implemented

The GPU mirrors the CPU weighting and chaining:

- **Merge weighting** (engaged by `exposure.is_bracketed`):
  - `spatial_acc_multi` / `spatial_acc_1ch` (per-comparison dispatch): the final
    weight is multiplied by `pc.f6` (= `1/exposure_scale` for this comparison,
    1.0 when not bracketed).
  - `freq_laplacian` (single dispatch): a `float trust[num_comp]` SSBO at
    binding 5 weights the low-frequency accumulation; high-freq max-abs is
    unchanged.
  - `freq_wiener_tile` (single dispatch ×4 phases): a `trust[num_comp]` SSBO at
    binding 4 scales each comparison's spectral blend, and the normalisation is
    precomputed on the host as `inv_stack = 1/(1+Σwn)` and passed via `pc.f2`.
- **Chained ("transmission") alignment** (engaged by
  `exposure.needs_chained_alignment`, spread > 2×): `GpuPipelineCore` processes
  frames in EV order, each aligned (via the shared `align_to_parent` primitive)
  against its EV-adjacent neighbour already warped into the reference frame
  (the neighbour's gray is rebuilt from its warped plane via `to_grayscale`).
  Mirrors the CPU `BuildAlignedComparisons` chained path; fixed-reference
  remains the path for non-chained bursts.

`wn == 1.0` (and a fixed-reference parent) is an exact no-op on the GPU, so
uniform / non-bracketed bursts are bit-identical to the legacy path (verified:
Seq CPU-vs-GPU MAD unchanged before/after the port).

**Dual-path consistency (CPU vs GPU), measured via `burstmerge_compare`:**

| sample.mode            | before GPU port | after (weighting + chained align) |
|------------------------|-----------------|-----------------------------------|
| Seq1 spatial           | 0.023           | 0.023 (unchanged — non-bracketed) |
| Bkt1 spatial-dense     | 2.36 (0.014%)   | **0.038** (MAXDIFF 2492→34)       |
| Bkt1 freq-wiener       | 0.82            | **0.030** (MAXDIFF 149→34)        |
| Bkt2 spatial           | 4.05            | **0.078**                         |
| Bkt2 freq-wiener       | 2.46            | **0.067**                         |

Bracketed divergence dropped ~30–60× and now matches the non-bracketed
baseline (~0.02–0.08 MAD, rel < 0.001%), well under the project's tolerance.

### 4. ExpBracketAverage — dedicated bracket-aware average

A fifth merge mode (`MergeAlgorithm::ExpBracketAverage`, CLI alias
`exp-bkt-average` / `expbkt-avg`) designed for bracketed input. Unlike
`TemporalAverage` (which uses its own sqrt + luminance-taper weighting derived
from a blurred luminance map), `ExpBracketAverage` uses the **same** EV weight
number (`wn = 1/exposure_scales[i]`) and the **same** clip gate
(`max-across-channels / scale ≥ clip_threshold → weight 0`) as the spatial
multichannel path. The reference frame is always seeded with weight 1.

Key properties:
- **No BoxBlur** — simpler and faster than `TemporalAverage` (which blurs every
  frame for its luminance-based weighting). CPU is ~5–7 % faster; GPU is equal.
- **Clip detection ignores highlight-recovery regions**: the max-across-channels
  test is dominated by the un-recovered R/B channels, so a recovered green value
  (extrapolated downward from clipped white) does not inflate the max. This means
  a sensor-clipped photosite is still detected via R/B even after green recovery.
- **Always uses EV weights**: `ExpBracketAverage` does not check
  `is_bracketed`. It always computes `wn = 1/scale` from the actual exposure
  data. When all frames share identical EV (`scale ≡ 1.0`), weights are
  uniform (`wn ≡ 1`). Near-bracketed sequences (EV spread < 1.4× but
  individual frames differ) receive non-uniform weights proportional to
  their actual EV difference.
- **Hard clip threshold**: unlike spatial merge's soft robustness weight (which
  gracefully degrades near clipping), the averaging algorithm uses a binary
  include/exclude decision. This means CPU-vs-GPU warp differences can flip the
  clip decision at boundary highlights, producing larger per-pixel divergence
  than spatial merge (Bkt1: 2 % differing pixels, MAD 0.044 / rel 0.004 %).
  The overall image is virtually identical; the differing pixels are
  concentrated in clipped highlight regions.

GPU shader: `expbkt_acc.comp` (binding 0=acc, 1=wsum, 2=cmp;
`pc.f0`=`clip×scale`, `pc.f1`=`scale`). Per-frame accumulate + `normalize_div`.

### 5. Dense-tile alignment fp64 precision (`BURSTMERGE_GPU_FP64`)

**Problem:** In extreme bracketing (EV spread > 6 stops), GPU dense-tile
alignment diverged catastrophically from CPU (ExBkt t=64: 100 % tiles wrong,
max 706px off). Root cause: CPU `TileCost` (align_common.cpp) accumulates
SAD/SSD using `double`; GPU `dense_level.comp` used `float`. The rounding
difference (~0.1 %) is negligible on a well-shaped cost surface, but in
extreme bracketing the SAD surface across candidate displacements is
extremely flat (large uniform regions after EV normalization). A 0.1 %
float error flips the argmin, causing tiles to lock onto wrong displacements.

**Solution — opt-in fp64 compilation:**
- `dense_level_fp64.comp`: identical to `dense_level.comp` except `tileCost`
  uses `double` accumulation (the only fp64 usage in the entire GPU pipeline).
  Pixel values are promoted to `double` before summation; everything else
  (pyramid traversal, candidate search, output) is unchanged.
- CMake option `BURSTMERGE_GPU_FP64` (default **OFF**): controls whether
  `dense_level_fp64.comp` is compiled and embedded (36 shaders ON, 35 OFF).
- Runtime: `VulkanBackend::HasFloat64()` checks `shaderFloat64` device
  support. `DenseAlignGPU` selects `"dense_level_fp64"` or `"dense_level"`.
- Non-fp64 GPUs or default builds use the float shader (graceful fallback).

**Where fp64 is used (exactly one place):**
`dense_level_fp64.comp::tileCost()` — the per-tile SAD/SSD cost summation
accumulator. No other shader or code path uses `double`.

**Why it improves alignment:** Double accumulation matches CPU `TileCost`
exactly, producing identical argmin decisions per tile. The flat cost
surface in extreme bracketing means float-vs-double rounding is the deciding
factor for which candidate displacement wins.

**Measured improvement:**

| Sample / tile | float (default) | fp64 (`BURSTMERGE_GPU_FP64=ON`) |
|---------------|-----------------|---------------------------------|
| ExBkt t=32    | 27–74px, 0.9 % >3px | 7–19px, 0.1 % >3px          |
| ExBkt t=64    | 294–706px, 100 %   | 5–12px, 0.1 % >3px          |
| ExBkt t=128   | 125–477px, 100 %   | **0px, 0 %**                 |
| Bkt1 t=32     | 37–444px, 16 %     | 12–54px, 0.6 % >3px         |
| Bkt1 t=64     | 12–38px, 0.7 %     | **0px, 0 %**                |
| Bkt1 t=128    | 4–13px, 0.5 %      | **0px, 0 %**                |

**Performance cost:** Consumer NVIDIA fp64 throughput is 1/64 of fp32.
Dense path: +2 % (tile=32) to +20 % (tile=128). Standard alignment path:
0 % (entirely float, unaffected).

**Behavior matrix:**

| Build               | GPU `shaderFloat64` | Shader dispatched           | Quality (argmax under t=32)   |
|---------------------|---------------------|-----------------------------|----------------------|
| FP64 OFF (default)  | N/A                 | `dense_level` (strided M=16) | **46 px** (from 74) |
| FP64 ON             | Yes                 | `dense_level_fp64`          | **19 px** (best)     |
| FP64 ON             | No                  | `dense_level` (strided M=16) | **46 px** (fallback) |

### 6. Strided fp32 accumulation (default dense_level.comp)

The default float shader (`dense_level.comp`) uses **M independent float
accumulators** with a pairwise merge tree to reduce sequential rounding error
by ~M×. SSD mode additionally uses `fma()` (single-rounding multiply-add).
This improves small-tile (t=32) dense alignment in extreme bracketing without
any fp64 support.

**Design:**
- Outer x-loop steps by `kStrideM`; inner `for (j=0; j<kStrideM; ++j)` has a
  constant trip count so the compiler unrolls it and keeps `acc[j]` in
  registers (a naive sequential loop with dynamic indexing spills to local
  memory and runs ~2× slower).
- After the pixel loop, a pairwise merge tree reduces the M accumulators to
  one in log₂(M) rounds.

**M=16 was chosen after benchmarking M=8 / M=12 / M=16 on ExBkt (t=32, 5 frames):**

| Variant   | max divergence | >3 px tiles | Notes                      |
|-----------|----------------|-------------|----------------------------|
| plain float | 74 px         | 0.55 %      | baseline (M=1 equivalent)  |
| M=8       | 74 px           | 0.49 %      | max unchanged — insufficient |
| M=12      | 74 px           | 0.27 %      | percentage improved, max not |
| **M=16**  | **46 px**       | **0.18 %**  | only M that reduces max    |
| fp64      | 19 px           | 0.04 %      | best (separate shader)     |

**Performance overhead (alignment stage, 5 comparison frames):**

| GPU                  | M=16 overhead | fp64 overhead |
|----------------------|---------------|---------------|
| RTX 3080             | +8–14 %       | +4–74 % (tile-dependent) |
| RTX 5060 Ti          | +26–50 %      | +74–132 %     |

fp64 cost scales worse on newer GPUs (5060 Ti fp64 throughput ≈ 1/128 vs
3080 ≈ 1/64), making the strided fp32 approach increasingly valuable.
Note: This is question-raising as that TechPowerup marks that 5060Ti did
has 1:64 FP64 perf. ratio. 

**Reverting:** Set `kStrideM = 1` in `dense_level.comp` (loop becomes
sequential, merge tree is a no-op). Requires cmake reconfigure.

**At t ≥ 64**, all variants (including plain float) produce identical tile
fields (≤12 px from chained warp propagation, not accumulation error).
The strided overhead at these tile sizes is harmless but provides no benefit.

## Tests

`test_stage0.cpp` adds:
- `test_exposure_classification` — uniform / narrow-bracket (1.4×..2.0×) / wide
  bracket (>2.0×) / no-EV regimes; verifies `is_bracketed`,
  `needs_chained_alignment`, reference selection, and `exposure_order` ordering.
- `test_spatial_exposure_weighting` — uniform-burst bit-identical invariance;
  EV weight-number domination in shadows (exact expected average); clip-gate
  rejection of a clipped bright frame despite a large `wn`.

Existing `test_stage1::CheckCenterSaturated` (bracketed highlights stay
saturated) and `test_stage3` (CPU-vs-GPU, incl. bkt1 spatial-dense now at
MAD 0.0045 / rel 2.8e-05%) continue to pass.
