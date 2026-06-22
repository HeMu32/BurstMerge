# libburstmerge core static library
add_library(burstmerge STATIC
    src/api.cpp
    src/api_c.cpp
    src/core/float_image.cpp
    src/core/profiler.cpp
    src/core/fft_util.cpp
    src/core/pipeline.cpp
    src/core/gpu_pipeline.cpp
    src/core/task_executor.cpp
    src/core/pipeline_io.cpp
    src/core/pipeline_frame.cpp
    src/core/pipeline_align.cpp
    src/compute/vulkan/vulkan_backend.cpp
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

# ---- Vulkan compute shaders -> embedded SPIR-V header (configure time) ----
# Compiles every src/compute/vulkan/shaders/*.comp with glslangValidator and
# emits a single generated header included by vulkan_backend.cpp.
set(BURSTMERGE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/compute/vulkan/shaders")
set(BURSTMERGE_GLSLANG "${PROJECT_SOURCE_DIR}/3rdparty/glslang/glslangValidator.exe")
set(BURSTMERGE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${BURSTMERGE_GENERATED_DIR}")
set(BURSTMERGE_SPIRV_INL "${BURSTMERGE_GENERATED_DIR}/spirv_embedded.inl")
execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.ps1"
            -Validator "${BURSTMERGE_GLSLANG}"
            -ShaderDir "${BURSTMERGE_SHADER_DIR}"
            -OutFile "${BURSTMERGE_SPIRV_INL}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _spv_result
)
if(NOT _spv_result EQUAL 0)
    message(WARNING "Shader embedding failed (code ${_spv_result}); Vulkan backend will not compile.")
endif()

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
            ${BURSTMERGE_GENERATED_DIR}
)

target_compile_definitions(burstmerge PRIVATE
    # Macro definitions are inherited from dng_sdk PUBLIC
)

target_link_libraries(burstmerge
    PUBLIC  dng_sdk
    PRIVATE OpenMP::OpenMP_CXX
)

# Link Vulkan loader for all compilers (GCC + MSVC). libvulkan-1.a is MinGW.
# PUBLIC: the static library's vulkan_backend.cpp.obj is pulled into consumers,
# so executables linking libburstmerge must also resolve Vulkan symbols.
if(WIN32)
    target_link_libraries(burstmerge PUBLIC ${VULKAN_LIBRARY})
endif()
