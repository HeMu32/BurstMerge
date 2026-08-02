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

# ---- GUI (wxWidgets, 完全可选) ----
# 设计要求: 没装 wx 时 CLI/console/tests 不能受任何影响. 因此:
#   1. 此段在 CLI/console 之后, 通过 option 守卫;
#   2. 没装 wx 源码时 FindWxWidgetsBurstMerge 不会污染任何全局变量, 仅设置 FOUND flag;
#   3. apps/gui 仅在 BURSTMERGE_WX_FOUND 为 TRUE 时被 add_subdirectory.
# 用户可通过 -DBURSTMERGE_BUILD_GUI=OFF 强制跳过 (即便 wx 源码已存在).
option(BURSTMERGE_BUILD_GUI "Build burstmerge_gui with wxWidgets (auto-skip if wx source not present)" ON)
if(BURSTMERGE_BUILD_GUI)
    # wxWidgets 3.2 自带 CMake 子项目; 通过 add_subdirectory 集成前强制写好其 cache
    # 关键变量, 避免默认值触发:
    #   - 共享库 (我们要静态, 与 burstmerge 静态库一致)
    #   - 子项目 (samples/tests/demos/bench) 编译浪费时间
    #   - PCH (MinGW GCC 11.x + wx 3.2 PCH 触发 "cc1plus not used because ... not defined"
    #     导致 wxbase 编译报 Error 1 却 g++ 单独跑 exit=0, 推测是 cmake_pch.hxx.gch 失配)
    #   - install / monolithic 取决于我们的链接方式
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
        message(STATUS "GUI: enabled (burstmerge_gui will be built)")
        add_subdirectory(apps/gui)
    else()
        message(STATUS "GUI: wxWidgets source not found - burstmerge_gui will NOT be built (CLI/tests unaffected)")
        set(BURSTMERGE_BUILD_GUI OFF CACHE BOOL "Build GUI" FORCE)
    endif()
endif()

# ---- 测试 ----
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(libburstmerge/test)
endif()
