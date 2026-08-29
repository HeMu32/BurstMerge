cmake_minimum_required(VERSION 3.16)
project(burstmerge VERSION 0.1.0 LANGUAGES CXX C)

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
add_subdirectory(apps/raw_resize)

# ---- GUI (wxWidgets, 完全可选) ----
option(BURSTMERGE_BUILD_GUI "Build GUI applications with wxWidgets (auto-skip if wx source not present)" ON)
if(BURSTMERGE_BUILD_GUI)
    set(wxBUILD_SHARED     OFF CACHE BOOL "wx shared" FORCE)
    set(wxBUILD_MONOLITHIC OFF CACHE BOOL "wx monolithic" FORCE)
    set(wxBUILD_PRECOMP    OFF CACHE BOOL "wx PCH (MinGW compat)" FORCE)
    set(wxBUILD_SAMPLES    OFF CACHE BOOL "wx samples" FORCE)
    set(wxBUILD_TESTS      OFF CACHE BOOL "wx tests" FORCE)
    set(wxBUILD_DEMOS      OFF CACHE BOOL "wx demos" FORCE)
    set(wxBUILD_BENCHMARKS OFF CACHE BOOL "wx benchmarks" FORCE)
    set(wxBUILD_INSTALL    OFF CACHE BOOL "wx install target" FORCE)

    include(cmake/FindWxWidgetsBurstMerge.cmake)
    if(BURSTMERGE_WX_FOUND)
        message(STATUS "GUI: enabled")
        if(EXISTS "${CMAKE_SOURCE_DIR}/apps/gui/CMakeLists.txt")
            add_subdirectory(apps/gui)
        endif()
        if(EXISTS "${CMAKE_SOURCE_DIR}/apps/raw_resize_gui/CMakeLists.txt")
            add_subdirectory(apps/raw_resize_gui)
        endif()
    else()
        message(STATUS "GUI: wxWidgets source not found - GUI apps will NOT be built (CLI/tests unaffected)")
        set(BURSTMERGE_BUILD_GUI OFF CACHE BOOL "Build GUI" FORCE)
    endif()
endif()

# ---- 测试 ----
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(libburstmerge/test)
endif()
