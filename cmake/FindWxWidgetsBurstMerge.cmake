# =============================================================================
# FindWxWidgetsBurstMerge.cmake
# -----------------------------------------------------------------------------
if(DEFINED BURSTMERGE_WXWIDGETS_ROOT AND NOT BURSTMERGE_WXWIDGETS_ROOT STREQUAL "")
    set(BURSTMERGE_WXWIDGETS_SOURCE_DIR "${BURSTMERGE_WXWIDGETS_ROOT}")
else()
    set(BURSTMERGE_WXWIDGETS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/3rdparty/wxWidgets")
endif()

if(EXISTS "${BURSTMERGE_WXWIDGETS_SOURCE_DIR}/CMakeLists.txt" AND
   EXISTS "${BURSTMERGE_WXWIDGETS_SOURCE_DIR}/include/wx/wx.h")
    set(BURSTMERGE_WXWIDGETS_FOUND TRUE)
    set(BURSTMERGE_WXWIDGETS_TARGETS
        wxcore
        wxbase
        wxadv
        wxaui
        wxpropgrid
        wxhtml
        wxnet
        wxxml
    )
    message(STATUS "wxWidgets: source located at ${BURSTMERGE_WXWIDGETS_SOURCE_DIR}")
else()
    set(BURSTMERGE_WXWIDGETS_FOUND FALSE)
    message(STATUS "wxWidgets: source not found at ${BURSTMERGE_WXWIDGETS_SOURCE_DIR}")
endif()
