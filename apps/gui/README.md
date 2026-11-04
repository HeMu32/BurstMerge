# BurstMerge GUI

`burstmerge_gui` is the wxWidgets desktop frontend for BurstMerge. It links the
`burstmerge` static library directly and does not invoke `burstmerge_cli`.

The frontend is split into cohesive modules: `gui_utils` handles paths, output
naming, platform theming, and generated badges; `panels` owns the Bin, Queue,
Options, and drag-and-drop controls; `process_thread` owns worker events and
queue execution; and `main_frame` coordinates the application workspace.
`thumbnail_loader` uses one background worker for bounded thumbnail requests.
It decodes common RGB files, embedded JPEG previews from classic TIFF-based RAW
files, RAF headers, and Canon CR3 preview UUID boxes. TIFF thumbnails prefer
reduced rendered IFDs and can also sample large linear RGB TIFFs row-by-row.
Unsupported or malformed files retain the file icon. `main.cpp` contains only
the wx application entry point.

## wxWidgets Setup

The GUI uses wxWidgets 3.2.5. The source tree is intentionally ignored by Git.
Download the release archive from:

https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.5/wxWidgets-3.2.5.tar.bz2

Extract the archive so that these files exist:

```text
3rdparty/wxWidgets/CMakeLists.txt
3rdparty/wxWidgets/include/wx/wx.h
```

On this project's Windows/MinGW environment, the bundled `7z.exe` can extract
the archive in two steps:

```powershell
C:\MinGW\bin\7z.exe e wxWidgets-3.2.5.tar.bz2 -o<temporary-directory>
C:\MinGW\bin\7z.exe x <temporary-directory>\wxWidgets-3.2.5.tar -o<temporary-directory>
```

Move the contents of the resulting `wxWidgets-3.2.5` directory into
`3rdparty/wxWidgets`.

wxWidgets is built as static libraries inside the main CMake build tree. No
separate wxWidgets install step is required. The GUI configuration disables
wxWidgets samples, tests, demos, benchmarks, shared libraries, and precompiled
headers. Disabling PCH avoids a wxWidgets 3.2/MinGW GCC compatibility issue.

## Build

Configure and build with MinGW/GCC:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBURSTMERGE_BUILD_MAIN_GUI=ON -DBURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS=ON
cmake --build build --target burstmerge_gui -j 8
cmake --build build --target thumbnail_preview_diagnostic -j 8
```

The executable is written to:

```text
build/apps/gui/burstmerge_gui.exe
```

The diagnostic validates thumbnail decoding and reports embedded JPEG metadata
when the input uses that preview path:

```powershell
build/apps/gui/thumbnail_preview_diagnostic.exe libburstmerge/test/samples/X1M5_Wide.dng
```

The diagnostic accepts multiple image paths, which is useful for checking that
one rejected file does not prevent later files from being processed:

```powershell
build/apps/gui/thumbnail_preview_diagnostic.exe `
    libburstmerge/test/samples/rgb_small/rgb8.png `
    libburstmerge/test/samples/tif1/DSC05857.tif `
    libburstmerge/test/samples/X1M5_Wide.dng
```

On Windows, the build also copies the project's `libtiff.dll` next to the
executable. wxWidgets itself is linked statically, so the GUI can be launched
from Explorer without preparing a wxWidgets or libtiff `PATH`.

The Windows target embeds the wxWidgets Common Controls v6 and Per-Monitor V2
DPI manifest. Controls and AUI panes use Windows system colours instead of a
fixed application palette, respond to system-colour changes, and keep icons and
key dimensions scaled in device-independent pixels. The Bin and Queue lists use
the native Explorer control theme.

Windows-specific C++ behavior is isolated behind `_WIN32`; the resource file,
GUI subsystem flag, Win32 libraries, and runtime DLL copy are guarded by CMake's
`WIN32`/`MINGW` platform checks. Other platforms retain the portable wxWidgets
system-colour, DPI, layout, queue, and processing paths without including Win32
headers or calling Win32 APIs.

The source tree is detected through `BURSTMERGE_WXWIDGETS_ROOT` in the gitignored
`local_config.cmake`. Its default project-local value is:

```cmake
set(BURSTMERGE_WXWIDGETS_ROOT "${PROJECT_ROOT}/3rdparty/wxWidgets")
```

## Thumbnail Support

The loader never decodes RAW sensor pixels for a thumbnail. It uses the
following paths:

- JPEG: bounded JPEG header validation followed by wx/libjpeg decode.
- PNG/BMP: bounded header validation followed by wx decode.
- DNG and classic TIFF-based RAW: bounded embedded JPEG IFD ranges.
- Sony NEX-5 ARW: old-style IFD0 `PreviewImage` tags 513/514 are supported.
- Newer Sony ARW: rendered RGB/YCbCr preview IFDs and `JpgFromRaw` layouts are
  supported. ARW files are kept away from the ordinary TIFF raster decoder.
- RAF: validated Fujifilm header preview offset and length.
- CR3: Canon `crx ` BMFF files with the Canon preview UUID/`PRVW` box.
- TIFF: reduced rendered IFDs, SubIFDs, BigTIFF, and large linear RGB TIFFs with
  bounded scanline sampling.

The implementation is intentionally conservative. A format that does not have
a recognized safe preview layout falls back to the file icon. Current safety
limits include:

- 64 MiB for ordinary JPEG/PNG/BMP input files.
- 256 MiB for TIFF, DNG, ARW, RAF, and CR3 container files.
- 16 MiB for an extracted embedded JPEG byte range.
- 16 MP for reduced TIFF raster decoding and 64 MP for embedded JPEG metadata.
- 16384 pixels for either preview dimension.
- 1 MiB maximum scanline buffer for the large linear TIFF path.
- 4096 staged Bin entries and 4096 pending thumbnail requests.
- 1 MiB maximum internal drag-and-drop path payload.

When a file exceeds a limit, is truncated, has unsupported compression, or
fails decoding, the worker catches the failure and leaves the file icon in
place. Removing a file or clearing the Bin invalidates both queued and active
thumbnail requests, so stale results are not added back to the UI.

The following samples have been exercised with the current implementation:

- `libburstmerge/test/samples/tif1/*.tif`, including 16-bit RGB TIFF files.
- `T:\BurstMerge_Samples\SLg2-1-48-lin.tif`, a 3840x2160 16-bit LZW RGB TIFF
  without a reduced IFD.
- Sony NEX-5 files under `T:\BurstMerge_Samples\LongUnder1`.
- Sony ILCE-7RM5 files under `T:\BurstMerge_Samples\Night4`.
- Sony ARW samples under `libburstmerge/test/samples/`.
- DNG and PNG samples under `libburstmerge/test/samples/`.

NEF, CR2, RAF, CR3, ORF, and RW2 behavior should still be checked against real
files from the relevant camera generations. The parser rejects unknown or
unverified layouts rather than attempting full RAW decoding or launching the
Adobe DNG Converter for thumbnail generation.

## Build Without The GUI

The GUI is completely optional:

```powershell
cmake -S . -B build_no_gui -G "MinGW Makefiles" -DBURSTMERGE_BUILD_MAIN_GUI=OFF -DBURSTMERGE_BUILD_RAW_RESIZE_GUI=OFF
cmake --build build_no_gui --target burstmerge_cli
```

With the option disabled, CMake does not inspect or add the wxWidgets source
tree and the CLI, console, core library, and tests retain their normal build.
If an optional wxWidgets frontend is enabled but `BURSTMERGE_WXWIDGETS_ROOT` does not contain a valid wxWidgets tree,
CMake prints a status message and skips the GUI without failing configuration.

## Workflow

1. Add or drag files or folders into the Bin. Dropped folders expand their
   immediate regular files in sorted order, matching the CLI `-f` behavior
   without recursive traversal. This only stages paths; it performs no RAW
   conversion or image processing.
2. Select Bin entries and add or drag them to one or more queues. A yellow Bin
   badge shows how many queues reference each file.
3. Configure pipeline, merge, alignment, exposure, and cleanup options.
4. Press **Start**. Non-empty queues run serially from top to bottom on a worker
   thread.
5. Each queue writes a file directly beneath the configured output root, which
   defaults to `./out`. The Pipeline tab offers two naming modes: processing
   parameters only, or the first frame's filename followed by processing
   parameters. Names include merge/alignment/exposure modes, alignment gamma,
   noise reduction, tile size, and bit depth. Existing names receive a numeric
   suffix instead of being overwritten.

Alignment gamma and noise reduction each provide a slider plus an editable
numeric field. Moving either control updates the other. The Bin/Queue divider is
draggable, and both the output directory and optional DNG conversion cache can
be entered directly or selected with a native directory dialog.

Folders may also be dropped directly on a Queue. Their immediate regular files
are added to that Queue and synchronized into the Bin, exactly like externally
dropped individual files.

`Remove Selection` appears between `New Queue` and `Clear Bin` in the Edit menu
and toolbar. It acts only on the most recently active Bin or Queue list, even if
another list still shows an inactive selection. Pressing Delete performs the
same action only while that active list retains keyboard focus, so Delete keeps
its normal editing behavior in path and numeric fields. Removing Queue entries
decrements their Bin reference badges; removing Bin entries also removes the
same files from every Queue.

The current BurstMerge API has no cancellation primitive, so the GUI does not
offer a Stop button. The window remains open until active processing finishes.
