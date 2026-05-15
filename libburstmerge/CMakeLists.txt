# libburstmerge 核心静态库
add_library(burstmerge STATIC
    src/api.cpp
)

target_include_directories(burstmerge
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE ${DNG_SDK_INCLUDE}
            ${POCKETFFT_INCLUDE}
            ${CXXOPTS_INCLUDE}
            ${VULKAN_INCLUDE}
)

target_link_libraries(burstmerge
    PUBLIC  dng_sdk
    PRIVATE OpenMP::OpenMP_CXX
)

if(MSVC)
    target_link_libraries(burstmerge PRIVATE ${VULKAN_LIBRARY})
endif()
