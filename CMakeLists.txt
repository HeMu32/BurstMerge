cmake_minimum_required(VERSION 3.16)
project(burstmerge VERSION 0.1.0 LANGUAGES CXX C)

# ---- Build options ----
option(BURSTMERGE_BUILD_TESTS
    "Build unit and integration tests"
    ON)
option(BURSTMERGE_BUILD_RAW_RESIZE_CLI
    "Build the standalone raw_resize command-line tool"
    ON)
option(BURSTMERGE_BUILD_RAW_RESIZE_GUI
    "Build the standalone raw resize wxWidgets GUI (auto-skip if wxWidgets is unavailable)"
    ON)
option(BURSTMERGE_BUILD_MAIN_GUI
    "Build the main wxWidgets GUI (auto-skip if wxWidgets is unavailable)"
    ON)
option(BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS
    "Build the main GUI thumbnail diagnostics tool"
    OFF)

# ---- 依赖路径集中配置 ----
include(local_config.cmake)

# ---- 编译器 flags ----
include(cmake/CompilerFlags.cmake)

# ---- OpenMP ----
find_package(OpenMP REQUIRED)

# ---- 三方库 ----
add_subdirectory(3rdparty/dng_sdk)

# ---- 核心库 ----
add_subdirectory(libburstmerge)

# ---- 前端 ----
add_subdirectory(apps/cli)
add_subdirectory(apps/console)

# ---- Optional frontends ----
if(BURSTMERGE_BUILD_RAW_RESIZE_CLI)
    add_subdirectory(apps/raw_resize)
endif()

if(BURSTMERGE_BUILD_MAIN_GUI OR
   BURSTMERGE_BUILD_RAW_RESIZE_GUI OR
   BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS)
    set(wxBUILD_SHARED     OFF CACHE BOOL "wx shared" FORCE)
    set(wxBUILD_MONOLITHIC OFF CACHE BOOL "wx monolithic" FORCE)
    set(wxBUILD_PRECOMP    OFF CACHE BOOL "wx PCH (MinGW compat)" FORCE)
    set(wxBUILD_SAMPLES    OFF CACHE BOOL "wx samples" FORCE)
    set(wxBUILD_TESTS      OFF CACHE BOOL "wx tests" FORCE)
    set(wxBUILD_DEMOS      OFF CACHE BOOL "wx demos" FORCE)
    set(wxBUILD_BENCHMARKS OFF CACHE BOOL "wx benchmarks" FORCE)
    set(wxBUILD_INSTALL    OFF CACHE BOOL "wx install target" FORCE)

    include(cmake/FindWxWidgetsBurstMerge.cmake)
    if(BURSTMERGE_WXWIDGETS_FOUND)
        message(STATUS "wxWidgets: enabled")

        if(EXISTS "${JPEG_INCLUDE_DIR}/jpeglib.h" AND EXISTS "${JPEG_LIBRARY}")
            set(wxUSE_LIBJPEG sys CACHE STRING "wx JPEG implementation" FORCE)
        else()
            set(wxUSE_LIBJPEG builtin CACHE STRING "wx JPEG implementation" FORCE)
        endif()
        if(EXISTS "${PNG_INCLUDE_DIR}/png.h" AND EXISTS "${PNG_LIBRARY}")
            set(wxUSE_LIBPNG sys CACHE STRING "wx PNG implementation" FORCE)
        else()
            set(wxUSE_LIBPNG builtin CACHE STRING "wx PNG implementation" FORCE)
        endif()
        if(EXISTS "${ZLIB_INCLUDE_DIR}/zlib.h" AND EXISTS "${ZLIB_LIBRARY}")
            set(wxUSE_ZLIB sys CACHE STRING "wx zlib implementation" FORCE)
        else()
            set(wxUSE_ZLIB builtin CACHE STRING "wx zlib implementation" FORCE)
        endif()
        if(EXISTS "${TIFF_INCLUDE_DIR}/tiffio.h" AND EXISTS "${TIFF_LIBRARY}")
            set(wxUSE_LIBTIFF sys CACHE STRING "wx TIFF implementation" FORCE)
        else()
            set(wxUSE_LIBTIFF builtin CACHE STRING "wx TIFF implementation" FORCE)
        endif()
        set(JPEG_INCLUDE_DIR "${JPEG_INCLUDE_DIR}" CACHE PATH "Shared JPEG include directory" FORCE)
        set(JPEG_LIBRARY "${JPEG_LIBRARY}" CACHE FILEPATH "Shared JPEG library" FORCE)
        set(PNG_PNG_INCLUDE_DIR "${PNG_INCLUDE_DIR}" CACHE PATH "Shared PNG include directory" FORCE)
        set(PNG_LIBRARY "${PNG_LIBRARY}" CACHE FILEPATH "Shared PNG library" FORCE)
        set(ZLIB_INCLUDE_DIR "${ZLIB_INCLUDE_DIR}" CACHE PATH "Shared zlib include directory" FORCE)
        set(ZLIB_LIBRARY "${ZLIB_LIBRARY}" CACHE FILEPATH "Shared zlib library" FORCE)
        set(TIFF_INCLUDE_DIR "${TIFF_INCLUDE_DIR}" CACHE PATH "Shared TIFF include directory" FORCE)
        set(TIFF_LIBRARY "${TIFF_LIBRARY}" CACHE FILEPATH "Shared TIFF library" FORCE)

        add_subdirectory(
            "${BURSTMERGE_WXWIDGETS_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/wx_build"
        )
        if(MINGW)
            foreach(_wx_target IN LISTS BURSTMERGE_WXWIDGETS_TARGETS)
                if(TARGET ${_wx_target})
                    target_compile_definitions(${_wx_target} PRIVATE wxHACK_BOOLEAN=int)
                endif()
            endforeach()
        endif()
        if((BURSTMERGE_BUILD_MAIN_GUI OR BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS) AND
           EXISTS "${CMAKE_SOURCE_DIR}/apps/gui/CMakeLists.txt")
            add_subdirectory(apps/gui)
        endif()
        if(BURSTMERGE_BUILD_RAW_RESIZE_GUI AND
           EXISTS "${CMAKE_SOURCE_DIR}/apps/raw_resize_gui/CMakeLists.txt")
            add_subdirectory(apps/raw_resize_gui)
        endif()
    else()
        message(STATUS "wxWidgets: source not found - optional wxWidgets frontends will NOT be built")
    endif()
endif()

# ---- Tests ----
if(BURSTMERGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(libburstmerge/test)
endif()
