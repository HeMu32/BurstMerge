# 对齐问题记录 + 单元测试方案

## 背景

Vulkan GPU 后端的对齐阶段与 CPU 路径存在差异。用最终 DNG 图像的 MAD/MSE 衡量
对齐质量是**不可靠**的 —— 原因和正确做法见下。本文记录两个已发现问题, 并定义后续
逐算法单元测试的方法与验收标准。

---

## 问题 1: VK 路径降噪效果差

### 现象
用户在 RAW 处理软件中对比 `R:\` 下 `_cpu` / `_vk` 样本, 发现 **VK 路径降噪明显弱于
CPU 路径** (画面更糙).

### 根因 (已查清)
1. **`tile_select` 着色器用 `double` 做加权平均, 但设备创建时未启用 `shaderFloat64`** →
   驱动对 double 运算返回不可靠结果 → 加权求和 `sw` 异常 → `round(swx/sw)` 产生 NaN →
   `int(NaN) = INT_MIN` → 整个 tile 位移场变成垃圾.
2. **结构性 bug**: 调试编辑期间误把 `tile_select` 的 dispatch 替换成了一个重复的
   `tile_sad` (tile 细化根本没执行), tsx/tsy 保持未初始化.

两者合起来: tile 位移垃圾 → warp 把对比帧采样到错误位置 → spatial 鲁棒权重
`w = 1/(1+(diff/noise)²)` 在大 diff 下 → 0 → **对比帧被全部拒绝 → 输出 ≈ 参考帧 (未降噪)**.
这正好解释了"VK 降噪差"的视觉观感.

### 修复
- 启用 `shaderFloat64` (设备创建 `VkPhysicalDeviceFeatures.shaderFloat64`).
- tile_select 权重改为数值稳定形式 `smin²/(score²+1e-8)` (CPU `1/(score²+1e-8)` 等价,
  不会下溢 NaN).
- 恢复 tile_select dispatch.

### 残留
即便 tile_select 不再产生 NaN, GPU 对齐的 **tile 位移场仍与 CPU 有差异** (见问题 2),
这会继续影响降噪质量 (个别 tile 错位 → 局部重影). 必须把 tile 位移对齐到 CPU (问题 2).

---

## 问题 2: 对齐 tile 位移 CPU/GPU 分歧; 图像 MSE 不可靠

### 为什么最终图像 MAD/MSE 不能用来衡量对齐质量
一个等曝光连拍里, 多数区域是静止或近静止的 → 多数 tile 的真实位移 ≈ 0, CPU 和 GPU 都
给出 0 → 这些 tile 对 MAD 贡献为 0. **哪怕有少数 tile 错位几十像素, 被 ~99% 的 0 位移 tile
平均掉, 整图 MAD 仍然很小** (实测 spatial 仅 ~1.7%, 看似乐观). 所以图像 MAD 对对齐误差
极不敏感, **不能用作对齐正确性的判据**.

(注: 之前一度报告 "seq3 0.018% = 像素等价", 那是脚本把 `-i $array` 解析成单帧的伪结果;
即便多帧, 小 MAD 也只说明"大部分 tile 没动", 不说明"对齐算法正确".)

### 正确做法: 直接比较 tile motion (位移场)
把对齐的**输出** —— tile 位移数组 `tile_shift_x[] / tile_shift_y[]` —— 在 CPU 和 GPU 之间
逐 tile 比较, 看每个 tile 的位移差 `|CPU − GPU|`:

- **max**: 最大单 tile 位移差 (最坏情况).
- **mean / 分位数**: 典型差异.
- **超阈 tile 数 / 占比**: 位移差 > 2px 的 tile 数.

### 验收标准
**CPU 与 GPU 的 tile 位移差: max ≤ 2px** (每个 tile, x 和 y 分量分别). 在此之内, warp 采样
偏差 ≤ 2px, 对降噪无可观影响.

---

## 单元测试方案

### 范围
逐**对齐算法**单独测试, 不走全管线:
1. **Standard** (`EstimateTranslation` / GPU 等价: 金字塔 + 全局 SAD 搜索 + `RefineTileField`).
2. **DenseTile** (`EstimateDenseTileField` / GPU `DenseAlignGPU`).
3. (后续) **Frequency** (`EstimateFrequencyTileField`).

### 测试输入
- 取两帧样本 (如 `Seq1` 的 frame0 / frame1), 解码 → plane → `ConvertPlanesToGrayscale`
  得到 `ref_gray`, `cmp_gray` (FloatImage, 单通道, plane 分辨率).
- **CPU 和 GPU 用同一份 gray 数据** (CPU 算完, 上传同一 buffer 给 GPU), 隔离 gray 计算
  差异, 只测对齐算法本身.

### 测试流程 (每个算法)
```
AlignParams params;  // tile_size, search_distance, mode, cfa_period=1 (plane)
AlignmentResult cpu = EstimateTranslation(ref_gray, cmp_gray, params);      // CPU
AlignmentResult gpu = GpuEstimateTranslation(vk, ref_gray, cmp_gray, params); // GPU
// 比较
assert(cpu.tiles_x == gpu.tiles_x && cpu.tiles_y == gpu.tiles_y);  // 几何必须一致
int max_dx = 0, max_dy = 0;
for each tile: max_dx = max(max_dx, abs(cpu.x[i]-gpu.x[i])); ... y
report max_dx, max_dy, mean, #tiles>2px
PASS if max_dx <= 2 && max_dy <= 2
```

### 需要的工程改造
- 把 GPU 对齐从 `GpuRunBurstPipeline` 内联代码**抽取成独立函数**
  `GpuEstimateTranslation(vk, ref_gray_h, cmp_gray_h, gw, gh, params) -> AlignmentResult`,
  下载并返回 tile 位移场 (与 CPU `AlignmentResult` 同结构).
- 新增测试程序 `test_align_gpu` (或加入 ctest), 对 Standard / Dense 各跑若干样本对,
  打印 max/mean/超阈占比, 退出码反映 max≤2px.

### 当前状态

**Standard — 已达标 (bit-identical).** 关键发现与修复:
- 逐候选 dump 对比 (`[DBG-CPU]`/`[DBG-GPU] tile(0,0) SADs`) 证明 GPU 与 CPU 的 **SAD 值本身 bit-identical** (同一份 gray 数据, 同样的 y-外-x-内累加顺序, 单精度即可, 不需要 double).
- 发散来自 **加权平均的求和精度**: CPU 用 `double` 算 `w=1/(score²+1e-8)` 再 `Σ(w·d)/Σw`, GPU 单精度 `float`. 远候选的微小权重带符号累加, 在近平局 tile 上把加权位移推过 round 边界 (如 x 从 -0.43 → -0.52, round 0 vs -1).
- **解决**: tile 细化改用 **argmin** (取最小 score 候选的位移), 而非加权平均. 因为当最小值明确时 (绝大多数 tile), 加权平均本就被最小候选主导, argmin 与之等价; argmin 只做比较、无求和, 完全不受 float/double 漂移影响.
- 结果 (Seq1 f0 vs f1): **max|d|=(0,0), mean=(0,0), 0% >2px** —— 与 CPU 逐 tile 完全一致. **全程单精度, 无 double** (符合消费级 GPU 性能要求).

**Dense — 未达标, 有 per-level 传播 bug.** 同样改用 argmin 后: mean 从 4.6 降到 1.0 左右, >2px 占比降, 但 **max 仍 ~195** (个别 tile 发散到 ~200px, 远超 ±96 的理论上界). 几何匹配 (147×97, tile32/spacing16). coarsest 层 (±3 around 0) 与 CPU 一致, 问题在逐层 propagate/correct 的某个环节让个别 tile 读到错误粗层值并放大. 待定位 (怀疑 scratch ping-pong buffer 的边界 tile 未写满, 或 fetchCoarse 的 ratio/px 映射在边缘 tile 越界).

### 下一步
- 定位 dense 的 max~195 tile: dump 该 tile 在各层的 seed/coarse 读值, 对比 CPU 的逐层传播.
- 修后 dense 也应达到 max ≤ 2px.
- Standard 已完成, 无需再动 (除非 CPU RefineTileField 本身改了).
