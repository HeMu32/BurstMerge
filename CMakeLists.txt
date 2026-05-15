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

# ---- 测试 ----
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(libburstmerge/test)
endif()
