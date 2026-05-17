#ifdef _WIN32

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include <windows.h>
#include <fileapi.h>

#include "burstmerge/internal/io/dng_io.h"
#include "dng_sdk_bridge.h"

namespace burstmerge
{
namespace
{

static std::wstring GetDngConverterPath()
{
    wchar_t buf[MAX_PATH];
    DWORD len = ExpandEnvironmentStringsW(
        L"C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe",
        buf, MAX_PATH);
    if (len > 0 && len <= MAX_PATH)
    {
        return std::wstring(buf, len - 1);
    }
    return L"C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe";
}

static bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static uint64_t GetFileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    return (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

static std::wstring ReplaceExtension(const std::wstring& path, const std::wstring& newExt)
{
    size_t dot = path.find_last_of(L'.');
    size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep))
    {
        return path + newExt;
    }
    return path.substr(0, dot) + newExt;
}

static std::wstring GetFileName(const std::wstring& path)
{
    size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos)
    {
        return path.substr(sep + 1);
    }
    return path;
}

static std::wstring NormalizeWindowsPath(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

static std::wstring StringToWide(const std::string& s)
{
    return io::Utf8ToWide(s);
}

static std::string WideToString(const std::wstring& ws)
{
    if (ws.empty()) return
    {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return
    {};
    std::string buf(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, buf.data(), len, nullptr, nullptr);
    buf.pop_back();
    return buf;
}

} // anonymous namespace

bool RunAdobeDngConverter(const std::vector<std::string>& input_files,
                          const std::string& output_dir,
                          std::vector<std::string>& output_files)
{
    output_files.clear();

    std::wstring converter_exe = GetDngConverterPath();
    if (!FileExists(converter_exe))
    {
        return false;
    }

    // Build command line
    // Adobe DNG Converter CLI: "Adobe DNG Converter.exe" -l -c -d <output_dir> <file1> <file2> ...
    // -l: lossy compression (not used)
    // -c: keep file modification time
    // -d: output directory
    std::wstring output_dir_w = NormalizeWindowsPath(StringToWide(output_dir));
    std::wstring cmd = L"\"" + converter_exe + L"\" -c -d \"" +
                       output_dir_w + L"\"";

    std::vector<std::wstring> expected_outputs;
    for (const auto& f : input_files)
    {
        std::wstring wf = NormalizeWindowsPath(StringToWide(f));
        cmd += L" \"" + wf + L"\"";
        // Expected DNG name: same basename with .dng extension in the output directory
        std::wstring baseName = GetFileName(wf);
        std::wstring outPath = output_dir_w + L"\\" + baseName;
        expected_outputs.push_back(ReplaceExtension(outPath, L".dng"));
    }

    // Create process
    STARTUPINFOW si =
    {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi =
    {};

    // CreateProcessW needs a mutable command line
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    BOOL created = CreateProcessW(
        converter_exe.c_str(),
        cmd_buf.data(),
        nullptr,           // process attributes
        nullptr,           // thread attributes
        FALSE,             // inherit handles
        CREATE_NO_WINDOW,  // creation flags
        nullptr,           // environment
        nullptr,           // current directory
        &si,
        &pi
    );

    if (!created)
    {
        return false;
    }

    // Wait for process to exit (it exits quickly, but conversion continues)
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (wait_result == WAIT_FAILED)
    {
        return false;
    }

    // Poll for output files to appear and stabilize
    // The converter process exits before files are fully written,
    // so we poll each expected output file until it exists and its
    // size is stable for at least 1 second.
    const int max_poll_ms = 120000;  // 2 minutes max
    const int poll_interval_ms = 500;
    const int stable_checks = 3;     // need 3 consecutive stable reads

    std::vector<uint64_t> prev_sizes(expected_outputs.size(), 0);
    std::vector<int> stable_counts(expected_outputs.size(), 0);
    std::vector<bool> done(expected_outputs.size(), false);

    int elapsed = 0;
    while (elapsed < max_poll_ms)
    {
        bool all_done = true;
        for (size_t i = 0; i < expected_outputs.size(); i++)
        {
            if (done[i]) continue;
            all_done = false;

            std::wstring dng_path = expected_outputs[i];

            // If output_dir is absolute, the file will be placed there.
            // Otherwise, it's relative to the current directory.
            // The converter puts output in the specified -d dir.
            if (!FileExists(dng_path))
            {
                continue;
            }

            uint64_t cur_size = GetFileSize(dng_path);
            if (cur_size > 0 && cur_size == prev_sizes[i])
            {
                stable_counts[i]++;
                if (stable_counts[i] >= stable_checks)
                {
                    done[i] = true;
                    output_files.push_back(WideToString(dng_path));
                }
            } else
            {
                stable_counts[i] = 0;
            }
            prev_sizes[i] = cur_size;
        }

        if (all_done) break;

        Sleep(poll_interval_ms);
        elapsed += poll_interval_ms;
    }

    return output_files.size() == input_files.size();
}

} // namespace burstmerge

#endif // _WIN32
