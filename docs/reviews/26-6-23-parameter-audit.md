# 2026-06-23 全参数审计报告

交叉验证 C++ 移植与 Swift 参考实现 (hdr-plus-swift) 之间的参数偏差。

**范围**: 管线全部参数, 涵盖 spatial/frequency merge, align, exposure, denoise/temporal, pipeline constants, 以及 GPU 后端对应路径。

**方法**: 逐项读取 C++ 源码并与 `Reference/hdr-plus-swift/` 对照; 所有数值均经交叉验证。

---

## 0. Spatial Merge NR 修复审计 (`ComputeRobustness`)

之前报告的 `ComputeRobustness` 方向性 bug 已被修复。验证结果:

| 文件 | 当前公式 | 行号 | 状态 |
|---|---|---|---|
| `pipeline_frame.cpp` | `0.12 * 1.3^(0.5*(36 - round(nr))) - 0.4529822`, `max(0.2, result)` | 41-51 | ✅ **公式正确** — 与 Swift `spatial.swift:21-22` 完全一致 |
| `pipeline.h` | 常量 `kRobustnessRevOffset=36.0`, `kRobustnessBase=0.12`, `kRobustnessExpBase=1.3`, `kRobustnessSubtract=0.4529822`, `kRobustnessMin=0.2` | 42-47 | ✅ **常量正确** — 附带完整注释解释方向和参考值 |
| `spatial.h` | 原 `kRobustness*` 常量已移除, 注释指向 `PipelineConstants` | 21-24 | ✅ **清理完成** — 无残留死常量 |
| `pipeline.cpp` (Spatial 分支) | 调用 `ComputeRobustness(settings_.noise_reduction)`, 线性模式使用 `EstimateLinearNoise` | 619-638 | ✅ **调用正确** |

**公式验证 (pipeline_frame.cpp:43-50)**:
```
nr_int = floor(nr + 0.5)         // Swift: Int(nr + 0.5)
rev    = 0.5 * (36.0 - nr_int)   // Swift: 0.5*(36.0-Double(Int(nr+0.5)))
rob    = 0.12 * pow(1.3, rev) - 0.4529822  // Swift: 0.12*pow(1.3, rev) - 0.4529822
rob    = max(0.2, rob)           // 保守 clamp, Swift 无此项
```

**方向验证** (`pipeline.h:20-47`):
- weight 公式 `w = 1/(1+(diff/nf)^2*rob)` 中, `rob↑ → w↓ → 更少平均 → 更少降噪`
- 新公式: `nr↑ → rev↓ → rob↓ → w↑ → 更多平均 → 更多降噪` ✔
- 旧公式(`nr/13`): `nr↑ → rob↑ → w↓ → 更少降噪` ✘ (已修复)

**结论**: 修复正确, 公式方向与 Swift 匹配。唯一有意偏离: `kRobustnessMin=0.2` 的 floor 避免 Swift 中 `robustness≤0 → weight=1` 的全接受行为, 保留了少量运动拒绝能力, 属保守设计。

---

## 1. Critical — 产生错误或不可预期的输出

### 1.1 热像素校正强度硬编码

| 方面 | C++ | Swift | 行号 |
|---|---|---|---|
| `correction_strength` | `= 1.0f` (定值) | `(min(80, max(5, sum(ISO*t)/sqrt(N) * factor)) - 5) / 75` 映射到 [0,1] | C++: `temporal.cpp:119` |
| 噪声降低 23 倍率 | 无 | `nr==23 ? 0.25 : 1.0` | Swift: `texture.swift:479` |
| X-Trans 阈值倍率 | 无 (step=6) | `hot_pixel_threshold *= 1.4` | Swift: `texture.swift:516` |

**影响**: 低 ISO/大 burst 下过校正, 高 ISO/小 burst 下欠校正。X-Trans 传感器热像素检测算法错误。

**验证**: `temporal.cpp:119` — 读到 `const float correction_strength = 1.0f;` (定值, 无 ISO/曝光时间/帧数输入)。

### 1.2 X-Trans 热像素检测算法错误

| 方面 | C++ | Swift |
|---|---|---|
| 检测模式 | Bayer 8-邻域 (角+边, `step=6`) | 独立 `find_hotpixels_xtrans` 着色器, 6×6 LUT 反距离加权 |
| 边界 | 6px | 2px |

**验证**: `temporal.cpp:135` — `const uint32_t step = std::max<uint32_t>(2, cfa_period);` for X-Trans period=6 → step=6, 之后走标准 Bayer 分支。

### 1.3 曝光校正默认 Off, 与 Swift 不符

| 方面 | C++ | Swift |
|---|---|---|
| 默认值 | `ExposureMode::Off` | `LinearFullRange` (始终启用线性曝光提升) |

**验证**: `api.h:50` — `ExposureMode exposure_mode = ExposureMode::Off;`; `apps/cli/main.cpp:141` — `--exposure-mode` 默认 `"off"`。

### 1.4 `color_factor_mean` 未从管线设置

| 方面 | C++ | Swift |
|---|---|---|
| 默认值 | `1.0f` | 按 CFA 计算: Bayer `(R+2G+B)/4`, X-Trans `(8R+20G+8B)/36` |

**验证**: `exposure.h:35` — `float color_factor_mean = 1.0f;`; `pipeline.cpp:702-721` — `ExposureParams params;` 后未设 `params.color_factor_mean`。

### 1.5 高光恢复 (`add_texture_highlights`) 未移植

| 特性 | C++ | Swift |
|---|---|---|
| 绿色通道外推 | 未实现 | `texture.metal`: 阈值 `0.8`/`0.99`, 权重 `0.9 - 4.5*clamp(...)` |

**验证**: 搜索 `add_texture_highlights` / `highlight.*recovery` 在 `libburstmerge/src/` 中无匹配。

---

## 2. Major — 行为显著偏差

### 2.1 频率合并缺少包围曝光偏移

| 方面 | C++ | Swift | 行号 |
|---|---|---|---|
| 非包围曝光 offset | `26.5` | `26.5` | `frequency.h:18` |
| 包围曝光 offset | 同 (无分支) | `28.5` | `frequency.swift:50` |

**验证**: `frequency.cpp:888,1004` 和 `gpu_pipeline.cpp:411` 均无条件使用 `26.5`。

### 2.2 对齐金字塔第一级不同

| 方面 | C++ | Swift |
|---|---|---|
| Level 0 | 全分辨率原图 | 已下采样 (2× for Bayer, 6× for X-Trans) |

**验证**: `align_pyramid.cpp:24` — `pyr = {img};` 即 level 0 = 原图。

### 2.3 对齐搜索半径不同

| 方面 | C++ | Swift |
|---|---|---|
| 粗级搜索半径 | 动态: `max(3, longest >> (3 + depth - level))` (7×7 起) | 固定 `search_dist=2` (5×5) |

**验证**: `align.h:47` — `kMinSearchRadius=3`; `align.cpp` — 动态半径公式。

### 2.4 粗级 Tile Size 不同

| 方面 | C++ | Swift |
|---|---|---|
| 所有层级 | 相同 (默认 16, 下限 16) | 逐级减半, 下限 8 |

**验证**: `align.h:22` — `kMinTileSize=16`; `align_pyramid.cpp` — 各层使用相同 tile_size。

### 2.5 OOB 惩罚值不同

| 方面 | C++ | Swift |
|---|---|---|
| L1 penalty | `65504.0/px` | ≈`131008/px` |
| L2 penalty | `65504^2 ≈ 4.29e9/px` | ≈`1.72e10/px` |

**验证**: `align_common.cpp:109` — `kOBPenalty=65504.0`。

### 2.6 像素钳位上限不同

| 方面 | C++ | Swift |
|---|---|---|
| 曝光后钳位 | DNG `white_level` (如 4095) | `UINT16_MAX_VAL` (65535) |

**验证**: `exposure.cpp` — 钳位到 `white` 参数 (从 DNG metadata 读取); Swift `exposure.metal` — `clamp(..., 0.0f, float(UINT16_MAX_VAL))`。

### 2.7 线性增益 max 值来源不同

| 方面 | C++ | Swift |
|---|---|---|
| 最大值来源 | 原图 `MaxValue(image)` | 模糊后 `texture_max(blur(img, 2, 2))` |

**验证**: `exposure.cpp` — `MaxValue(image)` 从原图计算。

### 2.8 时域平均权重过渡不同

| 方面 | C++ | Swift |
|---|---|---|
| 暗部→中间调 | 分段线性, 转折点 0.25/0.99 | 连续指数公式 |
| 高光权重模糊 | 无 | 5-tap binomial blur |

**验证**: `temporal.cpp:283-294` — `if (lum < 0.25) ... else if (lum < 0.99) ...`。

### 2.9 `freq_align.cpp` 6×6 与 `frequency.h` 7×7 内部不一致

| 位置 | 值 | 用途 |
|---|---|---|
| `freq_align.cpp:82` | `kGrid = 6` (6×6, ±0.417px) | 傅里叶对齐亚像素搜索 |
| `frequency.h:13` | `kFourierSearchGrid = 7` (7×7, ±0.5px) | 频域合并相移搜索 |

**验证**: 两处代码存在, 用途不同但网格数字不同。已知问题 (见 `AGENTS.md`)。

---

## 3. Moderate — 架构差异 / C++ 独有

| # | 项目 | C++ | Swift | 说明 |
|---|---|---|---|---|
| 3.1 | **DenseTile 对齐** | 已实现 | 无 | 额外对齐模式 |
| 3.2 | **WienerFftRobust** | CPU: 实现, GPU: 抛异常 | 实现 | GPU 路径需补齐 |
| 3.3 | **SmoothTileField** | 可选 (bool) | 无 | 中值滤波后处理 |
| 3.4 | **AlignGamma** | 可选 (float) | 无 | 对齐前 gamma 校正 |
| 3.5 | **TemporalMedian** | 已实现 | 无 | 额外合并模式 (C++ 创新) |
| 3.6 | **X-Trans CFA** | TODO 占位 | 完整支持 | `float_image.h:46` |
| 3.7 | **频域稳健合并 offset** | 无 `exposure_corr1/2` | `frequency.swift:42-47` | 缺少包围曝光修正因子 |
| 3.8 | **拉普拉斯合并** | 已实现 | 无 (Swift 只有频域+空域) | C++ 独有模式 |
| 3.9 | **多输出格式** | PNG/JPEG/BMP/TIFF/DNG | DNG 仅 | 架构扩展 |
| 3.10 | **金字塔停止条件** | 瓦片计数控制 (`kMinCoarseTiles=4`,`kMaxCoarseTiles=8`) | `while (res > search_distance)` | 不同策略, 潜在不同层级数 |

---

## 4. 已验证 — 与 Swift 一致

以下参数已确认与参考实现完全一致:

| 参数 | C++ 值 | Swift 值 | 验证位置 |
|---|---|---|---|
| `kHighlightFactor` | `0.92` | `0.92` | `pipeline.h:15` |
| `kClipFactor` | `0.98` | `0.98` | `pipeline.h:16` |
| `kNoiseFormulaMul` | `4.0` | `*4.0` | `pipeline.h:17` |
| `kNoiseFloorMin` | `8.0` | `8.0` | `pipeline.h:18` |
| `kMinTileSize` | `16` | min 16 | `pipeline.h:51` |
| `kDefaultSearchDistance` | `64` | "Medium"=64 | `align.h:55` |
| `kRobustnessNormBase` | `7.5` | `7.5` | `frequency.h:19` |
| `kReadNoiseBase` | `10.0` | `10.0` | `frequency.h:20` |
| `kReadNoiseExp` | `1.6` | `1.6` | `frequency.h:21` |
| `kMismatchTargetMean` | `0.12` | `0.12` | `frequency.h:23` |
| `kMaxGain` | `16.0` | `16.0` | `exposure.h:11` |
| `kGain1Divisor` | `1.4` | `1.4` | `exposure.h:17` |
| `kWeightStopsFactor` | `0.25` | `0.25` | `exposure.h:24` |
| Wiener deconv 权重 `cw[8]` | `{0,0.02,0.04,0.08,0.04,0.08,0.04,0.02}` | 相同数组 | `frequency.cpp:808-809` |
| `kSegmentation` (Wiener 相位搜索) | `8` + 4 pass | 8 + 4 pass | `frequency.h:16` |

---

## 5. 建议修复优先级

| 优先级 | 项目 | 工作量 | 影响范围 |
|---|---|---|---|
| P0 | `correction_strength` 移植 Swift 动态公式 | ~1d | 所有 ISO/帧数组合的热像素校正质量 |
| P0 | X-Trans 热像素检测算法改写 | ~2d | X-Trans 传感器用户 |
| P1 | `color_factor_mean` 从 DNG AnalogBalance 计算并传递 | ~0.5d | Reinhard 曝光模式色偏 |
| P1 | 频率合并包围曝光 offset (`28.5`) 分支 | ~0.5d | 包围曝光序列的 Wiener 行为 |
| P1 | 默认曝光模式改为 `Linear` (匹配 Swift) | ~0.25d | 所有用户的默认输出亮度 |
| P2 | 高光恢复 `add_texture_highlights` 移植 | ~3d | 高光区域绿色通道细节 |
| P2 | `freq_align.cpp` 6×6 ↔ `frequency.h` 7×7 对齐 | ~0.25d | 数值一致性 |
| P3 | OOB penalty 对齐到 Swift 值 (×2) | ~0.1d | 对齐得分, 边际影响 |
| P3 | 铝合金字塔 level 0 全分辨率 vs 已下采样 | ~需要架构讨论 | 可能会影响对齐行为 |
| P4 | SmoothTileField / AlignGamma / TemporalMedian / DenseTile 的策略差异 | 低 | C++ 独有功能, 非语义错误 |
