#include "burstmerge/api.h"

#include <wx/aui/aui.h>
#include <wx/artprov.h>
#include <wx/dcmemory.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/thread.h>
#include <wx/wrapsizer.h>
#include <wx/wx.h>

#ifdef _WIN32
#include <windows.h>
#include <uxtheme.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

wxDEFINE_EVENT(wxEVT_BM_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_BM_QUEUE_DONE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_BM_PROCESS_DONE, wxThreadEvent);

const wxDataFormat kBinItemsFormat("BurstMergeBinItems");

enum
{
    ID_START = wxID_HIGHEST + 1,
    ID_NEW_QUEUE,
    ID_CLEAR_BIN,
    ID_TOGGLE_OPTIONS,
    ID_ADD_FILES,
    ID_ADD_FOLDER,
    ID_OUTPUT_BROWSE,
    ID_BACKEND,
    ID_MERGE_ALGORITHM,
    ID_EXPOSURE_MODE,
    ID_REMOVE_QUEUE_ITEM
};

std::string PathKey(const std::filesystem::path& path)
{
    std::string key = path.lexically_normal().u8string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return key;
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

std::vector<std::string> DecodePathList(const void* data, size_t size)
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
    wxBitmap bitmap(size, size, 32);
    wxMemoryDC dc(bitmap);
    const wxColour window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    dc.SetBackground(wxBrush(window));
    dc.Clear();

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

class FileDropTarget final : public wxFileDropTarget
{
public:
    explicit FileDropTarget(std::function<void(const std::vector<std::string>&)> on_files)
        : on_files_(std::move(on_files))
    {
    }

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override
    {
        std::vector<std::string> paths;
        paths.reserve(filenames.size());
        for (const wxString& filename : filenames)
        {
            const std::optional<std::string> path = NormalizePath(filename);
            if (path)
            {
                paths.push_back(*path);
            }
        }
        if (paths.empty())
        {
            return false;
        }
        on_files_(paths);
        return true;
    }

private:
    std::function<void(const std::vector<std::string>&)> on_files_;
};

class QueueDropTarget final : public wxDropTarget
{
public:
    QueueDropTarget(
        std::function<void(const std::vector<std::string>&)> on_files,
        std::function<void(const std::vector<std::string>&)> on_bin_items)
        : files_(new wxFileDataObject()),
          bin_items_(new wxCustomDataObject(kBinItemsFormat)),
          composite_(new wxDataObjectComposite()),
          on_files_(std::move(on_files)),
          on_bin_items_(std::move(on_bin_items))
    {
        composite_->Add(files_, true);
        composite_->Add(bin_items_);
        SetDataObject(composite_);
    }

    wxDragResult OnData(wxCoord, wxCoord, wxDragResult result) override
    {
        if (!GetData())
        {
            return wxDragNone;
        }

        if (composite_->GetReceivedFormat() == kBinItemsFormat)
        {
            on_bin_items_(DecodePathList(bin_items_->GetData(), bin_items_->GetSize()));
        }
        else
        {
            std::vector<std::string> paths;
            for (const wxString& filename : files_->GetFilenames())
            {
                const std::optional<std::string> path = NormalizePath(filename);
                if (path)
                {
                    paths.push_back(*path);
                }
            }
            if (paths.empty())
            {
                return wxDragNone;
            }
            on_files_(paths);
        }
        return result;
    }

private:
    wxFileDataObject* files_;
    wxCustomDataObject* bin_items_;
    wxDataObjectComposite* composite_;
    std::function<void(const std::vector<std::string>&)> on_files_;
    std::function<void(const std::vector<std::string>&)> on_bin_items_;
};

class BinPanel final : public wxPanel
{
public:
    explicit BinPanel(wxWindow* parent)
        : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(250, 300)));
        ApplySystemTheme();

        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
        wxStaticText* title = new wxStaticText(this, wxID_ANY, "BIN");
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title_font.SetPointSize(title_font.GetPointSize() + 1);
        title->SetFont(title_font);
        root->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 12);
        root->Add(new wxStaticText(this, wxID_ANY, "Stage files once, reuse them across queues."),
            0, wxLEFT | wxRIGHT | wxTOP, 12);

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxLC_LIST);
#ifdef _WIN32
        ApplyWindowsExplorerTheme(list_);
#endif
        RebuildImageList();
        root->Add(list_, 1, wxEXPAND | wxALL, 10);
        SetSizer(root);

        list_->Bind(wxEVT_LIST_BEGIN_DRAG, &BinPanel::OnBeginDrag, this);
    }

    void SetFileHandler(std::function<void(const std::vector<std::string>&)> handler)
    {
        file_handler_ = std::move(handler);
        list_->SetDropTarget(new FileDropTarget(file_handler_));
    }

    void AddPath(const std::string& path)
    {
        const std::string key = PathKey(std::filesystem::u8path(path));
        if (rows_.find(key) != rows_.end())
        {
            return;
        }

        const long row = list_->InsertItem(list_->GetItemCount(),
            DisplayPath(std::filesystem::u8path(path).filename().u8string()), 0);
        list_->SetItemData(row, static_cast<wxUIntPtr>(paths_.size()));
        rows_[key] = row;
        paths_.push_back(path);
    }

    void SetReferences(const std::string& path, int references)
    {
        auto it = rows_.find(PathKey(std::filesystem::u8path(path)));
        if (it != rows_.end())
        {
            list_->SetItemImage(it->second, std::min(5, std::max(0, references)));
        }
    }

    std::vector<std::string> SelectedPaths() const
    {
        std::vector<std::string> selected;
        long row = -1;
        while ((row = list_->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
        {
            const size_t index = static_cast<size_t>(list_->GetItemData(row));
            if (index < paths_.size())
            {
                selected.push_back(paths_[index]);
            }
        }
        return selected;
    }

    void Clear()
    {
        list_->DeleteAllItems();
        paths_.clear();
        rows_.clear();
    }

    void SetLocked(bool locked)
    {
        list_->Enable(!locked);
    }

    void ApplySystemTheme()
    {
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        if (list_ != nullptr)
        {
#ifdef _WIN32
            ApplyWindowsExplorerTheme(list_);
#endif
            list_->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
            list_->SetTextColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
            RebuildImageList();
            list_->Refresh();
        }
        Refresh();
    }

private:
    void RebuildImageList()
    {
        const int icon_size = FromDIP(32);
        wxImageList* images = new wxImageList(icon_size, icon_size, true, 6);
        for (int refs = 0; refs <= 5; ++refs)
        {
            images->Add(MakeFileBadgeBitmap(refs, icon_size));
        }
        list_->AssignImageList(images, wxIMAGE_LIST_SMALL);
        image_list_ = images;
    }

    void OnBeginDrag(wxListEvent&)
    {
        const std::vector<std::string> selected = SelectedPaths();
        if (selected.empty())
        {
            return;
        }

        const std::string payload = EncodePathList(selected);
        wxCustomDataObject data(kBinItemsFormat);
        data.SetData(payload.size(), payload.data());
        wxDropSource source(list_);
        source.SetData(data);
        source.DoDragDrop(wxDrag_CopyOnly);
    }

    wxListCtrl* list_ = nullptr;
    wxImageList* image_list_ = nullptr;
    std::vector<std::string> paths_;
    std::unordered_map<std::string, long> rows_;
    std::function<void(const std::vector<std::string>&)> file_handler_;
};

class QueuePanel final : public wxPanel
{
public:
    QueuePanel(wxWindow* parent, int number)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            QueueBorderStyle()),
          number_(number)
    {
        ApplySystemTheme();
        SetMinSize(wxSize(-1, FromDIP(124)));

        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* heading = new wxBoxSizer(wxHORIZONTAL);
        title_ = new wxStaticText(this, wxID_ANY, wxString::Format("QUEUE %d", number_));
        wxFont title_font = title_->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title_->SetFont(title_font);
        add_button_ = new wxButton(this, wxID_ANY, "Add Selected", wxDefaultPosition, wxDefaultSize,
            wxBU_EXACTFIT);
        close_button_ = new wxButton(this, wxID_ANY, "Close", wxDefaultPosition, wxDefaultSize,
            wxBU_EXACTFIT);
        heading->Add(title_, 0, wxALIGN_CENTER_VERTICAL);
        heading->AddStretchSpacer();
        heading->Add(add_button_, 0, wxRIGHT, 6);
        heading->Add(close_button_, 0);
        root->Add(heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxLC_LIST);
#ifdef _WIN32
        ApplyWindowsExplorerTheme(list_);
#endif
        const int icon_size = FromDIP(32);
        wxImageList* images = new wxImageList(icon_size, icon_size, true, 1);
        images->Add(MakeFileBadgeBitmap(0, icon_size));
        list_->AssignImageList(images, wxIMAGE_LIST_SMALL);
        root->Add(list_, 1, wxEXPAND | wxALL, 8);
        SetSizer(root);

        add_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
        {
            if (on_add_selected_)
            {
                on_add_selected_(*this);
            }
        });
        close_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
        {
            if (on_close_)
            {
                on_close_(*this);
            }
        });
        list_->Bind(wxEVT_CONTEXT_MENU, &QueuePanel::OnContextMenu, this);
    }

    void SetHandlers(
        std::function<void(QueuePanel&)> on_add_selected,
        std::function<void(QueuePanel&)> on_close,
        std::function<void(QueuePanel&, const std::vector<std::string>&, bool)> on_drop,
        std::function<void(QueuePanel&, const std::string&)> on_remove)
    {
        on_add_selected_ = std::move(on_add_selected);
        on_close_ = std::move(on_close);
        on_drop_ = std::move(on_drop);
        on_remove_ = std::move(on_remove);

        list_->SetDropTarget(new QueueDropTarget(
            [this](const std::vector<std::string>& paths)
            {
                on_drop_(*this, paths, true);
            },
            [this](const std::vector<std::string>& paths)
            {
                on_drop_(*this, paths, false);
            }));
    }

    bool AddPath(const std::string& path)
    {
        const std::string key = PathKey(std::filesystem::u8path(path));
        if (keys_.find(key) != keys_.end())
        {
            return false;
        }

        const long row = list_->InsertItem(list_->GetItemCount(),
            DisplayPath(std::filesystem::u8path(path).filename().u8string()), 0);
        list_->SetItemData(row, static_cast<wxUIntPtr>(paths_.size()));
        paths_.push_back(path);
        keys_[key] = path;
        return true;
    }

    bool RemovePath(const std::string& path)
    {
        const std::string key = PathKey(std::filesystem::u8path(path));
        auto key_it = keys_.find(key);
        if (key_it == keys_.end())
        {
            return false;
        }

        paths_.erase(std::remove_if(paths_.begin(), paths_.end(), [&](const std::string& item)
        {
            return PathKey(std::filesystem::u8path(item)) == key;
        }), paths_.end());
        keys_.erase(key_it);
        RebuildList();
        return true;
    }

    const std::vector<std::string>& Paths() const
    {
        return paths_;
    }

    int Number() const
    {
        return number_;
    }

    void SetNumber(int number)
    {
        number_ = number;
        title_->SetLabel(wxString::Format("QUEUE %d", number_));
    }

    void Clear()
    {
        paths_.clear();
        keys_.clear();
        list_->DeleteAllItems();
    }

    void SetLocked(bool locked)
    {
        list_->Enable(!locked);
        add_button_->Enable(!locked);
        close_button_->Enable(!locked);
    }

    void ApplySystemTheme()
    {
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        if (list_ != nullptr)
        {
#ifdef _WIN32
            ApplyWindowsExplorerTheme(list_);
#endif
            list_->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
            list_->SetTextColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
            list_->Refresh();
        }
        Refresh();
    }

private:
    void RebuildList()
    {
        list_->DeleteAllItems();
        for (size_t i = 0; i < paths_.size(); ++i)
        {
            const long row = list_->InsertItem(list_->GetItemCount(),
                DisplayPath(std::filesystem::u8path(paths_[i]).filename().u8string()), 0);
            list_->SetItemData(row, static_cast<wxUIntPtr>(i));
        }
    }

    void OnContextMenu(wxContextMenuEvent& event)
    {
        wxPoint point = event.GetPosition();
        if (point == wxDefaultPosition)
        {
            point = wxGetMousePosition();
        }
        point = list_->ScreenToClient(point);

        int flags = 0;
        const long row = list_->HitTest(point, flags);
        if (row < 0)
        {
            return;
        }
        const size_t index = static_cast<size_t>(list_->GetItemData(row));
        if (index >= paths_.size())
        {
            return;
        }
        const std::string path = paths_[index];

        wxMenu menu;
        menu.Append(ID_REMOVE_QUEUE_ITEM, "Remove from queue");
        menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&)
        {
            if (on_remove_)
            {
                on_remove_(*this, path);
            }
        }, ID_REMOVE_QUEUE_ITEM);
        list_->PopupMenu(&menu, point);
    }

    int number_ = 0;
    wxStaticText* title_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxButton* add_button_ = nullptr;
    wxButton* close_button_ = nullptr;
    std::vector<std::string> paths_;
    std::unordered_map<std::string, std::string> keys_;
    std::function<void(QueuePanel&)> on_add_selected_;
    std::function<void(QueuePanel&)> on_close_;
    std::function<void(QueuePanel&, const std::vector<std::string>&, bool)> on_drop_;
    std::function<void(QueuePanel&, const std::string&)> on_remove_;
};

class OptionsPanel final : public wxPanel
{
public:
    explicit OptionsPanel(wxWindow* parent)
        : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(345, 500)));
        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
        wxNotebook* notebook = new wxNotebook(this, wxID_ANY);
        notebook->AddPage(CreatePipelinePage(notebook), "Pipeline");
        notebook->AddPage(CreateMergePage(notebook), "Merge");
        notebook->AddPage(CreateAlignPage(notebook), "Align");
        notebook->AddPage(CreateExposurePage(notebook), "Exposure");
        notebook->AddPage(CreateCleanupPage(notebook), "Cleanup");
        root->Add(notebook, 1, wxEXPAND | wxALL, 6);
        SetSizer(root);

        backend_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            UpdateEnabledState();
        });
        merge_algorithm_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            UpdateEnabledState();
        });
        exposure_mode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            UpdateEnabledState();
        });
        UpdateEnabledState();
    }

    burstmerge::BackendType Backend() const
    {
        return backend_->GetSelection() == 1
            ? burstmerge::BackendType::Vulkan
            : burstmerge::BackendType::CPU;
    }

    burstmerge::Settings Settings() const
    {
        burstmerge::Settings settings;
        settings.gpu_device_index = gpu_device_->GetValue();
        settings.tile_size = tile_size_->GetValue();
        settings.bit_depth = std::stoi(bit_depth_->GetStringSelection().ToStdString());
        switch (output_format_->GetSelection())
        {
            case 1: settings.output_format = burstmerge::OutputFormat::PNG; break;
            case 2: settings.output_format = burstmerge::OutputFormat::JPEG; break;
            case 3: settings.output_format = burstmerge::OutputFormat::BMP; break;
            case 4: settings.output_format = burstmerge::OutputFormat::TIFF; break;
            case 5: settings.output_format = burstmerge::OutputFormat::DNG; break;
            default: settings.output_format = burstmerge::OutputFormat::Auto; break;
        }
        switch (merge_algorithm_->GetSelection())
        {
            case 1: settings.merge_algo = burstmerge::MergeAlgorithm::Frequency; break;
            case 2: settings.merge_algo = burstmerge::MergeAlgorithm::TemporalAverage; break;
            case 3: settings.merge_algo = burstmerge::MergeAlgorithm::TemporalMedian; break;
            case 4: settings.merge_algo = burstmerge::MergeAlgorithm::ExpBracketAverage; break;
            default: settings.merge_algo = burstmerge::MergeAlgorithm::Spatial; break;
        }
        settings.spatial_mode = spatial_mode_->GetSelection() == 1
            ? burstmerge::SpatialMergeMode::Linear
            : burstmerge::SpatialMergeMode::Standard;
        switch (frequency_mode_->GetSelection())
        {
            case 1: settings.frequency_mode = burstmerge::FrequencyMode::WienerFft; break;
            case 2: settings.frequency_mode = burstmerge::FrequencyMode::WienerFftRobust; break;
            default: settings.frequency_mode = burstmerge::FrequencyMode::Laplacian; break;
        }
        switch (alignment_mode_->GetSelection())
        {
            case 1: settings.alignment_mode = burstmerge::AlignmentMode::DenseTile; break;
            case 2: settings.alignment_mode = burstmerge::AlignmentMode::Frequency; break;
            case 3: settings.alignment_mode = burstmerge::AlignmentMode::Skip; break;
            default: settings.alignment_mode = burstmerge::AlignmentMode::Standard; break;
        }
        settings.align_gamma = static_cast<float>(align_gamma_->GetValue()) / 20.0f;
        settings.smooth_tile_field = smooth_tile_field_->GetValue();
        switch (exposure_mode_->GetSelection())
        {
            case 1: settings.exposure_mode = burstmerge::ExposureMode::Linear; break;
            case 2: settings.exposure_mode = burstmerge::ExposureMode::Curve; break;
            default: settings.exposure_mode = burstmerge::ExposureMode::Off; break;
        }
        settings.exposure_curve_mode = curve_mode_->GetSelection() == 1
            ? burstmerge::ExposureCurveMode::LocalReinhard
            : burstmerge::ExposureCurveMode::Global;
        settings.exposure_stops = static_cast<float>(exposure_stops_->GetValue()) / 10.0f;
        settings.highlight_recovery = highlight_recovery_->GetValue();
        settings.hot_pixel_repair = hot_pixel_repair_->GetValue();
        settings.noise_reduction = static_cast<float>(noise_reduction_->GetValue()) / 2.0f;
        if (!dng_convert_dir_->GetValue().empty())
        {
            const std::optional<std::string> path = NormalizePath(dng_convert_dir_->GetValue());
            if (path)
            {
                settings.dng_convert_dir = *path;
            }
        }
        return settings;
    }

    std::optional<std::string> OutputRoot() const
    {
        wxString value = output_dir_->GetValue();
        if (value.empty())
        {
            value = "./out";
        }
        return NormalizePath(value);
    }

    bool StopOnFirstError() const
    {
        return stop_on_error_->GetValue();
    }

    bool Validate(std::string& error) const
    {
        if (backend_->GetSelection() == 1 && merge_algorithm_->GetSelection() == 1 &&
            frequency_mode_->GetSelection() == 2)
        {
            error = "Wiener robust frequency merge is not implemented by the Vulkan backend.";
            return false;
        }
        if (backend_->GetSelection() == 1 && hot_pixel_repair_->GetValue())
        {
            error = "Hot-pixel repair is not implemented by the Vulkan backend. Disable it or use CPU.";
            return false;
        }
        if (!dng_convert_dir_->GetValue().empty() && !NormalizePath(dng_convert_dir_->GetValue()))
        {
            error = "The DNG conversion cache path is invalid.";
            return false;
        }
        return true;
    }

    void SetLocked(bool locked)
    {
        Enable(!locked);
    }

    void ApplySystemTheme()
    {
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        Refresh();
    }

private:
    wxPanel* CreatePipelinePage(wxWindow* parent)
    {
        wxPanel* panel = new wxPanel(parent);
        wxFlexGridSizer* grid = MakeGrid();
        backend_ = AddChoice(panel, grid, "Backend", {"CPU", "Vulkan"}, 0);
        gpu_device_ = AddSpin(panel, grid, "GPU device", -1, 64, -1);
        tile_size_ = AddSpin(panel, grid, "Tile size", 16, 256, 32);
        bit_depth_ = AddChoice(panel, grid, "Bit depth", {"8", "10", "12", "14", "16"}, 3);
        output_format_ = AddChoice(panel, grid, "Output format",
            {"Auto", "PNG", "JPEG", "BMP", "TIFF", "DNG"}, 0);

        grid->Add(new wxStaticText(panel, wxID_ANY, "Output directory"), 0, wxALIGN_CENTER_VERTICAL);
        wxBoxSizer* output_row = new wxBoxSizer(wxHORIZONTAL);
        output_dir_ = new wxTextCtrl(panel, wxID_ANY, "./out");
        wxButton* browse = new wxButton(panel, ID_OUTPUT_BROWSE, "...", wxDefaultPosition,
            wxSize(36, -1), wxBU_EXACTFIT);
        output_row->Add(output_dir_, 1, wxRIGHT, 4);
        output_row->Add(browse, 0);
        grid->Add(output_row, 1, wxEXPAND);
        browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
        {
            wxDirDialog dialog(this, "Choose output root", output_dir_->GetValue(),
                wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK)
            {
                output_dir_->SetValue(dialog.GetPath());
            }
        });

        grid->Add(new wxStaticText(panel, wxID_ANY, "DNG conversion cache"),
            0, wxALIGN_CENTER_VERTICAL);
        dng_convert_dir_ = new wxTextCtrl(panel, wxID_ANY);
        dng_convert_dir_->SetHint("Default: alongside output");
        grid->Add(dng_convert_dir_, 1, wxEXPAND);
        SetPageSizer(panel, grid);
        return panel;
    }

    wxPanel* CreateMergePage(wxWindow* parent)
    {
        wxPanel* panel = new wxPanel(parent);
        wxFlexGridSizer* grid = MakeGrid();
        merge_algorithm_ = AddChoice(panel, grid, "Algorithm",
            {"Spatial", "Frequency", "Temporal average", "Median", "Exposure bracket average"}, 0);
        spatial_mode_ = AddChoice(panel, grid, "Spatial mode", {"Standard", "Linear"}, 0);
        frequency_mode_ = AddChoice(panel, grid, "Frequency mode",
            {"Laplacian", "Wiener", "Wiener robust"}, 0);
        SetPageSizer(panel, grid);
        return panel;
    }

    wxPanel* CreateAlignPage(wxWindow* parent)
    {
        wxPanel* panel = new wxPanel(parent);
        wxFlexGridSizer* grid = MakeGrid();
        alignment_mode_ = AddChoice(panel, grid, "Mode",
            {"Standard", "Dense tile", "Frequency", "Skip"}, 0);
        align_gamma_ = AddSlider(panel, grid, "Alignment gamma", 2, 40, 20,
            "0.10", "2.00");
        smooth_tile_field_ = AddCheck(panel, grid, "Smooth tile field", false);
        SetPageSizer(panel, grid);
        return panel;
    }

    wxPanel* CreateExposurePage(wxWindow* parent)
    {
        wxPanel* panel = new wxPanel(parent);
        wxFlexGridSizer* grid = MakeGrid();
        exposure_mode_ = AddChoice(panel, grid, "Mode", {"Off", "Linear", "Curve"}, 0);
        curve_mode_ = AddChoice(panel, grid, "Curve mode", {"Global", "Local Reinhard"}, 0);
        exposure_stops_ = AddSlider(panel, grid, "Stops", -30, 30, 0, "-3.0", "+3.0");
        SetPageSizer(panel, grid);
        return panel;
    }

    wxPanel* CreateCleanupPage(wxWindow* parent)
    {
        wxPanel* panel = new wxPanel(parent);
        wxFlexGridSizer* grid = MakeGrid();
        highlight_recovery_ = AddCheck(panel, grid, "Highlight recovery", true);
        hot_pixel_repair_ = AddCheck(panel, grid, "Hot-pixel repair", false);
        noise_reduction_ = AddSlider(panel, grid, "Noise reduction", 0, 60, 26, "0", "30");
        stop_on_error_ = AddCheck(panel, grid, "Stop on first error", true);
        SetPageSizer(panel, grid);
        return panel;
    }

    void SetPageSizer(wxPanel* panel, wxSizer* contents)
    {
        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
        root->Add(contents, 1, wxEXPAND | wxALL, FromDIP(12));
        panel->SetSizer(root);
    }

    wxFlexGridSizer* MakeGrid()
    {
        wxFlexGridSizer* grid = new wxFlexGridSizer(2, 8, 8);
        grid->AddGrowableCol(1, 1);
        grid->SetFlexibleDirection(wxHORIZONTAL);
        grid->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
        return grid;
    }

    wxChoice* AddChoice(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        std::initializer_list<wxString> choices, int selection)
    {
        grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        wxArrayString values;
        for (const wxString& value : choices)
        {
            values.Add(value);
        }
        wxChoice* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, values);
        choice->SetSelection(selection);
        grid->Add(choice, 1, wxEXPAND);
        return choice;
    }

    wxSpinCtrl* AddSpin(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        int minimum, int maximum, int value)
    {
        grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        wxSpinCtrl* spin = new wxSpinCtrl(parent, wxID_ANY);
        spin->SetRange(minimum, maximum);
        spin->SetValue(value);
        grid->Add(spin, 1, wxEXPAND);
        return spin;
    }

    wxSlider* AddSlider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        int minimum, int maximum, int value, const wxString& min_text, const wxString& max_text)
    {
        grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        wxBoxSizer* row = new wxBoxSizer(wxVERTICAL);
        wxSlider* slider = new wxSlider(parent, wxID_ANY, value, minimum, maximum,
            wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        wxBoxSizer* limits = new wxBoxSizer(wxHORIZONTAL);
        limits->Add(new wxStaticText(parent, wxID_ANY, min_text));
        limits->AddStretchSpacer();
        limits->Add(new wxStaticText(parent, wxID_ANY, max_text));
        row->Add(slider, 0, wxEXPAND);
        row->Add(limits, 0, wxEXPAND);
        grid->Add(row, 1, wxEXPAND);
        return slider;
    }

    wxCheckBox* AddCheck(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, bool value)
    {
        grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        wxCheckBox* check = new wxCheckBox(parent, wxID_ANY, "Enabled");
        check->SetValue(value);
        grid->Add(check, 1, wxEXPAND);
        return check;
    }

    void UpdateEnabledState()
    {
        gpu_device_->Enable(backend_->GetSelection() == 1);
        spatial_mode_->Enable(merge_algorithm_->GetSelection() == 0);
        frequency_mode_->Enable(merge_algorithm_->GetSelection() == 1);
        noise_reduction_->Enable(merge_algorithm_->GetSelection() <= 1);
        curve_mode_->Enable(exposure_mode_->GetSelection() == 2);
        exposure_stops_->Enable(exposure_mode_->GetSelection() != 0);
    }

    wxChoice* backend_ = nullptr;
    wxSpinCtrl* gpu_device_ = nullptr;
    wxSpinCtrl* tile_size_ = nullptr;
    wxChoice* bit_depth_ = nullptr;
    wxChoice* output_format_ = nullptr;
    wxTextCtrl* output_dir_ = nullptr;
    wxTextCtrl* dng_convert_dir_ = nullptr;
    wxChoice* merge_algorithm_ = nullptr;
    wxChoice* spatial_mode_ = nullptr;
    wxChoice* frequency_mode_ = nullptr;
    wxChoice* alignment_mode_ = nullptr;
    wxSlider* align_gamma_ = nullptr;
    wxCheckBox* smooth_tile_field_ = nullptr;
    wxChoice* exposure_mode_ = nullptr;
    wxChoice* curve_mode_ = nullptr;
    wxSlider* exposure_stops_ = nullptr;
    wxCheckBox* highlight_recovery_ = nullptr;
    wxCheckBox* hot_pixel_repair_ = nullptr;
    wxSlider* noise_reduction_ = nullptr;
    wxCheckBox* stop_on_error_ = nullptr;
};

struct ProcessJob
{
    int queue_number = 0;
    std::vector<std::string> paths;
    std::string output_dir;
};

struct ProcessProgress
{
    int queue_number = 0;
    int percent = 0;
    std::string stage;
    double elapsed_seconds = 0.0;
};

struct QueueResult
{
    int queue_number = 0;
    bool success = false;
    std::string output_path;
    std::string error;
    double elapsed_seconds = 0.0;
};

struct ProcessSummary
{
    int completed = 0;
    int failed = 0;
};

class ProcessThread final : public wxThread
{
public:
    ProcessThread(wxEvtHandler* target, std::vector<ProcessJob> jobs,
        burstmerge::BackendType backend, burstmerge::Settings settings, bool stop_on_error)
        : wxThread(wxTHREAD_JOINABLE),
          target_(target),
          jobs_(std::move(jobs)),
          backend_(backend),
          settings_(std::move(settings)),
          stop_on_error_(stop_on_error)
    {
    }

protected:
    ExitCode Entry() override
    {
        ProcessSummary summary;
        for (const ProcessJob& job : jobs_)
        {
            const auto started = std::chrono::steady_clock::now();
            QueueResult queue_result;
            queue_result.queue_number = job.queue_number;

            try
            {
                std::filesystem::create_directories(std::filesystem::u8path(job.output_dir));
                burstmerge::BurstMerge merge(backend_);
                for (const std::string& path : job.paths)
                {
                    merge.AddImage(path);
                }
                merge.Configure(settings_);
                merge.SetProgressCallback([this, number = job.queue_number, started](
                    float progress, const std::string& stage)
                {
                    ProcessProgress payload;
                    payload.queue_number = number;
                    payload.percent = std::clamp(
                        static_cast<int>(std::lround(progress * 100.0f)), 0, 100);
                    payload.stage = stage;
                    payload.elapsed_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started).count();
                    wxThreadEvent* event = new wxThreadEvent(wxEVT_BM_PROGRESS);
                    event->SetPayload(payload);
                    wxQueueEvent(target_, event);
                });

                const burstmerge::Result result = merge.Process(job.output_dir);
                queue_result.success = result.success;
                queue_result.output_path = result.output_path;
                queue_result.error = result.error_msg;
            }
            catch (const std::exception& exception)
            {
                queue_result.error = exception.what();
            }
            catch (...)
            {
                queue_result.error = "Unknown processing error";
            }

            queue_result.elapsed_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            ++summary.completed;
            if (!queue_result.success)
            {
                ++summary.failed;
            }

            wxThreadEvent* event = new wxThreadEvent(wxEVT_BM_QUEUE_DONE);
            event->SetPayload(queue_result);
            wxQueueEvent(target_, event);

            if (!queue_result.success && stop_on_error_)
            {
                break;
            }
        }

        wxThreadEvent* event = new wxThreadEvent(wxEVT_BM_PROCESS_DONE);
        event->SetPayload(summary);
        wxQueueEvent(target_, event);
        return nullptr;
    }

private:
    wxEvtHandler* target_;
    std::vector<ProcessJob> jobs_;
    burstmerge::BackendType backend_;
    burstmerge::Settings settings_;
    bool stop_on_error_;
};

class MainFrame final : public wxFrame
{
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "BurstMerge GUI", wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_FRAME_STYLE | wxCLIP_CHILDREN),
          aui_(this)
    {
        SetSize(FromDIP(wxSize(1280, 820)));
        SetMinSize(FromDIP(wxSize(960, 640)));
#ifdef _WIN32
        SetDoubleBuffered(true);
#endif
        BuildMenu();
        BuildToolbar();
        BuildWorkspace();
        BuildStatusArea();
        BindEvents();
        AddQueue();
        ApplySystemTheme();
        Centre();
    }

    ~MainFrame() override
    {
        if (worker_ != nullptr)
        {
            worker_->Wait();
            delete worker_;
        }
        aui_.UnInit();
    }

private:
    void BuildMenu()
    {
        wxMenu* file = new wxMenu;
        file->Append(ID_ADD_FILES, "Add Files...\tCtrl+O");
        file->Append(ID_ADD_FOLDER, "Add Folder...");
        file->AppendSeparator();
        file->Append(ID_CLEAR_BIN, "Clear Bin");
        file->AppendSeparator();
        file->Append(wxID_EXIT, "Exit");

        wxMenu* edit = new wxMenu;
        edit->Append(ID_NEW_QUEUE, "New Queue\tCtrl+N");
        edit->Append(ID_TOGGLE_OPTIONS, "Toggle Options\tCtrl+,");

        wxMenu* process = new wxMenu;
        process->Append(ID_START, "Start All Queues\tCtrl+Enter");

        wxMenu* help = new wxMenu;
        help->Append(wxID_ABOUT, "About BurstMerge GUI");

        wxMenuBar* menu_bar = new wxMenuBar;
        menu_bar->Append(file, "File");
        menu_bar->Append(edit, "Edit");
        menu_bar->Append(process, "Process");
        menu_bar->Append(help, "Help");
        SetMenuBar(menu_bar);
    }

    void BuildToolbar()
    {
        wxToolBar* toolbar = CreateToolBar(wxTB_FLAT | wxTB_TEXT);
        toolbar->AddTool(ID_START, "Start", wxArtProvider::GetBitmap(wxART_GO_FORWARD));
        toolbar->AddSeparator();
        toolbar->AddTool(ID_NEW_QUEUE, "New Queue", wxArtProvider::GetBitmap(wxART_NEW));
        toolbar->AddTool(ID_CLEAR_BIN, "Clear Bin", wxArtProvider::GetBitmap(wxART_DELETE));
        toolbar->AddSeparator();
        toolbar->AddTool(ID_TOGGLE_OPTIONS, "Options", wxArtProvider::GetBitmap(wxART_LIST_VIEW));
        toolbar->AddTool(wxID_ABOUT, "About", wxArtProvider::GetBitmap(wxART_INFORMATION));
        toolbar->Realize();
        toolbar_ = toolbar;
    }

    void BuildWorkspace()
    {
        wxPanel* center = new wxPanel(this);
        center->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        wxBoxSizer* center_sizer = new wxBoxSizer(wxHORIZONTAL);

        bin_ = new BinPanel(center);
        bin_->SetFileHandler([this](const std::vector<std::string>& paths)
        {
            AddToBin(paths);
        });
        center_sizer->Add(bin_, 0, wxEXPAND | wxALL, 8);

        queue_scroll_ = new wxScrolledWindow(center, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxVSCROLL | wxBORDER_NONE);
        queue_scroll_->SetScrollRate(0, 12);
        queue_scroll_->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        queue_sizer_ = new wxBoxSizer(wxVERTICAL);
        new_queue_button_ = new wxButton(queue_scroll_, ID_NEW_QUEUE, "+ New Queue",
            wxDefaultPosition, wxSize(-1, 38));
        queue_sizer_->Add(new_queue_button_, 0, wxEXPAND | wxALL, 8);
        queue_scroll_->SetSizer(queue_sizer_);
        center_sizer->Add(queue_scroll_, 1, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 8);
        center->SetSizer(center_sizer);

        options_ = new OptionsPanel(this);
        aui_.AddPane(center, wxAuiPaneInfo().Name("workspace").CenterPane().PaneBorder(false));
        aui_.AddPane(options_, wxAuiPaneInfo().Name("options").Caption("Options")
            .Right().BestSize(FromDIP(365), FromDIP(640)).MinSize(FromDIP(330), FromDIP(400)).Floatable(true)
            .Dockable(true).CloseButton(true).Show(true));
        aui_.Update();
    }

    void BuildStatusArea()
    {
        log_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 130),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        aui_.AddPane(log_, wxAuiPaneInfo().Name("log").Caption("Processing Log")
            .Bottom().BestSize(-1, FromDIP(150)).MinSize(-1, FromDIP(80)).Floatable(true)
            .Dockable(true).CloseButton(true).Show(true));
        aui_.Update();

        status_bar_ = CreateStatusBar(3);
        int widths[] = {-2, 180, 110};
        status_bar_->SetStatusWidths(3, widths);
        status_bar_->SetStatusText("Ready", 0);
        gauge_ = new wxGauge(status_bar_, wxID_ANY, 100);
        gauge_->SetValue(0);
        PositionGauge();
    }

    void BindEvents()
    {
        Bind(wxEVT_MENU, &MainFrame::OnAddFiles, this, ID_ADD_FILES);
        Bind(wxEVT_MENU, &MainFrame::OnAddFolder, this, ID_ADD_FOLDER);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { AddQueue(); }, ID_NEW_QUEUE);
        Bind(wxEVT_MENU, &MainFrame::OnClearBin, this, ID_CLEAR_BIN);
        Bind(wxEVT_MENU, &MainFrame::OnToggleOptions, this, ID_TOGGLE_OPTIONS);
        Bind(wxEVT_MENU, &MainFrame::OnStart, this, ID_START);
        Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
        Bind(wxEVT_TOOL, &MainFrame::OnStart, this, ID_START);
        Bind(wxEVT_TOOL, [this](wxCommandEvent&) { AddQueue(); }, ID_NEW_QUEUE);
        Bind(wxEVT_TOOL, &MainFrame::OnClearBin, this, ID_CLEAR_BIN);
        Bind(wxEVT_TOOL, &MainFrame::OnToggleOptions, this, ID_TOGGLE_OPTIONS);
        Bind(wxEVT_TOOL, &MainFrame::OnAbout, this, wxID_ABOUT);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddQueue(); }, ID_NEW_QUEUE);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event)
        {
            PositionGauge();
            event.Skip();
        });
        Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event)
        {
            ApplySystemTheme();
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
        Bind(wxEVT_BM_PROGRESS, &MainFrame::OnProgress, this);
        Bind(wxEVT_BM_QUEUE_DONE, &MainFrame::OnQueueDone, this);
        Bind(wxEVT_BM_PROCESS_DONE, &MainFrame::OnProcessDone, this);
    }

    void AddQueue()
    {
        if (processing_)
        {
            return;
        }
        QueuePanel* queue = new QueuePanel(queue_scroll_, static_cast<int>(queues_.size()) + 1);
        queue->SetHandlers(
            [this](QueuePanel& target)
            {
                AddPathsToQueue(target, bin_->SelectedPaths(), false);
            },
            [this](QueuePanel& target)
            {
                RemoveQueue(target);
            },
            [this](QueuePanel& target, const std::vector<std::string>& paths, bool external)
            {
                AddPathsToQueue(target, paths, external);
            },
            [this](QueuePanel& target, const std::string& path)
            {
                RemovePathFromQueue(target, path);
            });
        queue_sizer_->Insert(queue_sizer_->GetItemCount() - 1, queue, 0,
            wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        queues_.push_back(queue);
        queue_scroll_->FitInside();
        queue_scroll_->Layout();
    }

    void RemoveQueue(QueuePanel& queue)
    {
        if (processing_)
        {
            return;
        }
        for (const std::string& path : queue.Paths())
        {
            DecrementReference(path);
        }
        auto it = std::find(queues_.begin(), queues_.end(), &queue);
        if (it != queues_.end())
        {
            queue_sizer_->Detach(&queue);
            queue.Destroy();
            queues_.erase(it);
        }
        if (queues_.empty())
        {
            AddQueue();
        }
        for (size_t i = 0; i < queues_.size(); ++i)
        {
            queues_[i]->SetNumber(static_cast<int>(i) + 1);
        }
        queue_scroll_->FitInside();
        queue_scroll_->Layout();
    }

    void AddToBin(const std::vector<std::string>& paths)
    {
        for (const std::string& path : paths)
        {
            std::error_code error;
            if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), error))
            {
                continue;
            }
            const std::string key = PathKey(std::filesystem::u8path(path));
            if (bin_refs_.find(key) == bin_refs_.end())
            {
                bin_refs_[key] = 0;
                bin_paths_[key] = path;
                bin_->AddPath(path);
            }
        }
    }

    void AddPathsToQueue(QueuePanel& queue, const std::vector<std::string>& paths, bool external)
    {
        if (external)
        {
            AddToBin(paths);
        }
        for (const std::string& path : paths)
        {
            const std::string key = PathKey(std::filesystem::u8path(path));
            if (bin_refs_.find(key) == bin_refs_.end())
            {
                continue;
            }
            if (queue.AddPath(bin_paths_[key]))
            {
                const int references = ++bin_refs_[key];
                bin_->SetReferences(bin_paths_[key], references);
            }
        }
    }

    void RemovePathFromQueue(QueuePanel& queue, const std::string& path)
    {
        if (queue.RemovePath(path))
        {
            DecrementReference(path);
        }
    }

    void DecrementReference(const std::string& path)
    {
        const std::string key = PathKey(std::filesystem::u8path(path));
        auto it = bin_refs_.find(key);
        if (it == bin_refs_.end())
        {
            return;
        }
        it->second = std::max(0, it->second - 1);
        bin_->SetReferences(bin_paths_[key], it->second);
    }

    void OnAddFiles(wxCommandEvent&)
    {
        wxFileDialog dialog(this, "Stage input files", wxEmptyString, wxEmptyString,
            "Photo files (*.dng;*.arw;*.cr2;*.cr3;*.nef;*.raf;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff)|"
            "*.dng;*.arw;*.cr2;*.cr3;*.nef;*.raf;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
        if (dialog.ShowModal() != wxID_OK)
        {
            return;
        }
        wxArrayString files;
        dialog.GetPaths(files);
        std::vector<std::string> paths;
        for (const wxString& file : files)
        {
            const std::optional<std::string> path = NormalizePath(file);
            if (path)
            {
                paths.push_back(*path);
            }
        }
        AddToBin(paths);
    }

    void OnAddFolder(wxCommandEvent&)
    {
        wxDirDialog dialog(this, "Stage files from folder", wxEmptyString,
            wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
        {
            return;
        }

        std::vector<std::string> paths;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(
            FileSystemPath(dialog.GetPath()), error))
        {
            if (entry.is_regular_file())
            {
                paths.push_back(entry.path().u8string());
            }
        }
        std::sort(paths.begin(), paths.end());
        AddToBin(paths);
    }

    void OnClearBin(wxCommandEvent&)
    {
        if (processing_)
        {
            return;
        }
        const int answer = wxMessageBox(
            "Clearing the Bin also removes all queue assignments. Continue?",
            "Clear Bin", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (answer != wxYES)
        {
            return;
        }
        for (QueuePanel* queue : queues_)
        {
            queue->Clear();
        }
        bin_->Clear();
        bin_refs_.clear();
        bin_paths_.clear();
    }

    void OnToggleOptions(wxCommandEvent&)
    {
        wxAuiPaneInfo& pane = aui_.GetPane("options");
        pane.Show(!pane.IsShown());
        aui_.Update();
    }

    void OnStart(wxCommandEvent&)
    {
        if (processing_)
        {
            return;
        }

        std::vector<QueuePanel*> non_empty;
        for (QueuePanel* queue : queues_)
        {
            if (!queue->Paths().empty())
            {
                non_empty.push_back(queue);
            }
        }
        if (non_empty.empty())
        {
            wxMessageBox("Add files to at least one queue before starting.",
                "Nothing to process", wxOK | wxICON_INFORMATION, this);
            return;
        }

        std::string validation_error;
        if (!options_->Validate(validation_error))
        {
            wxMessageBox(DisplayPath(validation_error), "Unsupported settings",
                wxOK | wxICON_ERROR, this);
            return;
        }

        const burstmerge::Settings settings = options_->Settings();
        if (settings.output_format == burstmerge::OutputFormat::DNG)
        {
            for (QueuePanel* queue : non_empty)
            {
                const bool has_raw = std::any_of(queue->Paths().begin(), queue->Paths().end(), IsRawPath);
                if (!has_raw)
                {
                    wxMessageBox(wxString::Format(
                        "Queue %d contains only RGB images. DNG output requires at least one RAW input.",
                        queue->Number()), "Unsupported output format", wxOK | wxICON_ERROR, this);
                    return;
                }
            }
        }

        const std::optional<std::string> output_root_value = options_->OutputRoot();
        if (!output_root_value)
        {
            wxMessageBox("The output directory path is invalid.", "Output error",
                wxOK | wxICON_ERROR, this);
            return;
        }
        const std::string output_root = *output_root_value;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::u8path(output_root), error);
        if (error || !std::filesystem::is_directory(std::filesystem::u8path(output_root)))
        {
            wxMessageBox("Cannot create output directory:\n" + DisplayPath(output_root) +
                "\n\n" + DisplayPath(error.message()), "Output error", wxOK | wxICON_ERROR, this);
            return;
        }

        const std::string stamp = MakeTimestamp();
        std::vector<ProcessJob> jobs;
        for (QueuePanel* queue : non_empty)
        {
            ProcessJob job;
            job.queue_number = queue->Number();
            job.paths = queue->Paths();
            const std::string base_name = "q" + std::to_string(job.queue_number) + "_" + stamp;
            std::filesystem::path queue_dir;
            for (int suffix = 0; suffix < 1000; ++suffix)
            {
                const std::string name = suffix == 0
                    ? base_name
                    : base_name + "_" + std::to_string(suffix);
                queue_dir = std::filesystem::u8path(output_root) / name;
                error.clear();
                if (std::filesystem::create_directory(queue_dir, error))
                {
                    break;
                }
                if (error)
                {
                    queue_dir.clear();
                    break;
                }
                queue_dir.clear();
            }
            if (queue_dir.empty())
            {
                wxMessageBox(wxString::Format("Cannot create a unique output directory for Queue %d.",
                    job.queue_number), "Output error", wxOK | wxICON_ERROR, this);
                return;
            }
            job.output_dir = queue_dir.u8string();
            jobs.push_back(std::move(job));
        }

        SetProcessingState(true);
        log_->Clear();
        AppendLog("Starting " + std::to_string(jobs.size()) + " queue(s)...");
        worker_ = new ProcessThread(this, std::move(jobs), options_->Backend(),
            settings, options_->StopOnFirstError());
        if (worker_->Create() != wxTHREAD_NO_ERROR || worker_->Run() != wxTHREAD_NO_ERROR)
        {
            delete worker_;
            worker_ = nullptr;
            SetProcessingState(false);
            wxMessageBox("Could not start the processing thread.", "Thread error",
                wxOK | wxICON_ERROR, this);
        }
    }

    void OnProgress(wxThreadEvent& event)
    {
        const ProcessProgress payload = event.GetPayload<ProcessProgress>();
        gauge_->SetValue(payload.percent);
        status_bar_->SetStatusText(wxString::Format("Queue %d: %s",
            payload.queue_number, DisplayPath(payload.stage)), 0);
        status_bar_->SetStatusText(wxString::Format("%d%%", payload.percent), 1);
        status_bar_->SetStatusText(wxString::Format("%.1fs", payload.elapsed_seconds), 2);

        const std::string key = std::to_string(payload.queue_number) + "\n" + payload.stage;
        if (last_progress_key_ != key || last_progress_percent_ != payload.percent)
        {
            std::ostringstream line;
            line << "Q" << payload.queue_number << " [" << std::setw(3)
                 << payload.percent << "%] " << payload.stage;
            AppendLog(line.str());
            last_progress_key_ = key;
            last_progress_percent_ = payload.percent;
        }
    }

    void OnQueueDone(wxThreadEvent& event)
    {
        const QueueResult result = event.GetPayload<QueueResult>();
        std::ostringstream line;
        line << "Queue " << result.queue_number << (result.success ? " completed" : " failed")
             << " in " << std::fixed << std::setprecision(2) << result.elapsed_seconds << "s";
        AppendLog(line.str(), result.success
            ? wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT)
            : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        if (!result.output_path.empty())
        {
            AppendLog("Output: " + result.output_path);
        }
        if (!result.error.empty())
        {
            AppendLog("Error: " + result.error, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        }
    }

    void OnProcessDone(wxThreadEvent& event)
    {
        const ProcessSummary summary = event.GetPayload<ProcessSummary>();
        if (worker_ != nullptr)
        {
            worker_->Wait();
            delete worker_;
            worker_ = nullptr;
        }
        SetProcessingState(false);
        gauge_->SetValue(summary.failed == 0 ? 100 : gauge_->GetValue());
        status_bar_->SetStatusText(summary.failed == 0 ? "All queues completed" : "Processing finished with errors", 0);
        status_bar_->SetStatusText(wxString::Format("%d done", summary.completed), 1);
        AppendLog(summary.failed == 0 ? "All queues completed successfully."
                                      : "Processing finished with " + std::to_string(summary.failed) + " error(s).",
            summary.failed == 0
                ? wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT)
                : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    }

    void OnAbout(wxCommandEvent&)
    {
        wxMessageBox(
            "BurstMerge GUI\n\nA queue-based desktop frontend for BurstMerge.\n"
            "Files remain staged until Start is pressed; each queue is processed independently.",
            "About BurstMerge GUI", wxOK | wxICON_INFORMATION, this);
    }

    void OnClose(wxCloseEvent& event)
    {
        if (processing_)
        {
            if (!event.CanVeto())
            {
                if (worker_ != nullptr)
                {
                    worker_->Wait();
                    delete worker_;
                    worker_ = nullptr;
                }
                event.Skip();
                return;
            }
            wxMessageBox("BurstMerge is still processing. The window cannot close until processing finishes.",
                "Processing in progress", wxOK | wxICON_INFORMATION, this);
            event.Veto();
            return;
        }
        event.Skip();
    }

    void SetProcessingState(bool processing)
    {
        processing_ = processing;
        toolbar_->EnableTool(ID_START, !processing);
        toolbar_->EnableTool(ID_NEW_QUEUE, !processing);
        toolbar_->EnableTool(ID_CLEAR_BIN, !processing);
        GetMenuBar()->Enable(ID_START, !processing);
        GetMenuBar()->Enable(ID_NEW_QUEUE, !processing);
        GetMenuBar()->Enable(ID_CLEAR_BIN, !processing);
        new_queue_button_->Enable(!processing);
        bin_->SetLocked(processing);
        options_->SetLocked(processing);
        for (QueuePanel* queue : queues_)
        {
            queue->SetLocked(processing);
        }
        if (!processing)
        {
            last_progress_key_.clear();
            last_progress_percent_ = -1;
        }
    }

    void PositionGauge()
    {
        if (status_bar_ == nullptr || gauge_ == nullptr)
        {
            return;
        }
        wxRect rect;
        status_bar_->GetFieldRect(1, rect);
        gauge_->SetSize(rect.Deflate(3));
    }

    void ApplySystemTheme()
    {
        const wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
        const wxColour window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        const wxColour shadow = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW);
        const wxColour highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
        const wxColour highlight_text = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);

        SetBackgroundColour(face);
        bin_->ApplySystemTheme();
        options_->ApplySystemTheme();
        queue_scroll_->SetBackgroundColour(face);
        for (QueuePanel* queue : queues_)
        {
            queue->ApplySystemTheme();
        }
        log_->SetBackgroundColour(window);
        log_->SetForegroundColour(text);

        wxAuiDockArt* art = aui_.GetArtProvider();
        art->SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, face);
        art->SetColour(wxAUI_DOCKART_SASH_COLOUR, face);
        art->SetColour(wxAUI_DOCKART_BORDER_COLOUR, shadow);
        art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, highlight);
        art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_GRADIENT_COLOUR, highlight);
        art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, highlight_text);
        art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, face);
        art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_GRADIENT_COLOUR, face);
        art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, text);
        art->SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);

        aui_.Update();
        Refresh();
    }

    void AppendLog(const std::string& text, const wxColour& colour = wxNullColour)
    {
        const wxColour actual = colour.IsOk()
            ? colour
            : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        log_->SetDefaultStyle(wxTextAttr(actual));
        log_->AppendText(DisplayPath(text) + "\n");
        log_->ShowPosition(log_->GetLastPosition());
    }

    static std::string MakeTimestamp()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::ostringstream value;
        value << std::put_time(&local, "%Y%m%d_%H%M%S");
        return value.str();
    }

    wxAuiManager aui_;
    wxToolBar* toolbar_ = nullptr;
    BinPanel* bin_ = nullptr;
    wxScrolledWindow* queue_scroll_ = nullptr;
    wxBoxSizer* queue_sizer_ = nullptr;
    wxButton* new_queue_button_ = nullptr;
    OptionsPanel* options_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxStatusBar* status_bar_ = nullptr;
    wxGauge* gauge_ = nullptr;
    std::vector<QueuePanel*> queues_;
    std::unordered_map<std::string, int> bin_refs_;
    std::unordered_map<std::string, std::string> bin_paths_;
    ProcessThread* worker_ = nullptr;
    bool processing_ = false;
    std::string last_progress_key_;
    int last_progress_percent_ = -1;
};

class BurstMergeApp final : public wxApp
{
public:
    bool OnInit() override
    {
        SetAppName("BurstMerge GUI");
        MainFrame* frame = new MainFrame;
        frame->Show();
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(BurstMergeApp);
