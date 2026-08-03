实施方案（进入计划模式 — 只读）

> 历史实施记录。当前 GUI 的实际构建、依赖、工作流和缩略图行为以
> `apps/gui/README.md` 为准；本文保留早期设计过程和已完成事项，部分路径
> 与 wxWidgets 构建方式已经过时。

一、获取 wxWidgets
下载 https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.5/wxWidgets-3.2.5.tar.bz2 → 解压到 3rdparty/wxWidgets/
压缩包约 25MB，解压后约 200MB 源码
使用 MinGW + build/msw/makefile.gcc 编译静态库到 3rdparty/wxWidgets/install/
配置: UNICODE + 静态 + 复用系统已装的 libjpeg/libpng/libtiff/zlib(在 C:\MinGW\lib\,通过 GCC 环境变量传入)
产物: 3rdparty/wxWidgets/install/lib/libwx_mswu_core-3.2.a、libwx_baseu-3.2.a、libwx_adv-3.2.a、libwx_aui-3.2.a(可选)、libwx_propgrid-3.2.a 等以及头文件树 install/include/wx/*
.gitignore 追加 3rdparty/wxWidgets/(整目录不入库)
apps/gui/README.md 记录重建步骤;该文件入库
二、CMake 配置(核心:可完全排除)
顶层 CMakeLists.txt 在末尾追加(不动现有任何行)
# ---- GUI (wxWidgets, optional) ----
option(BURSTMERGE_BUILD_GUI "Build burstmerge_gui with wxWidgets (auto-skip if wx not found)" ON)
if(BURSTMERGE_BUILD_GUI)
    include(cmake/FindWxWidgetsBurstMerge.cmake)
    if(BURSTMERGE_WX_FOUND)
        add_subdirectory(apps/gui)
    else()
        message(STATUS "wxWidgets not found - GUI app will NOT be built (CLI/tests unaffected)")
        set(BURSTMERGE_BUILD_GUI OFF CACHE BOOL "Build GUI" FORCE)
    endif()
endif()
关键性质:

-DBURSTMERGE_BUILD_GUI=OFF → 一行不扫 GUI;apps/gui 不入工程;完全无 wx 依赖
默认 ON 但没装 wx → FindWxWidgetsBurstMerge.cmake 检测失败, fallback STATUS 跳过, set OFF FORCE;CLI/tests 不受影响
apps/cli、apps/console、libburstmerge/test 的 CMakeLists.txt 完全不改一行
cmake/FindWxWidgetsBurstMerge.cmake(新文件)
按项目"路径集中在 local_config.cmake"惯例做 三段查找:

优先 local_config.cmake 显式指定的 WX_ROOT或 WX_INCLUDE_DIR+WX_LIBRARY_DIR+WX_LIBRARIES(用户本机配置)
次选 CMake 自带 find_package(wxWidgets) 模块(对系统装好的 wxWidgets 走 find_package_handle_standard_args,但不一定有 CMake Config)
检查存在:头 wx/wx.h 与库文件存在性校验 + wx-config --version 不在 PATH 时跳过系统查找
输出 BURSTMERGE_WX_FOUND、BURSTMERGE_WX_INCLUDE_DIRS、BURSTMERGE_WX_LIBRARIES。

local_config.cmake 末尾追加(本机配置,不入库)
# ---- wxWidgets (本地静态编译产物) ----
set(WX_ROOT             "${PROJECT_ROOT}/3rdparty/wxWidgets/install")
set(WX_INCLUDE_DIR      "${WX_ROOT}/include")
set(WX_LIBRARY_DIR      "${WX_ROOT}/lib")
set(WX_LIBRARIES
    wx_mswu_core-3.2 wx_mswu_adv-3.2 wx_mswu_aui-3.2 wx_mswu_propgrid-3.2
    wx_baseu-3.2 wx_baseu_net-3.2 wx_baseu_xml-3.2
)
apps/gui/CMakeLists.txt(新文件,完全独立)
add_executable(burstmerge_gui WIN32 main.cpp)
target_link_libraries(burstmerge_gui PRIVATE
    burstmerge
    ${BURSTMERGE_WX_LIBRARIES})
target_include_directories(burstmerge_gui PRIVATE
    ${BURSTMERGE_WX_INCLUDE_DIRS}
    ${CXXOPTS_INCLUDE})
target_link_options(burstmerge_gui PRIVATE -mwindows)
CMake WIN32 让默认 subsystem=WINDOWS(无控制台黑窗),-mwindows 给 GCC 链接器明确。

三、GUI 应用设计(完整覆盖所有能力)
主窗口布局(顶层)
+--------------------------------------------------------------+
| [MenuBar: File / Edit / Help]                                |  ← 1. menu bar
+--------------------------------------------------------------+
| [Toolbar buttons: New Queue | Remove Queue | Start | Stop]   |  ← toolbar
+----------------+---------------------------------------------+
|                |   Queue #1  Queue #2  Queue #3 ...  [+ Add]  |  ← 3. queue 标签页
|   Bin          | +-----------------------------------------+ |
| (icon+name)    | | (selected queue)                       | |
|   复选列表      | | [icon] IMG_0001.dng    (file icon)      | |
|                | | [icon] IMG_0002.dng                       | |
|                | | ...                                      | |
|                | +-----------------------------------------+ |
+----------------+---------------------------------------------+
|  Options panel (right side or top-right, all capabilities)    |  ← 4. Options
|   [Pipeline tab] [Merge tab] [Align tab] [Exposure tab] ...   |
+--------------------------------------------------------------+
| [Status bar: status text | progress bar % | current stage]   |
+--------------------------------------------------------------+
| [Log text area, multiline read-only]                          |
+--------------------------------------------------------------+
推荐使用 wxAuiManager 做可拖浮的布局(让用户可隐藏 Bin/Options),或 wxSplitterWindow 固定结构。基于复杂度考虑，采用 wxFrame + wxSplitterWindow 左右分栏 + 右侧 wxNotebook(每 tab 一个 Queue) 是最稳妥稳定的方案。

1. MenuBar / Toolbar
MenuBar:
File: Add Files... / Add Folder... / Clear Bin / Exit
Edit: New Queue / Remove Current Queue / Preferences...(占位)
Process: Start / Start All Queues(按 Queue 顺序串行处理) / Stop(暂不实现,先置灰)
Help: About
Toolbar: Start / New Queue / Remove Queue / Clear Bin / About 图标按钮
2. Bin (左侧,文件暂存)
控件: wxListView(report mode + wxLC_LIST 或 report mode 自带图标列) 或 wxListCtrl
用 wxImageList 放文件类型图标 (Raw/DNG/PNG/JPG/TIFF,共几种)
角标 indicator 实现: wxListCtrl::SetItemImage 给每行插一个图标索引;用一个预渲染的小图标池,每个"引用次数 n"对应一个角标小图(SetItemImage(row, badge_index[n]))。不支持的角标位置(右上角覆盖),用一个小 bitmap 合成: 加载基础文件图标 + 右上角绘制 8×8 黄色方块带白字数字;badge_index[0] = 无角标的图标,badge_index[1..N] = 对应黄色角标。保持简单: 最多 5 个槽位(1~5),若超过 5 显示黄色方块带 "5+"
多选: wxListCtrl::SetItemState(state, wxLIST_STATE_SELECTED)
拖入: wxFileDropTarget 子类挂在 Bin 上 → OnDropFiles 解析路径,只暂存不转换(符合要求"转换只发生在开始处理后")
拖出: Bin 启用 wxListCtrl 的 DnD source — 用 wxCustomDataObject 定义自定义格式 wxEVT_DRAG_BIN_ITEMS,文件拖起时打包路径列表;DnD source 用 wxDropSource::DoDragDrop
3. Queue 列表(右侧内侧)
容器: wxNotebook — 每个 tab 是一个 Queue
每个 Queue 顶部有一个 wxButton "Add Selected"(点击时把 Bin 中当前选中的项追加到该 Queue)
Queue 内部: wxListCtrl(icon+name); 同样地拖入用 wxFileDropTarget 子类
从 Bin 拖入 Queue: Queue 也挂自定义 wxDropTarget 接受 wxCustomDataObject 的 wxEVT_DRAG_BIN_ITEMS; 接收后在 Bin 中也确保有该项(如果从外部拖入 Queue 则同步添加到 Bin — 符合要求 3. "在 queue 中就一定在 bin 中")
加号按钮 [+] 在 Notebook tab 栏右侧 (或单独按钮在 notebook 旁) → 新建一个空 Queue tab Queue #n+1
关闭 Queue: tab 上右键 → "Remove this queue"; 或 toolbar 的 Remove Queue 按钮。删除 Queue 时减少 Bin 中对应文件的引用计数(每删一条 Queue 项,对应 Bin 项的引用计数减 1; 若新值 = 0, 角标变无; = 0 还继续存在 bin 不删)
Queue 顺序: tab 顺序即处理顺序(单次"Start"按 Queue 1→2→N 串行处理)
Queue 内项可单独删除(右键 → "Remove from queue")
4. Options 面板(覆盖所有 CLI 能力)
放在右侧下方,作为 wxNotebook 隔离 tab,或主窗口第三栏。结构上推荐: Options 直接作为主窗口右侧第三个 wxWindow (上下分栏: 上面 Notebook of Queues, 下面 Options)。或使用 wxAuiManager 把 Options 设成可隐藏的下层面板。

Options 内部: wxNotebook 多个 tab (对应 CLI 各能力):

Pipeline(基本):
Backend: wxChoice (CPU / Vulkan)
GPU Device Index(-1=auto): wxSpinCtrl(若选 CPU 置灰)
Tile Size: wxSpinCtrl (16~256)
Bit Depth: wxChoice (8/10/12/14/16)
Output Format: wxChoice (Auto / PNG / JPEG / BMP / TIFF / DNG)
Merge:
Algorithm: wxChoice (Spatial / Frequency / Temporal-Average / Exp-Bracket-Average / Median)
Spatial Mode: wxChoice (Standard / Linear)(仅 Spatial 时启)
Frequency Mode: wxChoice (Laplacian / Wiener / Wiener-Robust)(仅 Frequency 时启)
Align:
Mode: wxChoice (Standard / Dense-Tile / Frequency / Skip)
Align Gamma: wxSlider (0.1~2.0, 步长 0.05, 显示数字)
Smooth Tile Field: wxCheckBox
Exposure:
Mode: wxChoice (Off / Linear / Curve)
Curve Mode: wxChoice (Global / Local-Reinhard)(仅 Curve 时启)
Stops: wxSlider (-3.0~+3.0, 步长 0.1)
Cleanup / Misc:
Highlight Recovery: wxCheckBox(默认勾选)
Hot Pixel Repair: wxCheckBox(默认不勾)
Noise Reduction(Merge 不为 Temporal/ExpBkt/Median 时启): wxSlider (0~30, 步长 0.5)
每个控件联动控制可用性: e.g. 选 Spatial 时禁用 Frequency Mode,选 Temporal 时禁用 Noise Reduction。

5. 处理流程("开始处理"按下后)
严格只在用户点击 Start 时才进行转换/处理:

读取所有 Queue 和当前 Options → 按队列顺序串行 (单线程的 GUI 工作线程内,串行跑 n 个 BurstMerge::Process):
每个 Queue 独立创建 burstmerge::BurstMerge bm(backend) 实例
该 Queue 的所有文件路径 → bm.AddImage()
Options 各控件值 → burstmerge::Settings
输出目录自动生成: <bin_dir>/out_q<index>_<timestamp>/ 或用户指定的文件夹
注册 bm.SetProgressCallback → 反映到进度条(用 wxThreadEvent 投递主线程)
UI 状态: Start 按钮禁用 / 所有 Queue 和 Bin 处于只读 / 状态栏显示 Processing Queue #1: align tilefield ... 23%,进度条滚动 / 日志窗追加进度文字
工作线程: 用 wxThread(joinable) , 在 Entry() 中循环跑各个 Queue; 用 wxQueueEvent 发 wxThreadEvent 给主线程 (主线程通过 Bind) 切回 UI 线程更新 wxGauge 和日志;严禁 子线程直接调任何 wx 控件 API
进度回调的线程切换: SetProgressCallback 的 fn 在 BurstMerge 内部(OpenMP 或 GPU 阶段)被调用时,很可能不是 GUI 线程 — 进度回调内只做 wxTheApp->GetTopWindow()->GetEventHandler()->QueueEvent(new wxThreadEvent(wxEVT_THREAD, id, payload)),主线程在 wxEVT_THREAD 处理函数里更新 UI
错误处理: Process() 返回 Result,若某 Queue 失败,状态栏提示 + 日志追加红色 Error: ...,询问/继续下一 Queue 或中止(选项中"Stop on first error" 默认开)
不调用 CLI: apps/gui/main.cpp 直接 #include "burstmerge/api.h", 与 CLI 完全同级,公平地共用核心库 (符合要求"作为不同前端")
6. DnD 细节
自定义 DnD 数据格式: 用 wxCustomDataObject 注册 BurstMergeBinItems 格式; payload = 序列化的路径列表
拖拽时显示自定义图标: wxDropSource::GiveFeedback(wxDragResult) 重写,设置自定义 cursor(可选,默认箭头-加号即可)
拖入判断: wxDropTarget::OnData 返回 true 时合并 Bin; Bin 单一来源,Queue 多挂一个 wxDropTarget 子类 QueueDropTarget
Bin 和 Queue 都接受 wxFileDropTarget(外部资源管理器拖入)和自定义 wxCustomDataObject (Bin 内拖出); Queue 只接受拖入而不接受拖出 (符合要求 - 用户复选 Bin 后拖动或点击 Add 按钮)
拖入 Queue 时同步加 Bin: QueueDropTarget::OnData 先把外部路径加 Bin,再加 Queue,确保"在 queue 中就一定在 bin 中"
从 Bin 拖到 Queue: Bin 项目本身已存在 (不需要再添加 Bin),只增加该 Queue 中文件 + Bin 角标 +1
7. Bin 角标 indicator (引用计数)
维护 std::unordered_map<std::string /*abs path*/, int /*ref count*/> m_bin_refs
AddToBin(path): 若 path 不在 m_bin_refs 则加进 Bin 视图 (角标 0); 不增基计数
IncrementRef(path): m_bin_refs[path]++ + 刷新该 row 的 ImageIndex 到对应角标 bitmap
DecrementRefFromQueue(path): m_bin_refs[path]-- (不 < 0) + 刷新; 若 == 0,仍保留在 Bin (符合要求 4. 添加后 bin 不消失)
角标 bitmap 池 (引用次数 0/1/2/3/4/5+: 共 6 张),通过 wxImageList 预渲染 (基础文件图标 + 右上角黄色 8×8 方块带白字数字布局)
8. apps/gui/main.cpp 框架
#include "burstmerge/api.h"
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/thread.h>
#include <wx/dnd.h>

// 自定义 DnD format
wxDEFINE_EVENT(wxEVT_BM_PROGRESS, wxThreadEvent);

class BurstMergeApp : public wxApp {
    bool OnInit() override;
};
wxIMPLEMENT_APP(BurstMergeApp);

// 类似 BinPanel, QueuePanel, OptionsPanel, MainFrame
class MainFrame : public wxFrame { /* menubar + toolbar + 2 splitters + log + status */ };

// 工作线程
class ProcessThread : public wxThread {
    void* Entry() override;
};
四、新增 / 修改文件清单（只读阶段仅列）
新增:

cmake/FindWxWidgetsBurstMerge.cmake
apps/gui/CMakeLists.txt
apps/gui/main.cpp
apps/gui/README.md(记录 wx 重建步骤)
修改:

顶层 CMakeLists.txt(仅末尾追加一段,不动现有行)
local_config.cmake(本机追加 WX_* 路径段,不入库)
.gitignore(追加 3rdparty/wxWidgets/)
完全不动:

apps/cli/*、apps/console/*、apps/cli/CMakeLists.txt
libburstmerge/**(包括 test/)
cmake/CompilerFlags.cmake
现有 local_config.cmake 既有 JPEG/PNG/TIFF 等段
五、验收步骤
不存在 wx 也能编: 临时把 local_config.cmake 中 WX_* 段注释掉 → cmake -B build 应输出 wxWidgets not found - GUI app will NOT be built,且 cmake --build build 仍产 burstmerge_cli.exe / burstmerge_console.exe / 全部测试
选项强制关: cmake -B build -DBURSTMERGE_BUILD_GUI=OFF → 同上,GUI 不参与
完整配置: 加 WX_* 段 + cmake -B build → STATUS "GUI: enabled" + cmake --build build 产 burstmerge_gui.exe
运行: 启动 GUI,从资源管理器拖几个 libburstmerge/test/samples/Seq*/ 的 DNG 进入 Bin(无角标) → 多选 Bin 中的几项 → 拖到 Queue 1(或点 Add 按钮)→ Bin 中该几项出现黄角标 "1" → 新建 Queue 2 → 又拖同样的项到 Queue 2 → Bin 中角标变 "2" → Options 默认值 → Start → 进度条滚动 → 完成后产物文件存在
外部直拖 Queue: 选一个新的 DNG 拖到 Queue 3 → Bin 中也自动出现该项角标 "1"; Queue 3 列表中也存在
多 Queue 串行: 建 2 个 Queue 各加 2 张 → Start → 日志先显示 Queue 1 处理,完成再 Queue 2,进度条每 Queue 单独计时
像素一致性: 用 tools/dump_dng.cpp 或 burstmerge_compare 对比 GUI 和 CLI 同参数同输入的输出像素,应≤0.01% MAD (与现 CPU 路径精度一致)
待你确认的最后事项
好的,Queue 改为纵向排列(每个 Queue 横长条,内部横向一行;Queue 之间纵向成列),Options 用 wxAuiManager 抽屉(默认可见、可隐藏浮动)。输出目录沿用 CLI 行为(默认 ./out,用户可指定)。

让我把最终方案整合下,确认我对最新布局的理解:

最终方案(进入计划模式 — 只读)
主窗口最终布局
+--------------------------------------------------------------------------+
| MenuBar: File / Edit / Process / Help                                    |
| Toolbar: [Start] [New Queue] [Remove Queue] [Clear Bin] [Toggle Options]  |
+--------+----------------------------------------------------------------+
|        | ┌─ Queue #1 ─────────────────────────────  [Add Selected] [x] ┐ |
|        | │ [icon]IMG_001.dng [icon]IMG_002.dng [icon]IMG_003.dng ...    │ |  ← 每个 Queue 横长条
|        | └────────────────────────────────────────────────────────────┘ |
|  Bin   | ┌─ Queue #2 ─────────────────────────────  [Add Selected] [x] ┐ |
| (左侧) | │ [icon]IMG_004.dng [icon]IMG_005.dng ...                       │ |
| 列表   | └────────────────────────────────────────────────────────────┘ |
| icon+  | ┌─ Queue #3 ─────────────────────────────  [Add Selected] [x] ┐ |
| 文件名 | │ (空)                                                          │ |
| 角标  | └────────────────────────────────────────────────────────────┘ |
|        |                [+ New Queue (在最底)]                            |
+--------+----------------------------------------------------------------+
| Options (wxAui pane, 可拖出/隐藏/抽屉):                                  |
|   Notebook: Pipeline | Merge | Align | Exposure | Cleanup              |
+--------------------------------------------------------------------------+
| StatusBar: 处理中状态 | [================50%================] | elapsed  |
+--------------------------------------------------------------------------+
| Log text (可隐藏):                                                       |
+--------------------------------------------------------------------------+
Queue 区: 用 wxScrolledWindow + 内部 wxBoxSizer(wxVERTICAL),每个 Queue 是一个自定义 QueuePanel : wxPanel(横向布局: 标题"Queue #n" + Add 按钮 + 关闭按钮 + wxListCtrl 横向图标视图 wxLC_ICON 或 wxLC_SMALL_ICON)。每个 Queue 是独立 panel,垂直堆叠;最下面一个"+ New Queue"按钮。
没有 tab,Queue 直接纵向列出,一目了然
Bin: 左侧 wxPanel(用 splitter 与右侧分开),内部 wxListCtrl(wxLC_SMALL_ICON) 列表图标+文件名+角标
Options: wxAuiPaneInfo().Name("options").Caption("Options").Left().Floatable(true).CloseButton(true).Show(true) — 默认在右侧 dock 状态(用 wxAuiManager 管理),用户可拖出成浮窗或关掉(用 toolbar 按钮可重新打开)
处理顺序: 从上到下 — Queue 1 → Queue N
输出目录: 沿用 CLI,即默认 ./out (相对当前工作目录);Options 的 Pipeline tab 提供 "Output Dir" 文本框 + Browse 按钮,空时用默认 ./out,非空时用指定路径。所有 Queue 共用同一个输出根目录;不同 Queue 的输出文件名如何避免冲突?——沿用 CLI 行为: CLI 本身 Process(output_target) 接收一个路径;若该路径是目录,输出文件名按规则自动生成 (沿用 BurstMerge 内部逻辑,与 CLI 完全一致)。多个 Queue 共用同一输出目录,后处理会覆盖先处理 → 解决办法: 每个 Queue 完成前,在 BurstMerge Process 调用前自动把输出目录追加 q<n>_ 子级别名,避免冲突
需要确认 — 这个"自动加 q"子目录方案 OK 吗? 或者你希望所有 Queue 输出到同一目录(后覆盖先)?

好的，所有细节已敲定。最终方案已完整成型。

最终方案 — 整合版（进入计划模式，只读，不进行任何修改）
一、获取与构建 wxWidgets
下载 https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.5/wxWidgets-3.2.5.tar.bz2（约 25MB）到 C:\Users\HeMu\AppData\Local\Temp\opencode\wx.tar.bz2
解压到 3rdparty/wxWidgets/（不含一层 wrapper）
用 MinGW 编译 wxWidgets 静态库（UNICODE + 静态 + 复用系统 JPEG/PNG/TIFF/ZLIB）：
入口：3rdparty/wxWidgets/build/msw/makefile.gcc（MinGW 风格，无需 MSYS2）
输出 3rdparty/wxWidgets/install/lib/libwx_mswu_*.a + 头树 install/include/wx/*
关键组件：core / adv / aui / propgrid / base / net / xml / html
.gitignore 追加 3rdparty/wxWidgets/（整目录不入库，仅本机构建）
apps/gui/README.md 记录重建步骤（入库）
二、CMake 配置（核心：可完全排除编译）
顶层 CMakeLists.txt 在末尾追加（其他行不动）：
option(BURSTMERGE_BUILD_GUI "Build burstmerge_gui with wxWidgets (auto-skip if wx not found)" ON)
if(BURSTMERGE_BUILD_GUI)
    include(cmake/FindWxWidgetsBurstMerge.cmake)
    if(BURSTMERGE_WX_FOUND)
        add_subdirectory(apps/gui)
    else()
        message(STATUS "wxWidgets not found - GUI app will NOT be built (CLI/tests unaffected)")
        set(BURSTMERGE_BUILD_GUI OFF CACHE BOOL "Build GUI" FORCE)
    endif()
endif()
新增 cmake/FindWxWidgetsBurstMerge.cmake
三段优先级查找：local_config.cmake 的 WX_* 显式路径 → CMake 自带 find_package(wxWidgets) → 失败时 BURSTMERGE_WX_FOUND=FALSE。

新增 apps/gui/CMakeLists.txt
add_executable(burstmerge_gui WIN32 main.cpp)
target_link_libraries(burstmerge_gui PRIVATE burstmerge ${BURSTMERGE_WX_LIBRARIES})
target_include_directories(burstmerge_gui PRIVATE ${BURSTMERGE_WX_INCLUDE_DIRS} ${CXXOPTS_INCLUDE})
target_link_options(burstmerge_gui PRIVATE -mwindows)
local_config.cmake 末尾追加（本机，不入库）：
set(WX_ROOT        "${PROJECT_ROOT}/3rdparty/wxWidgets/install")
set(WX_INCLUDE_DIR "${WX_ROOT}/include")
set(WX_LIBRARY_DIR "${WX_ROOT}/lib")
set(WX_LIBRARIES wx_mswu_core-3.2 wx_mswu_adv-3.2 wx_mswu_aui-3.2 wx_mswu_propgrid-3.2
                 wx_baseu-3.2 wx_baseu_net-3.2 wx_baseu_xml-3.2 wx_mswu_html-3.2)
完全不改: apps/cli/*、apps/console/*、libburstmerge/**、cmake/CompilerFlags.cmake、local_config.cmake 现有 JPEG/PNG/TIFF 段。

三、GUI 应用设计
主窗口布局
MenuBar:  File | Edit | Process | Help
Toolbar:  [Start] | [New Queue] | [Remove Queue] | [Clear Bin] | [Toggle Options] | [About]
+---------+-------------------------------------------------------+
|         |  [Queue #1]  [Add Selected] [x]                       |
|         |  [ico]IMG_001.dng [ico]IMG_002.dng [ico]IMG_003 ...   |  ← 横向长条
|   Bin   +-------------------------------------------------------+
| (左侧) |  [Queue #2]  [Add Selected] [x]                       |
| icon+   |  [ico]IMG_004.dng [ico]IMG_005.dng ...                 |
| 文件名 +-------------------------------------------------------+
| 角标    |  [Queue #3]  [Add Selected] [x]                       |
|         |  (空)                                                  |
|         +-------------------------------------------------------+
|         |  [+ New Queue]                                         |
+---------+-------------------------------------------------------+
| Options (wxAuiManager 抽屉 pane, 可隐藏/浮动/拖出)               |
|   Notebook: Pipeline | Merge | Align | Exposure | Cleanup         |
+-------------------------------------------------------+
| StatusBar: 处理状态 | [====50%====] | elapsed                       |
+-------------------------------------------------------+
| Log (可隐藏的 AUI pane):                                            |
+-------------------------------------------------------+
Bin: 左侧 wxPanel + wxListCtrl(wxLC_SMALL_ICON | wxLC_LIST)，多选；wxFileDropTarget 接收外部文件；自定义 wxDropSource 拖出
Queue 区: wxScrolledWindow 包 wxBoxSizer(wxVERTICAL)；每个 Queue 是 QueuePanel : wxPanel，横向布局：标题 Queue #n + Add 按钮 + 关闭按钮 + wxListCtrl(wxLC_ICON | wxLC_SMALL_ICON)；最下 "+ New Queue"
Options: wxAuiManager 管理的 pane，wxAuiPaneInfo().Name("options").Caption("Options").Right().Floatable(true).CloseButton(true).Show(true)；内部 wxNotebook 分五 tab
Bin 角标: wxImageList 预渲染 6 张角标 bitmap (无 / 1 / 2 / 3 / 4 / 5+)；SetItemImage(row, badge_index) 即可
Options 分组（覆盖所有 CLI 能力）
Pipeline: Backend (CPU/Vulkan) / GPU Device (-1 自动, 仅 Vulkan 启) / Tile Size / Bit Depth (8/10/12/14/16) / Output Format (Auto/PNG/JPEG/BMP/TIFF/DNG) / Output Dir (空=默认./out) + Browse
Merge: Algorithm / Spatial Mode (仅 Spatial 启) / Frequency Mode (仅 Frequency 启)
Align: Mode (Standard/Dense/Frequency/Skip) / Align Gamma (slider) / Smooth Tile Field (checkbox)
Exposure: Mode (Off/Linear/Curve) / Curve Mode (仅 Curve 启) / Stops (slider)
Cleanup: Highlight Recovery (默认开) / Hot Pixel Repair (默认关) / Noise Reduction (仅 Spatial/Frequency 启)
DnD 细节
自定义 wxCustomDataObject 格式 BurstMergeBinItems，payload = 路径列表
Bin 接收外部文件: wxFileDropTarget 挂 Bin → 暂存（不转换）；角标 0
Bin 拖出: 多选后拖动 → 激活 wxDropSource，携带 [BinPath1, BinPath2, ...]
Queue 接收:
接收外部文件 (wxFileDropTarget) → 加 Bin + 加 Queue (引用 +1)
接收 Bin 自定义格式 (QueueDropTarget 接 BurstMergeBinItems) → 加 Queue + Bin 已存在只引用 +1
Bin Add 按钮: "Add Selected" 把 Bin 当前选中项加到 Queue + Bin 角标 +1，Bin 项不消失，可复用
Queue 内删除项: 右键 → "Remove from queue" → Bin 引用 -1（不减至 <0；=0 时不消失）
角标显示: 数字 1/2/3/4，超过 5 显示 "5+"；0 = 无角标
处理流程（Start 按下后）
严格仅在点击 Start 时触发后端。从上到下按 Queue 1→N 串行处理。
每个 Queue:
创建 burstmerge::BurstMerge bm(backend) 实例（与 CLI 同 API）
bm.AddImage() 该 Queue 所有路径
Options → burstmerge::Settings（与 CLI 调用方式完全一致）
输出目录 = 用户指定的 Output Dir，自动追加 q<n>_<时间戳> 子目录避免多 Queue 冲突
bm.SetProgressCallback → 在被调用线程内仅 wxQueueEvent(wxThreadEvent) 投递主线程，主线程更新 wxGauge + StatusBar + Log
工作线程: wxThread joinable，Entry() 内串行跑各 Queue。严禁子线程直接调 wx 控件
错误处理: 任一 Queue 失败 → Log 红字 "Error: ..."；选项 Stop on first error 默认开（继续处理下一 Queue 前 stop 整个序列）
不调用 CLI：apps/gui/main.cpp 独立 #include "burstmerge/api.h"，与 CLI 平级共用核心库
不实现 Stop 按钮 (按你之前的要求,不引入 cancel API)
四、新增 / 修改文件清单
新增: cmake/FindWxWidgetsBurstMerge.cmake / apps/gui/CMakeLists.txt / apps/gui/main.cpp / apps/gui/README.md 修改: 顶层 CMakeLists.txt (末尾追加一段) / local_config.cmake (本机追加 WX_* 段) / .gitignore (追加 3rdparty/wxWidgets/) 完全不动: apps/cli/* / apps/console/* / libburstmerge/**

五、验证步骤
cmake -B build -DBURSTMERGE_BUILD_GUI=OFF → cmake --build build 成功，GUI 不参与
注释掉 local_config.cmake 中 WX_* 段(模拟没装) → cmake -B build 自动 STATUS 跳过 GUI，其余照编
完整配置 → cmake --build build --target burstmerge_gui → 产出 burstmerge_gui.exe
运行：拖 DNG 到 Bin (无角标) → 多选拖到 Queue 1 → Bin 角标变 "1" → 新建 Queue 2 → 又拖同样项 → 角标变 "2"
外部直拖 Queue 3 → Bin 同步出现角标 "1"
Options 默认 → Start → 进度条 + Log 更新 → out/q1_<ts>/... 产物存在
与 CLI 同参数比像素，应 ≤ 0.01% MAD
方案完整，等你确认进入执行阶段
