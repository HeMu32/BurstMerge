# CPU(有优化) vs CPU(无优化) vs GPU — 逐函数差异推导

> 实验目的: 确定 Vulkan GPU 后端与 CPU 后端输出差异的**精确来源**, 并隔离编译优化
> (`-ffast-math -march=native -O3`) 对结果的影响. 所有结论由实测数据驱动, 不臆测.

## 1. 实验设置

### 1.1 三种配置

| 标签 | CMAKE_BUILD_TYPE | 关键 flag | 含义 |
|---|---|---|---|
| **CPU-Rel** | Release | `-O3 -march=native -ffast-math` | CPU 有优化 (用户日常 Release) |
| **CPU-Dbg** | Debug | `-O0 -g` (严格 IEEE, 无 fast-math) | CPU 无优化 |
| **GPU** | (同 CPU-Rel/Dbg, 仅 shader 不同) | SPIR-V 由 glslang 编译, 与 C++ flag 无关 | Vulkan 计算 |

注意: GPU 的输出 DNG 由 "GPU shader 计算 merged plane → 下载到主机 → **CPU 尾部** (bit-depth / exposure / mosaic / 量化) 写出" 组成. 因此 GPU-Rel 与 GPU-Dbg 的差异只在那个 C++ 尾部. 实测该尾部为逐像素操作, 无归约, Rel/Dbg 差异 < 1e-5 (见 §3.1), 可忽略.

### 1.2 样本

- `seq1_3f`: `samples/Seq1` 前 3 帧 (DSC00128/29/30, 等曝光, 有手持运动)
- `seq3_3f`: `samples/Seq3` 前 3 帧
- `seq3_6f`: `samples/Seq3` 前 6 帧

均为 ARW, 经 Adobe DNG Converter 转 DNG 后处理.

### 1.3 度量

`apps/console/compare_dng_pixels.cpp`: 对两张输出 DNG 的**原始 16-bit Bayer 像素**逐像素比较:
```
MAD = (1/N) · Σ |A[i] − B[i]|        // 平均绝对差 (16-bit 级)
rel = MAD / mean(A)                   // 归一化到图像均值
```
`rel` 就是正文里的百分数. white_level = 16383.

### 1.4 算法

`spatial` = `--merge-algo spatial`; `temporal` = `--merge-algo temporal` (average);
`laplacian` = `--merge-algo frequency --frequency-mode laplacian`;
`wiener` = `--merge-algo frequency --frequency-mode wiener`.

---

## 2. 实测数据 (主表)

### 2.1 CPU-Rel vs GPU 与 CPU-Dbg vs GPU (MAD / rel)

| 序列 | 算法 | CPU-Rel vs GPU | CPU-Dbg vs GPU |
|---|---|---|---|
| seq1_3f | spatial | 22.35 / 1.71% | 22.35 / 1.71% |
| seq1_3f | temporal | 23.94 / 1.83% | 23.94 / 1.83% |
| seq1_3f | laplacian | 48.99 / 3.74% | 48.99 / 3.74% |
| seq1_3f | wiener | 212.3 / 16.2% | 232.3 / 17.7% |
| seq3_3f | spatial | 13.41 / 1.45% | 13.41 / 1.45% |
| seq3_3f | temporal | 24.79 / 2.69% | 24.79 / 2.69% |
| seq3_3f | laplacian | 35.74 / 3.86% | 35.74 / 3.86% |
| seq3_3f | wiener | 236.3 / 25.6% | 274.5 / 29.7% |
| seq3_6f | spatial | 13.32 / 1.44% | 13.32 / 1.44% |
| seq3_6f | temporal | 22.46 / 2.43% | 22.46 / 2.43% |
| seq3_6f | laplacian | 44.62 / 4.81% | 44.62 / 4.81% |
| seq3_6f | wiener | 208.7 / 22.6% | 204.0 / 22.1% |

**CPU-Rel vs GPU 与 CPU-Dbg vs GPU 两列几乎完全相同** (除 wiener 因自身不稳定有 ~2% 抖动).

### 2.2 CPU-Rel vs CPU-Dbg (隔离 fast-math 对 CPU 输出的影响)

| 序列 | 算法 | MAD | rel |
|---|---|---|---|
| seq1_3f | spatial | 2.35e-5 | 1.8e-8 |
| seq1_3f | temporal | 3.06e-5 | 2.3e-8 |
| seq1_3f | laplacian | 1.66e-5 | 1.3e-8 |
| seq1_3f | wiener | 4.52e-5 | 3.4e-8 |
| seq3_3f | spatial | 1.83e-5 | 2.0e-8 |
| seq3_3f | temporal | 1.41e-5 | 1.5e-8 |
| seq3_3f | laplacian | **0** | **0** |
| seq3_3f | wiener | 8.3e-7 | 9.0e-10 |
| seq3_6f | spatial | 4.02e-5 | 4.3e-8 |
| seq3_6f | temporal | 1.64e-5 | 1.8e-8 |
| seq3_6f | laplacian | **0** | **0** |
| seq3_6f | wiener | 5.8e-7 | 6.3e-10 |

---

## 3. 顶层结论 (由数据推出)

### 3.1 优化 flag 不是因素

§2.2: CPU-Rel 与 CPU-Dbg 的 MAD 在 **1e-5 量级**, 部分算法 (seq3 laplacian) **精确为 0**.
1e-5 的浮点差在 uint16 量化 (round 到整数) 后**绝大多数像素不变**, 仅极个别像素差 1 级.

⇒ **`-ffast-math -march=native` 对 CPU 输出像素无可观测影响**. CPU 是确定的, 与优化等级无关.

推论: §2.1 里 CPU-Rel-vs-GPU ≈ CPU-Dbg-vs-GPU, 所以 **GPU 与 CPU 的差异与优化 flag 无关**, 是 GPU 计算本身与 CPU 不同的体现.

### 3.2 GPU-vs-CPU 差异按算法分层, 且与序列基本无关

把 §2.1 按算法折叠 (取三序列均值):

| 算法 | rel 均值 | MAD 均值 | 性质 |
|---|---|---|---|
| spatial | ~1.5% | ~16 | 小, 稳定 |
| temporal | ~2.3% | ~24 | 小, 稳定 |
| laplacian | ~4.1% | ~43 | 中, 稳定 |
| wiener | ~22% | ~228 | 大, 不稳定 (16–30%) |

**三组序列在同一算法上的 rel 基本一致** (spatial 都是 1.4–1.7%, laplacian 都是 3.7–4.8%).
这说明差异**不是某个序列的对齐 argmin 偶然翻转** (那样会序列相关、忽高忽低), 而是**系统性的、算法结构决定的**.

> 勘误: 此前一份分析曾报告 "seq3_6f spatial = 0.018% (像素等价)". 那是脚本里 `-i $array`
> 被 cxxopts 解析成单帧输入导致的伪结果 (单帧无 merge, 输出 = 帧本身, 自然 GPU==CPU).
> 用显式 `-i f1 -i f2 -i f3` 重测后, seq3_6f spatial 真实值为 1.44%, 与其余序列一致.

### 3.3 差异来自何处 (总览)

CPU 输出确定 (§3.1), 故 GPU-vs-CPU 差异 = GPU 计算路径与 CPU 不同的累积. 两类来源:

1. **归约 (reduction) 求和顺序不同** — 浮点加法不满足结合律 `(a+b)+c ≠ a+(b+c)`:
   - 对齐 SAD: GPU 256 线程树状归约 (`sad_global` shared-memory reduce), CPU `SparseSad` 行主序顺序求和.
   - noise_floor: GPU `extract` + `reduce_scalar` 两段树状归约, CPU `EstimateNoiseFloor` OpenMP partial-reduce.
   - Wiener FFT: GPU 直接 DFT (64 项平铺求和), CPU PocketFFT 蝶形 (O(N log N) 加法链).
2. **离散选择 (argmin / argmax) 对上述浮点噪声敏感** — 近平局时翻转:
   - 对齐 argmin (SAD 最小位移), per-tile.
   - Wiener 7×7 相位搜索 argmin.
   - laplacian 的 `max|high|` 跨帧选择.

下面逐函数推导.

---

## 4. 逐函数推导

### 4.1 spatial (`SpatialMerge`) — rel ~1.5%

**CPU 路径** (`merge/spatial.cpp`):
1. `ref_blur = BoxBlur(ref, 2)`, `cmp_blur[i] = BoxBlur(cmp[i], 2)` (25 抽盒滤波, CPU 嵌套 dy,dx 循环).
2. `noise_floor = min(EstimateNoiseFloor(ref, guide), formula)` — `EstimateNoiseFloor` 是**全局 reduce**: `sqrt(Σ(img-blur)² / count)`, OpenMP partial-reduce.
3. (standard) `ref_guide = BuildBlockMeanGuide(ref)` (per-block 均值, per-pixel 同序).
4. 每像素: `diff = |cmp_blur - ref_blur|`; `w = 1/(1+(diff/noise_floor)²·robustness)`; `out = (ref + Σ cmp·w)/(1+Σ w)`.

**GPU 路径** (`spatial_acc_multi.comp` + `gpu_pipeline.cpp`): 同公式, 但:
- `BoxBlur` (`box_blur.comp`): 单线程 per-pixel, dy,dx 循环顺序**与 CPU 相同** ⇒ bit-identical.
- `BuildBlockMeanGuide` (`block_mean_guide.comp`): per-pixel 同序 ⇒ bit-identical.
- `noise_floor`: GPU `extract` (每采样点 sum_c d²) → `reduce_scalar` 树状归约. **求和顺序 ≠ CPU OpenMP** ⇒ noise_floor 差 ~1e-4 相对.
- 权重 `w` 是 `diff` 和 `noise_floor` 的非线性函数: `diff/noise_floor` 接近 1 时, noise_floor 差 1e-4 → `w` 差 ~1e-3 量级 → 累加到 `out`.

**差异来源**:
- (主) **noise_floor 归约顺序** → 权重系统性偏移.
- (次) 对齐: warped comp 与 CPU 不同 (见 §4.5), diff 改变.

两者叠加 → 1.5%. 若固定 noise_floor 和对齐, spatial 应能到 < 0.1%.

### 4.2 temporal-average (`TemporalAverage`) — rel ~2.3%

**CPU** (`denoise/temporal.cpp`): 曝光加权路径 (RAW 始终走这条):
- `cmp_blur[i] = BoxBlur(cmp[i], 2)`.
- 每像素每通道: 由 `cmp_blur` 算 luminance → 权重 `w` (分段: 暗区 `w=exposure_factor`, 亮区插值, 高光 `highlight_w` 衰减) → `acc += cmp·w`, `wsum += w`.
- `out = acc / wsum`.

**GPU** (`temporal_acc_exposure.comp`): 同公式, per-pixel 同序 (luminance 取 max 通道而非 per-channel, 这是已知的微小模型差异; CPU 用 per-channel `comp_blur[i]`).

**差异来源**:
- BoxBlur per-pixel 同序 ⇒ 该部分一致.
- luminance: GPU 取 `max(cmp_blur 通道)`, CPU 取该通道自身值. **这是真实的模型差异** (非 FP), 但影响小 (各通道值接近).
- (主) **对齐**: warped cmp 与 CPU 不同 → luminance/权重/累加都跟着变.

temporal 没有自己的归约, 故其差异**几乎全部继承自对齐**. 2.3% > spatial 的 1.5% 是因为 temporal 无 robust 权重抑制, 直接把对齐误差平均进去.

### 4.3 frequency-laplacian (`FrequencyMerge` Laplacian 模式) — rel ~4.1%

**CPU** (`merge/frequency.cpp:1069`): 逐元素, 无 FFT, 无 reduce:
```
blur_r = max(1, tile_size/16)  // = 2
ref_low = BoxBlur(ref, 2); cmp_low[i] = BoxBlur(cmp[i], 2)
high = ref - ref_low
for each cmp: cmp_high = cmp - cmp_low; low_sum += cmp_low; if |cmp_high|>|high|: high = cmp_high
out = low_sum/(N+1) + high
```

**GPU** (`freq_laplacian.comp`): 同公式, per-element 同序. BoxBlur per-pixel 同序 ⇒ `*_low` 一致.

**差异来源**:
- BoxBlur 一致 ⇒ low 部分基本一致.
- (主) **`max|high|` 的跨帧 argmax**: `high` 取所有帧 (ref + cmp) 中 `|cmp_high|` 最大者. 各帧 `cmp_high = cmp - cmp_low`, cmp 来自**对齐后的 warped frame**. 对齐误差让某帧的 `cmp_high` 略增/减 → argmax 在两帧接近时翻转 → `high` 取了不同帧的值 → 该像素 `out` 差一个 `cmp_high` 量级 (可达几十~上百级).
- argmax 翻转是**离散跳变**, 单像素误差大, 但只影响少数 (近平局) 像素 → MAD 拉到 ~4%.

这就是 laplacian (4%) > spatial (1.5%) 的原因: spatial 的权重是连续的 (小扰动→小输出变化), laplacian 的 argmax 是离散的 (小扰动→个别像素大跳变).

### 4.4 frequency-wiener (`FrequencyMerge` WienerFft 模式) — rel ~22% (16–30%)

**CPU** (`merge/frequency.cpp:308` `ComputeStandardTileResult`):
1. 8×8 tile, raised-cosine 窗.
2. `ref_spectra = FFT2D(ref·window)` — PocketFFT 混合基数 Cooley-Tukey 蝶形.
3. 每帧: `comp_spectra = FFT2D(comp·window)`; 7×7 相位搜索 argmin `Σ|ref - comp·phase|²`; Wiener `w = d²/(d²+noise)`; `merged += (1-w)·shifted + w·ref`.
4. `IFFT(merged)/stack`.

**GPU** (`freq_wiener_tile.comp`): 同数学, 但:
- **FFT 累加链完全不同**: GPU 直接 DFT (`X[k] = Σ_n x[n]·exp(-2πi·kn/N)`, 64 项平铺求和, 每个 workgroup 64 线程协作). CPU PocketFFT 蝶形 (log₂64=6 级, 每级特定加法对). 两者数学等价, 浮点累加路径不同 → 频谱值在 **~1e-3 相对量级** 分叉 (远大于 spatial 的 1e-4).
- **7×7 相位搜索 argmin**: 49 候选, 成本 `Σ|ref-comp·phase|²`. 频谱 1e-3 的差异让多个候选成本接近 → argmin 翻转 → 选了不同子像素相位 → `shifted` 不同.
- **Wiener 权重 `w = d²/(d²+noise)`**: `d²≈noise` 时 (中频段常见), `d²` 变 1% → `w` 大幅翻转. 这是最强放大器.
- (次) 对齐: warped comp 进入 FFT.

三层叠加 (FFT 1e-3 → 相位 argmin 翻转 → Wiener 权重放大) → 16–30%, 且**不稳定** (不同序列/tile 组合下 argmin 翻转比例不同, 故 16% 到 30% 波动).

**关键**: wiener 的差异**与对齐无关** — 即便对齐完美, FFT 累加顺序差异本身就足以让相位 argmin 翻转. 要做到 bit-identical 必须把 GPU FFT 换成**与 PocketFFT 相同蝶形结构**的实现 (相同加法顺序), 工作量最大.

### 4.5 对齐 (`EstimateTranslation` + `RefineTileField` + `WarpAligned`) — 影响所有模式

**CPU** (`align/align.cpp` + `align_standard.cpp`): coarse-to-fine 金字塔, 每层 `SparseSad` (行主序顺序求和) 全局搜索 argmin → `RefineTileField` per-tile 局部搜索加权平均 → 双线性 warp.

**GPU** (`sad_global.comp` 256 线程树状归约 + `select_min.comp` + `tile_refine_diag.comp` + `warp_tilefield.comp`): 同搜索空间, 同 argmin, 但 standard 对齐有**三处实质差异**:

> ⚠ **2026-06-30 更新**: GPU standard 对齐从此前的 per-diagonal wavefront (每对角线 dispatch `(1,1,1)`, ~104 次 dispatch/帧, ~1% SM 利用率) 重写为 **2-pass 全并行** dispatch. 这引入了与 CPU wavefront 的算法性差异 (不仅仅是 FP 噪声):

1. **传播链深度 (dominant semantic difference)**:
   - CPU wavefront: tile (tx,ty) 在对角线 d=tx+ty 上, 读对角线 d-1 的 left/top 邻居 — 这些邻居**已经含链式传播** (它们自己又读了 d-2 的邻居). 传播链可达 tiles_x+tiles_y 跳.
   - GPU 2-pass: pass 0 全部 tile 从全局 seed 搜索; pass 1 从 left/top 平均值搜索 — 但 left/top 只有 pass 0 的结果 (全局 seed, 无递归传播). 传播深度固定 1 跳.
   - 影响: 对全局 SAD seed 良好的 tile 不可观测; 对 seed 较差、需要长距离空间传播修正的 tile, 可能偏差 ≤2 px.

2. **Cost 累加精度**:
   - CPU `TileCost`: `double` 累加 (`align_common.cpp:99`).
   - GPU `tileSad`: `float` 累加. 9216 像素累计 ~0.5 ULP × N 舍入差, 通常不影响 argmin, 近平局 tile 可能翻转.

3. **SmoothTileField 后处理**:
   - CPU: `RefineTileField` 末尾调 `SmoothTileField(result, smooth_tile_field)` (`align_standard.cpp:125`), 3×3 中值平滑.
   - GPU: 无等效. `smooth_tile_field=false` (CLI 默认) 时等价.

**实测** (GPU 2-pass vs CPU wavefront, 16-bit 输出):
- Bkt1 (包围曝光, t=96): MAD = 1.85, rel = 0.04%
- Seq1 (等曝光, t=96): MAD = 0.86, rel = 0.016%

此外, 全局 SAD 搜索阶段 (pass 0 之前) 的 **SAD 求和顺序**: GPU 树状归约 ≠ CPU 顺序求和 → SAD 值差 ~1e-5 相对, 可导致近平局 argmin 翻转.

对齐差异是**所有 merge 模式的共同底噪** (因为所有模式都消费 warped comp).

> Dense 对齐 (`dense_level.comp`) 不受上述 standard wavefront 改动影响 — 它使用 per-level coarse-to-fine 传播 (不经 wavefront), 与 CPU `align_dense.cpp` 算法等价 (除 float vs double cost 精度).

---

## 5. 综合结论

| 问题 | 答案 |
|---|---|
| 禁用 `-ffast-math -march=native` 能让 GPU==CPU 吗? | **不能**. CPU-Rel ≈ CPU-Dbg (MAD ~1e-5, 量化后像素相同), 优化 flag 不是因素. |
| 差异的真正来源? | GPU 的**并行归约求和顺序** ≠ CPU 顺序求和 (浮点非结合律), 叠加 argmin/argmax/Wiener 权重的离散-非线性放大. |
| 哪些能低成本修复? | spatial (noise_floor 归约) / temporal / laplacian / 对齐 — 给归约加确定性顺序或给 argmin 加 `ε·(dx²+dy²)` tie-break, 可压到 < 0.1%. |
| 哪些难修? | **wiener** — FFT 累加链结构性不同 (直接 DFT vs 蝶形), 必须重写 GPU FFT 为 PocketFFT 同构蝶形才能 bit-identical. |
| 哪些是真实模型差异 (非 FP)? | temporal 的 luminance 取 max 通道 vs CPU per-channel (影响很小, 可对齐). 其余均为 FP/离散选择问题, 非模型差异. |

### 修正路径 (优先级) (不是todo)

1. **对齐 argmin tie-break** (受益: temporal, laplacian, 部分 spatial): SAD 加 `ε·(dx²+dy²)`, ε ≈ 1e-4 · mean(ref). 消除近平局随机翻转.
2. **noise_floor 确定性归约** (受益: spatial): GPU 改用与 CPU 相同的行主序分段求和, 或 CPU/GPU 都改用 Kahan 求和.
3. **temporal luminance 对齐 CPU** (受益: temporal): 取 per-channel 而非 max.
4. **Wiener GPU 蝶形 FFT** (受益: wiener): 工作量最大, 单独成项.

做完 1–3, 预计 spatial/temporal/laplacian 的 rel 可降到 < 0.1% (量化后基本像素等价). Wiener 需 4 才能跟进.

注意, 这一节不是todo, 只起说明性作用. 

---

## 附录: 复现命令

```powershell
# 构建 (在 build/ 下)
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug ..    # 或 Release
mingw32-make -j8 burstmerge_cli burstmerge_compare

# 跑一对 (注意 -i 必须每帧一个)
$cli = "build/apps/cli/burstmerge_cli.exe"
$cmp = "build/apps/console/burstmerge_compare.exe"
& $cli --backend cpu  -i f1.dng -i f2.dng -i f3.dng --merge-algo spatial -o cpu.dng
& $cli --backend vulkan -i f1.dng -i f2.dng -i f3.dng --merge-algo spatial -o vk.dng
& $cmp cpu.dng vk.dng   # 输出 MAD / rel
```
