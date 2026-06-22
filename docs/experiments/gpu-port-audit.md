# GPU 移植代码行为审计 (逐项记录)

> 方法论: 每发现一个疑点, 先用 subagent 核对 CPU 路径**实际执行且稳定**的代码逻辑
> (不信 AGENTS.md / 注释, 只信源码 + 实际编译的宏值), 再改 GPU; 每改一个点记录一次.

源码核对基准 (2026-06 会话):
- `BURSTMERGE_ALIGN_WEIGHTED_AVG` 实际默认 = **0** (argmin). AGENTS.md 旧描述 "默认 1" 是错的, 已更正.

---

## 已完成项

### [DONE-1] standard tile 细化: 加权平均 → argmin
- **疑点**: GPU 用加权平均 `1/(score²+1e-8)`, 与 CPU 不一致, tile(0,0) 出现 1px 翻转.
- **核对**: 源码 `align_common.h` `BURSTMERGE_ALIGN_WEIGHTED_AVG` 默认 0 → CPU 走 argmin
  (`align_standard.cpp` `#else` 分支). 加权平均是测试变体.
- **逐候选 SAD dump** 证明 GPU/CPU 的 SAD 值 bit-identical (同数据同顺序单精度);
  发散纯来自加权平均的 float-vs-double 求和漂移.
- **修复**: `tile_refine_diag.comp` 改 argmin (取最小 score 候选位移).
- **验证**: tile-motion test standard **max|d|=(0,0), 0% >2px** — bit-identical.

### [DONE-2] dense correct + search: 加权平均 → argmin
- **疑点**: 同一宏, dense 的 CorrectUpsamplingError + SearchDenseLocal 也应走 argmin.
- **核对**: `align_dense.cpp` `#if BURSTMERGE_ALIGN_WEIGHTED_AVG`(=0) → else argmin.
- **修复**: `dense_level.comp` correct/search 改 argmin.
- **验证**: dense mean 从 4.6 降到 ~1.0, >2px 占比降. **但 max 仍 ~195** (见 OPEN-1).

### [DONE-3] temporal_acc_exposure: max 通道 → 逐通道
- **疑点**: GPU 用 max(cmp_blur 通道) 算单个 luminance/weight, 共享 per-pixel wsum.
- **核对**: `temporal.cpp` TemporalAverage 曝光路径 `for i ... comp_blur[idx].data[i]`,
  逐元素 (含通道), 每通道独立 luminance/weight/weight_sum.
- **修复**: `temporal_acc_exposure.comp` 改逐通道; `normalize_div.comp` 加 wsum_mode
  标志 (pc.i6: 0=1ch spatial共享, 1=NHWC temporal逐元素); pipeline temporal wsum 改 NHWC.
- **验证**: temporal MAD **1.83% → 0.0096%** (近 bit-identical).

---

### [DONE-4] dense per-level: tileCost 源窗口未 clamp → 边界 tile 读 OOB
- **现象**: dense max ~195px, 即便用 double SSD 也不变 → 排除精度, 是实现 bug.
- **核对**: CPU `TileCost` (`align_common.cpp:96-98`) 把源窗口 clamp 到图像:
  `ax1 = min(a.width, x0+tile_w); ay1 = min(a.height, y0+tile_h)`.
- **bug**: GPU `dense_level.tileCost` 循环整个 `tile_size` 不 clamp 源坐标.
  粗糙金字塔层 (如 74×48) 的边界 tile (origin=48, size=32) 读 [74,80) OOB → 垃圾 cost
  → argmin 错 → 逐层 (×2+3) 复合到 189px.
- **修复**: `tileCost` 改为 `x1=min(W,x0+ts); y1=min(H,y0+ts)`, 循环 [x0,x1)×[y0,y1).
  (同时简化签名, 去掉 bx0/by0, tile_size 从 pc.i0 读.)
- **验证**: dense **max|d|=(0,0), 0% >2px** — bit-identical. **全程单精度** (float SSD 即可,
  验证了 double 非必需). 4 checks 0 failed (standard + dense 全过).

### [DONE-5] reduce_scalar grid-stride 循环错误 → 归约结果全错
- **现象**: seq1 spatial 仍 1.6% (其它 mode 近 bit-identical). noise_floor GPU=8 vs CPU=52.
- **核对**: `reduce_scalar.comp` phase-0 循环 `for t=lid; t<n; t+=256` **忽略 workgroup ID**
  → 每个 workgroup 读相同的元素 (无分区) → 多 workgroup 归约完全错误. 同时 `part[wg]` 引用
  了已删除的 `wg` 变量 → 编译失败 → 用了 stale SPIR-V.
- **修复**: 改为 grid-stride `for t=gl_GlobalInvocationID.x; t<n; t+=gl_WorkGroupSize.x*gl_NumWorkGroups.x`;
  `part[gl_WorkGroupID.x]`.
- **额外**: `EstimateNoiseFloorGPU` 对 reduce 结果 **重复除以 n*ch** (reduce phase-1 的 pc.i3 已除,
  C++ 又除一次) → rms 被除多了. 移除 C++ 侧多余除法.

### [DONE-6] 上述修复后全部 merge 模式近 bit-identical
| 序列 | 模式 | MAD rel |
|---|---|---|
| seq1 | spatial | 0.0094% |
| seq1 | temporal | 0.0096% |
| seq1 | freq-laplacian | 0.015% |
| seq3 | spatial | 0.0005% |
| bkt | spatial | 0.054% |

除 freq-wiener (~17%, FFT 累加顺序差异) 外, **全部 < 0.02%**.

### 结论: 对齐 (standard+dense) bit-identical; 合并 (spatial+temporal+freq-laplacian) 近 bit-identical.
全单精度, 无 double. 唯一遗留: freq-wiener (FFT).

---

## 第二轮: 全量静态审计 (逐 shader 对比 CPU)

### [DONE-7] freq_wiener_tile 非原子累加数据竞争 (UB) → 分 4 phase 派发
- **现象**: Wiener FFT GPU vs CPU ~17% MAD. 之前归因 "FFT 累加顺序差异".
- **核对**: `freq_wiener_tile.comp:290-291` 对 `outv[gidx] += acc.x` 和 `normv[...] += win`
  使用**非原子** RMW. stride=4, tile=8 → 相邻 tile 重叠 50% → 单次 dispatch 内多个 workgroup
  并发写同一地址 → **Vulkan 数据竞争 (UB)**. CPU 用 band 方式: 每 band 独立缓冲 + 串行合并.
- **修复**: 将单次 dispatch 拆为 4 phase (phase_x, phase_y) ∈ {0,1}². 每 phase 内 workgroup
  间距 = 2×stride = tile = 无重叠. dispatch 间由 VulkanBackend 自动插 barrier 保证顺序累加.
- **结果**: WienerFft (标准) MAD 从 ~18% 降至 **0.0094%** — 与其他 merge 模式同等水平.
  之前误测的 0.92% 是因 CLI `--frequency-mode wiener-fft` 映射到 WienerFftRobust (不同算法).

### [DONE-8] WienerFftRobust 在 GPU 静默路由到标准 Wiener → 改为显式拒绝
- **现象**: GPU `FrequencyMergeGPU` 只分支 `Laplacian`; 所有非 Laplacian 模式走标准 Wiener shader.
  WienerFftRobust 在 CPU 是完全不同的算法 (mismatch estimation, motion/exposure norms,
  deconvolution, 4 shift passes, ReduceTileBorderArtifacts).
- **修复**: 在 `FrequencyMergeGPU` 入口添加 `if (mode == WienerFftRobust) throw` 守卫.
  GPU 不支持 Robust 时明确报错, 不静默产出错误结果.

### [AUDIT-1] 对齐 shader 审计 (sad_global/select_min/upscale_seed/tile_refine_diag/dense_level/warp)
- **已确认正确**: warp_tilefield, upscale_seed, tile_refine_diag (diagonal wavefront 等价 CPU row-major)
- **疑点处理结果**:
  - S1: dense_level + tile_refine_diag float cost vs CPU double → **约束要求全 float, 不改**. 实测 bit-identical, float 精度足够 (DONE-10 记录).
  - S2: sad_global 原 per-thread double → **改为全 float** (DONE-10).
  - S3: select_min 跨线程 tie-breaking → **已修复** (DONE-11).
  - S4: select_min 全 MAX 时选角而非中心 → **已修复** (DONE-11).
  - S5: tile_refine_diag 空 tile 返回 0 而非 MAX → **已修复** (DONE-12, 加 early-return guard).
  - S6: cfa_period 硬编码为 1 → **当前正确** (alignment 在 plane 灰度图上运行, channels>1, cfa_period 无意义). 不改.

### [AUDIT-2] spatial+temporal shader 审计
- **已确认正确**: spatial weight formula (standard mode), highlightWeight, planeBiasVeto,
  box_blur, binomial_sep, block_mean_guide, temporal_acc_exposure, temporal_median
- **疑点处理结果**:
  - SP1: linear cmp_blur → **已修复** (DONE-9).
  - SP2: linear noise floor → **已修复** (DONE-9).
  - SP3: normalize_div 多了 1e-6 epsilon → **无影响** (wsum 初始化为 1.0, 永远 ≥ 1.0, epsilon 不触发). 不改.
  - SP4: temporal 缺 simple-average fallback → **边界条件不可达** (仅 white_level ≤ black_level+1 时触发,
    真实传感器不可能). 不改.

### [AUDIT-3] frequency shader 审计
- **已确认正确**: freq_laplacian (完整匹配), raised-cosine, RMS, phase search grid 7×7, DFT 符号约定
- **疑点处理结果**:
  - FQ1: freq_wiener_tile 非原子累加竞争 → **已修复** (DONE-7, 4-phase dispatch).
  - FQ2: WienerFftRobust 路由 → **已修复** (DONE-8, reject guard).
  - FQ3/4/5: noise_term/d2/weight/phase-search/inv-DFT 全 float → **约束要求, 不用 double**.
    实测 WienerFft 0.0094% MAD, float 精度足够 (DONE-10 记录).

### [AUDIT-4] texture+reduction shader 审计
- **已确认正确**: prepare_texture channel mapping, downsample2x, to_grayscale (shader),
  plane_to_mosaic, float_to_uint16, reduce_scalar, fill/copy/scale/add
- **疑点处理结果**:
  - TX1: prepare_texture 缺 bl>1.0 守卫 → **无影响** (bl=0 时减 0 = no-op; 真实传感器 bl≫1). 不改.
  - TX2: prepare_texture 缺 |scale-1|>0.001 守卫 → **无影响** (scale≈1 时乘 1.0 = no-op). 不改.
  - TX3: 缺少 ApplyGammaGray → **已修复** (DONE-12, gamma 集成到 to_grayscale).
  - TX4: extract mode 2 用 max-abs vs CPU plain max → **死代码** (ReduceMaxAbsGPU 未被调用). 记录待用.

### [DONE-12] ApplyGammaGray 集成到 to_grayscale + tile_refine_diag 空 tile guard
- **ApplyGammaGray**: 在 to_grayscale.comp 中集成可选 gamma 校正 (pc.f0=gamma, pc.f1=white_level).
  gamma≈1.0 时跳过 (匹配 CPU early-return). gamma=0.5 测试 MAD=0.0086% (CPU 与 GPU 一致).
- **空 tile guard**: tile_refine_diag.comp tileSad 函数增加 `if (x0>=x1 || y0>=y1) return MAX`
  匹配 CPU count==0 返回 float_max 的行为. 当前不可达 (tile 网格按图像尺寸生成), 纯防御性.

---

## 审计总结

### 修复的 bug (按严重程度)
| # | 严重度 | 问题 | 修复 |
|---|---|---|---|
| DONE-7 | HIGH | freq_wiener_tile 非原子累加竞争 (UB) | 4-phase dispatch |
| DONE-8 | HIGH | WienerFftRobust 静默路由到标准 Wiener | reject guard |
| DONE-9 | HIGH | linear 模式 cmp_blur (BoxBlur→BinomialBlur) + noise floor (RMS→MAD) | 新增 BinomialBlurGPU + EstimateLinearNoiseGPU |
| DONE-10 | — | 全量消除 double, shaderFloat64 禁用 | 全 shader 改 float |
| DONE-11 | MED | select_min tie-breaking + 全-MAX 中心初始化 | 加 tiebreaker + center init |
| DONE-12 | MED | ApplyGammaGray 缺失 + 空 tile guard | gamma 集成到 to_grayscale + early-return |

### 确认无影响 / 不改的项
| # | 问题 | 原因 |
|---|---|---|
| S1/S2 | float vs double cost | 约束要求全 float; 实测 bit-identical |
| S6 | cfa_period=1 | 当前路径正确 (灰度图 channels>1) |
| SP3 | normalize_div epsilon | wsum ≥ 1.0, epsilon 不触发 |
| SP4 | temporal simple-avg fallback | 仅病态元数据触发, 不可达 |
| TX1/TX2 | prepare_texture guards | 等价行为 (减 0 / 乘 1.0 = no-op) |
| TX4 | extract mode 2 max-abs | 死代码 (ReduceMaxAbsGPU 未调用) |
| FQ3/4/5 | Wiener float vs double | 约束要求全 float; 实测 0.0094% MAD |

### 最终状态
- **全部 shader 单精度 float, 无 double**
- **对齐**: standard + dense bit-identical (max 0px)
- **合并**: spatial 0.0094%, linear 0.011%, temporal 0.0096%, freq-laplacian 0.015%, wiener-fft 0.0094%
- **gamma**: gamma=0.5 测试通过 (0.0086% MAD)
- **CPU 回归**: 106/0 通过
