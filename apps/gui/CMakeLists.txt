# =============================================================================
# apps/gui/CMakeLists.txt
# -----------------------------------------------------------------------------
# burstmerge_gui: 基于 wxWidgets 3.2 的跨平台 GUI 前端.
#
# 关键性质:
#   - 此文件仅在顶层 CMakeLists.txt 检测到 wx 源码可用后才被 add_subdirectory.
#     没装 wx 时本文件根本不会被处理, 因此 CLI/console/tests/path 与 GUI 全解耦.
#   - 不修改 apps/cli/* 和 apps/console/* 任何文件, GUI 与 CLI 平级共用核心 lib.
#   - process_thread.cpp 通过 burstmerge/api.h 驱动 PipelineOrchestrator;
#     不会 spawn CLI 进程, GUI 与 CLI 完全独立.
#
# wxWidgets 编译策略:
#   wxWidgets 3.2 的 CMake 模式自带 project() 指令 当 add_subdirectory 进来时会
#   重新 enter 一个 project() 上下文. 为避免它修改顶层 project() 设的全局变量,
#   在 add_subdirectory 之前先把关键 wx options 设好:
#     - wxBUILD_SHARED=OFF        静态库, 方便与 burstmerge 静态库一同链接
#     - wxBUILD_MONOLITHIC=OFF    多分片 lib (与 wX_TARGETS 列表对应)
#     - wxBUILD_PRECOMP=OFF       MinGW GCC 11.x + wx 3.2 的 PCH 有兼容问题,
#                                 关闭 PCH 以避免 wxbase 编译 死锁 Error 1
#     - wxBUILD_SAMPLES/TESTS/...
#                                全部 OFF, 不浪费编译时间
#     - wxBUILD_INSTALL=OFF       不生成 install 目标
# =============================================================================

# wxWidgets is added once by the top-level CMakeLists.txt. Both GUI frontends
# consume the same static targets and codec configuration.

set(BURSTMERGE_GUI_SOURCES
    main.cpp
    gui_utils.h
    gui_utils.cpp
    panels.h
    panels.cpp
    process_thread.h
    process_thread.cpp
    thumbnail_loader.h
    thumbnail_loader.cpp
    main_frame.h
    main_frame.cpp
)

if(BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS)
    add_executable(thumbnail_preview_diagnostic
        thumbnail_preview_diagnostic.cpp
        thumbnail_loader.h
        thumbnail_loader.cpp
    )
    target_link_libraries(thumbnail_preview_diagnostic PRIVATE
        ${BURSTMERGE_WXWIDGETS_TARGETS}
    )
endif()

if(BURSTMERGE_BUILD_MAIN_GUI AND WIN32)
    add_executable(burstmerge_gui WIN32
        ${BURSTMERGE_GUI_SOURCES}
        burstmerge_gui.rc
    )
elseif(BURSTMERGE_BUILD_MAIN_GUI)
    add_executable(burstmerge_gui
        ${BURSTMERGE_GUI_SOURCES}
    )
endif()

# WIN32 关键字让 CMake 默认把 subsystem 设为 WINDOWS (无控制台黑窗).
# MinGW/GCC 上需要额外 -mwindows 让链接器收集正确的运行时入口与库依赖.
# 不加 -mwindows 时会出现 "undefined reference to `WinMain@16`" 之类错误.
if(BURSTMERGE_BUILD_MAIN_GUI AND MINGW)
    target_link_options(burstmerge_gui PRIVATE -mwindows)
endif()

# Link BurstMerge core static lib (PIMPL, hides DNG SDK headers from GUI).
if(BURSTMERGE_BUILD_MAIN_GUI)
    target_link_libraries(burstmerge_gui PRIVATE
        burstmerge
        ${BURSTMERGE_WXWIDGETS_TARGETS}
    )
endif()

if((BURSTMERGE_BUILD_MAIN_GUI OR BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS) AND
   EXISTS "${TIFF_INCLUDE_DIR}/tiffio.h" AND EXISTS "${TIFF_LIBRARY}")
    if(BURSTMERGE_BUILD_MAIN_GUI)
        target_compile_definitions(burstmerge_gui PRIVATE BURSTMERGE_GUI_HAVE_TIFF=1)
        target_include_directories(burstmerge_gui PRIVATE "${TIFF_INCLUDE_DIR}")
        target_link_libraries(burstmerge_gui PRIVATE "${TIFF_LIBRARY}")
    endif()
    if(BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS)
        target_compile_definitions(thumbnail_preview_diagnostic PRIVATE BURSTMERGE_GUI_HAVE_TIFF=1)
        target_include_directories(thumbnail_preview_diagnostic PRIVATE "${TIFF_INCLUDE_DIR}")
        target_link_libraries(thumbnail_preview_diagnostic PRIVATE "${TIFF_LIBRARY}")
    endif()
endif()

# wxWidgets 3.2 静态库 (MinGW MSW) 需要外部显式链一批 Win32 系统库. 注意:
#   * wxCMake 模式在自己 target 的 INTERFACE_LINK_LIBRARIES 中只声明了一部分
#     依赖, 与 .a 文件实际需要的符号集可能不完整, 因此在主项目里显式补齐.
#   * 在 Linux/macOS 上这些链库名都不存在, 通过 WIN32 守卫只在 Windows 添加.
if(BURSTMERGE_BUILD_MAIN_GUI AND WIN32)
    target_link_libraries(burstmerge_gui PRIVATE
        gdi32
        user32
        comctl32
        comdlg32
        shell32
        ole32
        oleaut32
        uuid
        advapi32
        rpcrt4
        uxtheme
        imm32
        winmm
        ws2_32
    )
endif()

if(BURSTMERGE_BUILD_GUI_THUMBNAIL_DIAGNOSTICS AND WIN32)
    target_link_libraries(thumbnail_preview_diagnostic PRIVATE
        gdi32 user32 comctl32 comdlg32 shell32 ole32 oleaut32 uuid advapi32
        rpcrt4 uxtheme imm32 winmm ws2_32
    )

    # The core library links the project's shared libtiff build. Unlike CLI/test
    # invocations, a desktop app launched from Explorer cannot rely on a prepared
    # PATH, so keep the required runtime next to the executable.
    set(BURSTMERGE_TIFF_RUNTIME "${TIFF_ROOT}/bin/libtiff.dll")
    if(EXISTS "${BURSTMERGE_TIFF_RUNTIME}")
        add_custom_command(TARGET thumbnail_preview_diagnostic POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BURSTMERGE_TIFF_RUNTIME}"
                "$<TARGET_FILE_DIR:thumbnail_preview_diagnostic>"
                COMMENT "Copying libtiff runtime for thumbnail diagnostics"
        )
    endif()
endif()

if(BURSTMERGE_BUILD_MAIN_GUI AND WIN32)
    set(BURSTMERGE_TIFF_RUNTIME "${TIFF_ROOT}/bin/libtiff.dll")
    if(EXISTS "${BURSTMERGE_TIFF_RUNTIME}")
        add_custom_command(TARGET burstmerge_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BURSTMERGE_TIFF_RUNTIME}"
                "$<TARGET_FILE_DIR:burstmerge_gui>"
                COMMENT "Copying libtiff runtime for burstmerge_gui"
        )
    endif()
endif()
