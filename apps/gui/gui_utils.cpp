#include "gui_utils.h"

#include <wx/artprov.h>
#include <wx/dcmemory.h>
#include <wx/settings.h>

#ifdef _WIN32
#include <windows.h>
#include <uxtheme.h>
#undef DrawText
#endif

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace burstmerge::gui
{
std::string PathKey(const std::filesystem::path& path)
{
#ifdef _WIN32
    std::wstring key = path.lexically_normal().wstring();
    if (!key.empty())
    {
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    }
    return std::filesystem::path(key).u8string();
#else
    return path.lexically_normal().u8string();
#endif
}

std::filesystem::path FileSystemPath(const wxString& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.ToStdWstring());
#else
    return std::filesystem::u8path(path.utf8_string());
#endif
}

std::optional<std::string> NormalizePath(const wxString& path)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(FileSystemPath(path), error);
    if (error)
    {
        return std::nullopt;
    }
    return absolute.lexically_normal().u8string();
}

void AppendFolderFiles(const std::filesystem::path& folder, std::vector<std::string>& paths)
{
    std::vector<std::string> folder_files;
    std::error_code error;
    std::filesystem::directory_iterator it(folder, error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end)
    {
        std::error_code type_error;
        if (it->is_regular_file(type_error) && !type_error)
        {
            folder_files.push_back(it->path().lexically_normal().u8string());
        }
        it.increment(error);
    }
    std::sort(folder_files.begin(), folder_files.end());
    paths.insert(paths.end(), folder_files.begin(), folder_files.end());
}

std::vector<std::string> ExpandDroppedPaths(const wxArrayString& dropped)
{
    std::vector<std::string> paths;
    for (const wxString& item : dropped)
    {
        const std::optional<std::string> normalized = NormalizePath(item);
        if (!normalized)
        {
            continue;
        }

        const std::filesystem::path path = std::filesystem::u8path(*normalized);
        std::error_code error;
        if (std::filesystem::is_directory(path, error) && !error)
        {
            // Match CLI -f: include immediate regular files, sorted, without recursion.
            AppendFolderFiles(path, paths);
        }
        else if (!error && std::filesystem::is_regular_file(path, error) && !error)
        {
            paths.push_back(*normalized);
        }
    }
    return paths;
}

wxString DisplayPath(const std::string& path)
{
    return wxString::FromUTF8(path);
}

#ifdef _WIN32
void ApplyWindowsExplorerTheme(wxWindow* window)
{
    if (window != nullptr && window->GetHandle() != nullptr)
    {
        SetWindowTheme(static_cast<HWND>(window->GetHandle()), L"Explorer", nullptr);
    }
}
#endif

long QueueBorderStyle()
{
#ifdef _WIN32
    return wxBORDER_THEME;
#else
    return wxBORDER_SIMPLE;
#endif
}

bool IsRawPath(const std::string& path)
{
    static const std::vector<std::string> extensions = {
        ".dng", ".arw", ".cr2", ".cr3", ".nef", ".nrw", ".orf", ".raf",
        ".rw2", ".pef", ".srw", ".x3f", ".sr2", ".srf", ".kdc", ".dcr",
        ".k25", ".mdc", ".mef", ".mrw", ".iiq", ".eip", ".bay", ".3fr",
        ".fff", ".mos"
    };
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::string OutputExtension(burstmerge::OutputFormat format, bool has_raw)
{
    switch (format)
    {
        case burstmerge::OutputFormat::PNG: return ".png";
        case burstmerge::OutputFormat::JPEG: return ".jpg";
        case burstmerge::OutputFormat::BMP: return ".bmp";
        case burstmerge::OutputFormat::TIFF: return ".tif";
        case burstmerge::OutputFormat::DNG: return ".dng";
        default: return has_raw ? ".dng" : ".png";
    }
}

std::string FormatOptionNumber(float value, int precision)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    std::string text = stream.str();
    while (text.size() > 1 && text.back() == '0')
    {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.')
    {
        text.pop_back();
    }
    return text;
}

std::string SanitizeFileStem(std::string stem)
{
    for (char& c : stem)
    {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
        {
            c = '_';
        }
    }
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
    {
        stem.pop_back();
    }
    return stem.empty() ? "burst" : stem;
}

std::vector<std::string> DecodePathList(const void* data, std::size_t size)
{
    std::vector<std::string> paths;
    if (data == nullptr || size == 0)
    {
        return paths;
    }

    const char* bytes = static_cast<const char*>(data);
    size_t begin = 0;
    for (size_t i = 0; i <= size; ++i)
    {
        if (i == size || bytes[i] == '\0')
        {
            if (i > begin)
            {
                paths.emplace_back(bytes + begin, i - begin);
            }
            begin = i + 1;
        }
    }
    return paths;
}

std::string EncodePathList(const std::vector<std::string>& paths)
{
    std::string data;
    for (const std::string& path : paths)
    {
        data += path;
        data.push_back('\0');
    }
    return data;
}

wxBitmap MakeFileBadgeBitmap(int references, int size)
{
    return ComposeThumbnailBitmap(nullptr, references, size);
}

wxBitmap ComposeThumbnailBitmap(const wxImage* image, int references, int size)
{
    wxBitmap bitmap(size, size, 32);
    wxMemoryDC dc(bitmap);
    const wxColour window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    dc.SetBackground(wxBrush(window));
    dc.Clear();

    if (image != nullptr && image->IsOk())
    {
        const double scale = std::min(
            static_cast<double>(size) / image->GetWidth(),
            static_cast<double>(size) / image->GetHeight());
        const int width = std::max(1, static_cast<int>(image->GetWidth() * scale));
        const int height = std::max(1, static_cast<int>(image->GetHeight() * scale));
        const wxImage fitted = width == image->GetWidth() && height == image->GetHeight()
            ? *image
            : image->Scale(width, height, wxIMAGE_QUALITY_HIGH);
        dc.DrawBitmap(wxBitmap(fitted), (size - width) / 2, (size - height) / 2, true);
    }
    else
    {
        const int inset = std::max(2, size / 8);
        const int file_size = size - inset * 2;
        const wxBitmap file = wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_OTHER,
            wxSize(file_size, file_size));
        if (file.IsOk())
        {
            dc.DrawBitmap(file, inset, inset, true);
        }
        else
        {
            dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW), 1));
            dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
            dc.DrawRectangle(inset * 2, inset, size - inset * 3, size - inset * 2);
            dc.SetPen(wxPen(text, 2));
            dc.DrawLine(size / 3, size / 2, size * 3 / 4, size / 2);
            dc.DrawLine(size / 3, size * 5 / 8, size * 3 / 4, size * 5 / 8);
        }
    }

    if (references > 0)
    {
        const wxString label = references >= 5 ? "5+" : wxString::Format("%d", references);
        const int badge_width = std::max(15, size * 15 / 32);
        const int badge_height = std::max(13, size * 13 / 32);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(accent));
        dc.DrawRoundedRectangle(size - badge_width, 0, badge_width, badge_height,
            std::max(3, size * 3 / 32));
        dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT));
        wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        font.SetPointSize(std::max(7, font.GetPointSize() - 1));
        font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(font);
        const wxSize text = dc.GetTextExtent(label);
        dc.DrawText(label, size - badge_width + (badge_width - text.x) / 2,
            (badge_height - text.y) / 2 - 1);
    }

    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

} // namespace burstmerge::gui
