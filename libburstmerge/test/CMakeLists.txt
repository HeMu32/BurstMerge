# 测试: 每个 test_*.cpp 是独立可执行文件
# 退出码 0=通过, 非0=失败

function(add_burstmerge_test name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE burstmerge OpenMP::OpenMP_CXX)
    target_include_directories(${name} PRIVATE
        ${POCKETFFT_INCLUDE}
        ${CXXOPTS_INCLUDE}
        ${VULKAN_INCLUDE}
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_burstmerge_test(test_deps)
target_sources(test_deps PRIVATE "${POCKETFFT_INCLUDE}/pocketfft.c")
