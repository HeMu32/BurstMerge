# =============================================================================
# FindWxWidgetsBurstMerge.cmake
# -----------------------------------------------------------------------------
if(NOT DEFINED BURSTMERGE_PROJECT_ROOT)
    set(BURSTMERGE_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
endif()

if(DEFINED WX_ROOT AND NOT WX_ROOT STREQUAL "")
    set(BURSTMERGE_WX_SOURCE_DIR "${WX_ROOT}")
else()
    set(BURSTMERGE_WX_SOURCE_DIR "${BURSTMERGE_PROJECT_ROOT}/3rdparty/wxWidgets")
endif()

if(EXISTS "${BURSTMERGE_WX_SOURCE_DIR}/CMakeLists.txt" AND EXISTS "${BURSTMERGE_WX_SOURCE_DIR}/include/wx/wx.h")
    set(BURSTMERGE_WX_FOUND TRUE)
    set(BURSTMERGE_WX_TARGETS
        wxcore
        wxbase
        wxadv
        wxaui
        wxpropgrid
        wxhtml
        wxnet
        wxxml
    )
    message(STATUS "GUI: wxWidgets source located at ${BURSTMERGE_WX_SOURCE_DIR}")
else()
    set(BURSTMERGE_WX_FOUND FALSE)
    message(STATUS "GUI: wxWidgets source not found at ${BURSTMERGE_WX_SOURCE_DIR}")
endif()
