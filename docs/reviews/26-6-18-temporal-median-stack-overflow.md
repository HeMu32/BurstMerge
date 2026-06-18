# TemporalMedian 栈缓冲区溢出 (>64 帧)

## 问题现象

用 `--algo temporal-median` 处理一个 150 帧的 RGB 连拍序列 (3840×2160×3ch×16bit),
对齐阶段全部完成并打印最后一条 `Warping frame 149/149`, 紧接着输出:

```
[ 70%] [+241.09s] Merging frames (temporal median)
```

随后进程**直接消失**, 没有 `Error: ...`, 没有异常, 没有 Windows 错误弹窗.

切换到 `--algo spatial` (`spatial-linear`) 处理**完全相同的序列**则全程正常完成.

## 根因

`TemporalMedian` (`libburstmerge/src/denoise/temporal.cpp:290`) 用三个固定大小的
栈数组承担按帧数索引的存储, 硬编码容量 `kStackFrames = 64`:

| 数组 | 位置 | 用途 | 写入索引 |
|---|---|---|---|
| `clip_scaled[64]` | 主线程栈 (串行段) | per-comparison 裁切阈值 | `k = 0..num_comp-1` |
| `buf[64]` | 每个 worker 栈 (ParallelFor 内) | per-pixel median 工作区 | `cnt = 1..num_comp+1` |
| `clipped[64]` | 每个 worker 栈 (ParallelFor 内) | per-comparison 裁切标记 | `k = 0..num_comp-1` |

当 `num_comp > 64` (即输入帧数 > 65, 因为 reference 占一个槽位) 时, 三个数组**全部越界**:

- `clip_scaled[k]` (`temporal.cpp` 修复前 `:335-340`): 主线程栈, 在 ParallelFor 之前
  的串行段就先越界 —— 这正是日志里"打印完 Merging 信息后立刻死"的原因. 主线程栈被
  写烂, 函数根本走不到 ParallelFor.
- `clipped[k]` (修复前 `:353`): 每个 worker 栈, 每像素都写.
- `buf[cnt++]` (修复前 `:383`): 每个 worker 栈, 每像素 × 每通道都写.

栈被破坏后, 要么命中 guard page → `SIGSEGV`/access violation, 要么破坏返回地址 →
进程直接终止. **不会**抛 `std::bad_alloc`, 也走不到 `pipeline.cpp:825` 的顶层 catch,
所以用户看不到错误信息, 只看到进程消失.

### 为什么 spatial-linear 不崩

`SpatialMerge` (`libburstmerge/src/merge/spatial.cpp:391-393`) 用的是
`std::vector<float> shared_w(aligned_comparisons.size(), 1.0f)` —— **动态大小 vector**,
从一开始就不会越界. `TemporalAverage` (`temporal.cpp:280-287`) 用的是
`for (const auto& img : aligned_comparisons) sum += img.data[i];` —— O(1) 栈, 与帧数无关.

作者在 spatial / frequency / temporal-average 三路都用了动态容器, 偏偏在 temporal-median
这一路用了定长栈数组. 是孤例疏漏, 不是设计意图.

### 同类问题排查

修复前对全代码库做了排查 (`grep '\[\s*\d+\s*\]\s*;'` + 按帧数/通道数/瓦片数分类),
**仅 `temporal.cpp` 存在按帧数索引的定长栈数组**. 其他固定大小栈数组都是按:

- 通道数 (≤4): `frequency.cpp:723 shifted[4]`、`pipeline.cpp:674 delta[4]`、
  `pipeline.cpp:783 scaled_bl[4]`、`temporal.cpp:103 black_level[4]` 等 —— 安全.
- 算法固定常量: `align_common.cpp:179 vals_x[9]/vals_y[9]` (3×3 抛物线拟合)、
  `align_warp.cpp:19-22 dx[4]/dy[4]/w[4]` (双线性 4 tap) —— 安全.
- 格式固定常量: `bmp_writer.cpp:101 palette[256][4]`、`png_decoder.cpp:47 sig[8]` —— 安全.
- 字符串缓冲: `fname[128]`、`buf[64]` 等 —— 安全.

## 修复方案

双路径: 保留 ≤64 帧的栈快路径, 为 >64 帧增加每线程预分配堆缓冲区回退路径.

### 关键设计点

1. **栈路径完全保留**: `buf_stack[64]` / `clipped_stack[64]` / `clip_scaled_stack[64]`
   与原实现等价. 唯一类型变化是 `bool clipped[]` → `uint8_t clipped[]` —— 字节布局
   完全一致, 读写语义完全一致 (`false`↔0, `true`↔1), 性能无差异. 改成 `uint8_t` 是
   为了让栈数组和 `std::vector<uint8_t>` 堆数组共享同一个指针类型, 避免在热循环里
   分支. `std::vector<bool>` 因位压缩不暴露 `bool*`, 不能用.

2. **堆缓冲区在串行段预分配** (`temporal.cpp:411-421`): 所有 `vector::resize` 都在
   ParallelFor 之前的主线程串行段完成. **不能**在 lambda 内部 resize —— 失败时抛
   `std::bad_alloc` 逃逸 OpenMP 结构化块是 UB, 在 MinGW libgomp 上倾向于
   `std::terminate` 而非 unwind 到 `pipeline.cpp:825`. 这个坑也是先前那次 OOM 分析
   误判的根源.

3. **按线程 id 索引**: 每个 worker 用 `omp_get_thread_num()` 取自己的预分配槽位,
   整个 TemporalMedian 调用期间复用. 不会在像素循环里 malloc/free, 性能与栈路径
   几乎一致 (仅多一次指针解引用).

4. **线程上限取 `MergeMaxThreads()`** (`temporal.cpp:33-44`, 匿名命名空间): 镜像
   `task_executor.cpp::RequestedThreadCount()` 的逻辑 —— `BURSTMERGE_THREADS` 环境变量
   优先, 否则 `omp_get_max_threads()`. 取 max 是为了处理用户把
   `BURSTMERGE_THREADS` 设得比 `omp_get_max_threads()` 还高的情况 (ParallelFor 真的
   会按这个数 spawn). 多分配几 KiB 比索引越界划算.

### 不变式

- `buf_cap = num_comp + 1` (reference 占一个槽)
- `use_stack = (buf_cap <= kStackFrames)`
- 栈路径: `buf_stack` / `clipped_stack` 容量 `kStackFrames = 64`, 仅当 `use_stack` 为真
  时使用, 永不越界
- 堆路径: `buf_heap[tid]` 容量 `buf_cap`, `clipped_heap[tid]` 容量 `num_comp`, 预分配
  足够大, 永不越界

### 性能影响

- **≤64 帧 (栈路径)**: 零变化. 与原实现相同的栈分配、相同的访问模式、相同的 cache 行为.
- **>64 帧 (堆路径)**:
  - 一次性预分配成本: `num_threads × (buf_cap × 4 + num_comp)` 字节, 例如 16 线程 ×
    150 帧 ≈ 18 KiB, 在串行段一次性 malloc, 可忽略.
  - 热循环: 多一次 `omp_get_thread_num()` 调用 (ParallelFor 区内恒定, 编译器可能
    hoist) 和一次指针解引用. 实测无感知.
  - `nth_element` 在 150 元素上比 64 元素慢约 2-3×, 但这是 median 算法本征 O(N),
    不是修复引入的回归.

## 未做的 (按用户要求)

- **未添加 >64 帧的单元测试**: 现有 `test_stage0.cpp::test_temporal_median_clip`
  只覆盖 ≤5 帧. 修复逻辑通过代码审查 + 手动 150 帧序列验证, 但缺少自动化回归保护.
  后续如要补, 一个 70 帧的合成用例 (任意分辨率均可) 即可锁定栈/堆路径分界.

## 文件清单

- `libburstmerge/src/denoise/temporal.cpp:290-501` — TemporalMedian 实现
- `libburstmerge/src/denoise/temporal.cpp:33-44` — MergeMaxThreads 辅助
- `libburstmerge/include/burstmerge/internal/denoise/temporal.h` — 声明 (未改)
- `libburstmerge/test/test_stage0.cpp::test_temporal_median_clip` — 现有测试 (未改)
