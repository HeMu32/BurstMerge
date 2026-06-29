# Vulkan 后端构建纪要 (非标准工具链实践)

记录在 MinGW / G++ 工具链下从零接入 Vulkan 计算后端时遇到的、**不属于常规 CMake/Mingw 编译流程**的坑及解法.
目标读者: 下次有人在新机器上重建 Vulkan 后端, 或向其他 GPU API (Metal/DirectX/Compute) 迁移时不再踩同样的雷.

环境基线: Windows + MinGW (GCC 11.2.0) + `MinGW Makefiles` 生成器, **不使用 MSVC**, 无 LunarG Vulkan SDK 安装, 系统仅有显卡驱动附带的 `vulkan-1.dll` + `vulkaninfo.exe`.

---

## 1. 没有 Vulkan SDK, 也没有着色器编译器

### 现象
机器上找不到 `glslangValidator.exe` / `glslang.exe` / `dxc.exe`, `VULKAN_SDK` 环境变量为空, LunarG SDK 未安装. `.comp` 着色器无法编译为 SPIR-V.

### 解法
- 不依赖系统 SDK. 在 `3rdparty/glslang/` 内 vendored 一份 `glslangValidator.exe`.
- 该二进制来源于本机已存在的 Android SDK emulator (`D:\AndroidSdk\emulator\lib64\vulkan\glslangValidator.exe`, 版本 7.8, 支持 GLSL 4.60 + `GL_GOOGLE_include_directive`), 拷入项目即可. 它能正确编译本项目全部 33 个 `.comp`.
- 若新机器连这个来源都没有: 从 GitHub `KhronosGroup/glslang` releases 下载 `glslang-<ver>-windows-x64-Release.zip` (注意 release asset 命名随版本变化, 14.x 起为 `glslang-<ver>-windows-x64-Release.zip`, 更早为 `glslang-master-windows-x64-Release.zip`). 只需 `glslangValidator.exe` 单文件.

### 为什么不在线下载放进仓库
本机网络不稳定 + 该二进制 ~3.3MB, vendored 进 `3rdparty/` 是最稳的. 它是 Khronos 官方工具, 无许可问题.

---

## 2. MinGW Vulkan 导入库 (`libvulkan-1.a`) 是个不完整子集

### 现象
仓库 `3rdparty/vulkan/Lib/libvulkan-1.a` 只有 **573 个符号**, 链接时报大量 `undefined reference to vkEnumerateInstanceLayerProperties / vkCmdPushConstants / vkResetDescriptorPool / ...`. 用 `nm` 查该 `.a`, 上述符号确实不存在. 同目录的 `vulkan-1.def` 也是同一份不完整导出列表.

### 根因
那份 `.a` / `.def` 是某个旧版本 Vulkan Loader 的子集, 覆盖不到本项目用到的全部 core 1.0 函数.

### 解法
用 `dlltool` 从 **Vulkan 头文件**重新生成完整导入库 (不需要 `vulkan-1.dll` 在手, `dlltool` 仅按 `.def` 生成桩):

```powershell
# 1. 从 vulkan_core.h 抽取所有 VKAPI_CALL vkXxx( 函数名
$names = Select-String -Path "3rdparty/vulkan/Include/vulkan/vulkan_core.h" `
    -Pattern "VKAPI_CALL\s+(vk[A-Z]\w+)\s*\(" |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
# 得到 767 个核心函数名 (含扩展, 远多于 573)

# 2. 写 .def
"LIBRARY vulkan-1.dll","EXPORTS" + $names | Set-Content vulkan_full.def

# 3. dlltool 生成 MinGW 导入库 (x64 不做名称修饰)
dlltool -d vulkan_full.def -D vulkan-1.dll -l libvulkan-1.a -m i386:x86-64
```

生成后 `libvulkan-1.a` ~657KB, 含全部 767 符号. 运行时仍链接系统的 `vulkan-1.dll` (由显卡驱动提供, 所有 767 符号都在里面).

### 要点
- Windows x64 ABI 下导入符号名就是裸函数名 (`vkCreateInstance`), 无 `_` 前缀 / `@N` 后缀, 所以 `.def` 直接列函数名即可.
- 这一步是一次性的离线步骤, **不在 CMake 里自动化** (避免每次 configure 都重跑 dlltool); 产物已落地在 `3rdparty/vulkan/Lib/libvulkan-1.a`. 换 Vulkan headers 版本时才需要重做.
- `local_config.cmake` 已改为按编译器选库: MinGW/GCC → `libvulkan-1.a`, MSVC → `vulkan-1.lib`.

---

## 3. `#define VK_NO_PROTOTYPES` 即使置 0 也会关闭函数原型

### 现象
`vulkan_backend.cpp` 里为了让链接器看到 `vkCreateInstance` 等原型, 写了:
```cpp
#define VK_NO_PROTOTYPES 0
#include <vulkan/vulkan.h>
```
结果编译报 `'vkQueueWaitIdle' was not declared; did you mean 'PFN_vkQueueWaitIdle'` —— 全部 `vk*` 函数都找不到, 只剩 `PFN_*` 函数指针类型.

### 根因
`vulkan_core.h` 的逻辑是 `#ifndef VK_NO_PROTOTYPES` 才声明原型. `#define VK_NO_PROTOTYPES 0` **仍然算 defined**, 于是原型被关掉. 这是 C 预处理的标准行为, 但很容易写错.

### 解法
**根本不要定义 `VK_NO_PROTOTYPES`**. 默认 (未定义) 就是 "提供原型 + 链接导入库" 的常规模式:
```cpp
#include <vulkan/vulkan.h>   // 不要 #define VK_NO_PROTOTYPES
```
配合第 2 步的完整 `libvulkan-1.a`, 所有 `vk*` 符号在链接期解析到 `vulkan-1.dll`.

---

## 4. `std140` UBO 的 `float[]` 步长是 16 字节, 不是 4

### 现象
二项式模糊权重放进 UBO:
```glsl
layout(std140, binding = 8) uniform Weights { float wgt[16]; } weights;
```
C++ 侧上传 `float wgt[16] = {6,4,1,0,...}` (64 字节). shader 里 `weights.wgt[1]` 读到的不是 `4` 而是 `0`, 整个模糊输出错误 (金字塔深层全黑).

### 根因
std140 布局规则: 数组元素的步长向上取整到 `vec4` (16 字节). 所以 `float wgt[16]` 实际占 `16 * 16 = 256` 字节, `wgt[i]` 位于偏移 `i*16`. C++ 上传的 64 字节数据只够填 `wgt[0..3]` 的第一个分量, 其余读越界 / 零.

### 解法
两种, 任选其一:
- **(本项目采用)** 改用 `std430` storage buffer (数组步长 = 元素大小, float 就是 4 字节):
  ```glsl
  layout(std430, binding = 7) readonly buffer Weights { float wgt[]; } weights;
  ```
  C++ 侧用普通 storage buffer 上传 (`CreateBufferFromFloats(wgt, 16)`), `wgt[i]` 偏移 `i*4`, 与 C++ 内存一致.
- 或者保留 UBO, 但 C++ 侧按 16 字节步长打包 (每个 float 后填 12 字节 padding). 更啰嗦, 不推荐.

### 通用教训
凡是要在 GLSL 里以数组下标访问的 `float[]` / `int[]`, **优先 std430 storage buffer**; 只有标量 / `vec4` / 定长 struct 才适合 std140 UBO. 两者在本项目的 descriptor set layout 里共存 (binding 0..6 = storage, 7 = storage 复用做权重).

---

## 5. glslangValidator 命令行与 `#include` 的若干细节

逐条列, 每条都让某次编译静默失败或报离谱错误:

1. **`-I` 必须紧贴路径, 不能有空格**:
   - 错: `glslangValidator -V -I shaders shader.comp` (报 `-I<dir> include path must immediately follow option`).
   - 对: `glslangValidator -V "-I$shadersDir" shader.comp` (PowerShell 里把 `-I` 和路径拼成单个参数).

2. **被 `#include` 的头文件不能含 `#version` / `#extension`**. 本项目 `common.glsl` 只放 push-constant block 和几个 helper 函数; `#version 460` 和 `#extension GL_GOOGLE_include_directive` 都在各 `.comp` 里. 否则报 `'#version' must occur first in shader` (因为 include 展开后 #version 不在文件首行).

3. **`half` 是 GLSL 保留字**, 不能当变量名 (`freq_wiener_tile.comp` 曾用 `int half = 3;` → `'half' : Reserved word`). 改名 `halfGrid` 即可.

4. **PowerShell `Set-Content -Encoding UTF8` 会写 BOM**, glslang 看到 BOM 报 `unexpected token` / `#version must occur first`. 编辑 shader 后写回要用 .NET 无 BOM 路径:
   ```powershell
   [System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding($false)))
   ```
   本项目所有脚本 (`cmake/embed_shaders.ps1` 等) 都用这个写法.

---

## 6. 着色器 `local_size` 必须与 dispatch 的"除数"匹配 — 否则只覆盖 1/N 像素

这是本次最严重的 bug, 单独成节.

### 现象
GPU 输出整图近全黑 (均值接近 0), 但 CPU 路径正常. 对齐阶段产生的位移全是垃圾 (如 `-535, -520`), 进而 warp 把整张图采样到角落.

### 根因
很多 per-pixel shader 用 `gl_GlobalInvocationID.xy` 当像素坐标, dispatch 时按 `(ceil(w/8), ceil(h/8), ch)` 切分 workgroup —— 这隐含假设 `local_size = (8,8,*)`. 但**有一批 shader 漏写了 `layout(local_size_x=8, local_size_y=8)`**, 默认 `local_size=(1,1,1)`. 于是全局 ID 只覆盖 `dispatch_x × dispatch_y = (w/8) × (h/8)` 个像素, 即 **1/64**. 未覆盖的像素保留 device-local 显存的未初始化值 (多数驱动是 0), 整图全黑.

最隐蔽的一个: `binomial_sep.comp` (金字塔模糊). 它只写 1/64 像素 → 金字塔深层均值从 ~800 跌到 ~12 → 对齐在粗糙层把所有负位移的 SAD 算成 ~0 (零和零比) → 搜索一路往负方向"棘轮"到 `-535` → warp 采样越界 → 输出黑. 串了四五层才表象显现.

### 解法
- 凡是 dispatch 写 `(w/8, h/8, ...)` 的 shader, 必须有 `layout(local_size_x=8, local_size_y=8) in;` (z 维按需).
- 凡是 dispatch 写 `(n/256, 1, 1)` 的 1D shader, 必须有 `layout(local_size_x=256) in;`.
- 已经显式声明 local_size 的 (`sad_global` 256 / `freq_wiener_tile` 64 / `reduce_scalar` 256 / `tile_sad` / `tile_select` / `extract` / `upscale_seed` 用 `(1,1,1)` 且 dispatch 就是全范围) 不受影响.

### 通用教训
**dispatch 的 group count × local_size 必须等于你想覆盖的范围**. 用 `gl_GlobalInvocationID` 当坐标时, 永远先确认 local_size. 建议在 shader 顶部强制写明, 不要依赖默认 `(1,1,1)`.

---

## 7. 录制式 command buffer + per-frame descriptor pool 重置

### 设计
不是每个 dispatch 都 `vkQueueSubmit` + fence wait (太慢), 而是:
- `BeginFrame()`: 重置 descriptor pool, 开始录制一个 primary command buffer.
- `Dispatch()`: 从 pool 分配一个 descriptor set, 写 binding, `vkCmdBindPipeline` / `vkCmdBindDescriptorSets` / `vkCmdPushConstants` / `vkCmdDispatch`, 然后**插一个 compute→compute memory barrier** (`vkCmdPipelineBarrier`, SRC/DST = COMPUTE_SHADER, `SHADER_WRITE`→`SHADER_READ|WRITE`).
- `FlushFrame()`: `vkEndCommandBuffer` + `vkQueueSubmit` + `vkQueueWaitIdle`, 重置 CB.
- 只在算法固有的 SyncPoint (reduce 读回均值) 才 `FlushFrame`, 其余 dispatch 全部攒在一个 CB 里一次提交.

### 要点
- descriptor pool 用 `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` + 每 frame `vkResetDescriptorPool` 重置, 避免每 dispatch 分配/释放的开销堆积.
- pool 容量按 `kMaxFrameDispatches = 8192` sets 预分配; 超了就自动 flush+reset (见 `Dispatch()` 末尾的 fallback).
- 每个 dispatch 后的全局 memory barrier 是保守做法 (确保上一个 dispatch 的 SSBO 写对下一个可见). 精细化可用 buffer-level barrier, 但对本项目 dispatch 数量级, 全局屏障开销可接受.
- `vkQueueWaitIdle` 做 frame 同步是简单但阻塞的做法; 若要榨性能可换 fence + 多 frame-in-flight, 但 compute pipeline 的 SyncPoint 本来就强制串行, 收益有限.

---

## 8. 静态库链接 Vulkan 必须用 `PUBLIC`

### 现象
`libburstmerge.a` 编译链接都过了, 但链接 `burstmerge_cli.exe` / `test_deps.exe` 时报一片 `undefined reference to vk*`.

### 根因
`vulkan_backend.cpp.obj` 在静态库 `libburstmerge.a` 里; 静态库不会主动把它的依赖传递给上层. 当可执行文件链接 `libburstmerge.a` 并因引用 `GpuRunBurstPipeline` 而拉入 `vulkan_backend.cpp.obj` 时, 该 obj 里未解析的 `vk*` 符号需要由**可执行文件**自己链接 vulkan 导入库. 用 `target_link_libraries(burstmerge PRIVATE vulkan)` 不会传播到 consumer.

### 解法
```cmake
target_link_libraries(burstmerge PUBLIC ${VULKAN_LIBRARY})
```
`PUBLIC` 让依赖穿透到所有链接 `burstmerge` 的目标 (CLI / console / tests). 这和项目里 `dng_sdk` 用 PUBLIC 是一个道理.

---

## 9. configure-time 嵌入 SPIR-V, 而非 build-time

### 选择
`vulkan_backend.cpp` `#include "spirv_embedded.inl"` —— 该头必须**早于**任何编译发生前就存在. 用 CMake `execute_process` 在 **configure 阶段**跑 `embed_shaders.ps1` 编译全部 `.comp` → 生成 `build/generated/spirv_embedded.inl` (uint32 字节数组 + `GetBurstSpirvByName` 查表函数), 并把 `build/generated` 加入 include 路径.

### 为什么不用 build-time custom command
build-time 生成头会让 `vulkan_backend.cpp` 依赖一个 generated 文件, CMake 能处理 (`add_custom_command` + 依赖追踪), 但 MinGW Makefiles 对 generated header 的依赖追踪偶尔不稳, 且首次 build 顺序敏感. configure-time 生成保证: 只要 `cmake configure` 成功, `.inl` 就在, 后续 `make` 不需要关心 shader 编译顺序.

### 代价
改 shader 后必须重新 `cmake configure` (而非仅 `make`). 对本项目 (shader 迭代频率低) 可接受. 嵌入式方案也意味着分发二进制不需要带 `.spv` 文件.

### 嵌入格式
`.inl` 里每个 shader 是 `static const uint32_t kSpv_<name>[] = { 0x...,... };` + `_words` 计数. SPIR-V 是 4 字节对齐的字节码, 直接当 uint32 数组读取 (注意小端: 从 `.spv` 字节流每 4 字节组装一个 LE uint32).

---

## 10. push constant 的 C++ ↔ GLSL 布局镜像

`ShaderPC` (C++, `vulkan_backend.h`) 和 `common.glsl` 里的 `PC` block 必须字段顺序、类型、大小完全一致:

```
16 个 int32 (w, h, channels, w2, h2, channels2, i0..i9) + 8 个 float (f0..f7) = 96 字节
```

全是 4 字节标量, 无 std140 对齐陷阱 (没有 vec3/mat). push constant 在 Vulkan 里保证至少 128 字节, 96 字节安全.

通用字段复用: 不同 shader 用同一组 `i*` / `f*` 槽位表达不同含义 (在 shader 头注释里标注, 如 `pc.i0 = radius, pc.f0 = robustness`). 避免为每个 shader 定义单独的 PC struct, 否则 pipeline layout 会膨胀.

---

## 11. descriptor set 统一布局

全部 33 个 shader 共用**一个** `VkDescriptorSetLayout`:
- binding 0..7: `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` (通用图像数据, NHWC float)
- binding 7: storage buffer (复用做 binomial 权重, 见第 4 节)
- (早期方案曾用 binding 8 = uniform, 但为统一性最终全部 storage)

每个 pipeline 用同一个 `VkPipelineLayout` (1 个 set + 96B push constant). pipeline cache 按 shader 名懒创建 (`get_pipeline`). 这样:
- 不用为每个 shader 维护单独的 set layout / pipeline layout.
- 每 dispatch 只需 `vkAllocateDescriptorSets` + `vkUpdateDescriptorSets` 写**该 shader 实际用到的那几个 binding**, 未用到的 binding 留空 (Vulkan 允许, 只要 shader 不访问).

---

## 文件落点速查

| 内容 | 位置 |
|---|---|
| Vulkan 后端抽象 | `libburstmerge/include/burstmerge/internal/compute/vulkan_backend.h`, `libburstmerge/src/compute/vulkan/vulkan_backend.cpp` |
| GPU 管线编排 | `libburstmerge/include/burstmerge/internal/core/gpu_pipeline.h`, `libburstmerge/src/core/gpu_pipeline.cpp` |
| 着色器源 | `libburstmerge/src/compute/vulkan/shaders/*.comp` (+ `common.glsl`) |
| 着色器编译/嵌入脚本 | `cmake/embed_shaders.ps1` |
| 嵌入式 SPIR-V (生成物) | `${CMAKE_BINARY_DIR}/generated/spirv_embedded.inl` |
| glslangValidator | `3rdparty/glslang/glslangValidator.exe` |
| 完整 MinGW 导入库 | `3rdparty/vulkan/Lib/libvulkan-1.a` (dlltool 重生成) |
| CMake 接入 | `libburstmerge/CMakeLists.txt` (embed + PUBLIC 链接 + generated include 路径) |

### §9. 性能与内存优化经验

**下载 staging buffer (NVIDIA HOST_CACHED)**:
- 默认 `HOST_VISIBLE|HOST_COHERENT` 在 NVIDIA 上可能是 DEVICE_LOCAL BAR (uncached), CPU 读取仅 ~100 MB/s.
- 改为优先选 `HOST_VISIBLE|HOST_CACHED` 内存类型 (RTX 3080: type 4, cached system RAM), 读取带宽提升 50× (56MB: 586ms→3ms).
- 回退: 若无 CACHED 类型, 接受任意 HOST_VISIBLE.

**uint16 GPU 端转换**:
- 默认上传原始 uint16 (`CreateBufferFromU16`), `prepare_texture` shader 在 GPU 端转 float — 消除 CPU 端 65M 次/帧转换循环.
- `prepare_texture.comp` 通过 `pc.i9` 选择输入格式 (0=uint16, 1=float32). CPU 回退路径保留 (`use_cpu_fp32_convert` 标志).

**系统内存释放**:
- GPU 上传完成后, 比较帧的 `pixels` (HostBuffer) 和 `dng_negative` (DngNegativeHolder) 立即释放. 参考帧保留供 DNG 写出.
- `file_buffers` (DNG 文件字节) 在解码后立即清空.
- 实测 15 帧 65MP RAW: GPU 对齐/合并期间工作集 4.7→1.8 GB.

**VRAM 跟踪**:
- `VulkanBackend` 内置 `vram_live`/`vram_peak` 计数器, 析构时打印峰值和泄露.

### §10. GPU 管线架构 (GpuPipelineCore)

`GpuPipelineCore` (`gpu_pipeline.cpp`) 是 RAW 和 RGB 共享的核心, 负责对齐+合并+下载:
- `GpuRunBurstPipeline` (RAW): prepare_texture (CFA deinterleave + uint16→float + black level) → 调用 Core
- `GpuRunBurstPipelineRgb` (RGB): 直接上传 float (RecordUpload 批量) → 调用 Core
- 两者在上传后均释放源数据的系统内存, 仅保留 GPU 缓冲.
