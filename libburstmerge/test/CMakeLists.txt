# Tests: each test_*.cpp is a standalone executable
# Exit code 0=pass, non-zero=fail

function(add_burstmerge_test name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE burstmerge OpenMP::OpenMP_CXX)
    target_include_directories(${name} PRIVATE
        ${POCKETFFT_INCLUDE}
        ${CXXOPTS_INCLUDE}
        ${VULKAN_INCLUDE}
    )
    target_compile_definitions(${name} PRIVATE
        TEST_DATA_DIR="${PROJECT_SOURCE_DIR}"
        TEST_BINARY_DIR="${PROJECT_BINARY_DIR}"
    )
    add_test(NAME ${name} COMMAND ${name})
    # 默认超时: 快速单元测试
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

add_burstmerge_test(test_deps)
target_sources(test_deps PRIVATE "${POCKETFFT_INCLUDE}/pocketfft.c")

add_burstmerge_test(test_dng_io)

add_burstmerge_test(test_stage0)
set_tests_properties(test_stage0 PROPERTIES TIMEOUT 300)

add_burstmerge_test(test_stage1)
set_tests_properties(test_stage1 PROPERTIES TIMEOUT 600)

add_burstmerge_test(test_common_rgb_fmt)
target_sources(test_common_rgb_fmt PRIVATE test_aux.cpp)
set_tests_properties(test_common_rgb_fmt PROPERTIES TIMEOUT 600)

# GPU pipeline tests (stage 2+): separated from stage0/stage1 CPU tests.
# Requires a Vulkan-capable GPU at runtime.
add_burstmerge_test(test_stage2)
set_tests_properties(test_stage2 PROPERTIES TIMEOUT 600)

# test_stage3: CPU vs GPU dual-path consistency (end-to-end DNG comparison).
# Requires Adobe DNG Converter + Vulkan GPU at runtime.
add_burstmerge_test(test_stage3)
set_tests_properties(test_stage3 PROPERTIES TIMEOUT 900)

set(TEST_COMMON_RGB_FMT_LD_PATH "${PROJECT_SOURCE_DIR}/3rdparty/libtiff/install/bin")
# Seprator for PATH environment variable: ";" on Windows, ":" on Unix
if (WIN32)
    set(TEST_PATH_SEP ";")
else()
    set(TEST_PATH_SEP ":")
endif()

set_tests_properties(test_common_rgb_fmt PROPERTIES
    ENVIRONMENT "PATH=${TEST_COMMON_RGB_FMT_LD_PATH}${TEST_PATH_SEP}$ENV{PATH}")
