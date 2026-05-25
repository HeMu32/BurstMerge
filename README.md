## Brief

This is a computational photography tool for multi-frame noise reduction and exposure bracketing.
It is inspired by the project https://github.com/martin-marek/hdr-plus-swift and based on Google HDR+.
For more detail, see the HDR+ paper: https://hdrplusdata.org/en//hdrplus.pdf

**Platform:** GCC (MinGW, Windows)

## Features

### Core capabilities

- RAW-in RAW-out: DNG RAW output support
- DNG RAW input support
- Camera manufacturer proprietary RAW format support (via Adobe DNG Converter)
- Common RGB picture formats input/output support (PNG, TIF, etc.)
    - Note: RAW-in RGB-out and RGB-in RAW-out are not supported
- Bayer color filter array support
- CLI application

### Merge and processing modes

- Simple temporal averaging merge
- Motion-robust merge in the spatial domain
- Motion-robust merge in the frequency domain
- Optional exposure correction for improved shadow tonality
- Burst support with bracketed exposure (not applicable for common RGB formats)

### Output precision

- Selectable 10/12/14/16 bit precision for DNG output
- Selectable 8/16 bit precision for TIFF and PNG output

### Performance and platform support

- CPU processing support
- Multi-threaded RAW decoding
- Basic CPU performance optimizations for:
    - DNG decoding
    - picture alignment
    - merging
- Folder-based sequence reading
- Windows 10 support (via MinGW-W64 9.0.0)

## TODOs

- GUI application
- OpenEXR support
- DPX support
- Hot pixel suppression (current implementation not working well)
- Preserve lens correction profiles (via exiftool, Sony ARW only)
- Support working together with Adobe LrC/ACR AI noise reduction
- Support working together with DxO PureRAW
- Support images with more channels
- Non-Bayer sensor support
- Multi-threaded RAW conversion and image loading
- Better alignment algorithm for extreme exposure bracket range
- More constrained alignment algorithms: perspective, ...
- Simple median and mid-percentage merge options
- More texture-preserving and noise-robust frame merging algorithm
- Vulkan processing support
- Support for additional operating systems
- CJK path support
- User-configurable cache folder path
- User-configurable reference frame selection
- Progress reporting for frame merging
- Performance optimizations for black level and normalization processing on CPU

## Dependency

- Standard C++17
- libjpeg-turbo
- libpng
- libtiff
- zlib
- OpenMP
- Adobe DNG SDK
- Pocket FFT

## Build Guide

### Scope

- Supported platform: Windows
- Supported toolchain: MinGW-w64 GCC / G++
- Compatibility with MSVC was not tested
- Default generator used by the current project: `MinGW Makefiles`

The top-level build is driven by `CMakeLists.txt`. The actual dependency paths are centralized in `local_config.cmake`, and the core build currently expects several libraries to be available under `C:/MinGW`.

### Required tools

- CMake 3.16 or newer
- MinGW-w64 with `gcc`, `g++`, `mingw32-make`, and OpenMP support
- A working shell environment where the MinGW `bin` directory is on `PATH`

Example expected compiler paths in the current repository setup:

```text
C:/MinGW/bin/gcc.exe
C:/MinGW/bin/g++.exe
C:/MinGW/bin/mingw32-make.exe
```

### Third-party layout expected by the project

The repository already vendors several dependencies under `3rdparty/`, including:

- `3rdparty/dng_sdk`
- `3rdparty/pocketfft`
- `3rdparty/cxxopts`
- `3rdparty/libtiff`

`local_config.cmake` currently assumes the following locations:

- `libjpeg-turbo`: `C:/MinGW/include` and `C:/MinGW/lib/libjpeg.a`
- `libpng`: `C:/MinGW/include` and `C:/MinGW/lib/libpng.a`
- `zlib`: `C:/MinGW/include` and `C:/MinGW/lib/libz.a`
- `libtiff`: `3rdparty/libtiff/install/include` and `3rdparty/libtiff/install/lib/libtiff.dll.a`

If your MinGW installation is not under `C:/MinGW`, update `local_config.cmake` before configuring the project.

### Build libtiff first

The main project treats TIFF support as optional, but `local_config.cmake` is already configured to consume a local install under `3rdparty/libtiff/install`. Building and installing `libtiff` there keeps the repository layout consistent with the current code.

Configure and install `libtiff` with MinGW:

```powershell
cmake -S 3rdparty/libtiff -B 3rdparty/libtiff/build_cmake -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_INSTALL_PREFIX="C:/Users/HeMu/Desktop/BurstMerge/3rdparty/libtiff/install"

cmake --build 3rdparty/libtiff/build_cmake --config Release
cmake --install 3rdparty/libtiff/build_cmake
```

If `libtiff` is not installed in that location, the main build still configures, but TIFF input/output support is disabled.

### Configure the main project

From the repository root:

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER="C:/MinGW/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/MinGW/bin/g++.exe" `
  -DBUILD_TESTS=ON
```

Notes:

- `BUILD_TESTS` defaults to `ON` in the top-level `CMakeLists.txt`.
- The build enables OpenMP with `find_package(OpenMP REQUIRED)`.
- The codebase is written for C++17 and sets `CMAKE_CXX_STANDARD 17`.

### Build targets

Build everything:

```powershell
cmake --build build --config Release
```

Important targets defined by the current project:

- `burstmerge`: core static library
- `burstmerge_cli`: command-line application in `apps/cli`
- `burstmerge_console`: interactive console placeholder in `apps/console`
- `burstmerge_compare`: DNG pixel comparison tool in `apps/console`
- `test_deps`
- `test_dng_io`
- `test_stage0`
- `test_stage1`
- `test_common_rgb_fmt`

### Run tests

After a successful build:

```powershell
ctest --test-dir build --output-on-failure
```

Test coverage in the current tree includes:

- dependency and low-level API checks
- DNG read/write paths
- single-frame and multi-frame processing
- RGB format input/output checks

Some tests read sample files from `libburstmerge/test/samples/`. The `test_common_rgb_fmt` target also needs the `libtiff` runtime directory on `PATH`, which is already configured in `libburstmerge/test/CMakeLists.txt` for CTest runs.

- See also notes. 

### Runtime notes

- Proprietary camera RAW input support depends on the Windows-only DNG conversion path used by `src/io/dng_converter.cpp`.
- The repository contains the Adobe DNG SDK source under `3rdparty/dng_sdk` and builds it as an internal static library.
- Vulkan headers are referenced by the core library include path, but Vulkan is not required for the current GNU/CPU build path.

## Note

Sample files used by the test suit were not uploaded to this repo. 
Some algorithms saw huge performance degradation under Debug build, especially frequency merge algorithms. 