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
    "${DNG_SDK_ROOT}/dng_sdk/source/dng_jxl.cpp"
)

# Add JXL stubs (replaces dng_jxl.cpp for builds without libjxl)
list(APPEND DNG_SDK_SRCS "${DNG_SDK_ROOT}/dng_sdk/source/dng_jxl_stubs.cpp")

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
