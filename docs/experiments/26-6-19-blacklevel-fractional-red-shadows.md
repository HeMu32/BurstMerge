# BlackLevel 分数表示导致的暗部发红问题

## 问题现象

在不同输出位深下, 合并后的 DNG 在 ACR / Lightroom 中补偿 +5EV 后, 深度阴影
区域出现红色色偏. 问题的严重程度和触发条件与位深强相关:

| 输出位深 | 现象 |
|----------|------|
| 8-bit    | 严重发红, 所有源都出现 |
| 10-bit   | 严重发红, 所有源都出现 |
| 12-bit   | 轻微发红, 仅 14-bit 源 (sensor_white=16383) 出现; 12-bit 源 (sensor_white=16380) 正常 |
| 14-bit   | 无问题 |
| 16-bit   | 无问题 |

8/10-bit 的问题在 2026-05-25 的提交 `0ba7f38` 中用 "零黑位" 方案修复.
12-bit 的问题在 2026-06-19 的提交 `bdd0de5` 中用 "BL 舍入" 方案修复.

本文档记录两次修复的根因分析, 并解释为什么它们是两个相关但独立的 ACR bug.


---


## 根因: ACR/Lightroom 的 BlackLevel 渲染缺陷

两次问题共享同一个症状 (暗部发红), 但由 ACR 内部两个不同的精度/逻辑缺陷
触发. 两者的边界由 **WhiteLevel 量级** 和 **BlackLevel 是否为整数** 共同决定.

### Bug A: 低 WhiteLevel + 非零 BlackLevel (8/10-bit)

当 WhiteLevel 很低 (实测 ≤ 1023, 即 ≤ 10-bit) 且 BlackLevel 非零时, ACR
的黑位减法本身存在缺陷 — **任何非零 BL 都会触发**, 与 BL 是否为整数无关.

在此 WL 范围下, 哪怕 BL 是干净的整数值 (例如 8 或 32), 暗部仍然会发红.
原因是 ACR 在低 WL 时的 BL 减法路径精度不足, 在深度阴影区域产生通道间
不一致的残差, 经 demosaic + 白平衡放大后表现为红色色偏.

### Bug B: 分数 BlackLevel (12-bit)

当 WhiteLevel 处于中等范围 (实测 4095 = 12-bit) 时, ACR 能正确处理**整数
BlackLevel**, 但对**分数 BlackLevel** 存在精度问题.

当 sensor_white 不能被 target_white 整除地映射到 BL 时, `ref_bl * bit_scale`
产生分数值 (如 127.976). 该分数 BL 经 DNG SDK 存储为有理数 (32762/256 =
127.9765625), ACR 读取后在暗部减法中引入亚 ADU 级的通道偏差, 经 +5EV
(×32) 放大后变为可见的红色色偏.

### 不触发的水位

当 WhiteLevel ≥ 16383 (14-bit 及以上) 时, ACR 的内部精度足以正确处理任何
BL 值, 包括分数 BL. 证据: LongUnder2 14-bit 输出的 BL=512.0938 (分数),
但完全正常.

### 两种 Bug 的边界

| WhiteLevel | 整数 BL | 分数 BL |
|------------|---------|---------|
| ≤ 1023 (8/10-bit) | **Bug A: 发红** | **Bug A: 发红** |
| 4095 (12-bit) | OK | **Bug B: 发红** |
| ≥ 16383 (14/16-bit) | OK | OK |


---


## 数据: 各位深 × 各 sensor_white 的 BlackLevel 值

下表计算了 `BL = 512 × (target_white / sensor_white)` 在不同组合下的精确值:

### sensor_white = 16383 (14-bit 源, 如 Sony ILCE-7RM5 Lossless RAW 2)

| bit_depth | bit_scale   | scaled_bl      | BL 性质 | 结果 |
|-----------|-------------|----------------|---------|------|
| 8         | 0.01556491  | 7.9692         | 分数    | Bug A: 发红 |
| 10        | 0.06244278  | 31.9707        | 分数    | Bug A: 发红 |
| 12        | 0.24995422  | 127.9766       | 分数    | **Bug B: 发红** |
| 14        | 1.00000000  | 512.0000       | 整数    | OK |
| 16        | 4.00018312  | 2048.0938      | 分数    | OK (WL 够高) |

### sensor_white = 16380 (12-bit 源被 Adobe DNG Converter 升采样)

| bit_depth | bit_scale   | scaled_bl      | BL 性质 | 结果 |
|-----------|-------------|----------------|---------|------|
| 8         | 0.01556777  | 7.9707         | 分数    | Bug A: 发红 |
| 10        | 0.06245421  | 31.9766        | 分数    | Bug A: 发红 |
| 12        | 0.25000000  | 128.0000       | **整数** | OK |
| 14        | 1.00018315  | 512.0938       | 分数    | OK (WL 够高) |
| 16        | 4.00091575  | 2048.4689      | 分数    | OK (WL 够高) |

关键观察:

1. **8/10-bit 的 BL 永远是分数**, 无论 sensor_white 是多少 — 所以 8/10-bit
   永远触发 Bug A, 零黑位是唯一出路.

2. **12-bit 的 BL 是否分数取决于 sensor_white**: 16383 → 127.976 (分数),
   16380 → 128.000 (整数). 这解释了为什么相同参数下, 不同格式的源表现不同.

3. **14/16-bit 即使 BL 是分数也不受影响** — WL 足够高, ACR 的精度足够.


---


## 源格式差异的背景

问题的发现源于两个同机型 (Sony ILCE-7RM5) 的连拍序列, 拓展名均为 .ARW,
但内部格式不同:

| 序列 | Sony Raw File Type | BitsPerSample | Compression | DNG 转换后 |
|------|-------------------|---------------|-------------|-----------|
| LongUnder4 | Lossless Compressed RAW 2 | 14 | JPEG | WL=16383 |
| LongUnder2 | Compressed RAW | 12 | Sony ARW Compressed | WL=16380 |

Adobe DNG Converter 对 12-bit 源做了升采样: 原始 12-bit 数据 (0-4095) 被乘以
~4, 映射到 14-bit 容器 (0-16380). 这使得两个序列转换后的 DNG 在元数据层面
几乎一致 (BL=512, WL≈16383), 但 sensor_white 的微小差异 (16383 vs 16380)
导致 12-bit 输出时 BL 一个是分数 (127.976), 一个是整数 (128.000).


---


## 修复 1: 零黑位方案 (8/10-bit, 提交 0ba7f38)

### 思路

对 ≤ 10-bit 输出, 将 BlackLevel 强制设为 0, 把黑位偏移完全烘焙进像素数据.
DNG 的 BL 标签为 [0,0,0,0], 解码器无需做任何黑位减法, 从而绕过 Bug A.

### 实现 (`pipeline.cpp`)

```cpp
bool use_zero_black = (settings_.bit_depth <= 10);
```

当 `use_zero_black = true` 时:

1. **像素数据**: 不还原黑位 (NormalizeFrames 已减去 BL), 仅做位深缩放.
2. **DNG 元数据**: BlackLevel = 0, WhiteLevel = (sensor_white - ref_bl) × bit_scale.
3. **曝光校正**: 传入 BL=0 和缩放后的 WL.
4. **逐通道 delta**: 跳过 (零黑位路径已均匀烘焙偏移).

### 效果

- 8/10-bit 暗部发红完全消除.
- WhiteLevel 从满量程降低到 (sensor_white - ref_bl) × bit_scale, 损失约 3%
  的数值范围, 但有效动态范围不变 (像素数据本身就是 black-subtracted).
- 对 RAW 转换器无副作用: BL=0 是合法且常见的 DNG 表示.


---


## 修复 2: BlackLevel 舍入 (12-bit, 提交 bdd0de5)

### 思路

对 > 10-bit 输出, 保留标准 DNG 表示 (BL 非零), 但确保 BL 始终为整数.
像素数据已经由 `FloatImageToUint16HostBuffer` 的 `lround` 量化为整数, 所以
将元数据 BL 舍入到同一整数值即可保持一致性, 不产生可见的像素变化.

### 实现 (`pipeline.cpp`)

三处改动, 均将 `ref_bl * bit_scale` 替换为 `std::round(ref_bl * bit_scale)`:

1. **像素数据还原** (scaled_bl 加回):
   ```cpp
   float scaled_bl = std::round(ref_bl * bit_scale);  // was: ref_bl * bit_scale
   ```

2. **逐通道 delta**:
   ```cpp
   delta[i] = std::round((bl_ch[i] - ref_bl) * bit_scale);  // was: (bl_ch[i] - ref_bl) * bit_scale
   ```

3. **DNG 元数据 BL**:
   ```cpp
   output.metadata.black_level[i] =
       std::round(output.metadata.black_level[i] * bit_scale);  // was: *= bit_scale
   ```

### 效果

- LongUnder4 12-bit 输出的 BL 从 127.9765625 (32762/256) 变为 128 (32768/256).
- 像素数据无可见变化 (lround 量化主导, 0.024 ADU 的差异远小于 0.5 的舍入阈值).
- 14-bit / 16-bit 输出不受影响 (其 BL 本就是整数或 WL 足够高).
- Bug B 消除.

### 局限

此修复**不能**用于 8/10-bit. 在 WL ≤ 1023 时, ACR 的 Bug A 对任何非零 BL
都会触发, 无论整数还是分数. 零黑位方案在低 WL 范围内是唯一有效的解法.


---


## 两种修复的对比

| 方面 | 零黑位 (8/10-bit) | BL 舍入 (12-bit) |
|------|-------------------|------------------|
| 机制 | BL=0, 烘焙偏移进像素 | BL=round(...), 保持非零 |
| 改动量 | 大 (条件分支 + 元数据/像素/曝光/逐通道) | 小 (3 处 std::round) |
| DNG 表示 | 非标准 (BL=0, WL 降低) | 标准 (BL 非零, WL 满量程) |
| WhiteLevel | (sensor_white - BL) × bit_scale | target_white (满量程) |
| 适用范围 | WL ≤ 4095 (Bug A + Bug B) | 仅 Bug B (WL=4095 + 分数 BL) |
| 对 8/10-bit | 有效 | **无效** (整数 BL 仍触发 Bug A) |
| 对 12-bit | 有效 | 有效 |
| 对 14/16-bit | 不需要 | 不需要 |

零黑位方案是**超集** — 它同时消除 Bug A 和 Bug B. 如果将阈值从 `<= 10`
扩展到 `<= 12`, 可以统一用零黑位覆盖所有受影响的位深, 但代价是 12-bit 输出
的 WhiteLevel 从 4095 降到 ~3967. BL 舍入方案是**精确补丁** — 仅修复 12-bit
的 Bug B, 保留标准 DNG 表示和满量程 WL.


---


## 关键文件与行号

| 位置 | 内容 |
|------|------|
| `pipeline.cpp:593` | `use_zero_black` 阈值定义 (当前 `<= 10`) |
| `pipeline.cpp:608-635` | 高位深路径: BL 还原 + 舍入 |
| `pipeline.cpp:700-712` | 逐通道 delta + 舍入 |
| `pipeline.cpp:792-811` | DNG 元数据 BL 设置 + 舍入 |
| `pipeline.cpp:822-829` | 零黑位路径: 元数据 BL=0 |
| `dng_sdk_bridge.cpp:284` | `SetDngBlackLevel` (经 `SetQuadBlacks` 存为有理数) |
| `dng_sdk_bridge.cpp:158` | `ExtractRawMetadata` (从 DNG 读取 BL/WL) |
| `float_image.cpp:77` | `FloatImageToUint16HostBuffer` (lround 量化) |


---


## 验证方法

诊断工具 `tools/perchan_stats.cpp` 可逐通道 (R/Gr/Gb/B) 输出暗部直方图和
最暗 1% 均值, 用于验证 BL 对齐情况:

```
perchan_stats.exe input.dng
```

验证 BL 是否为整数:

```
exiftool -v3 output.dng
```

查看 `BlackLevel` 标签的有理数表示 (如 `32762/256` = 分数, `32768/256` =
整数 128).
