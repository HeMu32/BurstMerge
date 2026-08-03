#pragma once

#include "burstmerge/api.h"

#include <wx/arrstr.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/string.h>
#include <wx/window.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace burstmerge::gui
{

constexpr std::size_t kMaximumGuiPaths = 4096;
constexpr std::size_t kMaximumPathPayloadBytes = 1024 * 1024;

enum
{
    ID_START = wxID_HIGHEST + 1,
    ID_NEW_QUEUE,
    ID_REMOVE_SELECTION,
    ID_CLEAR_BIN,
    ID_TOGGLE_OPTIONS,
    ID_ADD_FILES,
    ID_ADD_FOLDER,
    ID_OUTPUT_BROWSE,
    ID_DNG_CACHE_BROWSE,
    ID_BACKEND,
    ID_MERGE_ALGORITHM,
    ID_EXPOSURE_MODE,
    ID_REMOVE_QUEUE_ITEM
};

std::string PathKey(const std::filesystem::path& path);
std::filesystem::path FileSystemPath(const wxString& path);
std::optional<std::string> NormalizePath(const wxString& path);
void AppendFolderFiles(const std::filesystem::path& folder, std::vector<std::string>& paths);
std::vector<std::string> ExpandDroppedPaths(const wxArrayString& dropped);
wxString DisplayPath(const std::string& path);
#ifdef _WIN32
void ApplyWindowsExplorerTheme(wxWindow* window);
#endif
long QueueBorderStyle();
bool IsRawPath(const std::string& path);
std::string OutputExtension(burstmerge::OutputFormat format, bool has_raw);
std::string FormatOptionNumber(float value, int precision);
std::string SanitizeFileStem(std::string stem);
std::vector<std::string> DecodePathList(const void* data, std::size_t size);
std::string EncodePathList(const std::vector<std::string>& paths);
wxBitmap MakeFileBadgeBitmap(int references, int size);
wxBitmap ComposeThumbnailBitmap(const wxImage* image, int references, int size);

} // namespace burstmerge::gui
