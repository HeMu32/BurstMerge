#include "main_frame.h"

#include "gui_utils.h"

#include <wx/app.h>
#include <wx/image.h>
#include <wx/string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace burstmerge::gui
{

// 从进程命令行收集应被当作"打开方式传入的图像"的常规文件路径.
// 设计要点:
//   - Windows: 即使本程序以 subsystem=WINDOWS + -mwindows 链接 (入口 WinMain 而非
//     main), MinGW CRT 仍会把 GetCommandLineW 经 WideCharToMultiByte(CP_ACP) 填到
//     ANSI argv[]. 对含中文 / 带空格 / 多个路径的"打开方式"调用会丢字符. 因此这里
//     直接调 GetCommandLineW() + CommandLineToArgvW() 自己拆 UTF-16 argv, 再用
//     wxString::FromWChar 包成 wxString 交给 NormalizePath, 完全绕开 wx 的 argv.
//   - 非 Windows: 退回到 wxAppConsole::argv (UNICODE wxMSW 下值为 UTF-16, 其它平台
//     为 UTF-8), 走 wxString 自然路径. GUI 项目目前只关心 Windows, 此分支仅为保持
//     跨平台风格 不空指针.
//   - 跳过 argv[0] (程序自身).
//   - 仅接受 is_regular_file, 目录与不存在路径一律忽略 (符合"打开方式选若干图像"
//     的语义; 与拖放 ExpandDroppedPaths 展开目录的行为有意不同).
static void CollectCommandLineFiles(std::vector<std::string>& out)
{
    out.clear();
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
    if (cmd == nullptr)
    {
        return;
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmd, &argc);
    if (argv == nullptr)
    {
        return;
    }
    // argv[0] 是本程序可执行文件, 跳过.
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr || argv[i][0] == L'\0')
        {
            continue;
        }
        // wxMSW 下 wxString 构造自 const wchar_t* 直接以 UTF-16 携带, 避开
        // ACP 转换; wx 非 MSW 等 UNICODE 构建走 char* (UTF-8) 也能正确构造.
        const wxString wide(argv[i]);
        const std::optional<std::string> normalized = NormalizePath(wide);
        if (!normalized)
        {
            continue;
        }
        std::error_code error;
        const std::filesystem::path path = std::filesystem::u8path(*normalized);
        if (std::filesystem::is_regular_file(path, error) && !error)
        {
            out.push_back(*normalized);
        }
    }
    LocalFree(argv);
#else
    if (wxTheApp == nullptr)
    {
        return;
    }
    const int argc = wxTheApp->argc;
    for (int i = 1; i < argc; ++i)
    {
        const wxString arg = wxTheApp->argv[i];
        const std::optional<std::string> normalized = NormalizePath(arg);
        if (!normalized)
        {
            continue;
        }
        std::error_code error;
        const std::filesystem::path path = std::filesystem::u8path(*normalized);
        if (std::filesystem::is_regular_file(path, error) && !error)
        {
            out.push_back(*normalized);
        }
    }
#endif
}

class BurstMergeApp final : public wxApp
{
public:
    bool OnInit() override
    {
        SetAppName("BurstMerge GUI");
        wxInitAllImageHandlers();
        MainFrame* frame = new MainFrame;
        frame->Show();

        // 接收来自 shell / 资源管理器"打开方式"一次性传入的图像文件, 直接加进 Bin.
        // 必须在 MainFrame 完整构造 (事件已绑定 / ThumbnailLoader 已就绪) 之后调用,
        // 否则 AddToBin 触发的缩略图请求无法正确挂载到主窗口.
        std::vector<std::string> initial_files;
        CollectCommandLineFiles(initial_files);
        if (!initial_files.empty())
        {
            frame->AddInitialFiles(initial_files);
        }
        return true;
    }
};

} // namespace burstmerge::gui

wxIMPLEMENT_APP(burstmerge::gui::BurstMergeApp);
