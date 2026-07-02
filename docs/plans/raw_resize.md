# raw_resize: 单帧 RAW 缩放工具

## 目的
构建一个新的 CLI 工具，对单帧 RAW (DNG) 进行尺寸缩放，输出 DNG。

## 管线
```
输入 DNG
  → DngReader::Read() → RawImage (mosaic, 1ch, W×H)
  → HostBufferToFloatImage() → FloatImage mosaic
  → ConvertMosaicToPlaneImage(mosaic, period) → FloatImage plane (ch=period², W/p×H/p)
  → ResizeImage(plane, dst_plane_w, dst_plane_h, method) → FloatImage resized
  → ConvertPlaneImageToMosaic(resized, dst_mosaic_w, dst_mosaic_h, period) → FloatImage out
  → FloatImageToUint16HostBuffer(out, white_level) → HostBuffer
  → RawImage { metadata=move(original.metadata), pixels=move(hb) }
  → metadata.width/height 更新为 dst_mosaic_w/dst_mosaic_h
  → DngWriter::Write() → 输出 DNG
```

LinearRaw (3ch, 无 CFA) 路径:
```
  输入 DNG (LinearRaw)
  → FloatImage RGB (3ch, W×H)
  → ResizeImage(rgb, dst_w, dst_h, method)
  → FloatImageToUint16HostBuffer → HostBuffer
  → RawImage + ClearDngMosaicInfo() + DngWriter::Write()
```

## 维度换算

关键公式:
```
plane_w = (mosaic_w + period - 1) / period     // 向上取整
plane_h = (mosaic_h + period - 1) / period

// 用户指定输出 mosaic 尺寸 → 计算目标 plane 尺寸:
dst_plane_w = dst_mosaic_w / period           // 必须整除
dst_plane_h = dst_mosaic_h / period
```

约束: 输出 mosaic 尺寸必须是 period 的整数倍 (Bayer=2)，否则向下取整到最近的倍数。

## 新增文件

| 文件 | 说明 |
|------|------|
| `apps/raw_resize/main.cpp` | CLI 入口: cxxopts 解析, 管线编排 |
| `apps/raw_resize/CMakeLists.txt` | 构建目标, 同 `apps/cli/CMakeLists.txt` 模式 |

## 修改文件

| 文件 | 修改 |
|------|------|
| `libburstmerge/include/burstmerge/internal/core/float_image.h` | 加 `InterpolationMethod` 枚举 + `ResizeImage()` 声明 |
| `libburstmerge/src/core/float_image.cpp` | 加 `ResizeImage()` 实现 (双线性 + 双立方 Catmull-Rom) |
| `CMakeLists.txt` | 加 `add_subdirectory(apps/raw_resize)` |

## ResizeImage 实现

```cpp
enum class InterpolationMethod { Bilinear, Bicubic };

FloatImage ResizeImage(const FloatImage& src,
                       uint32_t dst_width, uint32_t dst_height,
                       InterpolationMethod method);
```

- 每通道独立缩放 (NHWC)
- 双线性: 2×2 加权平均
- 双立方: Catmull-Rom (a=-0.5), 4×4 邻域
- 边界: clamp-to-edge
- 并行: OpenMP ParallelForRows (同项目风格)

## CLI 参数

```
-i, --input      输入 DNG/RAW 路径 (必需)
-o, --output     输出 DNG 路径 (默认 ./out.dng)
-W, --width      输出 mosaic 宽度 (px, period 整数倍)
-H, --height     输出 mosaic 高度 (px, period 整数倍)
--scale          统一缩放因子 (-W/-H 与 --scale 二选一)
--interp         插值: bilinear | bicubic (默认 bicubic)
--bit-depth      输出位深: 8/10/12/14/16 (默认 16)
-h, --help       显示帮助
```
