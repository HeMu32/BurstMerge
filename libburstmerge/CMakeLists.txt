# libburstmerge core static library
add_library(burstmerge STATIC
    src/api.cpp
    src/api_c.cpp
    src/core/float_image.cpp
    src/core/profiler.cpp
    src/core/fft_util.cpp
    src/core/pipeline.cpp
    src/core/task_executor.cpp
    src/core/pipeline_io.cpp
    src/core/pipeline_frame.cpp
    src/core/pipeline_align.cpp
    src/align/align.cpp
    src/align/align_common.cpp
    src/align/align_pyramid.cpp
    src/align/align_standard.cpp
    src/align/align_dense.cpp
    src/align/align_warp.cpp
    src/align/freq_align.cpp
    src/merge/spatial.cpp
    src/merge/frequency.cpp
    src/denoise/temporal.cpp
    src/exposure/exposure.cpp
    src/io/dng_sdk_bridge.cpp
    src/io/dng_reader.cpp
    src/io/dng_writer.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/pocketfft/pocketfft.c
)

if(WIN32)
    target_sources(burstmerge PRIVATE src/io/dng_converter.cpp)
endif()

target_include_directories(burstmerge
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE ${DNG_SDK_INCLUDE}
            ${POCKETFFT_INCLUDE}
            ${CXXOPTS_INCLUDE}
            ${VULKAN_INCLUDE}
)

target_compile_definitions(burstmerge PRIVATE
    # Macro definitions are inherited from dng_sdk PUBLIC
)

target_link_libraries(burstmerge
    PUBLIC  dng_sdk
    PRIVATE OpenMP::OpenMP_CXX
)

if(MSVC)
    target_link_libraries(burstmerge PRIVATE ${VULKAN_LIBRARY})
endif()
