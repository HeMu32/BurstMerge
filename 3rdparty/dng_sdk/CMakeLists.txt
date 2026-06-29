# DNG SDK 内部静态库
# 完全隐藏于 libburstmerge 内部, 不暴露任何头文件给上层

set(DNG_SDK_SRCS
    dng_1d_function.cpp
    dng_1d_table.cpp
    dng_abort_sniffer.cpp
    dng_area_task.cpp
    dng_bad_pixels.cpp
    dng_big_table.cpp
    dng_bmff.cpp
    dng_bottlenecks.cpp
    dng_camera_profile.cpp
    dng_color_space.cpp
    dng_color_spec.cpp
    dng_date_time.cpp
    dng_exceptions.cpp
    dng_exif.cpp
    dng_file_stream.cpp
    dng_filter_task.cpp
    dng_fingerprint.cpp
    dng_gain_map.cpp
    dng_globals.cpp
    dng_host.cpp
    dng_hue_sat_map.cpp
    dng_ifd.cpp
    dng_image.cpp
    dng_image_writer.cpp
    dng_info.cpp
    dng_iptc.cpp
    dng_jpeg_image.cpp
    dng_jpeg_memory_source.cpp
    dng_lens_correction.cpp
    dng_linearization_info.cpp
    dng_local_string.cpp
    dng_lossless_jpeg.cpp
    dng_lossless_jpeg_shared.cpp
    dng_matrix.cpp
    dng_memory.cpp
    dng_memory_stream.cpp
    dng_misc_opcodes.cpp
    dng_mosaic_info.cpp
    dng_mutex.cpp
    dng_negative.cpp
    dng_opcode_list.cpp
    dng_opcodes.cpp
    dng_orientation.cpp
    dng_parse_utils.cpp
    dng_pixel_buffer.cpp
    dng_point.cpp
    dng_preview.cpp
    dng_rational.cpp
    dng_read_image.cpp
    dng_rect.cpp
    dng_ref_counted_block.cpp
    dng_reference.cpp
    dng_render.cpp
    dng_resample.cpp
    dng_safe_arithmetic.cpp
    dng_shared.cpp
    dng_simple_image.cpp
    dng_spline.cpp
    dng_stream.cpp
    dng_string.cpp
    dng_string_list.cpp
    dng_tag_types.cpp
    dng_temperature.cpp
    dng_tile_iterator.cpp
    dng_tone_curve.cpp
    dng_update_meta.cpp
    dng_utils.cpp
    dng_xmp.cpp
    dng_xmp_sdk.cpp
    dng_xy_coord.cpp
)

list(TRANSFORM DNG_SDK_SRCS PREPEND "${DNG_SDK_ROOT}/dng_sdk/source/")

# 排除独立的验证工具 + 因 qDNGUseXMP=0 无法编译的文件
list(REMOVE_ITEM DNG_SDK_SRCS
    "${DNG_SDK_ROOT}/dng_sdk/source/dng_validate.cpp"
    "${DNG_SDK_ROOT}/dng_sdk/source/dng_xmp_sdk.cpp"
    "${DNG_SDK_ROOT}/dng_sdk/source/dng_update_meta.cpp"
)

# JPEG XL support: build vendored libjxl (highway + brotli + skcms are
# pulled in as add_subdirectory by libjxl's own third_party/CMakeLists.txt)
# as a static, decoder-capable library, then compile the real dng_jxl.cpp
# bridge instead of the throw-on-every-call stubs.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JPEGLI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_VIEWERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_AVX512 OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_AVX512_SPR OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_AVX512_ZEN4 OFF CACHE BOOL "" FORCE)
set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "" FORCE)
set(JPEGXL_FORCE_SYSTEM_BROTLI OFF CACHE BOOL "" FORCE)
set(JPEGXL_FORCE_SYSTEM_HWY OFF CACHE BOOL "" FORCE)
set(JPEGXL_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# libjxl / Highway require precise IEEE-754 floating point (the arithmetic
# coder and color transforms break under -ffast-math) and Highway drives its
# own per-target ISA dispatch, so -march=native must not leak in. Temporarily
# strip both from the Release flag set for this subdirectory only.
set(_bm_saved_cxx_release "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "-ffast-math" "" _bm_clean_cxx_release "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "-march=native" "" _bm_clean_cxx_release "${_bm_clean_cxx_release}")
set(CMAKE_CXX_FLAGS_RELEASE "${_bm_clean_cxx_release}")
add_subdirectory(
    "${DNG_SDK_ROOT}/libjxl/libjxl"
    "${CMAKE_BINARY_DIR}/libjxl_build"
    EXCLUDE_FROM_ALL)
set(CMAKE_CXX_FLAGS_RELEASE "${_bm_saved_cxx_release}")

list(APPEND DNG_SDK_SRCS "${DNG_SDK_ROOT}/dng_sdk/source/dng_jxl.cpp")

add_library(dng_sdk STATIC ${DNG_SDK_SRCS})

target_include_directories(dng_sdk SYSTEM PRIVATE
    ${DNG_SDK_INCLUDE}
)

target_compile_definitions(dng_sdk PUBLIC
    $<$<PLATFORM_ID:Windows>:qWinOS>
    $<$<PLATFORM_ID:Linux>:qLinux>
    $<$<PLATFORM_ID:Darwin>:qMacOS>
    DNG_SDK_STATIC
    _FILE_OFFSET_BITS=64
    _LARGEFILE64_SOURCE
    qDNGUseXMP=0
    qDNGXMPFiles=0
    qDNGXMPDocOps=0
    # MinGW-w64 自带 pthread.h ?DNG SDK ?pthread 模拟冲突
    $<$<CXX_COMPILER_ID:GNU>:qDNGThreadSafe=0>
)

set_target_properties(dng_sdk PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
    CXX_VISIBILITY_PRESET hidden
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dng_sdk PRIVATE
        -fpermissive -w -Wno-deprecated
        -Wno-enum-compare -Wno-format-truncation
        -Wno-class-memaccess
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
endif()
if(MSVC)
    target_compile_options(dng_sdk PRIVATE /W0 /wd4996)
endif()

# zlib is used by DNG SDK for ZIP compression
target_link_libraries(dng_sdk PUBLIC z)

# libjxl for JPEG XL decode/encode (static). The full `jxl` target carries
# both encoder and decoder objects (dng_jxl.cpp references both); jxl_threads
# provides the resizable parallel runner used by the decoder.
target_link_libraries(dng_sdk PRIVATE jxl jxl_threads)
