# GPU 路径维护指南

本文档面向维护 BurstMerge GPU (Vulkan) 计算后端的开发者。涵盖架构、关键约束、常见陷阱和调试方法。

---

## 1. 架构概览

### 1.1 入口与数据流

```
pipeline.cpp
  ├── RAW 路径 → GpuRunBurstPipeline(std::vector<RawImage>&, ...)
  │   ├── 准备: CreateBufferFromU16 → prepare_texture (GPU 端 uint16→float + CFA deinterleave)
  │   │     → highlight_recovery (GPU 端 Bayer 高光恢复, prepare_texture 之后)
  │     ├── 释放比较帧系统内存 (pixels + dng_negative)
  │     └── GpuPipelineCore(vk, plane, raw_meta, ...)
  │           ├── to_grayscale (灰度化, 可选 gamma)
  │           ├── 对齐 (金字塔 + SAD 搜索 + tile 细化 + warp)
  │           ├── 合并 (spatial / temporal / frequency)
  │           └── download (HOST_CACHED staging → FloatImage)
  │
  └── RGB 路径 → GpuRunBurstPipelineRgb(std::vector<FloatImage>&, ...)
        ├── 准备: RecordUpload (批量上传 float 数据, 无 CFA)
        ├── 释放 float_images[i].data
        └── GpuPipelineCore(...)  ← 共享核心
```

### 1.2 关键文件

| 文件 | 职责 |
|---|---|
| `gpu_pipeline.cpp` | GpuRunBurstPipeline / GpuRunBurstPipelineRgb / GpuPipelineCore + 内部辅助函数 |
| `gpu_pipeline.h` | 公开接口声明 |
| `vulkan_backend.cpp/.h` | Vulkan 设备管理、buffer 生命周期、command buffer 录制、dispatch |
| `shaders/*.comp` | 34 个 compute shader + `common.glsl` (push-constant 定义) |
| `embed_shaders.ps1` | configure-time SPIR-V 编译与嵌入 |
| `pipeline.cpp` | 后端路由: `backend_==Vulkan` 时调 GPU 入口 |

### 1.3 GpuPipelineCore 共享核心

RAW 和 RGB 两条路径的唯一差异是 **数据准备** (prepare)：
- RAW: `CreateBufferFromU16` + `prepare_texture` shader (uint16→float + deinterleave + 减黑电平 + 曝光缩放)
- RGB: `RecordUpload` 直接上传 float (无 CFA, 无黑电平)

prepare 之后两条路径汇入 `GpuPipelineCore`，执行完全相同的 to_grayscale → align → merge → download。

---

## 2. 硬约束 (务必遵守)

### 2.1 全单精度 float

**所有 shader 默认使用 `float`，不允许 `double` 以保证兼容性。** `shaderFloat64` 默认 **不启用**。

例外: `BURSTMERGE_GPU_FP64=ON` 时编译 `dense_level_fp64.comp` (double 累加, `shaderFloat64` 按需启用), 仅供极端包围曝光 dense 对齐使用。默认 fp32 路径 `dense_level.comp` 使用 strided M=16 累加器 (pairwise merge) + FMA 减少 float 舍入误差, 仍是纯 float, 不违反此约束。

> **2026-06-30 dense_level.comp 优化**: shader 从 52 次独立 `tileCost()` 调用 (每次独立遍历 tile 全部像素, 重复读取 ref buffer) 重写为 **fused candidate evaluation**: 候选按 `kCandGroup=4` 分批, 每批内 `ref[pixel]` 只读一次, 共享给批内所有候选; 仅 cmp 按候选位移分别读取。全局内存读取从 104 次/像素降至 69 次/像素 (~34%). 实测 RTX 3080 上 dense 对齐阶段加速 ~28% (0.43s→0.31s/帧), 输出与旧版 bit-identical (MAD=6.3e-05, MAXDIFF=1). Strided 累加精度不变.

CPU 端的 `double` 计算 (如 `robustness_norm`, `read_noise`) 在 C++ 中完成，结果以 `float` 通过 push constant 传入 shader。这不需要修改——只是不要在 GLSL 中引入 `double` 类型。

### 2.2 Push-constant 布局

`ShaderPC` (common.glsl) = 16 int + 8 float = 96 字节。这是 Vulkan 保证的最小 push-constant 大小 (128B) 以内。

新增 shader 时，如果需要更多参数，优先复用现有的 `i0..i9` / `f0..f7` 槽位，并在 shader 注释中标注语义。注意同一槽位在不同 shader 中含义不同（例如 `i0` 在 `extract.comp` 中是 mode，在 `box_blur.comp` 中是 radius）。

### 2.3 Shader 修改后必须重新 cmake configure

SPIR-V 在 cmake configure 阶段由 `glslangValidator` 编译并嵌入到 `spirv_embedded.inl`。修改 `.comp` 文件后，仅 `mingw32-make` 不会触发重新编译——必须先 `cmake ..` 。

### 2.4 Descriptor set 统一布局

所有 shader 共用一个 `VkDescriptorSetLayout`：binding 0..7 = `STORAGE_BUFFER`，binding 8 = UBO。binding 7 被 `binomial_sep` 复用为权重 buffer (std430 float[])。

---

## 3. VulkanBackend 编程模型

### 3.1 录制式 command buffer

```cpp
vk.BeginFrame();          // 开始录制
vk.Dispatch("shader", pc, gx, gy, gz, bindings, n);  // 录制 compute + barrier
vk.Dispatch(...);
vk.FlushFrame();          // submit + vkQueueWaitIdle
```

每次 `Dispatch` 后自动插入 compute→compute memory barrier。`FlushFrame` 提交并等待 GPU 完成。**不需要在 Dispatch 之间手动插 barrier。**

### 3.2 Buffer 生命周期

| 方法 | 何时使用 | 何时释放 |
|---|---|---|
| `CreateBuffer(n)` | 创建设备 buffer (n 个 float) | `DestroyBuffer(h)` 或 `DeferredDestroy(h)` |
| `CreateHostBuffer(n)` | 创建 host-visible buffer (CPU 可读) | `DestroyBuffer(h)` |
| `CreateBufferFromFloats(data, n)` | 上传 float 数据 (one-shot submit+wait) | `DestroyBuffer(h)` |
| `CreateBufferFromU16(data, n)` | 上传 uint16 数据 (one-shot, GPU 端转换) | `DestroyBuffer(h)` |
| `RecordUpload(h, data, bytes)` | 帧内批量上传 (staging 在 FlushFrame 释放) | `DestroyBuffer(h)` |
| `CopyBufferRegion(dst, off, src, n)` | 帧内 GPU→GPU 拷贝 | 无需手动释放 |
| `DeferredDestroy(h)` | 延迟到下次 FlushFrame 后释放 | 自动 |

### 3.3 下载优化 (HOST_CACHED)

`DownloadFloats` 使用持久化 staging buffer，优先选择 `HOST_CACHED` 内存类型。在 NVIDIA RTX 3080 上：
- `HOST_CACHED` (type 4): ~15 GB/s 读取
- `DEVICE_LOCAL|HOST_VISIBLE` (BAR, uncached): ~100 MB/s 读取

如果没有 `HOST_CACHED` 类型，回退到任意 `HOST_VISIBLE`。

### 3.4 VRAM 跟踪

```cpp
// 在 VulkanBackend 析构时自动打印:
// [VRAM] peak: 1571.2 MB (10 buffers), leaked: 0.0 MB (0 buffers)
```

编程式查询 (用于测试):
```cpp
VulkanBackend::LastLeakedBytes();   // 上一个实例的泄露字节数
VulkanBackend::LastLeakedBuffers(); // 上一个实例的泄露 buffer 数
```

### 3.5 设备选择

```cpp
VulkanBackend::EnumerateDevices();  // 返回可用 GPU 名称列表
vk.Initialize(device_index);        // -1 = 自动选最佳, >=0 = 指定设备
```

CLI: `--list-gpus`, `--gpu-device N` / `--gpu N`。

---

## 4. 内存管理规则

### 4.1 VRAM (GPU 内存)

- **比较帧的 `gray[i]` 和 `plane[i]` 在 warp 后立即释放** (DestroyBuffer)。GPU 此时 idle (刚 FlushFrame)，零同步代价。
- **参考帧金字塔层级**在所有比较帧对齐完毕后释放。
- **`aligned[k]` 在 spatial/temporal 累加后立即释放**。
- **Guide map 只在 ch==1 时创建** (spatial_acc_1ch 路径)。ch>1 时 spatial_acc_multi 不读取 guide map。

### 4.2 系统 RAM (CPU 内存)

- **RAW 路径**: GPU 上传后释放比较帧的 `pixels` (HostBuffer) 和 `dng_negative` (通过 RawMetadata move-assignment 触发 DestroyNegativeHolder)。仅保留参考帧。
- **RGB 路径**: GPU 上传后清空 `float_images[i].data`。`decoded[i].pixels` 在 metadata 提取后清空。
- **`file_buffers`** (DNG 文件字节) 在解码后立即 `clear()`。
- **注意**: `DngNegativeHolder` 是非拷贝非移动的 struct，通过裸指针持有。释放方式是 `RawMetadata move-assignment`（不能用 `= nullptr`，会泄漏）。详见 `dng_sdk_bridge.cpp`。

---

## 5. Shader 维护

### 5.1 prepare_texture 双模式

`prepare_texture.comp` 通过 `pc.i9` 选择输入格式：
- `i9 = 0` (默认): packed uint16 输入，GPU 端转 float
- `i9 = 1`: float32 输入 (CPU 预转换的回退路径)

push constant 布局:
```
pc.f1 = mean black level
pc.f2 = exposure scale (1.0 = 参考帧)
pc.i9 = 输入格式 (0=uint16, 1=float32)
```

切换 CPU/GPU 转换: 修改 `GpuRunBurstPipeline` 中的 `const bool use_cpu_fp32_convert`。

### 5.2 freq_wiener_tile 4-phase dispatch

Wiener FFT 使用 4 个非重叠 phase (phase_x, phase_y ∈ {0,1}) 避免累加数据竞争。修改 dispatch 时必须保持 4-phase 结构。

### 5.3 新增 shader 清单

1. 在 `shaders/` 下创建 `.comp` 文件
2. 包含 `#extension GL_GOOGLE_include_directive : enable` + `#include "common.glsl"`
3. 声明 `layout(local_size_x=8, local_size_y=8)` 用于 2D shader（必须匹配 `/8` 的 dispatch 尺寸）
4. 运行 `cmake ..` 重新编译 SPIR-V
5. 在 `vulkan_backend.cpp` 中调用 `vk.Dispatch("新shader名", ...)`

### 5.4 highlight_recovery (第 34 个 shader)

Bayer-only 高光恢复, 在 `prepare_texture` 之后、RAM 释放之前 dispatch.
- **调度**: `(ceil(pw/8), ceil(ph/8), 1)`, `local_size 8x8`, 每线程一个超级像素
- **操作**: in-place 修改 plane buffer (4 通道), 仅写入两个绿色通道
- **Push constant**: `pc.w2/h2`=plane 尺寸, `pc.i0/i1`=两个绿色通道索引, `pc.f0`=effective_range, `pc.f1..f4`=各通道 colour factor
- **常量**: 硬编码 0.8/0.99/0.9/4.5/0.2, 必须与 `HighlightRecoveryParams` (`pipeline_frame.h`) 一致
- **安全**: 相邻线程仅读取非绿色通道 (R/B), 写入仅限各自绿色通道 → 无数据竞争
- **非 Bayer 跳过**: `period != 2 || ch != 4` 时不 dispatch (CPU 路径覆盖其他 CFA 模式)
- **分发位置**: `gpu_pipeline.cpp` 的 `time.gpu.prepare` ProfileScope 内, 在 `rawbufs` 释放之后

---

## 6. 性能要点

### 6.1 FlushFrame 开销

每次 `FlushFrame()` 执行 `vkQueueSubmit` + `vkQueueWaitIdle`。尽量减少调用次数：
- 批量 dispatch 放入一个 BeginFrame/FlushFrame
- 仅在需要读回数据 (DownloadFloats) 或释放 buffer (DestroyBuffer) 时才 FlushFrame
- DestroyBuffer 在 GPU idle 时 (FlushFrame 后) 是零同步代价的

### 6.2 对齐模式选择

- **Dense**: 每个 level 一次 dispatch (~140ms/帧) — 推荐生产使用
- **Standard (legacy)**: 每个 diagonal 一次 dispatch (~1.3s/帧) — 仅 CLI 默认值，不要用于基准测试

### 6.3 profiler

启用: `BURSTMERGE_PROFILE=1` 环境变量 (仅 Debug build，已设为 `-O2 -g`)。

GPU 阶段 ProfileScope 条目:
- `time.gpu.prepare` — 上传 + prepare_texture + [新增] highlight_recovery + to_grayscale
- `time.gpu.align` — 金字塔 + 对齐 + warp
- `time.gpu.merge` — 合并
- `time.gpu.download` — 下载

CPU 阶段:
- `time.pipeline.prepare_dng_inputs` — ARW→DNG 转换
- `time.pipeline.decode_dng` — DNG 解码

---

## 7. 测试

### 7.1 GPU 测试结构

| 测试 | 范围 | 内容 |
|---|---|---|
| test_stage2 | GPU 单元 | 对齐 tile-motion: standard + dense × {ts16,ts32,ts64} × {sd32,sd64,sd128} (13 checks) |
| test_stage3 | GPU 端到端 | CPU vs GPU 双路径一致性 + VRAM 泄漏检查 + 压力测试 (41 checks) |

### 7.2 前提条件

- `test_stage2/3` 需要: Adobe DNG Converter + Vulkan GPU + 样本文件 (`libburstmerge/test/samples/Seq1/*.ARW`)
- 缺失前提时输出 SKIP (退出码 0)，不会 FAIL

### 7.3 手动一致性验证

```bash
BURSTMERGE_PROFILE=1 burstmerge_cli --backend cpu --merge-algo spatial -a dense -i a.arw -i b.arw -i c.arw -o cpu.dng
BURSTMERGE_PROFILE=1 burstmerge_cli --backend vulkan --merge-algo spatial -a dense -i a.arw -i b.arw -i c.arw -o gpu.dng
burstmerge_compare cpu.dng gpu.dng
```

### 7.4 VRAM 监控

运行结束时自动打印 `[VRAM] peak: X MB, leaked: Y MB`。leaked > 0 表示有 buffer 未释放。

---

## 8. 常见陷阱

### 8.1 Shader local_size

每个 2D shader 必须声明 `layout(local_size_x=8, local_size_y=8)` 以匹配 `(w+7)/8, (h+7)/8` 的 dispatch。否则只覆盖 1/64 像素。

### 8.2 FillFloat 的 pc.h

`FillFloat` 必须设 `pc.h=1`。否则 shader 内部 `n=w*h*ch=0`，不写入任何数据。（`FillFloat` 内部已正确处理，但如果手写 fill 调用需注意。）

### 8.3 binomial 权重 buffer

`binomial_sep` 的权重必须用 `std430` storage buffer (binding 7)，不能用 `std140` UBO。后者 `float[]` 步长 16B 而非 4B。

### 8.4 pipeline.cpp 的 RAM 释放

`GpuRunBurstPipeline` 接受 `std::vector<RawImage>&` (非 const)，因为它要释放比较帧的 pixels 和 dng_negative。**不要改回 const** — 会导致系统内存在 GPU 处理期间无法释放。

### 8.5 Stage 分离

test_stage0/stage1 = CPU 测试，test_stage2/stage3 = GPU 测试。不要在 CPU 测试中引入 GPU 依赖，反之亦然。
