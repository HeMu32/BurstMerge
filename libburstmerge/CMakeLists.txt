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

# ---- Vulkan compute shaders -> embedded SPIR-V header (build time) ----
# Compiles every src/compute/vulkan/shaders/*.comp with glslangValidator and
# emits a single generated header included by vulkan_backend.cpp.
#
# BURSTMERGE_GPU_FP64 (default OFF): when ON, the fp64 variant
# dense_level_fp64.comp is compiled and embedded, improving dense-tile
# alignment consistency for extreme bracketing. Requires a GPU with
# shaderFloat64 support (all NVIDIA/AMD desktop GPUs). At runtime, the backend
# checks HasFloat64() and falls back to the float shader if unsupported.
#
# Why build-time (not configure-time): the previous implementation used
# execute_process() at configure time, which regenerates spirv_embedded.inl
# only when `cmake -B <build>` is re-run. A subsequent `cmake --build` after
# editing a *.comp file would NOT re-embed the SPIR-V, leaving the executable
# silently linked against stale shader bytecode (this exact bug caused a ~31x
# slowdown in release-build standard alignment for weeks). The custom command
# below lists every *.comp as a DEPENDENCY, so `cmake --build` re-runs the
# embed step automatically whenever any shader changes.
option(BURSTMERGE_GPU_FP64 "Compile GPU fp64 dense-alignment shader for improved bracketing alignment consistency" OFF)
set(BURSTMERGE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/compute/vulkan/shaders")
set(BURSTMERGE_GLSLANG "${PROJECT_SOURCE_DIR}/3rdparty/glslang/glslangValidator.exe")
set(BURSTMERGE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${BURSTMERGE_GENERATED_DIR}")
set(BURSTMERGE_SPIRV_INL "${BURSTMERGE_GENERATED_DIR}/spirv_embedded.inl")

# Glob every *.comp shader so the custom command re-triggers the embed step as
# soon as any shader source changes. Note the list is captured at configure
# time; if a NEW shader is added, re-running `cmake -B <build>` regenerates
# the list (and `cmake --build` then compiles it). Editing existing shaders
# is fully covered without re-configure.
file(GLOB BURSTMERGE_SHADER_SOURCES CONFIGURE_DEPENDS
     "${BURSTMERGE_SHADER_DIR}/*.comp")

# Step 1 -- generate the SPIR-V embed header. Output goes through a temp
# file then a forced mv so the final header's mtime is always updated, even
# if the SPIR-V bytes happen to be bit-identical to the previous build (this
# guarantees the downstream vulkan_backend.cpp.obj recompiles whenever any
# shader source changes, regardless of glslangValidator's output stability).
add_custom_command(
    OUTPUT  "${BURSTMERGE_SPIRV_INL}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BURSTMERGE_GENERATED_DIR}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.ps1"
            -Validator "${BURSTMERGE_GLSLANG}"
            -ShaderDir "${BURSTMERGE_SHADER_DIR}"
            -OutFile "${BURSTMERGE_SPIRV_INL}.tmp"
            $<$<BOOL:${BURSTMERGE_GPU_FP64}>:-IncludeFp64>
    COMMAND ${CMAKE_COMMAND} -E rename
            "${BURSTMERGE_SPIRV_INL}.tmp" "${BURSTMERGE_SPIRV_INL}"
    DEPENDS "${BURSTMERGE_SHADER_SOURCES}"
            "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.ps1"
    VERBATIM
    COMMENT "Embedding Vulkan SPIR-V shaders into ${BURSTMERGE_SPIRV_INL}"
)

# Step 2 -- declare the generated header as a source so vulkan_backend.cpp
# (which #includes it) rebuilds whenever the embed step re-runs. Using a
# generated file as a source makes CMake create a GENERATED property on it
# and emit a build rule that runs the custom command above on demand.
target_sources(burstmerge PRIVATE "${BURSTMERGE_SPIRV_INL}")
set_source_files_properties("${BURSTMERGE_SPIRV_INL}" PROPERTIES
    GENERATED TRUE
)

if(BURSTMERGE_GPU_FP64)
    target_compile_definitions(burstmerge PRIVATE BURSTMERGE_HAVE_GPU_FP64)
    message(STATUS "GPU fp64 dense alignment: enabled (36 shaders)")
else()
    message(STATUS "GPU fp64 dense alignment: disabled (35 shaders, default)")
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

if(BURSTMERGE_GPU_FP64)
    target_compile_definitions(burstmerge PRIVATE BURSTMERGE_HAVE_GPU_FP64)
endif()

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
