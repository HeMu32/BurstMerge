# =============================================================================
# apps/raw_resize_gui/CMakeLists.txt
# -----------------------------------------------------------------------------
# raw_resize_gui: 基于 wxWidgets 的独立单帧 RAW 缩放工具图形前端
# =============================================================================

set(RAW_RESIZE_GUI_SOURCES
    main.cpp
)

if(WIN32)
    add_executable(burstmerge_raw_resize_gui WIN32
        ${RAW_RESIZE_GUI_SOURCES}
        raw_resize_gui.rc
    )
else()
    add_executable(burstmerge_raw_resize_gui
        ${RAW_RESIZE_GUI_SOURCES}
    )
endif()

if(MINGW)
    target_link_options(burstmerge_raw_resize_gui PRIVATE -mwindows)
endif()

target_link_libraries(burstmerge_raw_resize_gui PRIVATE
    burstmerge
    ${BURSTMERGE_WX_TARGETS}
)

if(WIN32)
    target_link_libraries(burstmerge_raw_resize_gui PRIVATE
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

    set(BURSTMERGE_TIFF_RUNTIME "${TIFF_ROOT}/bin/libtiff.dll")
    if(EXISTS "${BURSTMERGE_TIFF_RUNTIME}")
        add_custom_command(TARGET burstmerge_raw_resize_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BURSTMERGE_TIFF_RUNTIME}"
                "$<TARGET_FILE_DIR:burstmerge_raw_resize_gui>"
                COMMENT "Copying libtiff runtime for burstmerge_raw_resize_gui"
        )
    endif()
endif()
