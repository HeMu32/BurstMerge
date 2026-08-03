# BurstMerge GUI

`burstmerge_gui` is the wxWidgets desktop frontend for BurstMerge. It links the
`burstmerge` static library directly and does not invoke `burstmerge_cli`.

The frontend is split into cohesive modules: `gui_utils` handles paths, output
naming, platform theming, and generated badges; `panels` owns the Bin, Queue,
Options, and drag-and-drop controls; `process_thread` owns worker events and
queue execution; and `main_frame` coordinates the application workspace.
`thumbnail_loader` uses one background worker for deduplicated thumbnail requests.
It decodes common RGB files, bounded embedded JPEG previews from classic
TIFF-based RAW files (including ARW and common NEF/CR2 layouts), RAF headers,
and Canon CR3 preview UUID boxes. TIFF thumbnails prefer reduced rendered IFDs;
small TIFF images may decode directly. Unsupported or malformed files retain the
file icon. `main.cpp` contains only the wx application entry point.

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
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBURSTMERGE_BUILD_GUI=ON -DBURSTMERGE_BUILD_GUI_DIAGNOSTICS=ON
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

The source tree is detected through `WX_ROOT` in the gitignored
`local_config.cmake`. Its default project-local value is:

```cmake
set(WX_ROOT "${PROJECT_ROOT}/3rdparty/wxWidgets")
```

## Build Without The GUI

The GUI is completely optional:

```powershell
cmake -S . -B build_no_gui -G "MinGW Makefiles" -DBURSTMERGE_BUILD_GUI=OFF
cmake --build build_no_gui --target burstmerge_cli
```

With the option disabled, CMake does not inspect or add the wxWidgets source
tree and the CLI, console, core library, and tests retain their normal build.
If the option is enabled but `WX_ROOT` does not contain a valid wxWidgets tree,
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
