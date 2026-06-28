Reference\hdr-plus-swift 是一个macOS项目, 包括源代码, 我们现在想将他的核心能力移植到GNU工具链, gcc等. 
核心功能意味着我们的关注是除了前端能力之外的部分. 

Reference\文件夹中有一些参考资料. 当你需要寻求参考的时候, 优先检查这个文件夹中的内容. 
里面包括: 
原始论文 hdrplus-paper
参考实现项目 hdr-plus-swift
开源RAW解码器 LibRAW (不允许使用这个项目的源代码, 项目仅仅用于参考对于raw文件的数据解释, 避免瞎猜, 不能用这个项目的代码来进行RAW文件处理)
DNG格式参考 DNG_Spec_1_7_1_0

项目采用MinGW / G++编译. 不允许使用MSVC编译. 
目前项目只关注Windows平台适配性, 其余平台的支持暂时作为后续拓展考虑. 

项目目录结构说明:
	- CMakeLists.txt: 顶层 CMake 构建入口。
	- apps/: 可执行程序目录，包含 cli/、console/ 等应用目标。
	- libburstmerge/: 核心库源码和单元测试目录，包括 include/、src/、test/。
	- 3rdparty/: 第三方依赖目录，例如 dng_sdk、openmp、pocketfft、vulkan 等。
	- Reference/: 参考资料目录，包含 hdr-plus-swift、hdrplus-paper、LibRaw、DNG 规范等。
	- docs/: 项目文档与计划文件。
	- build/: CMake 生成的构建输出目录。

你还有一些样本文件 (包括Sample sequences), 在 (工作区根目录)\libburstmerge\test\samples\ 下. 当任何时候你需要样本文件的时候, 都可以查看这个文件夹. 其中 Seq 开头的文件夹包含一个曝光值恒定 (或者接近恒定) 的连拍序列; Bkt 开头的文件夹包含一个曝光包围序列. 部分样本文件和序列还被测试用例所引用. 

PATH中有exiftool, ffmepg (含ffprobe), dcraw.exe 可用. 你可能会用到它们辅助排查. 

代码风格约定 (务必遵循):
	- 缩进: 4 空格, 大括号独占一行 (Allman 风格). 
	- C++17, `CMAKE_CXX_EXTENSIONS OFF` (不使用 GNU 扩展). 
	- 不允许使用 MSVC 编译, 但源码中保留了少量 `#ifdef _WIN32` / MSVC 分支以备未来, 不要删除. 
	- 第三方 RAW 解码 (LibRaw 等) 源代码禁止引入; LibRaw 仅用于查证 RAW 字段含义. 
	- 不引入 stb_image; 每种图像格式由对应专用库 (libjpeg-turbo / libpng / libtiff) 处理, BMP 为手写. 
	- 默认情况下不要添加注释, 除非用户要求. 

==============================
项目整体架构 (Architecture)
==============================

## 分层与编译产物
	- `libburstmerge` (静态库, `.a`): 全部核心算法, 不暴露 DNG SDK 头文件给上层. 
	- `burstmerge_cli` (`apps/cli/main.cpp`): 命令行前端, 用 cxxopts 解析参数. 
	- `burstmerge_console` (`apps/console/main.cpp`): 占位 REPL, 仅识别 `process`/`exit`. 
	- `burstmerge_compare` (`apps/console/compare_dng_pixels.cpp`): DNG 逐像素回归比对工具. 
	- `tools/dump_dng.cpp`: 独立的 DNG 像素检查器, **不在 CMake 中构建**, 需手动 g++ 编译. 

## 双层 API
	- C++ 主入口: `burstmerge::BurstMerge` (PIMPL, `include/burstmerge/api.h`), 委托给 `PipelineOrchestrator` (`src/core/pipeline.cpp:196`). 
	- C ABI: `BM_*` 系列函数 (`include/burstmerge/api_c.h`), 为未来 Python/Rust/C# GUI 预留. 

## 核心处理管线 (`pipeline.cpp:196`)
顺序大致为:
	1. 后端守卫: 仅接受 CPU 和 Vulkan 后端 (`pipeline.cpp:206`, "Unsupported backend"). 
	2. `ClassifyInputs`: RAW / Rgb / Mixed; Mixed 会递归过滤掉非 RAW 后走 RAW 路径. 
	3. RGB 分支: `io::ReadImage` 逐帧解码 → 参考帧取 `size()/2` → `BuildRgbImages`. 若 `backend==Vulkan`, 调 `GpuRunBurstPipelineRgb` 上传到 GPU 并在对齐/合并后返回; 否则走 CPU 对齐→合并. → 写出. 
	4. RAW 分支:
		- `PrepareDngInputs` (`pipeline_io.cpp:106`): 非 DNG 的 RAW 在 Windows 上调 Adobe DNG Converter 转换. 
		- 两阶段读取: 先顺序读到内存, 再 `ReadDngFromBuffer` 并行解码 (`ParallelFor` 带 `decode_dng` tag). 
		- `SelectExposureRefIndex` (`pipeline_frame.cpp:147`): 包围曝光 (max_ev > 1.25×min_ev) 取最暗帧, 否则取中间帧. 
		- `RepairHotPixels` → `NormalizeFrames` (减黑电平, 按 EV 比例缩放) → `BuildAlignedComparisons`. 
		- [新增] `RecoverHighlights` (`pipeline_frame.cpp`): 在 `NormalizeFrames` 之后、`BuildAlignedComparisons` 之前执行. 对被裁切的绿色通道进行高光恢复 (从邻近 R/B 像素外推). 三层分发: Bayer (全空间邻居算法, 匹配 Swift `texture.metal:add_texture_highlights`), LinearRAW (逐像素同像素 R/B 外推), 其他 mosaic (同超级像素 R/B 外推). 常量定义在 `HighlightRecoveryParams` (`pipeline_frame.h`). GPU 端仅 Bayer (`highlight_recovery.comp`). CLI `--highlight-recovery` (默认开启).
		- 三选一合并: `TemporalAverage` / `FrequencyMerge` / `SpatialMerge`. 
		- 位深缩放 (`pipeline.cpp:531-606`): ≤10bit 走 "黑电平置零" 路径, >10bit 路径还原黑电平. 此处注释记录了 ACR/Lightroom 低 bit 黑电平渲染 bug 的规避. 
		- 可选 `ApplyExposure` → 逐通道黑电平 delta → `ConvertPlaneImageToMosaic` → 写出. 
	5. 清理: 删除转换临时目录; 若设置了 `BURSTMERGE_PROFILE` 打印 profile 报告. 

## 模块职责与对应参考实现 (hdr-plus-swift)
	- `core/`: 管线编排/线程/性能/缓冲/FFT. 对应 Swift 的 `denoise.swift` (TileInfo/progress/Metal device) 和 `texture.swift`. 
	- `align/`: 金字塔 + 三种估计器 (Standard/Dense/Frequency) + warp. 对应 `align/align.swift` + `align.metal`. 
	- `merge/`: spatial (像素域加权) 与 frequency (Laplacian / WienerFft / WienerFftRobust). 对应 `merge/spatial.swift` 和 `merge/frequency.swift` + `.metal`. 
	- `denoise/temporal.cpp`: 热像素修复 + 时域平均. 
	- [新增] `pipeline_frame.cpp::RecoverHighlights`: 高光恢复 (裁切绿色通道外推). 对应 Swift `texture.swift:add_texture_highlights` + `texture.metal:add_texture_highlights`. 常量 `HighlightRecoveryParams` 与 GPU shader `highlight_recovery.comp` 必须一致.
	- `exposure/exposure.cpp`: 线性 / Reinhard 曲线提亮. 对应 `exposure/exposure.swift`. 
	- `io/dng_sdk_bridge.cpp`: 用不透明指针持有 `dng_negative`, 完整保留 Opcode/CameraProfile/EXIF/XMP. 对应 Swift 的 `dng_sdk_wrapper.cpp` Obj-C++ 桥.
	- `io/dng_reader.cpp`: DNG 读取入口. 读 Stage1Image (Bayer CFA) 后, 若 `info.fEnhancedIndex != -1` 则调 `ReadEnhancedImage` (默认 host = mode 2 "linear-only": 将 enhanced LinearRaw 解码到 `fStage3Image`, 清除 MosaicInfo/OpcodeLists/`fEnhanceParams`), 再 `ClearStage1Image` 丢弃 Bayer, 使 `RawImage()` 回退到 Stage3. `is_linear_rgb` (planes==3) 检测将其标记为 `R16_Uint_RGB`, 走现有 LinearRaw 管线. 支持 ACR NR / Enhanced-Image (`sfEnhancedImage`) DNG (典型为 JPEG XL 压缩的 LinearRaw SubIFD). 对应 Swift `dng_sdk_wrapper.cpp` 的 `ReadDNG`.
	- `io/dng_converter.cpp` (Windows-only): 包装 Adobe DNG Converter CLI. 对应 `io_dng_sdk.swift::convert_raws_to_dngs`. 
	- `core/fft_util.cpp`: 用 PocketFFT 替代 Metal FFT shader; **必须 thread-local 持有 plan** (pocketfft plan 内部有工作缓冲, 不可多线程共享). 

## 关键数据结构
	- `FloatImage` (`core/float_image.h`): `width/height/channels + std::vector<float>`, NHWC 布局, 贯穿整个管线. 
	- `RawImage` / `RawMetadata` (`io/dng_io.h`): DNG 解码产物, 含 `DngNegativeHolder*` 不透明指针 (move-only). 
	- `HostBuffer` (`core/image_buffer.h`): 拥有所有权、move-only 的主机缓冲; `DeviceBuffer` 是非拥有的 GPU 句柄 (目前未用). 
	- `AlignmentResult` (`align/align.h`): 全局标量位移 + 每瓦片位移向量 + 置信度. 
	- `TileInfo` (`core/types.h`): 瓦片几何. 
	- `SubGraph` (`core/sub_graph.h`): 计算后端执行用的 DAG IR (NodeType 枚举 ~40 种), **当前 CPU 路径不产生也不消费**, 是为 Vulkan 预留的. 
	- `IComputeBackend` (`compute/compute_backend.h`): 后端抽象接口, **当前无任何具体实现**. 

## 后端现状
	- **CPU (OpenMP)** 和 **Vulkan (GPU compute)** 双后端均可用. CLI 通过 `--backend cpu|vulkan` 选择 (默认 cpu, `apps/cli/main.cpp`). 
	- **Vulkan 后端** (已实现): 见下方 "Vulkan GPU 后端" 章节. 在 RTX 3080 / 5060 Ti 上验证, 全部合并模式与 CPU 近像素级一致 (spatial/temporal/freq-laplacian/wiener-fft MAD < 0.02%; 包围曝光序列 < 0.25%). 对齐 standard+dense bit-identical (max 0px). **全单精度 float, 无 double**. 
	- `pipeline.cpp` 的 RAW 分支在 `SelectExposureRefIndex` 之后按 `backend_` 分流: Vulkan 走 `GpuRunBurstPipeline` (prepare/align/merge 全在 GPU), CPU 走原有路径; 二者共用后续 bit-depth/exposure/DNG 写出尾部. 
	- `SubGraph` / `IComputeBackend` 仍保留为接口, 当前未被 Vulkan 后端使用 (Vulkan 用 GPU-native 管线直接调度, 非通用节点图). 

==============================
Vulkan GPU 后端 (已实现)
==============================
	- 入口: `GpuRunBurstPipeline()` (`gpu_pipeline.cpp`) 用于 RAW, `GpuRunBurstPipelineRgb()` 用于 RGB. 两者各自负责数据准备 (RAW: uint16 上传+CFA deinterleave; RGB: float 上传), 然后调用共享核心 `GpuPipelineCore()` 完成 to_grayscale→align→merge→download. 由 `pipeline.cpp` 在 `backend_==Vulkan` 时调用. 返回 merged plane `FloatImage`, 复用 CPU 尾部.
	- 后端抽象: `burstmerge::vulkan::VulkanBackend` (`include/burstmerge/internal/compute/vulkan_backend.h` + `src/compute/vulkan/vulkan_backend.cpp`). 提供 buffer 创建/上传/下载/fill, UBO, dispatch (录制式: `BeginFrame`/`Dispatch`/`FlushFrame`), `CopyBufferRegion` (帧内 buffer 拷贝), `RecordUpload` (帧内批量上传), `DeferredDestroy` (延迟释放), `EnumerateDevices`/`Initialize(device_index)` (多 GPU 选择). VRAM 峰值跟踪 (`vram_peak`/`vram_live` 计数器, 析构时报告). 仅在算法固有 SyncPoint (noise_floor reduce, mismatch mean) 才 FlushFrame 读回.
	- 数据布局: 所有 GPU 图像数据为 `std430` storage buffer 的 `float[]` (NHWC, 与 `FloatImage` 一致). 输入 uint16 mosaic 默认由 `CreateBufferFromU16` 上传原始 uint16 数据, `prepare_texture` shader 在 GPU 端完成 uint16→float+deinterleave+减黑电平+曝光缩放 (消除 CPU 端转换瓶颈). Shader 支持双模式 (`pc.i9`): 0=uint16 输入 (默认), 1=float32 输入 (CPU 转换, 保留为回退路径).
	- 系统内存管理: GPU 上传完成后立即释放比较帧的系统 RAM — RAW 路径释放 `pixels` (HostBuffer) 和 `dng_negative` (DNG SDK 对象, 通过 RawMetadata move-assignment 触发 DestroyNegativeHolder); RGB 路径清空 `float_images[i].data`. 仅保留参考帧. DNG 文件字节 (`file_buffers`) 在解码后立即清空. 实测 15 帧 65MP RAW 在 GPU 对齐/合并期间工作集从 ~4.7 GB 降至 ~1.8 GB.
	- 下载优化: 持久化 staging buffer 优先使用 `HOST_CACHED` 内存类型 (NVIDIA RTX 3080: type 4), 避免 DEVICE_LOCAL BAR 的 uncached 读取 (56MB 读取: 586ms→3ms). 
	- Shader 集 (33 个 `.comp`, `src/compute/vulkan/shaders/`): 覆盖全部算法 — texture ops (prepare/downsample/box_blur/binomial_sep/to_grayscale/block_mean_guide/plane_to_mosaic/copy/fill/scale/add), align (sad_global/select_min/upscale_seed/tile_sad/tile_select/tile_refine_diag/dense_level/warp_tilefield/warp_translate), spatial (spatial_acc_multi/spatial_acc_1ch/normalize_div), temporal (temporal_acc_exposure/temporal_median), frequency (freq_laplacian/freq_wiener_tile — 后者含直接 8×8 DFT + 7×7 相位搜索 + Wiener 收缩, 4-phase 非重叠 dispatch), exposure (exposure_curve_global/exposure_reinhard_local/max_to_gray), reductions (extract/reduce_scalar), format (float_to_uint16). 共用 push-constant 块 `ShaderPC` (14 int + 8 float = 88B, 见 `common.glsl`). **全部单精度 float, 无 double, shaderFloat64 不启用**.
	- [新增] `highlight_recovery.comp` (第 34 个 shader): Bayer-only 高光恢复, 在 `prepare_texture` 之后 dispatch. 操作 4 通道 plane buffer (in-place), 仅修改绿色通道. Push constant: `pc.w2/h2`=plane 尺寸, `pc.i0/i1`=两个绿色通道索引, `pc.f0`=effective_range, `pc.f1..f4`=各通道 colour factor. 常量必须与 `HighlightRecoveryParams` (`pipeline_frame.h`) 一致. 非 Bayer (X-Trans 等) 在 GPU 路径跳过 (CPU 路径覆盖).
	- 对齐 (全 GPU, 零读回): 灰度金字塔 (2-then-4 模式) → 逐层 coarse-to-fine SAD 搜索 (sad_global 每 candidate 一个 workgroup, 256 线程 reduce; select_min 在 SSBO 内更新 seed, 无 CPU 读回; tie-breaking 匹配 CPU 字典序) → standard 路径 diagonal-wavefront tile 细化 (tile_refine_diag) 或 dense 路径逐层 propagate/correct/search (dense_level) → warp_tilefield 双线性混合 4 角 tile 位移. 
	- Descriptor set: 统一 layout, binding 0..7 = STORAGE_BUFFER, binding 7 复用于 binomial 权重 (std430, 无 padding). Push constants 88B. Pipeline cache 按 shader 名懒创建. 
	- 调试要点 (曾踩的坑): 每个 2D shader 必须声明 `layout(local_size_x=8,local_size_y=8)` 以匹配 `/8` 的 dispatch (否则只覆盖 1/64 像素); `FillFloat` 必须设 `pc.h=1` (否则 `n=w*h*ch=0` 不写入); binomial 权重必须用 std430 storage buffer 而非 std140 UBO (后者 float[] 步长 16B). 

==============================
构建与依赖 (Build & Dependencies)
==============================

## 配置入口
	- 顶层 `CMakeLists.txt` → `include(local_config.cmake)` (所有依赖路径集中) + `cmake/CompilerFlags.cmake`. 
	- `local_config.cmake` 假设 libjpeg/libpng/zlib 在 `C:/MinGW`, libtiff 在 `3rdparty/libtiff/install`. `local_config.cmake` 被 `.gitignore` 忽略 (顶层 `/*.cmake`), 属于本机配置. 
	- 生成器: `MinGW Makefiles`. 标准配置命令见 `Readme.md`. 

## 编译开关 (重要)
	- `find_package(OpenMP REQUIRED)`: OpenMP 必须存在. 
	- `BUILD_TESTS` (默认 ON): 构建单元测试. 
	- 条件宏 (在 `libburstmerge/CMakeLists.txt` 中按依赖是否存在定义): 
		- `BURSTMERGE_HAVE_JPEG` / `BURSTMERGE_HAVE_PNG` / `BURSTMERGE_HAVE_TIFF` 
		- 缺少对应库时, 该格式编解码器源文件不参与编译, 运行时解码/编码会抛 "... not available (... not linked)". 
	- `WIN32` 时额外编译 `src/io/dng_converter.cpp` (Adobe DNG Converter 包装). 

## 三方依赖状态
	- `dng_sdk`: 完整 vendored 源码, 编译为内部静态库, 符号隐藏 (`-fvisibility=hidden`). 构建 fighting 见 `docs/plan-dsv4.md` (`qWinOS` 宏, XMP INT64 宏冲突, SEH 移除等). `dng_host` 非线程安全, 并行解码需每线程独立构造. **JPEG XL 已启用**: `3rdparty/dng_sdk/CMakeLists.txt` 以 `add_subdirectory` 方式编译 vendored libjxl (含 highway + brotli + skcms 子目录), 链接 `jxl` + `jxl_threads` 到 `dng_sdk` (PRIVATE); 编译真实 `dng_jxl.cpp` 而非 `dng_jxl_stubs.cpp`. `dng_jxl.cpp` 中两处 XMP 使用 (EncodeJXL_Common 的 XMP box + ProcessXMPBox 的 `dng_xmp` 构造) 用 `#if qDNGUseXMP` 守护 (本机构建 `qDNGUseXMP=0`). **MinGW 限制**: Highway 的 AVX2 target 在 MinGW 下因函数多版本派发与 16 字节栈对齐 Windows ABI 冲突而 SIGSEGV, libjxl CMakeLists 中 `MINGW` 分支禁用 `HWY_AVX2` (SSE4 回退, 解码仍然正确); libjxl 子目录的 `CMAKE_CXX_FLAGS_RELEASE` 临时剥离 `-ffast-math` 和 `-march=native` (Highway 需精确 IEEE-754, 自管 ISA 派发).
	- `pocketfft`: 单文件 `.c` + `.h`, MIT. 直接编进 `burstmerge` 静态库. 
	- `cxxopts`: header-only, 仅 CLI 使用. 
	- `libtiff`: 完整源码 + 预编译 install (`3rdparty/libtiff/install/`). 需要运行时 `libtiff.dll` 在 PATH (`libburstmerge/test/CMakeLists.txt` 的 `add_burstmerge_test` 函数已为所有测试统一前置 `3rdparty/libtiff/install/bin` 到 `PATH`).
	- `vulkan`: 头/库/dll 齐全. **已启用** — `libburstmerge/CMakeLists.txt` 对所有编译器 (GCC/MinGW + MSVC) 链接 `libvulkan-1.a` (PUBLIC, 传播给所有可执行目标). 原始 `.a` 是不完整子集 (573 符号), 已用 `dlltool` 从 `vulkan_core.h` 导出的 767 个函数名重新生成完整导入库 (`cmake` 无此步骤, `.a` 已在 `3rdparty/vulkan/Lib/` 落地). 运行时需 `vulkan-1.dll` (系统已装). 
	- `glslang`: `3rdparty/glslang/glslangValidator.exe` (v7.8, GLSL 4.60). **configure-time** 由 `cmake/embed_shaders.ps1` 将 `src/compute/vulkan/shaders/*.comp` 编译为 SPIR-V 并嵌入到 `${CMAKE_BINARY_DIR}/generated/spirv_embedded.inl` (uint32 字节数组), `vulkan_backend.cpp` 直接 `#include`. 修改 shader 后需重新 `cmake configure`. 
	- `openmp`: 当前 MinGW 静态链接 `libgomp.a`, 无需分发 DLL; 仅含 readme. 

## 运行时环境变量
	- `BURSTMERGE_THREADS`: 覆盖 OpenMP 线程数 (`task_executor.cpp:16`). 
	- `BURSTMERGE_GRAIN_SCALE`: 缩放并行粒度 (`task_executor.cpp:26`). 
	- `BURSTMERGE_PROFILE`: 启用性能报告输出 (仅 Debug 生效, Release 整模块被 `#ifndef NDEBUG` 抹除). 

==============================
测试 (Testing)
==============================
	- 测试位于 `libburstmerge/test/`, 每个独立可执行, 退出码 0=pass. 
	- `test_deps`: 依赖与底层 API 检查. 
	- `test_dng_io`: DNG 读写路径. 
	- `test_stage0`: 单帧处理. 
	- `test_stage1`: 多帧处理 (超时 600s). 
	- `test_common_rgb_fmt`: RGB 格式 IO 检查 (需 libtiff runtime 在 PATH). 
	- 测试引用的样本在 `libburstmerge/test/samples/` (Seq* = 等曝光连拍, Bkt* = 包围曝光; 还含 ARW/DNG/jpg/png/tif 单帧与若干 `folder_*` 目录用于混合输入测试). 样本不上传 git. 

==============================
重要断言与临时措施 (Important Assertions & Temporary Measures)
==============================
本节是排查/重构时的重点提示. 

## 明确标注的临时/弱措施
	- `libburstmerge/src/io/dng_converter.cpp:164` 有 `// WEAK!!!`: Adobe DNG Converter 进程退出后文件可能还没写完, 用 200ms 轮询 + 1s 稳定窗口 (最长 2 分钟) 等 DNG 落盘. 这是明确的弱实现. 
	- `apps/console/main.cpp:6`: `burstmerge_console` 是 placeholder, 只认 `process`/`exit`, 输出硬编码到 `./out`. 
	- `dng_writer_adapter.cpp:31`: DNG 通过通用 `ImageWriter` 写出会抛 `"use the dedicated RAW pipeline for DNG output"`. 这是**故意的误用防护**, 不是缺陷. 

## Enhanced-Image DNG 读取 (重要修复)
	- ACR NR / Enhanced-Image DNG (含 `sfEnhancedImage` SubIFD, 典型为 JPEG XL 压缩的 LinearRaw) 曾在输出阶段崩溃: `WriteDNGWithMetadata` 中 `hasEnhancedImage = (&RawImage() != Stage3Image()) && EnhanceParams().NotEmpty()` 为 true (因 `negative.Parse` 从 enhanced IFD 设置了 `fEnhanceParams`, 但读取器只调 `ReadStage1Image` 未调 `ReadEnhancedImage`, `fStage3Image` 为 NULL), 随后 `Stage3Image()->Bounds()` 解引用 NULL → 0xC0000005.
	- 修复 (`dng_reader.cpp::RawReadDngFromStream`): `ReadStage1Image` 之后, 若 `info.fEnhancedIndex != -1 && !host.IgnoreEnhanced()` 则调 `ReadEnhancedImage` (默认 host = mode 2 "linear-only": 解码 enhanced 到 `fStage3Image`, 清除 MosaicInfo/OpcodeLists/`fEnhanceParams`) 再 `ClearStage1Image` 丢弃 Bayer. `is_linear_rgb` (planes==3) → `R16_Uint_RGB` → 现有 LinearRaw 管线. 写出时 `hasEnhancedImage` 为 false (`EnhanceParams` 已清空), 不再崩溃.
	- 前置依赖: libjxl 必须已链接 (否则 enhanced SubIFD 若为 JPEG XL 压缩, `ReadEnhancedImage` 会抛 `"JXL not supported"`). 见上方 "三方依赖状态 > dng_sdk > JPEG XL 已启用".

## todos.txt 记录的已知问题 (根目录 `todos.txt`)
	- 参考帧选取: 用户指定参考帧**未实现/未移植** (自动选取已实现: 包围取最暗, 普通取中间; 但无 CLI `--reference`). 
	- HotPixel Suppression: "目前的算法好像是坏的" (另见 `Readme.md` TODOs: "current implementation not working well"). 
	- `NormalizeFrames`: 只考虑 ISO 与快门速度, 不考虑其他因素及 EXIF 不准确问题. 
	- `ImageMetadata::tags`: 字段从未被写入 (dead field). 
	- CLI 待加: 指定缓存目录 / 指定参考图 / 拷贝元数据(含畸变) / 指定搜索位移限制. 

## 硬编码路径与平台假设
	- Adobe DNG Converter 路径硬编码: `C:\Program Files\Adobe\Adobe DNG Converter\Adobe DNG Converter.exe` (`dng_converter.cpp:23,29`). 
	- `apps/cli/main.cpp`: 后端默认 CPU, 可通过 `--backend cpu|vulkan|gpu` 选择. `--list-gpus` 列出可用 GPU 并退出. `--gpu-device N` / `--gpu N` 按序号选择 GPU (-1=自动). `Settings.gpu_device_index` 传递到 `vk.Initialize(device_index)`.
	- `pipeline_align.cpp:244`: 调试 BMP dump 到硬编码 `R:\` (Windows 盘符), 由 `kEnableAlignmentGrayDump=false` 关闭. 
	- 非显式 DNG 的 RAW 输入在非 Windows 平台会抛 `"Non-DNG RAW input requires pre-conversion on this platform"` (`pipeline_io.cpp:153`). 
	- 根目录脚本大量硬编码本机路径: `SyncARWLensCorr.ps1` (`C:\MultiMediaTools\Bin\exiftool.exe`), `Scripts/benchmark_parallel.ps1` (`Z:\seq1/seq2`, 注意默认 CLI 路径用大写 `Build` 与实际不符), `_bench.ps1` (用 `NUL` 作输出, 传位置参数且用了未定义的 `--profiler`, **当前 CLI 下无法运行**). 

## 尚未实现 / 占位
	- Vulkan 后端 **已实现** (见上方 "Vulkan GPU 后端" 章节). 全部合并模式 (spatial/linear/temporal/freq-laplacian/wiener-fft) 与 CPU 近像素级一致 (等曝光 MAD < 0.02%; 包围曝光 < 0.25%); 对齐 standard+dense bit-identical (max 0px). **全单精度 float, shaderFloat64 不启用**. WienerFftRobust 在 GPU 路径未实现 (会抛异常拒绝); hot-pixel repair 尚未在 GPU 路径实现 (当前跳过). [新增] 高光恢复 (highlight recovery) 在 GPU 路径已实现 (Bayer-only, `highlight_recovery.comp`, `prepare_texture` 之后 dispatch). `IComputeBackend`/`SubGraph` 通用节点图接口保留但未被 Vulkan 后端使用.
	- X-Trans CFA 支持: `float_image.h:46` 与 `float_image.cpp:349` 各有一条 `TODO(X-Trans)`, 当前非 Bayer 走通道平均 fallback, 可能不保色相位. 
	- `tools/dump_dng.cpp` 不在 CMake 构建中; 其头注释 (列 `x,y,r,g,b`) 与实际输出 (`x,y,raw_value`) 不一致, 且有未使用变量 `search_rows`. 

## 数值/算法层面的不一致与魔法数
	- `freq_align.cpp:82` 的傅里叶亚像素搜索网格实际是 6×6 (±0.417), 但 `frequency.h:13` 常量 `kFourierSearchGrid=7` (frequency merge 用 7×7). 两处网格不一致, 均能跑通. 
	- `kPi = 3.14159265358979323846` 在 `frequency.cpp` (5 处) 和 `freq_align.cpp` (1 处) 重复字面量, 未共享常量. 
	- `align_common.cpp:109` 越界惩罚 `kOBPenalty = 65504.0` (half-finite max). 
	- `frequency.cpp:766` 去卷积权重 `cw[8] = {0.00,0.02,0.04,0.08,0.04,0.08,0.04,0.02}` 等魔法数无注释. 
	- `temporal.cpp:93-94` 热像素阈值/强度实为常量 (`2.0f`/`1.0f`); `cfa_period` 为 0/1 时被 `max(2, ...)` 提升到 2. 
	- `task_executor.h:13` `kRowGrainMinPixels = 1<<18` 注释自承 "a magic number for CPU processing". 
	- `align_common.h` 编译期开关 `BURSTMERGE_ALIGN_WEIGHTED_AVG` (**默认 0=argmin 最佳值候选**, 这是生产/规范行为; 加权平均 `1/(score²)` 是不稳定的测试变体, 需 `-DBURSTMERGE_ALIGN_WEIGHTED_AVG=1` 显式开启). CPU 与 GPU (Vulkan) 均走 argmin.
	- `CompilerFlags.cmake:5` 使用 `-ffast-math -march=native`: 前者会影响浮点精度 (使 `compare_dng_pixels.cpp:43` 的精确浮点相等比较变得脆弱), 后者产生不可移植二进制. 

## 复核遗留风险 (来自 `docs/reviews/26-5-23-0-21.md` 最终复核)
	- RGB 路径 `WriteImage` 未接收已解析的 `eff_fmt` (A.2/C.3, 部分修复). 
	- TIFF `bps==32` 的 uint 样本仍被当 float 读取 ("数据损坏问题", B.4/C.1, 部分修复). 
	- BMP 8-bit 调色板分支假设 40 字节 DIB 头, 未校验 `biSize`/`biClrUsed`/`bfOffBits` (C.2). 
	- `ResolveImageOutputPath()` 会"静默改扩展名", 对 CLI 友好但对库 API 调用方未必可预期. 
	- `ImageDecoder::CanDecode()` 在工厂路径中几乎没被用到, 路由全靠扩展名. 

==============================
子项目: Sony 镜头校正注入 (见 PROGRESS.md)
==============================
独立于主管线的 PowerShell 子项目, 目标是把 Sony ARW 的畸变/暗角参数注入 DNG, 使 Capture One 能识别. 
	- `SyncARWLensCorr.ps1` (根目录): 从 ARW/DNG 读 Sony MakerNote `DistortionCorrParams`/`VignettingCorrParams` (16 个 int16 样条节点), 拟合多项式, 组装 `OpcodeList3` 二进制 (WarpRectilinear + FixVignetteRadial), 直接 patch 目标 DNG 的 TIFF IFD. 
	- 关键: C1 用 `OpcodeList3` 里的 WarpRectilinear, **不用** Sony MakerNote 或 Adobe `DistortionCorrParams`; C1 通过 `kr1` 符号判断桶形/枕形方向. 
	- 已知限制 (PROGRESS.md): 暗角参数翻译结果与原 ARW 在 C1 中不匹配 (但 C1 自己对 ARW 暗角解释也很差); CA 校正参数未拷贝. 
	- PS 脚本坑: PowerShell 变量不区分大小写 (`$X`/`$x` 同一变量), 脚本已全部改用小写规避. 

==============================
关键文件速查 (File:Line 参考)
==============================
	- 公共 API: `libburstmerge/include/burstmerge/api.h`, `api_c.h`
	- 管线主流程: `libburstmerge/src/core/pipeline.cpp:192` (`PipelineOrchestrator::Process`)
	- 管线常量: `libburstmerge/src/core/pipeline.h:13` (`PipelineConstants`)
	- 对齐常量/入口: `libburstmerge/include/burstmerge/internal/align/align.h:11` (`AlignConstants`), `align.cpp:22` (`EstimateTranslation`)
	- 空间合并: `libburstmerge/src/merge/spatial.cpp:243` (`SpatialMerge`)
	- 频域合并: `libburstmerge/src/merge/frequency.cpp:1026` (`FrequencyMerge`)
	- 热像素/时域平均: `libburstmerge/src/denoise/temporal.cpp:13` / `:223`
	- 曝光: `libburstmerge/src/exposure/exposure.cpp:118` (`ApplyExposure`)
	- DNG 读取: `libburstmerge/src/io/dng_reader.cpp:23` (`RawReadDngFromStream`), `:172` (`ReadDngFromBuffer`); Enhanced-Image 读取 `:59` (`ReadEnhancedImage` + `ClearStage1Image`)
	- DNG SDK 桥 (不透明指针): `libburstmerge/src/io/dng_sdk_bridge.cpp`
	- Adobe DNG Converter 包装: `libburstmerge/src/io/dng_converter.cpp:93`
	- 线程/粒度: `libburstmerge/src/core/task_executor.cpp:64` (`ParallelFor`)
	- FFT (thread-local plan): `libburstmerge/src/core/fft_util.cpp:8`
	- 参考帧选择: `libburstmerge/src/core/pipeline_frame.cpp:147` (`SelectExposureRefIndex`)
	- [新增] 高光恢复: `libburstmerge/src/core/pipeline_frame.cpp` (`RecoverHighlights`), 常量 `HighlightRecoveryParams` (`pipeline_frame.h`)
	- [新增] GPU 高光恢复 shader: `libburstmerge/src/compute/vulkan/shaders/highlight_recovery.comp`
	- CLI 选项定义: `apps/cli/main.cpp:129`
