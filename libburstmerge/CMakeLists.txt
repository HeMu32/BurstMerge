# libburstmerge core static library
add_library(burstmerge STATIC
    src/api.cpp
    src/api_c.cpp
    src/io/dng_sdk_bridge.cpp
    src/io/dng_reader.cpp
    src/io/dng_writer.cpp
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
