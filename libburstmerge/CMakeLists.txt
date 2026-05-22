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
    src/io/image_decoder.cpp
    src/io/image_writer.cpp
    src/io/dng_decoder.cpp
    src/io/dng_writer_adapter.cpp
    src/io/bmp_decoder.cpp
    src/io/bmp_writer.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/pocketfft/pocketfft.c
)

# ---- 条件编译解码器 ----
# libjpeg-turbo
if(JPEG_INCLUDE_DIR AND JPEG_LIBRARY)
    target_sources(burstmerge PRIVATE src/io/jpeg_decoder.cpp src/io/jpeg_writer.cpp)
    target_include_directories(burstmerge PRIVATE ${JPEG_INCLUDE_DIR})
    target_link_libraries(burstmerge PRIVATE ${JPEG_LIBRARY})
    target_compile_definitions(burstmerge PRIVATE BURSTMERGE_HAVE_JPEG)
    message(STATUS "JPEG support: enabled (${JPEG_LIBRARY})")
else()
    message(STATUS "JPEG support: disabled")
endif()

# libpng + zlib
if(PNG_INCLUDE_DIR AND PNG_LIBRARY AND ZLIB_LIBRARY)
    target_sources(burstmerge PRIVATE src/io/png_decoder.cpp src/io/png_writer.cpp)
    target_include_directories(burstmerge PRIVATE ${PNG_INCLUDE_DIR} ${ZLIB_INCLUDE_DIR})
    target_link_libraries(burstmerge PRIVATE ${PNG_LIBRARY} ${ZLIB_LIBRARY})
    target_compile_definitions(burstmerge PRIVATE BURSTMERGE_HAVE_PNG)
    message(STATUS "PNG support: enabled (${PNG_LIBRARY})")
else()
    message(STATUS "PNG support: disabled")
endif()

# libtiff (可选)
if(TIFF_INCLUDE_DIR AND TIFF_LIBRARY)
    target_sources(burstmerge PRIVATE src/io/tiff_decoder.cpp src/io/tiff_writer.cpp)
    target_include_directories(burstmerge PRIVATE ${TIFF_INCLUDE_DIR})
    target_link_libraries(burstmerge PRIVATE ${TIFF_LIBRARY})
    target_compile_definitions(burstmerge PRIVATE BURSTMERGE_HAVE_TIFF)
    message(STATUS "TIFF support: enabled (${TIFF_LIBRARY})")
else()
    message(STATUS "TIFF support: disabled (libtiff not found)")
endif()

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
