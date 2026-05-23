# 非 RAW 格式支持计划 (v4)

## 核心原则

- **编码规范**：统一 Microsoft/Allman 缩进风格（大括号独占一行），缩进 4 空格
- **不引入 stb_image**，每种格式由对应专用库处理
- **Decoder/Writer 接口抽象化**，新增格式只需注册一行
- **管线分支**：RAW 和非 RAW 走不同路径，不做"进入函数再跳过"的拼凑
- **输出精度统一参数**：`--bit-depth` 管全部（8/10/12/14/16）
- **非 RAW 暂不做曝光合成**
- **非 RAW 不做 OETF 反向矫正**，像素值 as-is 进 float32
- **非 RAW 路线目标"能跑通"**，后续根据实际行为偏差再调整
- **RAW → 非 DNG 输出**：不做去马赛克/白平衡/色适应，白点归一化后直接以 Bayer 灰度图形式写出

---

## 一、新增依赖

| 依赖 | MinGW 包 | 用途 |
|---|---|---|
| **libjpeg-turbo** | `libjpeg-turbo` | JPEG 解码/编码 |
| **libpng** | `libpng` + `zlib` | PNG 解码/编码 (含 16-bit) |
| **libtiff** | `libtiff` | TIFF 解码/编码 |
| **zlib** | (MinGW 自带或 vcpkg) | libpng / libtiff 传递依赖 |

**BMP**：不自带外部依赖，自写 ~80 行。

### Build 集成方式

各库路径**全部由 `local_config.cmake` 管理**，不硬编码 `3rdparty/` 路径。

```
local_config.cmake 提供:
  JPEG_ROOT      → libjpeg-turbo 安装目录
  PNG_ROOT       → libpng 安装目录
  TIFF_ROOT      → libtiff 安装目录
  ZLIB_ROOT      → zlib 安装目录 (可选, 通常随 MinGW)
```

CMake 策略：
```cmake
# 1. 先查 local_config 提供的路径
# 2. 再 fallback 到 find_package (系统/vcpkg)
# 3. 都没有则报错提示

if(JPEG_ROOT AND EXISTS "${JPEG_ROOT}")
    # 从 JPEG_ROOT 配置
else()
    find_package(JPEG REQUIRED)
endif()
# PNG, TIFF 同理
```

依赖库源代码**不随项目分发**——用户/CI 确保通过 vcpkg / pacman / 手动放入 手动放入时还需配置 `local_config.cmake`。

---

## 二、Decoder 体系

### RAW 输入预处理（现有流程）

项目当前对非 DNG 相机 RAW（`.arw`/`.cr2`/`.nef` 等）的入口依赖 **Adobe DNG Converter CLI**（仅 Windows）：

```
输入 .arw/.cr2/... → Adobe DNG Converter CLI → 临时 .dng → DngDecoder
```

现有 `src/io/dng_converter.cpp` 封装了 CLI 调用（条件编译 `#ifdef _WIN32`），输入管线的第一步仍保留此环节：
- 若输入含非 `.dng` 扩展名的 RAW 文件 → 调用 Adobe DNG Converter 并行转换为临时 DNG
- 若所有输入已经是 `.dng` → 跳过此步骤
- 非 Windows 平台：仅接受 `.dng` 输入，非 `.dng` 报错提示用户预转换

DngDecoder 只处理 `.dng`（包括原生 DNG 和转换后的）。

### 接口

**`include/burstmerge/internal/io/image_decoder.h`**

```cpp
// 像素格式编码: 高 2 byte = 类别, 低 2 byte = 通道数
// 类别 0 (通用 RGB 系):
//   低 2 byte = 通道数: 1=灰, 2=灰+Alpha, 3=RGB, 4=RGBA
// 高 2 byte = 类别: 0x0000 = 通用, 0x0001+ 保留给 YUV/CMYK/XYZ 等
using PixelFormat = uint32_t;
constexpr PixelFormat kPixelGray  = 0x00000001;
constexpr PixelFormat kPixelGA    = 0x00000002;
constexpr PixelFormat kPixelRGB   = 0x00000003;
constexpr PixelFormat kPixelRGBA  = 0x00000004;

// 图像元数据基座, 每种格式可扩展
struct ImageMetadata
{
    // 基础维度
    uint32_t  width       = 0;
    uint32_t  height      = 0;
    PixelFormat pix_fmt   = kPixelGray;
    uint32_t  bit_depth   = 8;     // 整数位深
    bool      is_float    = false; // 浮点格式 (OpenEXR)

    // RAW 元数据
    float     iso_exposure_time   = 0.0f;
    float     exposure_bias       = 0.0f;
    float     white_level         = 255.0f;
    float     black_level[4]      = {};
    uint32_t  mosaic_pattern_width = 0;
    bool      is_raw              = false;

    // DNG SDK 句柄 (仅 RAW)
    // DngNegativeHolder 定义于 dng_sdk_bridge.h, shared_ptr 共享所有权
    std::shared_ptr<io::DngNegativeHolder> dng_negative;

    // 图像元数据 (EXIF等), 自由键值对
    // key = "Make", "Model", "Orientation", "ColorSpace", ...
    std::map<std::string, std::string> tags;
};

struct DecodedImage
{
    ImageMetadata info;
    std::vector<float> pixels;  // interleaved, [0, white_level)
};

class ImageDecoder
{
public:
    virtual ~ImageDecoder() = default;
    virtual bool CanDecode(const std::string& path) = 0;
    virtual DecodedImage Decode(const std::string& path) = 0;
};

std::unique_ptr<ImageDecoder> SelectDecoder(const std::string& path);
```

### 实现

| 类 | 文件 | 依赖 | 格式 | 要点 |
|---|---|---|---|---|
| `DngDecoder` | `src/io/dng_decoder.cpp` | dng_sdk | DNG | 包装现有 DngReader，填入 RAW 元数据 |
| `JpegDecoder` | `src/io/jpeg_decoder.cpp` | libjpeg-turbo | JPEG | 8-bit，**不做 OETF 反转**；灰度/RGB |
| `PngDecoder` | `src/io/png_decoder.cpp` | libpng + zlib | PNG | 8/16-bit，灰度/RGB/RGBA |
| `BmpDecoder` | `src/io/bmp_decoder.cpp` | — | BMP | 自写，读 24-bit 无压缩 BMP |
| `TiffDecoder` | `src/io/tiff_decoder.cpp` | libtiff | TIFF | 8/16-bit，RGB/灰度，LZW/Deflate |

### SelectDecoder

```cpp
switch (ext):
    ".dng"            → DngDecoder
    ".jpg" / ".jpeg"  → JpegDecoder
    ".png"            → PngDecoder
    ".bmp"            → BmpDecoder
    ".tif" / ".tiff"  → TiffDecoder
    // future:
    ".exr"            → ExrDecoder
    ".dpx"            → DpxDecoder
```

---

## 三、Writer 体系

### 接口

**`include/burstmerge/internal/io/image_writer.h`**

```cpp
enum class OutputFormat
{
    Auto,   // 自动推断 (默认值, 见优先级规则)
    PNG,
    JPEG,
    BMP,
    TIFF,
    DNG
};

struct WriteParams
{
    OutputFormat format;
    uint32_t     bit_depth;     // 8/10/12/14/16
};

class ImageWriter
{
public:
    virtual ~ImageWriter() = default;
    virtual bool CanWrite(OutputFormat fmt, uint32_t bit_depth) = 0;
    virtual void Write(const std::string& path,
                       const FloatImage& image,
                       const WriteParams& params) = 0;
};

std::unique_ptr<ImageWriter> SelectWriter(OutputFormat fmt);
```

### 实现

| 类 | 文件 | 依赖 | 格式 | 位深 | 说明 |
|---|---|---|---|---|---|
| `JpegWriter` | `src/io/jpeg_writer.cpp` | libjpeg-turbo | JPEG | 仅 8 | quality=96, RGB/灰度 |
| `PngWriter` | `src/io/png_writer.cpp` | libpng + zlib | PNG | 8/16 | RGB/灰度/RGBA |
| `BmpWriter` | `src/io/bmp_writer.cpp` | — | BMP | 仅 8 | 自写，无压缩 |
| `TiffWriter` | `src/io/tiff_writer.cpp` | libtiff | TIFF | 8/16 | 无压缩 / LZW |
| `DngWriter` (改造) | `src/io/dng_writer.cpp` | dng_sdk | DNG | 8/10/12/14/16 | 适配 `FloatImage` 输入 |

### 位深 × 格式约束矩阵

| 输出格式 | 8 | 10 | 12 | 14 | 16 |
|---|---|---|---|---|---|
| PNG | ✓ | 报错 | 报错 | 报错 | ✓ |
| JPEG | ✓ | 报错 | 报错 | 报错 | 报错 |
| BMP | ✓ | 报错 | 报错 | 报错 | 报错 |
| TIFF | ✓ | 报错 | 报错 | 报错 | ✓ |
| DNG | ✓ | ✓ | ✓ | ✓ | ✓ |

---

## 四、管线重构

### RAW 管线（现有流程 + 非 DNG 输出分支）

```
所有输入 RAW (is_raw == true)
       │
       ▼
  BuildFloatImages (CFA 拆分)
  RepairHotPixels
  NormalizeFrames
       │
       ▼
  对齐 + 融合 (公共模块)
       │
       ▼
  ConvertPlaneImageToMosaic
  白电平缩放 + 黑电平恢复
       │
  ┌────┴────┐
  ▼         ▼
DngWriter  白点归一化 → 量化 → Writer
           (Bayer 灰阶, 不插值)
  │         │
  ▼         ▼
DNG 文件   PNG/JPEG/BMP/TIFF
```

**RAW → 非 DNG 约束：**
- `ConvertPlaneImageToMosaic` 后为单通道 Bayer 灰阶图
- 白点归一化直接输出，不做 demosaic/WB/色适应

### 非 RAW 管线

```
所有输入非 RAW (is_raw == false)
       │
       ▼
  BuildRgbImages (直接 FloatImage)
  (无热像素/黑电平/曝光归一化)
       │
  ⚠ gamma 域对齐/融合偏差已知
       │
       ▼
  对齐 + 融合 (公共模块)
       │
       ▼
  按 output_format + bit_depth 选 Writer
  PNG/JPEG/BMP → 对应 Writer
  TIFF → TiffWriter (libtiff)
  DNG → 报错
       │
       ▼
  PNG/JPEG/BMP/TIFF 文件
```

### 管线选择

```
if all(is_raw)    → RAW 管线
elif all(!is_raw) → RGB 管线
else → warn 非RAW跳过 → 过滤 → RAW 管线
```

### `output_format` 推断优先级

```
Settings.output_format 默认值 = OutputFormat::Auto

推断链 (PipelineOrchestrator::Process):
  1. 若 output_format != Auto → 直接使用该值 (不警告)
  2. 若 output_format == Auto:
     a. InferFormatFromExtension(output_path_or_dir, fallback)
        - 匹配输出文件名扩展名:
          .png            → PNG
          .jpg / .jpeg    → JPEG
          .bmp            → BMP
          .tif / .tiff    → TIFF
          .dng            → DNG
        - 匹配成功 → 警告 "inferred {format} from filename extension"
        - 无匹配 → 走 fallback:
          - 全 RAW   → DNG  (警告 "defaulting to DNG")
          - 非 RAW   → PNG  (警告 "defaulting to PNG")
          - 混合输入 → 先过滤非RAW（警告 "Skipping non-RAW file"），
                       剩余 RAW 走 RAW 管线

WriteImage 内部格式决议也使用相同逻辑:
  InferFormatFromExtension(path, InferOutputFormat(settings, all_raw))
  确保实际写出的格式与输出文件扩展名一致。

约束:
  - DNG + 非 RAW 输入 → 报错 "Cannot output DNG for non-RAW inputs"
  - JPEG / BMP 的 AdjustBitDepth → 强制降至 8-bit 并警告
  - PNG / TIFF 的 AdjustBitDepth → 8-bit 或 16-bit (10/12/14 不可用, 降至 8bit 并警告)
```

规则表：

| 输入 | 用户指定 | 输出文件扩展名 | 实际输出 | 警告 |
|---|---|---|---|---|
| 全 RAW | `Auto` | `(无匹配/目录)` | DNG | "defaulting to DNG" |
| 全 RAW | `Auto` | `.jpg` | JPEG | "inferred JPEG from filename extension" |
| 全 RAW | `Auto` | `.png` | PNG | "inferred PNG from filename extension" (Bayer mosaic) |
| 全 RAW | `PNG/JPEG/BMP/TIFF` | (任意) | 按指定 | 无 |
| 全 RAW | `DNG` | (任意) | DNG | 无 |
| 全非 RAW | `Auto` | `(无匹配/目录)` | PNG | "defaulting to PNG" |
| 全非 RAW | `Auto` | `.jpg` | JPEG | "inferred JPEG from filename extension" |
| 全非 RAW | `Auto` | `.dng` | 报错 | "Cannot output DNG for non-RAW" |
| 全非 RAW | `PNG/JPEG/BMP/TIFF` | (任意) | 按指定 | 无 |
| 全非 RAW | `DNG` | (任意) | 报错 | "Cannot output DNG for non-RAW" |
| 混合 | 任意 | — | 过滤→RAW管线 | "Skipping non-RAW file" |

---

## 五、Settings / CLI 变更

### Settings (`api.h`)

```cpp
// bit_depth 扩展至 {8, 10, 12, 14, 16}
// output_format 默认 Auto (自动推断)
OutputFormat output_format = OutputFormat::Auto;
```

### CLI (`apps/cli/main.cpp`)

```
--output-format auto|png|jpg|bmp|tiff|dng
    (auto = 自动推断, 默认)
    (注意: 非RAW输入不能指定 dng)

--bit-depth 8|10|12|14|16
    (扩展, 原仅12/14/16)
```

### 输出摘要

```
Output format: TIFF
Bit depth: 16
Output tags:
  Make: Sony
  Model: ILCE-7M4
```

---

## 六、文件变更总表

| # | 文件 | 操作 |
|---|---|---|
| 1 | `include/burstmerge/internal/io/image_decoder.h` | **新建** |
| 2 | `include/burstmerge/internal/io/image_writer.h` | **新建** |
| 3 | `src/io/dng_decoder.cpp` | **新建** |
| 4 | `src/io/jpeg_decoder.cpp` | **新建** |
| 5 | `src/io/png_decoder.cpp` | **新建** |
| 6 | `src/io/bmp_decoder.cpp` | **新建** |
| 7 | `src/io/tiff_decoder.cpp` | **新建** |
| 8 | `src/io/jpeg_writer.cpp` | **新建** |
| 9 | `src/io/png_writer.cpp` | **新建** |
| 10 | `src/io/bmp_writer.cpp` | **新建** |
| 11 | `src/io/tiff_writer.cpp` | **新建** |
| 12 | `src/io/image_decoder.cpp` | **新建** — SelectDecoder + ReadImage |
| 13 | `src/core/pipeline.cpp` | 重构 — 分支管线 + Writer 派发 |
| 14 | `src/core/pipeline_frame.cpp` | 修改 — 新增 `BuildRgbImages` |
| 15 | `src/core/pipeline_io.cpp` | 修改 — 保留 Adobe DNG Converter 预处理, 其后接 ReadImage 派发 |
| 16 | `include/burstmerge/api.h` | 修改 — Settings 加 output_format; bit_depth 扩展 |
| 17 | `apps/cli/main.cpp` | 修改 — --output-format + 校验 |
| 18 | `local_config.cmake` | 修改 — 加 JPEG/PNG/TIFF ROOT 路径变量 |
| 19 | `src/io/dng_converter.cpp` | **保留** — 非 DNG RAW → Adobe DNG Converter 调用, 接入新管线输入侧 |

### 无需修改

- `src/merge/spatial.cpp`
- `src/merge/frequency.cpp`
- `src/align/*.cpp`
- `src/core/pipeline_align.cpp`
- `src/exposure/exposure.cpp`

---

## 七、格式 × 库对照表

| 格式 | 读 (Decoder) | 写 (Writer) | 读库 | 写库 |
|---|---|---|---|---|
| DNG | `DngDecoder` | `DngWriter` | dng_sdk | dng_sdk |
| JPEG | `JpegDecoder` | `JpegWriter` | libjpeg-turbo | libjpeg-turbo |
| PNG | `PngDecoder` | `PngWriter` | libpng+zlib | libpng+zlib |
| BMP | `BmpDecoder` | `BmpWriter` | 自写 ~50行 | 自写 ~30行 |
| TIFF | `TiffDecoder` | `TiffWriter` | libtiff | libtiff |
| DPX | 待定 | 暂不支持 | — | — |
| OpenEXR | 待定 | 暂不支持 | — | — |

---

## 八、决策记录

| # | 主题 | 决定 |
|---|---|---|
| 1 | 不引入 stb_image | 用户明确要求 |
| 2 | 每种格式专用库 | libjpeg-turbo / libpng+zlib / libtiff |
| 3 | BMP 自写 | 格式极简，不依赖三方库 |
| 4 | 非 RAW 不做 OETF 反转 | 像素 as-is 进 float32 |
| 5 | 非 RAW 不做曝光归一化 | 像素值->相对亮度依赖 CMS，现阶段不引入 |
| 6 | 管线分支不 skip | 分支结构，不做"进入再跳过" |
| 7 | RAW→非DNG 不做 demosaic/WB | 直接输出 Bayer mosaic |
| 8 | --bit-depth 统管全部 | 8/10/12/14/16，格式约束决定能用到哪 |
| 9 | 混合输入过滤非RAW | 警告后剩RAW，走RAW管线 |
| 10 | output_format 默认 Auto | `Auto` 为默认值，显式指定 DNG 与自动推断 DNG 可区分 |
| 11 | CLI 支持 `auto` 和 `dng` | `--output-format auto\|png\|jpg\|bmp\|tiff\|dng` |
| 12 | `dng_negative` 类型 | `std::shared_ptr<io::DngNegativeHolder>`，见 `dng_sdk_bridge.h` |
| 13 | 依赖路径由 `local_config.cmake` 管理 | 不硬编码 `3rdparty/` |
| 14 | 元数据传递 | `ImageMetadata::tags` (std::map<string,string>)，各 Decoder 填充 |
| 15 | PixelFormat 编码 | 高 2 byte = 类别, 低 2 byte = 通道数 |
| 16 | 16-bit PNG 支持 | 保留, PngWriter 支持 8/16-bit |
| 17 | 缩进规范 | Microsoft/Allman 风格 (大括号独占一行), 4 空格缩进 |
