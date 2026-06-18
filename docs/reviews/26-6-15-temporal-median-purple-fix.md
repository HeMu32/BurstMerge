# TemporalMedian 高光发紫问题修复

## 问题现象

使用 TemporalMedian (per-channel median) 合并多帧 RAW 爆光序列后, 输出 DNG
在封闭的小亮点内部出现紫色/品红色偏色.  单帧 RAW 在相同位置不出现此问题.

## 根因

问题由两个独立机制共同导致, 缺一不可:

### 机制 A: 比较帧裁切值污染 median (Frankenstein pixel)

Per-channel median 对 R / Gr / Gb / B 四个 plane **各自独立** 取中位数.
四个通道的中位数值可能来自**不同的帧**, 合成一个物理上不存在的像素 —

其 R/G/B 比例在任何一帧的实际观测中都不存在.

在高光区域, 不同帧因微小错位或噪声导致各通道值分布不同, per-channel median
拼接出的通道比例偏离真实值, demosaic + 白平衡后表现为彩色噪点/杂边
(用户描述: "五彩斑斓").

### 机制 B: median 洗掉 reference 的裁切信号

Bayer 传感器中绿色通道灵敏度更高, 在高光区**绿色先裁切** (到达 white_level),
R/B 仍有细节.

Per-channel median 用未裁切比较帧的绿色值把 reference 裁切的绿色从
white_level **拉到 white_level 以下**. RAW 转换器 (ACR/Lightroom) 不再检测到
裁切 → **不触发高光恢复** → 绿色人为偏低 → R+B > G → 白平衡放大后发紫.

单帧不发紫: 绿色在 white_level → 转换器检测到裁切 → 用未裁切的 R/B 重建 G
→ 通道平衡 → 中性.

关键观察: **发紫区域与单帧 RAW 的高光恢复区域完全重合** — 说明转换器的高光
恢复是正确处理这些像素的机制, 但 median 破坏了它所需的裁切信号.

## 修复方案

两个机制需要两个独立的修补, 均在 `TemporalMedian()` (`temporal.cpp:290`) 内:

### 修补 1: Clip detection — 拒绝裁切的比较帧

**目的**: 阻止比较帧的裁切值进入 median (消除机制 A).

**实现** (`temporal.cpp:351-363`):

对每个像素, 对每个比较帧 k:
1. 取该比较帧在此像素的 **max-across-channels**: `cmp_max = max(R, Gr, Gb, B)`
2. 还原原始曝光空间: 预计算 `clip_scaled[k] = clip_threshold × exposure_scale[k]`
3. 若 `cmp_max >= clip_scaled[k]` → 该比较帧在此像素的所有通道都排除出 median

"通道耦合" 是关键: 只要任一通道裁切, 该帧的全部通道都被排除 — 保证不会出现
R 用了 G 没用的撕裂.

阈值: `clip_threshold = (white_level - black_level) × 0.98`, 与 SpatialMerge 的
`kClipFactor` 一致 (`spatial.h:20`).

### 修补 2: Highlight bypass — 保留 reference 裁切值

**目的**: 不让 median 把 reference 的裁切值拉到 white_level 以下 (消除机制 B).

**实现** (`temporal.cpp:374-378`):

对每个像素的每个通道 c:
```
if (ref_ptr[ci] >= clip_threshold)
    out_ptr[ci] = ref_ptr[ci];   // 原样输出, 跳过 median
    continue;
```

Per-pixel, per-channel: 裁切的通道绕过 median, 未裁切的通道仍走 median 降噪.
这样绿色保留在 white_level (触发转换器恢复), R/B 继续享受多帧降噪.

## 两个修补的互补关系

| | 防止的对象 | 机制 |
|---|---|---|
| Clip detection | 比较帧的裁切值进入 median | 通道耦合拒绝整帧 |
| Highlight bypass | reference 的裁切值被拉低 | per-channel 直接输出 |

仅用 clip detection 不够: reference 自身的裁切值仍会被 median 拉低 (机制 B).
仅用 bypass 不够: 比较帧的裁切值仍会进入 median 制造 Frankenstein pixel (机制 A).

## 尝试过但废弃的方案

| 方案 | 效果 | 废弃原因 |
|---|---|---|
| 加权 median (weighted percentile) | 修复发紫 | 引入黑色伪像; 用户不允许 median 中加权 |
| 高光去饱和 (desaturation pass) | 仅对 white_level 像素有效 | 紫色出现在中高亮度非裁切区, 零效果 |
| Frame-level median (选整帧) | 完全消除偏色 | 降噪效果大不如前 (每像素仅用 1-2 帧) |
| 调低 clip 阈值 (0.85/0.93) | 扩大保护范围 | 包围曝光 (Bkt2) 场景过度拒绝 → 黑色伪像 |

## 性能

| 版本 | 1线程 | 4线程 | 8线程 |
|---|---|---|---|
| 原始 (无修补) | 1.39s | 0.44s | 0.28s |
| 最终版 (clip + bypass) | 1.67s | 0.50s | 0.30s |
| 退化 | +20% | +14% | +7% |

单线程退化主要来自 clip detection 的 max-across-channels 计算
(每像素 16 次读 + 12 次比较).  优化措施:

- 预计算 `clip_scaled[k] = clip_threshold × scale`, 消除热循环中的除法
- 粒度 `(1<<16) / channels`, 匹配原始版本的元素级调度

8 线程下退化 7% (0.02s), 实际使用中可忽略.

## 测试

`test_stage0.cpp::test_temporal_median_clip` 覆盖:
- Bypass: reference 通道值 ≥ 阈值 → 直接输出 (不经 median)
- Clip detection: 比较帧 max ≥ 阈值 → 从 median 排除 (通道耦合)
- 包围曝光: `exposure_scale` 还原原始值后判定裁切
- 非裁切像素: 正常 per-channel median

## 文件清单

- `libburstmerge/src/denoise/temporal.cpp:290-407` — TemporalMedian 实现
- `libburstmerge/include/burstmerge/internal/denoise/temporal.h:28` — 声明
- `libburstmerge/test/test_stage0.cpp::test_temporal_median_clip` — 测试
