# =============================================================================
# FindWxWidgetsBurstMerge.cmake
# -----------------------------------------------------------------------------
# 用于定位 BurstMerge 项目自带的 wxWidgets 源码树, 用作 burstmerge_gui 的可选
# GUI 后端.
#
# 策略说明:
#   BurstMerge 把 wxWidgets 3.2.x 源码 vendored 在 3rdparty/wxWidgets/, 并通过
#   CMake 的 add_subdirectory() 直接在主项目编译时一并构建 wx 静态库; 产出的目
#   标名 (wxcore / wxbase / wxadv / wxaui / wxpropgrid / wxhtml / wxnet / wxxml)
#   由父项目 target_link_libraries() 直接消费, 不需要 install/ 形式的产物.
#
#   这种集成方式的好处:
#     - 不需要用户预先在 3rdparty/wxWidgets/install/ 跑一次 build
#     - 不需要事先知道 wx 给出的实际库文件名 (随 v3.2 CMake 模式有变动)
#     - 主项目和 wx 共用编译器/标准/标志, 减少配置不一致风险
#     - 没装 wx 源码 (例如没解压 tar) 时此脚本判 NOT FOUND, 顶层 CMake 优雅跳过
#       GUI 编译, CLI/console/tests 完全不感知 此模块存在.
#
# 唯一输入: 项目根目录下的 3rdparty/wxWidgets/CMakeLists.txt 是否存在.
#
# 输出变量:
#   BURSTMERGE_WX_FOUND          - BOOL, 是否成功定位到 wxWidgets 源码
#   BURSTMERGE_WX_SOURCE_DIR      - wxWidgets 根目录 (含 CMakeLists.txt)
#   BURSTMERGE_WX_TARGETS         - 需要被 burstmerge_gui 链接的 wx target 列表
# =============================================================================

# 项目根目录由顶层 CMake 传入 (PROJECT_SOURCE_DIR 已经在 BurstMerge 的顶层 project()
# 之后定义). 但若在某些子目录 add_subdirectory 上下文中 PROJECT_SOURCE_DIR 是 BurstMerge
# 的根而非 wxWidgets 的, 在此手动 fallback 到已知的相对路径.
if(NOT DEFINED BURSTMERGE_PROJECT_ROOT)
    set(BURSTMERGE_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
endif()

# local_config.cmake 通常会把 WX_ROOT 指向 3rdparty/wxWidgets (即与默认值一致);
# 若用户改写, 此处遵从用户值. 也可不设, 即默认 3rdparty/wxWidgets.
if(DEFINED WX_ROOT AND NOT WX_ROOT STREQUAL "")
    set(BURSTMERGE_WX_SOURCE_DIR "${WX_ROOT}")
else()
    set(BURSTMERGE_WX_SOURCE_DIR "${BURSTMERGE_PROJECT_ROOT}/3rdparty/wxWidgets")
endif()

# v3.2.x 的 wxWidgets CMake 顶层文件 即可定位
if(EXISTS "${BURSTMERGE_WX_SOURCE_DIR}/CMakeLists.txt" AND EXISTS "${BURSTMERGE_WX_SOURCE_DIR}/include/wx/wx.h")
    set(BURSTMERGE_WX_FOUND TRUE)
    # 这些是 BurstMerge GUI 实际会引用到的 wx components; 对应 wxWidgets/build/cmake/lib/
    # 下 CMake add_library() 暴露的 target 名 (不带 wx_ 前缀 与 libwx32 后缀 的目标短名).
    set(BURSTMERGE_WX_TARGETS
        wxcore        # wxMSW 核心 (事件循环、窗口、控件、菜单、布局)
        wxbase        # 基础库 (string、thread、file、regexp、IPC base)
        wxadv         # wxPropertyGrid 等高级控件基础
        wxaui         # wxAuiManager (抽屉式浮动布局, Options 面板要用)
        wxpropgrid    # wxPropertyGrid (Options 面板的可能用)
        wxhtml        # wxHTML 简单渲染 (About 对话框会用)
        wxnet         # 网络 (wxapp 内部依赖)
        wxxml         # XML 解析 (xrc 资源可选, 但 wxbase_xml 一并链)
    )
    message(STATUS "GUI: wxWidgets source located at ${BURSTMERGE_WX_SOURCE_DIR}")
    message(STATUS "GUI: will be built in-tree via add_subdirectory (PCH disabled for MinGW compatibility)")
else()
    set(BURSTMERGE_WX_FOUND FALSE)
    message(STATUS "GUI: wxWidgets source not found at ${BURSTMERGE_WX_SOURCE_DIR}")
    message(STATUS "     (CLI/console/tests are unaffected; to enable GUI, extract wxWidgets-3.2.x")
    message(STATUS "      release tarball into 3rdparty/wxWidgets/)")
endif()
