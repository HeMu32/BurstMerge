## Brief

This is a computational photography tool for multi-frame noise reduction and exposure bracketing.
It is inspired by the project https://github.com/martin-marek/hdr-plus-swift and based on Google HDR+.
For more detail, see the HDR+ paper: https://hdrplusdata.org/en//hdrplus.pdf

**Platform:** GCC (MinGW, Windows)

## Features

### Core capabilities

- Multi-Frame Noise Reduction (MFNR)
- Process RAW pictures with RAW-in RAW-out
- Process Non-RAW pictures
- STF synthesis like Minolta α7 camera (but with inter-frame alignment)
- Bayer color filter array support
- CLI application

### Merge and processing modes

- Simple temporal averaging merge
- Simple temporal median merge
- Motion-robust merge in the spatial domain
- Motion-robust merge in the frequency domain
- Optional exposure correction for improved shadow tonality
- Burst support with bracketed exposure (not applicable for common RGB formats)

### File Input&Output formats

- DNG RAW input/output support
- Camera manufacturer proprietary RAW format input support (via Adobe DNG Converter)
    - Detailed compatibility to be determined
- Common RGB picture formats input/output support
    - JPEG, PNG, BMP, TIF input and output
    - Note: RAW-in RGB-out and RGB-in RAW-out are not supported
- Selectable 10/12/14/16 bit precision for DNG output
- Selectable 8/16 bit precision for TIFF and PNG output

### Performance and platform support

- CPU processing support
- Multi-threaded RAW decoding
- Vulkan GPU compute backend
    - GPU device selection (`--gpu N`, `--list-gpus`)
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
- Configurable path to Adobe DNG Converter (hard-coded for now, will not be a problem on most machines)
- Support working together with Adobe LrC/ACR AI noise reduction
- Support working together with DxO PureRAW
- More comperhensive metadata copying for output
- Write processing parameters and program version to output
- Support images with more channels
- Non-Bayer sensor support
- Multi-threaded image loading
- Support for AVIF and/or HEIF
- Improve alignment algorithms for extreme exposure bracket range
- Improve merge algorithms for extreme exposure bracket range
- More constrained alignment algorithms: perspective, ...
- Mid-percentage merge option
- More texture-preserving and noise-robust frame merging algorithm
- Algorithms dedicated for STF synthesis
- Super-resolution with inter-frame redundancy
- De-Bayer with inter-frame redundancy
- Support for additional operating systems
- CJK path support
- User-configurable cache folder path
- User-configurable reference frame selection
- Progress reporting for frame merging
- Performance optimizations for alignment gamma apply processing on CPU
- Add example pictures to here

## Usage

### Invocation

The CLI ships as `burstmerge_cli.exe` (get it from `Releases`, or produced under `build/apps/cli/` if you build it yourself). Run it from a shell that has the MinGW `bin` directory on `PATH` (and, for TIFF I/O, the `libtiff` runtime directory).

```powershell
build\apps\cli\burstmerge_cli.exe --help
```

The minimum invocation needs inputs (`-i` / `--folder`) and an output target (`-o`). Accepted inputs are DNG or camera RAW (ARW/NEF/CR2/... — non-DNG RAW is converted via Adobe DNG Converter on Windows) and common RGB images (PNG/JPEG/BMP/TIFF). RAW and RGB classes cannot be freely interchanged: RAW-in → RGB-out and RGB-in → RAW-out are not supported. If RAW and RGB files are mixed in one run, the RGB frames are dropped and the RAW pipeline runs.

### Options

| Option | Default | Description |
| --- | --- | --- |
| `-i, --input <path>` | — | Input file. Repeatable (`-i a.dng -i b.dng`). |
| `-f, --folder <dir>` | — | Add every regular file in `<dir>` (sorted). Repeatable. |
| `-o, --output <path>` | `./out` | Output file path (with extension) **or** output directory. |
| `-m, --merge <algo>` | `spatial` | Merge algorithm: `spatial`, `frequency`, `temporal` (average), `median`. (alias: `--merge-algo`) |
| `-a, --alignment <mode>` | `standard` | Alignment mode: `standard`, `dense`, `freq`. |
| `--spatial-mode <mode>` | `standard` | Spatial sub-mode: `standard`, `linear`. (alias: `--spa-mode`) |
| `--frequency-mode <mode>` | `laplacian` | Frequency sub-mode: `laplacian`, `wiener`, `wiener-robust`. (alias: `--freq-mode`) |
| `-n, --noise-reduction <f>` | `13.0` | Noise reduction strength. Ignored when merge is `temporal`/`median`. |
| `-t, --tile <int>` | `32` | Alignment tile size (clamped to minimum 16). |
| `-b, --bit-depth <int>` | `14` | Output bit depth: `8`, `10`, `12`, `14`, `16`. |
| `--output-format <fmt>` | `auto` | `auto`, `png`, `jpg`, `bmp`, `tiff`, `dng`. `auto` selects DNG for RAW input and PNG for RGB input. |
| `--exposure-mode <mode>` | `off` | `off`, `linear`, `curve`. |
| `--exposure-stops <f>` | `0` | Exposure correction in stops (consumed by `linear`/`curve`). |
| `--exposure-curve <mode>` | `global` | Curve sub-mode: `global`, `local` (local Reinhard). |
| `--align-gamma <f>` | `1.0` | Gamma applied to alignment grayscale; `< 1.0` lifts shadows, `1.0` = off. |
| `--smooth-tile-field` | off | Median-smooth the alignment tile displacement field. |
| `--backend <name>` | `cpu` | Compute backend: `cpu`, `vulkan` (alias: `gpu`). |
| `--gpu-device <int>` | `-1` | GPU index for the Vulkan backend (`-1` = automatic). (alias: `--gpu`) |
| `--list-gpus` | — | List available Vulkan GPU devices and exit. |
| `--frequency` | — | Deprecated shorthand for `--merge frequency`. |
| `-h, --help` | — | Print help and exit. |

Notes:

- `--bit-depth` is honored as-is for DNG output; for RGB containers it is snapped to the nearest supported depth (8 for PNG/JPEG/BMP, 8 or 16 for TIFF).
- Bracketed-exposure bursts are supported only for RAW input. The reference frame is chosen automatically (darkest frame for bracketed bursts, middle frame otherwise); manual reference-frame selection is not yet exposed on the CLI.
- With `--backend vulkan`, `--frequency-mode wiener-robust` is rejected (the GPU path is not implemented — use `wiener` or `laplacian`), and hot-pixel repair is skipped. Every other merge mode matches the CPU output to within rounding.

### Processing pipeline

Frames flow through the stages below. Stages marked with an option list expose a user-selectable algorithm (linked to the flags above); the rest are fixed. The RAW path is the primary one; the RGB path is the same sequence with the RAW-only stages omitted.

1. **Input preparation** — non-DNG camera RAW is converted to DNG via Adobe DNG Converter (Windows only). *(fixed)*
2. **Decode** — DNGs are read into memory sequentially, then decoded in parallel across frames. *(fixed; multi-threaded)*
3. **Reference-frame selection** — automatic: darkest frame for a bracketed burst, middle frame otherwise (RGB always uses the middle frame). Manual selection is not exposed on the CLI. *(fixed)*
4. **Hot-pixel repair** — outlier suppression on the RAW mosaic. *(fixed; RAW only, skipped on the GPU backend)*
    - `-t, --tile` size of square tiles counted in pixel width used in alignment. Bigger values brings better noise robustness but tend to ignore regional movements. 
5. **Normalization** — subtract each frame's black level and scale it by its exposure ratio (EV) so comparison frames match the reference. *(fixed; RAW only)*
6. **Alignment** — per-tile translational registration of every comparison frame to the reference through a coarse-to-fine grayscale pyramid (both RAW and RGB paths align on a luminance image; RAW inputs are first deinterleaved from the Bayer mosaic).
    - `--alignment standard` *(default)*: global SAD search at the coarsest level, then diagonal-wavefront tile refinement. Less robust. 
    - `--alignment dense`: dense per-tile propagation and correction across all pyramid levels.
    - `--alignment freq`: Fourier-based sub-pixel shift estimation.
    - Geometry/quality knobs: `--tile` (tile size, min 16), `--align-gamma` (gamma on the alignment grayscale), `--smooth-tile-field` (median-smooth the displacement field).
7. **Merge** — Do MFNR, aka. combine the aligned comparison frames with the reference. Four mutually exclusive algorithms:
    - `--merge spatial` *(default)*: pixel-domain weighted blending, motion-robust via per-pixel weights. Sub-mode `--spatial-mode`: `standard` *(default)* or `linear`.
    - `--merge frequency`: frequency-domain merge. Sub-mode `--frequency-mode`: `laplacian` *(default)*, `wiener` (FFT Wiener), `wiener-robust` (CPU only).
    - `--merge temporal`: exposure-weighted temporal average (simplest; ignores `--noise-reduction`).
    - `--merge median`: per-pixel median across frames (robust to outliers; but tend to produce bad results in motion scene; ignores `--noise-reduction`).
    - Strength knob for spatial/frequency: `--noise-reduction`.
8. **Bit-depth scaling** — rescale to the requested depth and reconcile black/white levels. *(fixed; `--bit-depth`)*
9. **Non-Linear Exposure Mapping** *(optional)* — `--exposure-mode`: `off` *(default)*, `linear` (gain), or `curve` (tone curve). Curve sub-mode `--exposure-curve`: `global` or `local` (Reinhard). Magnitude: `--exposure-stops`.
10. **Mosaic rebuild & write** — re-pack to Bayer mosaic (RAW path) and write the output container. *(fixed; `--output-format`, `-o`)*

### Examples

Default RAW burst → 14-bit DNG (folder input, all defaults):
```powershell
burstmerge_cli.exe -f .\seq1 -o .\out\merged.dng
```

Frequency merge (FFT Wiener) on a RAW burst, 16-bit DNG:
```powershell
burstmerge_cli.exe -f .\seq1 -m frequency --freq-mode wiener -b 16 -o merged.dng
```

JPEG/PNG burst → PNG result:
```powershell
burstmerge_cli.exe -i 1.jpg -i 2.jpg -i 3.jpg --output-format png -o out.png
```

GPU-accelerated spatial merge on a selected device:
```powershell
burstmerge_cli.exe -f .\seq1 --backend vulkan --gpu 0 -a dense -m spatial -o out.dng
```

Bracketed RAW exposure with shadow-lifting local-Reinhard curve (+1.5 stops):
```powershell
burstmerge_cli.exe -f .\bkt1 -m spatial --exposure-mode curve --exposure-stops 1.5 --exposure-curve local -o out.dng
```

Enumerate Vulkan GPUs and exit:
```powershell
burstmerge_cli.exe --list-gpus
```

## Dependency

- Standard C++17
- libjpeg-turbo
- libpng
- libtiff
- zlib
- OpenMP
- Adobe DNG SDK
- Pocket FFT
- Vulkan

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

Still, you can modify the CMake configs to find the dependencies if you've alreadly configured them in your environment. 

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
- Vulkan is linked into every build (`libvulkan-1.a` PUBLIC, SPIR-V embedded at configure-time). At runtime, `vulkan-1.dll` must be present (installed with GPU drivers). Use `--backend cpu` to skip GPU entirely.
- Shader changes require a CMake configure re-run (SPIR-V is compiled by `3rdparty/glslang/glslangValidator.exe` and embedded into `spirv_embedded.inl`).

## Note

Sample files used by the test suit were not uploaded to this repo. 
Some algorithms saw huge performance degradation under Debug build, especially frequency merge algorithms. 