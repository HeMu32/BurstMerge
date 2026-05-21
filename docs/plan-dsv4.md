# BurstMerge — 移植架构规划 v4

## 链接策略

- **libburstmerge**: 静态链接 (`.lib` / `.a`)，所有前端 (CLI/Console/GUI) 直接静态链接核心库
- **运行时依赖**: 按照常见实践保持动态链接 (VC Runtime, Vulkan Loader, OpenMP runtime, 系统 CRT)
- **3rdparty/dng_sdk**: 编译为内部静态库，完全隐藏于 libburstmerge 内部，不暴露任何头文件给上层
- 前端只需 `#include` 头文件 + 链接 `.lib`

---

## 项目文件树

```
burstmerge/
├── CMakeLists.txt                    # 顶层 CMake 项目
├── local_config.cmake                # ★ 所有三方依赖路径集中管理
│                                      #   include / lib / dll 路径
│                                      #   VK_SDK_PATH, DNG_SDK_PATH 等
│
├── cmake/
│   ├── CompilerFlags.cmake           # GCC/MSVC/Clang 各自 flags
│   └── FindDNG_SDK.cmake             # 基于 local_config 的 DNG SDK 查找
│
├── 3rdparty/                         # ★ 所有三方依赖的头文件 + 源码
│   ├── dng_sdk/                      # Adobe DNG SDK (内含 libjpeg + xmp)
│   │   └── CMakeLists.txt            # 独立 CMake, 宽松 flags
│   ├── pocketfft/                    # PocketFFT (header-only, MIT)
│   ├── cxxopts/                      # CLI11 或 cxxopts (header-only)
│   └── tests/                        # 测试辅助 (如有简单 header, 可放此处)
│   ├── vulkan/                       # Vulkan 头文件 + MinGW .lib/.dll
│   │   ├── Include/
│   │   └── Lib/
│   └── openmp/                       # (可选) OpenMP 运行时 dll 拷贝
│
├── libburstmerge/                    # ★ 核心库 (静态 .lib/.a)
│   ├── include/burstmerge/
│   │   ├── api_c.h                   # 纯 C API (跨语言场景, 可选)
│   │   ├── api.h                     # C++ API 入口 (主接口)
│   │   └── internal/                 # 内部接口, 不对外暴露
│   │       ├── core/
│   │       │   ├── types.h           # TileInfo, BurstSettings, ImageMetadata
│   │       │   ├── image_buffer.h    # HostBuffer / DeviceBuffer
│   │       │   └── sub_graph.h       # 局部 DAG (两帧对齐+融合)
│   │       ├── io/
│   │       │   └── dng_io.h          # DngReader / DngWriter 抽象
│   │       ├── compute/
│   │       │   └── compute_backend.h # IComputeBackend 虚基类
│   │       ├── align/
│   │       │   └── align.h
│   │       ├── merge/
│   │       │   ├── spatial.h
│   │       │   └── frequency.h
│   │       ├── denoise/
│   │       │   └── temporal.h
│   │       └── exposure/
│   │           └── exposure.h
│   │
│   ├── src/
│   │   ├── api/                      # API 入口层
│   │   │   ├── api.cpp               # C++ API → 内部编排器
│   │   │   └── api_c.cpp             # C API → C++ API (薄封装层)
│   │   │
│   │   ├── core/
│   │   │   ├── pipeline.cpp          # PipelineOrchestrator: 逐帧流式调度
│   │   │   ├── sub_graph.cpp         # 局部 DAG 构建 (两帧对齐+融合)
│   │   │   ├── image_buffer.cpp      # HostBuffer/DeviceBuffer impl
│   │   │   └── lru_cache.cpp         # LRU 缓存
│   │   │
│   │   ├── io/
│   │   │   ├── dng_reader.cpp        # DngReader: DNG SDK → RawImage
│   │   │   ├── dng_writer.cpp        # DngWriter: RawImage → DNG SDK 写盘
│   │   │   ├── dng_converter.cpp     # (Windows 条件编译) Adobe DNG Converter CLI 调用
│   │   │   └── dng_sdk_bridge.cpp    # 桥接层 (不透明指针管理 dng_negative)
│   │   │
│   │   ├── compute/
│   │   │   ├── cpu/
│   │   │   │   ├── cpu_backend.cpp
│   │   │   │   ├── texture_ops.cpp
│   │   │   │   ├── pyramid.cpp
│   │   │   │   └── fft.cpp
│   │   │   └── vulkan/
│   │   │       ├── vulkan_backend.cpp
│   │   │       ├── vulkan_device.cpp
│   │   │       ├── vulkan_pipeline_cache
│   │   │       ├── staging_buffer.cpp
│   │   │       └── shaders/
│   │   │
│   │   ├── align/
│   │   │   ├── align.cpp
│   │   │   └── warp.cpp
│   │   │
│   │   ├── merge/
│   │   │   ├── spatial.cpp
│   │   │   └── frequency.cpp
│   │   │
│   │   ├── denoise/
│   │   │   └── temporal.cpp
│   │   │
│   │   └── exposure/
│   │       └── exposure.cpp
│   │
│   └── test/
│       ├── CMakeLists.txt
│       ├── test_align.cpp
│       ├── test_merge.cpp
│       ├── test_exposure.cpp
│       ├── test_denoise.cpp
│       ├── test_texture_ops.cpp
│       ├── test_dng_io.cpp
│       ├── test_pipeline.cpp
│       └── fixtures/
│
├── apps/
│   ├── cli/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   ├── console/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   └── gui/                          # (后期)
│       ├── CMakeLists.txt
│       ├── main.cpp
│       └── ...
│
└── docs/
    └── plan-dsv4.md
```

---

## 三方依赖集中管理

### local_config.cmake

项目根下唯一的依赖配置入口，所有路径在此集中声明，各 CMake 模块通过 `include(local_config)` 读取。

```cmake
# ===== burstmerge/local_config.cmake =====

# 项目根目录 (由调用方传入或自动推导)
if(NOT DEFINED PROJECT_ROOT)
    set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}")
endif()

# ---- DNG SDK ----
set(DNG_SDK_ROOT     "${PROJECT_ROOT}/3rdparty/dng_sdk")
set(DNG_SDK_INCLUDE  "${DNG_SDK_ROOT};${DNG_SDK_ROOT}/xmp_headers;${DNG_SDK_ROOT}/libjpeg")
set(DNG_SDK_SOURCES
    "${DNG_SDK_ROOT}/dng_sdk/dng_info.cpp"
    "${DNG_SDK_ROOT}/dng_sdk/dng_image.cpp"
    # ... 完整源文件列表见实际文件
)

# ---- PocketFFT (header-only) ----
set(POCKETFFT_INCLUDE  "${PROJECT_ROOT}/3rdparty/pocketfft")

# ---- Vulkan SDK ----
# MinGW: 手动下载 LunarG Vulkan SDK 放入 3rdparty/vulkan/
# MSVC:  可自动检测 VULKAN_SDK 环境变量
set(VULKAN_ROOT        "${PROJECT_ROOT}/3rdparty/vulkan")
set(VULKAN_INCLUDE     "${VULKAN_ROOT}/Include")
set(VULKAN_LIBRARY     "${VULKAN_ROOT}/Lib/vulkan-1.lib")
#   (运行时: vulkan-1.dll 由用户安装 Vulkan Runtime 或从 3rdparty 拷贝)

# ---- cxxopts ----
set(CXXOPTS_INCLUDE    "${PROJECT_ROOT}/3rdparty/cxxopts/include")

# ---- OpenMP (MinGW) ----
# 当前 MinGW (GCC 11.2.0) 静态链接 libgomp.a, 无需运行时 DLL
# 若切换到 MSVC 或动态 MinGW 构建, 取消下行注释
# set(OPENMP_RUNTIME_DLL "${PROJECT_ROOT}/3rdparty/openmp/libgomp-1.dll")
```

### 3rdparty 目录职责

```
3rdparty/
├── dng_sdk/          # Adobe DNG SDK 源码 (内部静态库, 不需要系统安装)
├── pocketfft/        # Header-only, include 即可
├── cxxopts/          # Header-only, include 即可
├── vulkan/           # MinGW: 手动放置 Vulkan SDK Include/ + Lib/
│   ├── Include/vulkan/vulkan.h
│   └── Lib/vulkan-1.lib
└── openmp/           # 运行时 DLL 拷贝 (当前 MinGW 静态链接 libgomp.a, 不需要)
    └── readme.txt     # 见 readme.txt 说明
```

### CMakeLists.txt 顶层引用方式

```cmake
# 顶层 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(burstmerge VERSION 0.1.0 LANGUAGES CXX C)

include(local_config.cmake)

add_subdirectory(3rdparty/dng_sdk)
add_subdirectory(libburstmerge)
add_subdirectory(apps/cli)
add_subdirectory(apps/console)

# ---- 测试: 使用 CTest + 独立 test 可执行文件 ----
# 不需要 GoogleTest, 每个 test_*.cpp 是独立可执行文件
# 通过 CTest add_test() 注册, 退出码 0=通过, 非0=失败
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(libburstmerge/test)
endif()
```

---

## 核心抽象结构

### 1. API 入口 (双接口策略)

**静态链接无需 DLL 导出宏，C++ API 是绝对主力入口**。但为保险（未来可能有 Python/Rust/C# GUI），额外提供一层纯 C API。

```cpp
// ===== libburstmerge/include/burstmerge/api.h (C++ API, 主力) =====
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace burstmerge {

enum class BackendType      { CPU, Vulkan };
enum class MergeAlgorithm   { Spatial, Frequency };
enum class ExposureMode     { Off, Linear, Curve };

struct Settings {
    int            tile_size        = 32;
    int            search_distance  = 64;
    MergeAlgorithm merge_algo       = MergeAlgorithm::Spatial;
    float          noise_reduction  = 13.0f;
    ExposureMode   exposure_mode    = ExposureMode::Off;
    int            exposure_stops   = 0;
};

struct Result {
    bool        success;
    std::string output_path;
    std::string error_msg;
};

class BurstMerge {
public:
    explicit BurstMerge(BackendType backend);
    ~BurstMerge();

    void AddImage(const std::string& path);
    void Configure(const Settings& settings);

    using ProgressFn = std::function<void(float, const std::string&)>;
    void SetProgressCallback(ProgressFn cb);

    Result Process(const std::string& output_dir);
    std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace burstmerge
```

```c
// ===== libburstmerge/include/burstmerge/api_c.h (C API, 跨语言后备) =====
#pragma once

#ifdef _WIN32
  #define BM_API __declspec(dllimport)
#else
  #define BM_API
#endif

typedef void* BM_Context;
typedef void (*BM_ProgressCb)(float percent, const char* stage, void* user);

BM_API BM_Context  BM_Create(int backend_type);
BM_API void        BM_Destroy(BM_Context ctx);
BM_API void        BM_AddImage(BM_Context ctx, const char* path);
BM_API void        BM_SetTileSize(BM_Context ctx, int size);
BM_API void        BM_SetSearchDistance(BM_Context ctx, int dist);
BM_API void        BM_SetNoiseReduction(BM_Context ctx, float strength);
BM_API void        BM_SetExposureMode(BM_Context ctx, int mode);
BM_API void        BM_SetProgressCallback(BM_Context ctx, BM_ProgressCb cb, void* user);
BM_API int         BM_Process(BM_Context ctx, const char* out_dir);
BM_API const char* BM_GetLastError(BM_Context ctx);
```

### 2. 流式逐帧管线 + 局部 DAG

**采用两阶段设计**：SetupPhase（一次性建立共享参考帧数据）→ PerFramePhase（逐帧构建局部 SubGraph，传入共享资源指针）。

频率融合路径有额外的 4-位移外循环，由 C++ Orchestrator 直接循环控制。

```
Orchestrator 伪代码 —— Spatial Merge 路径:

files = ValidateInput(paths)

// ---- RAW → DNG (仅 Windows) ----
if platform == Windows && any(files, ext != ".dng"):
    files = RunAdobeDngConverterCLI(files)

// ---- DNG 解码 ----
images = DngReader::LoadAll(files)          // RawImage[]

ref_idx = SelectReference(images)

// ============ SetupPhase ============
// 参考帧 prepare + 金字塔 + 模糊 + noise_sd (一次性, 所有 comp 共享)
ref_prepared = PrepareTexture(ref, hotpixel, pad, 0, black_level[ref]) // R32_Float
ref_cropped  = Crop(ref_prepared, pad)
bl_mean      = Mean(black_level[ref])
ref_pyramid  = BuildPyramid(ref_prepared, downscale_factors, bl_mean, color_factors[ref])
ref_blurred  = Blur(ref_cropped, kernel=16)
noise_sd     = EstimateColorNoise(ref_cropped, ref_blurred)  // ← SyncPoint: GPU readback
robustness   = ComputeRobustness(settings.noise_reduction)

// 分配累加器 (per burst, 持续驻留)
accumulator = CreateDeviceBuffer(ref.width, ref.height, R32_Float)
norm        = CreateDeviceBuffer(ref.width, ref.height, R32_Float)
FillZeros(accumulator)
FillZeros(norm)

// ============ PerFramePhase (逐帧) ============
for each comp_idx != ref_idx:
    // --- SubGraph for this pair ---
    SubGraph sg;
    sg.shared_inputs = {ref_pyramid, ref_blurred, noise_sd, accumulator, norm}
                          //  ↑ 指针指向 SetupPhase 创建的 DeviceBuffer

    sg.Add(PrepareTexture,  {comp_idx, hotpixel, pad, exposure_diff, black_level[comp]})
    sg.Add(BuildPyramid,    {comp_prepared, downscale_factors})
    sg.Add(TileAlign,       {comp_pyramid, ref_pyramid})
    sg.Add(Warper,          {comp_prepared, alignment_vectors, downscale_factor})
    sg.Add(RobustMerge,     {ref_cropped, ref_blurred, warped, kernel, robustness, noise_sd})
    sg.Add(Accumulate,      {merged, accumulator, n_images})   // add_texture(merged, accumulator, 1/n)

    backend->ExecuteSubGraph(sg)

    // 释放此 comp 的中间 GPU 资源
    backend->DestroyBuffer(comp_prepared)
    backend->DestroyPyramid(comp_pyramid)
    // 对齐向量、merged 纹理自动随 SubGraph 析构

// ============ PostProcess ============
ExposureCorrect(accumulator, norm, ...)       // 含 SyncPoint: texture_max readback
ConvertFloatToUint16(accumulator, white_level)
Download(accumulator → host_buffer)
DngWriter::Write(out_path, {host_buffer, ref_metadata})
```

```
Orchestrator 伪代码 —— Frequency Merge 路径:

// SetupPhase: 准备参考帧 (每位移循环内重建, 见下)
accumulator = CreateAndZero(...)

for shift in [(0,0), (0,hs), (ws,0), (ws,hs)]:
    // 每位移: 参考帧 prepare/pyramid/FFT 重新构建 (pad 参数不同)
    ref_prepared = PrepareTexture(ref, hotpixel, pad+shift, 0, black_level[ref])
    ref_rgba     = ConvertToRGBA(ref_prepared, crop_x, crop_y)
    ref_pyramid  = BuildPyramid(ref_prepared, downscale_factors, ...)
    rms          = CalculateRMS(ref_rgba, tile_info_merge)
    ref_ft       = ForwardFFT(ref_rgba, tile_info_merge)

    final_ft     = CopyTexture(ref_ft)
    mismatch_acc = CreateAndZero(like rms)

    for each comp_idx != ref_idx:
        SubGraph sg;
        sg.Add(PrepareTexture,     {comp, hotpixel, pad+shift, exposure_diff, black_level[comp]})
        sg.Add(BuildPyramid,       {comp_prepared, downscale_factors})
        sg.Add(TileAlign,          {comp_pyramid, ref_pyramid})
        sg.Add(Warper,             {comp_prepared, alignment, downscale})
        sg.Add(ConvertToRGBA,      {warped, crop_x, crop_y})
        sg.Add(CalcMismatch,       {comp_rgba, ref_rgba, rms, exposure_factor, tile_info})
        sg.Add(NormalizeMismatch,  {mismatch, mean_mismatch})   // ← SyncPoint: GPU readback
        sg.Add(CalcHighlightsNorm, {comp_rgba, exposure_factor, ...})
        sg.Add(ForwardFFT,         {comp_rgba, tile_info})
        sg.Add(FreqMerge,          {ref_ft, comp_ft, final_ft, rms, mismatch, highlights_norm, ...})

        backend->ExecuteSubGraph(sg)
        backend->Destroy*(...)  // 逐帧释放

    Deconvolute(final_ft, mismatch_acc, tile_info)
    output = BackwardFFT(final_ft, tile_info, n_images)
    ReduceArtifacts(output, ref_rgba, tile_info, black_level[ref])
    output_bayer = ConvertToBayer(Crop(output, ...))
    Accumulate(output_bayer, accumulator, 1)    // 4 位移结果累加

Download(accumulator → host_buffer)
DngWriter::Write(out_path, ...)
```

**SubGraph 节点类型清单** (对应 NodeType enum):

```cpp
enum class NodeType {
    // ---- Texture Ops ----
    PrepareTexture,        // R16_Uint → R32_Float: black subtract + hotpixel + exposure + pad
    BuildPyramid,          // avg_pool 迭代降采样
    ConvertToRGBA,         // Bayer 交错 → RGBA 平面
    ConvertToBayer,        // RGBA 平面 → Bayer 交错
    Blur,                  // 可分离二项式模糊 (先 x 后 y)
    Copy,                  // 深拷贝
    Crop,                  // 去 padding
    Upsample,              // 最近邻或双线性
    FillZeros,             // 清零
    Accumulate,            // add_texture: 加权累加到 accumulator

    // ---- Align ----
    TileAlign,             // 多尺度: tile_diff → find_best → upscale → correct_error
    Warp,                  // 按 alignment 向量做双线性 warp (Bayer/X-Trans 分支)

    // ---- Merge (Spatial) ----
    ColorDifference,       // 超像素间各通道绝对差之和
    ComputeMergeWeight,    // weight = 1 / (1 + (diff/noise_sd)^2 * robustness)
    AddWeighted,           // ref * weight + comp * (1-weight)

    // ---- Merge (Frequency) ----
    ForwardFFT,            // 8×8 tile FFT 或通用 DFT
    FreqMerge,             // Wiener 滤波: d^2 / (d^2 + noise) 收缩
    Deconvolute,           // 简单去卷积
    BackwardFFT,           // IFFT
    ReduceArtifacts,       // Tile 边界伪影抑制
    CalculateRMS,          // 每 tile 均方根
    CalculateMismatch,     // |ref - comp| / RMS
    NormalizeMismatch,     // 除以均值
    CalcHighlightsNorm,    // 高光归一化

    // ---- Temporal Average ----
    AddTextureHighlights,  // 带高光外推的累加
    AddTextureExposure,    // 曝光加权累加
    Normalize,             // 除以 per-pixel norm

    // ---- Exposure ----
    ExposureLinear,        // 线性色调映射
    ExposureCurve,         // Reinhard 曲线
    TextureMax,            // 全局最大值 (← SyncPoint: GPU readback)
    TextureMean,           // 全局或 per-channel 均值 (← SyncPoint: GPU readback)

    // ---- Format ----
    FloatToUint16,         // R32_Float → R16_Uint, 含 16-bit scaling
};
```

### 3. 显存生命周期

一次 N 帧 burst，GPU 上最多同时驻留：
- 1 个参考帧金字塔 (3-4 级 ≈ 原始图像的 ~1.3x)
- 1 个对比帧 (原始大小)
- 1 个累加器 (原始大小)
- 中间临时缓冲 (几 MB)

VRAM ≈ 4~5× 单帧大小 ≈ ~600MB @ 24MP。

```cpp
// libburstmerge/include/burstmerge/internal/core/image_buffer.h

enum class MemoryLocation { Host, Device };
enum class PixelFormat    { R16_Uint, R32_Float, RGBA32_Float };
enum class BufferType     { Staging, DeviceLocal };

struct HostBuffer {
    std::byte*  data;
    size_t      size;
    PixelFormat format;
    uint32_t    width, height;
    uint32_t    row_stride;
};

struct DeviceBuffer {
    uint64_t       handle;       // VkImage / VkBuffer (opaque)
    BufferType     type;
    PixelFormat    format;
    uint32_t       width, height, depth = 1;  // depth > 1 用于 3D tile-diff 纹理
    MemoryLocation location;
};
```

**数据流** (含 Vulkan 格式清洗):

```
输入 → DngReader → HostBuffer (R16_Uint, CPU)
                       │ Staging Upload
                       │ ★ Vulkan 后端: 上传时自动将 R16_Uint 清洗为 R32_Float
                       │   (避免 VK_FORMAT_R16_UINT 作为 storage image 的兼容性问题)
                       ▼
                DeviceBuffer (R32_Float / RGBA32_Float, DeviceLocal)
                       │ 所有中间计算: 金字塔/对齐/融合/曝光
                       │ 全程 float, 绝不出整型纹理
                       ▼  Staging Download (R32_Float → CPU)
                       │ ★ CPU 端截断打包为 uint16_t → DngWriter
                HostBuffer (最终 uint16 Bayer)
                       → DngWriter → 输出 DNG
```

**CPU 后端**: `DeviceBuffer` 退化为 `HostBuffer` 的别名，无格式转换开销，直接指针运算。

### 4. DNG I/O 隔离层 (含不透明指针)

**丢弃 `vector<uint8_t> opcode_list3` 的 byte 搬运方案**。使用不透明指针保留 DNG SDK 原生对象树。

```cpp
// libburstmerge/include/burstmerge/internal/io/dng_io.h

// 核心系统只认这些纯结构体 (完全不依赖 DNG SDK 头文件)
struct RawMetadata {
    uint32_t  width, height;
    uint32_t  mosaic_pattern_width;   // 2=Bayer, 6=X-Trans
    uint16_t  mosaic_pattern[36];
    uint32_t  white_level;
    float     black_level[4];
    float     color_factors[4];
    float     exposure_bias;
    float     iso_exposure_time;

    // ★ 不透明指针: 指向 dng_negative 内部实例
    void*     internal_dng_negative = nullptr;
};

struct RawImage {
    RawMetadata  metadata;
    HostBuffer   pixels;
};

class DngReader {
public:
    explicit DngReader(const char* path);
    RawImage Read();
    // 内部构造 dng_negative → metadata.internal_dng_negative
};

class DngWriter {
public:
    explicit DngWriter(void* ref_negative);  // 接管不透明指针
    void Write(const char* out_path, const RawImage& image);
    // 内部: pixels → dng_image → ref_negative->SetRawImage → WriteDNG
    // 完美透传 OpcodeList, CameraProfile, EXIF, XMP...
};
```

### 5. ComputeBackend 虚基类

```cpp
// libburstmerge/include/burstmerge/internal/compute/compute_backend.h

class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;

    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual void Synchronize() = 0;

    virtual DeviceBuffer CreateDeviceTexture(uint32_t w, uint32_t h, uint32_t d,
                                              PixelFormat fmt) = 0;
    virtual DeviceBuffer CreateStaging(uint32_t w, uint32_t h, PixelFormat fmt) = 0;
    virtual void DestroyBuffer(DeviceBuffer& buf) = 0;
    virtual void Upload(const HostBuffer& src, DeviceBuffer& dst) = 0;
    virtual void Download(const DeviceBuffer& src, HostBuffer& dst) = 0;
    virtual void Copy(const DeviceBuffer& src, DeviceBuffer& dst) = 0;

    // 局部 DAG 执行入口
    virtual void ExecuteSubGraph(const SubGraph& graph,
                                 const Settings& settings) = 0;
    // CPU:   遍历 nodes, 每节点同步调用内部函数, OpenMP 内并行
    // Vulkan: 编译为单 CommandBuffer, 一次性提交 Queue + Barriers
};
```

### 6. GPU 同步点 (SyncPoint)

原项目有多处 GPU→CPU 的同步读回，在架构中必须显式标注为 SyncPoint 节点：

| SyncPoint | 用途 | 读回数据量 | 所在路径 |
|-----------|------|-----------|---------|
| `TextureMean` | noise_sd 计算: color diff 均值 | 1 float (或 4 floats per-channel) | 空域融合 |
| `TextureMax` | exposure correction 全局最大值 | 1 float | 曝光校正 |
| `TextureMean` | hotpixel 检测: 各通道均值 | mosaic_width² floats | 热像素 |
| `TextureMean` | mismatch 归一化均值 | 1 float | 频域融合 |

**Vulkan 后端处理**: 这些节点在 ExecuteSubGraph 时触发 `vkQueueSubmit` + `vkWaitIdle`，读回 buffer，再继续录制后续 CommandBuffer。**接受非纯一次性提交**——这是算法固有特性，无可避免。

**CPU 后端处理**: 无特殊操作，`texture_mean` 直接 `std::accumulate` 即可。

---

## Pipeline 算法流程

```
① 路径校验 (扩展名一致, 数量 ≥ 2)

② RAW→DNG 转换 (条件编译)
   └─ #ifdef _WIN32: 调用 Adobe DNG Converter CLI 并行转换非 DNG 文件
   └─ #else: 仅接受 .dng 输入, 非 .dng 报错提示用户预转换

③ DngReader 加载 DNG → RawImage[]
   └─ internal_dng_negative (不透明指针)
   └─ white_level, black_level (含 masked areas)
   └─ mosaic_pattern, exposure_bias, ISO*time, color_factors

④ 选择参考帧
   └─ 非均匀曝光 → 最暗帧 / 均匀曝光 → 中间帧

⑤ 热像素检测 (所有帧平均 → 检测邻域偏离像素)

⑥ 融合路径 (Three-way):
   A) noise_reduction ≈ 23 → 简单时域平均
   B) frequency (仅 Bayer) → 4 次亚像素偏移 + 金字塔对齐 + FFT→Wiener→IFFT
   C) spatial (默认, 支持 X-Trans) → 金字塔对齐 + 鲁棒权重 + 加权平均
  └─ 以上均通过 流式逐帧 SubGraph 实现

⑦ 曝光校正 (可选: Linear / Reinhard Curve)

⑧ Float32 → UInt16 转换 (映射到 white_level)

⑨ DngWriter 输出 DNG
   └─ 接管 internal_dng_negative → 替换像素 → WriteDNG
```

---

## DNG/XMP SDK 编译对抗

### 已知问题 + 对策

| 问题 | 现象 | 对策 |
|------|------|------|
| 平台宏缺失 | `qWinOS`/`qMacOS`/`qLinux` 未定义 | CMake `-DqWinOS=1` |
| XMP 类型冲突 | XMP INT64 与 MinGW stdint.h 冲突 | include 前 `#define XMP_INCLUDE_XMP_IS_MACRO` |
| MSVC SEH | `__try/__except` | MinGW 需 #ifdef 移除 |
| FILE_OFFSET_BITS | 大文件支持缺失 | `-D_FILE_OFFSET_BITS=64` |
| DLL 导入声明 | `DLL_EXPORT`/`DLL_IMPORT` | 静态链接: `-DDNG_SDK_STATIC` 定义为空 |

### 3rdparty/dng_sdk/CMakeLists.txt

```cmake
add_library(dng_sdk STATIC ${DNG_SDK_SRCS} ${LIBJPEG_SRCS})

target_include_directories(dng_sdk SYSTEM PRIVATE
    "${PROJECT_ROOT}/3rdparty/dng_sdk"
    "${PROJECT_ROOT}/3rdparty/dng_sdk/xmp_headers"
    "${PROJECT_ROOT}/3rdparty/dng_sdk/libjpeg"
)

target_compile_definitions(dng_sdk PRIVATE
    $<$<PLATFORM_ID:Windows>:qWinOS>
    $<$<PLATFORM_ID:Linux>:qLinux>
    $<$<PLATFORM_ID:Darwin>:qMacOS>
    DNG_SDK_STATIC _FILE_OFFSET_BITS=64 _LARGEFILE64_SOURCE
)

# 符号隔离: 隐藏 dng_sdk 及其内嵌 libjpeg 的所有符号
# 防止与上层应用（如 Qt 的 libjpeg）发生符号冲突
set_target_properties(dng_sdk PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
    CXX_VISIBILITY_PRESET hidden
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dng_sdk PRIVATE
        -fpermissive -w -Wno-deprecated
        -Wno-enum-compare -Wno-format-truncation
        -fvisibility=hidden                    # ★ 隐藏 libjpeg/XMP 符号
        -fvisibility-inlines-hidden)
endif()
if(MSVC)
    target_compile_options(dng_sdk PRIVATE /W0 /wd4996)
endif()
```

---

## 依赖清单

| 组件 | 链接 | 位置 |
|------|------|------|
| Adobe DNG SDK | 静态链接 | 3rdparty/dng_sdk/ |
| PocketFFT | header-only | 3rdparty/pocketfft/ |
| cxxopts | header-only | 3rdparty/cxxopts/ |
| Vulkan SDK | 动态 (vulkan-1.dll) | 3rdparty/vulkan/ (MinGW, 可选) |
| OpenMP | 动态 (libgomp.dll) | MinGW runtime |
| CTest | CMake 内置 | 无需三方依赖 |
| C++17 | 语言标准 | GCC ≥ 8, Clang ≥ 7, MSVC ≥ 2017 |

---

## 额外注意点

### Windows 路径编码

MinGW std::string 为 UTF-8，Windows API 需要 UTF-16:
```cpp
// dng_sdk_bridge.cpp
#ifdef _WIN32
static std::wstring Utf8ToWide(const char* utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring buf(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf.data(), len);
    return buf;
}
#endif
```

### Vulkan SDK on MinGW

Vulkan SDK 头文件和库手动放入 `3rdparty/vulkan/`，路径在 `local_config.cmake` 集中配置，无需依赖系统环境变量。

### OpenMP + MinGW

```cmake
find_package(OpenMP REQUIRED)
target_link_libraries(burstmerge PRIVATE OpenMP::OpenMP_CXX)
# 分发时附带 libgomp-1.dll (从 MinGW bin/ 拷贝到 3rdparty/openmp/)
```

### DNG SDK 线程安全

`dng_host` 非线程安全。并行加载多个 DNG 时每线程独立构造 `dng_host` 实例。

### PocketFFT 线程安全

PocketFFT 的 `pocketfft_plan` 内部有临时工作缓冲区，不可多线程共享。CPU 后端频域融合中，每 Tile 的 FFT 在 OpenMP 多线程下执行时，**必须使用线程局部存储 (thread-local)** 持有 FFT plan。

```cpp
// fft.cpp: 每次调用前获取或创建当前线程的 FFT plan
static thread_local std::unordered_map<plan_key_t, PocketFFTPlan> tls_plans;
```

### Vulkan 上传格式清洗

Vulkan 后端 `Upload()` 实现在 Staging Buffer → Device Texture 的拷贝过程中，将 Host 端 `R16_Uint` 数据自动转换为 `R32_Float` 纹理。原因：

- `VK_FORMAT_R16_UINT` 作为 Storage Image 在中低端 GPU 上支持不完整
- 原项目在 `prepare_texture()` 中也是 uint16→float32 转换，语义一致
- GPU 内所有中间运算统一使用 float，避免混合精度问题

```cpp
// staging_buffer.cpp (Vulkan 后端)
void Upload(const HostBuffer& src, DeviceBuffer& dst) {
    // 1. src.format == R16_Uint, dst.format == R32_Float
    // 2. Staging buffer 中拷贝 uint16_t* → 在 device shader 中转换为 float
    //    或在 staging → device 的 copy 命令中插入 vkCmdBlitImage 转换
    // 3. 对于 CPU 后端, Upload 是纯 memcpy (DeviceBuffer == HostBuffer)
}
```

---

## 构建阶段规划

```
阶段 0: 基础设施
  ├── CMake 项目骨架 + local_config.cmake 集中配置
  ├── 3rdparty 目录初始化 (dng_sdk, pocketfft, cxxopts, googletest, vulkan)
  ├── 3rdparty/dng_sdk/CMakeLists.txt (对抗 GCC/MinGW 编译)
  ├── DngReader / DngWriter (含不透明指针)
  ├── api.h + api_c.h 接口
  ├── DngConverter (Windows 条件编译, CLI 调用)
  └── UTF-8→UTF-16 路径编码层

阶段 1: CPU 后端 (算法验证)
  ├── HostBuffer / DeviceBuffer (CPU: DeviceBuffer = HostBuffer)
  ├── texture_ops.cpp (blur, copy, upsample, crop, convert, prepare)
  ├── fft.cpp (PocketFFT + 线程局部 plan 缓存)
  ├── Align (多尺度金字塔 + 3D tile-diff + 最佳位移 + Warp)
  ├── Merge — spatial.cpp + frequency.cpp (含频域 4-位移外循环)
  ├── TemporalAverage + ExposureCorrection (含 TextureMean/Max SyncPoint)
  ├── SubGraph + PipelineOrchestrator (SetupPhase + PerFramePhase)
  └── test/ 单元测试 (CTest: 独立可执行文件 + add_test 注册)
  │   └── 退出码 0=通过, 非0=失败
  │   └── 双后端一致性: CPU_Result ≈ Vulkan_Result (EPSILON=1/65535)

阶段 2: Vulkan 后端 (性能优化)
  ├── VulkanDevice / PipelineCache
  ├── DeviceBuffer (含 depth 字段, 支持 3D tile-diff 纹理)
  ├── Upload 格式清洗层 (R16_Uint → R32_Float 自动转换)
  ├── SyncPoint 处理 (vkQueueWaitIdle + buffer readback)
  ├── ResourcePool (显存上限管理)
  ├── 移植 Metal shader → GLSL compute shader (~30 个)
  └── ExecuteSubGraph → 含 Barrier 的 CommandBuffer 提交

阶段 3: 双后端一致性测试
  ├── Ground Truth 数据 (从原 Swift 项目 dump 中间 tile)
  └── 扩展 test/: GPU 后端加入后, 同一套 test_*.cpp 运行两次
       └── ctest -R cpu   → 仅有 CPU 后端的测试
       └── ctest -R vulkan → 仅有 Vulkan 后端的测试
       └── ctest           → 全部

阶段 4: 前端
  ├── CLI (cxxopts)
  ├── Interactive Console (readline REPL)
  └── GUI (Qt6/imgui, 后期)
```
