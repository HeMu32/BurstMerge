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
