#include "panels.h"

#include "gui_utils.h"

#include <wx/dnd.h>
#include <wx/filename.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/wx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace burstmerge::gui
{

namespace
{

const wxDataFormat kBinItemsFormat("BurstMergeBinItems");
class FileDropTarget final : public wxFileDropTarget
{
public:
    explicit FileDropTarget(std::function<void(const std::vector<std::string>&)> on_files)
        : on_files_(std::move(on_files))
    {
    }

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override
    {
        const std::vector<std::string> paths = ExpandDroppedPaths(filenames);
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
            const std::vector<std::string> paths = ExpandDroppedPaths(files_->GetFilenames());
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

} // namespace

BinPanel::BinPanel(wxWindow* parent)
    : wxPanel(parent)
{
    SetMinSize(FromDIP(wxSize(180, 300)));
    ApplySystemTheme();

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    wxStaticText* title = new wxStaticText(this, wxID_ANY, "BIN");
    wxFont title_font = title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 1);
    title->SetFont(title_font);
    root->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    root->Add(new wxStaticText(this, wxID_ANY,
        "Drop files or folders; reuse them across queues."),
        0, wxLEFT | wxRIGHT | wxTOP, 8);

    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxLC_LIST);
#ifdef _WIN32
    ApplyWindowsExplorerTheme(list_);
#endif
    RebuildImageList();
    root->Add(list_, 1, wxEXPAND | wxALL, 6);
    SetSizer(root);

    list_->Bind(wxEVT_LIST_BEGIN_DRAG, &BinPanel::OnBeginDrag, this);
}

void BinPanel::SetFileHandler(std::function<void(const std::vector<std::string>&)> handler)
{
    file_handler_ = std::move(handler);
    list_->SetDropTarget(new FileDropTarget(file_handler_));
}

void BinPanel::SetActiveHandler(std::function<void()> handler)
{
    active_handler_ = std::move(handler);
    list_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event)
    {
        if (active_handler_)
        {
            active_handler_();
        }
        event.Skip();
    });
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& event)
    {
        if (active_handler_)
        {
            active_handler_();
        }
        event.Skip();
    });
}

void BinPanel::AddPath(const std::string& path)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    if (rows_.find(key) != rows_.end())
    {
        return;
    }

    references_[key] = 0;
    const long row = list_->InsertItem(list_->GetItemCount(),
        DisplayPath(std::filesystem::u8path(path).filename().u8string()), -1);
    list_->SetItemData(row, static_cast<wxUIntPtr>(paths_.size()));
    rows_[key] = row;
    paths_.push_back(path);
    const int image_index = image_list_->Add(
        ComposeThumbnailBitmap(nullptr, 0, FromDIP(48)));
    list_->SetItemImage(row, image_index);
}

void BinPanel::SetReferences(const std::string& path, int references)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    auto it = rows_.find(key);
    if (it != rows_.end())
    {
        references_[key] = references;
        UpdateImage(path);
    }
}

void BinPanel::SetThumbnail(const std::string& path, const wxImage& image)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    if (rows_.find(key) == rows_.end() || !image.IsOk())
    {
        return;
    }
    thumbnails_[key] = image;
    UpdateImage(path);
}

std::vector<std::string> BinPanel::SelectedPaths() const
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

void BinPanel::Clear()
{
    list_->DeleteAllItems();
    paths_.clear();
    rows_.clear();
    references_.clear();
    thumbnails_.clear();
    RebuildImageList();
}

void BinPanel::RemovePaths(const std::vector<std::string>& paths)
{
    if (paths.empty())
    {
        return;
    }
    std::unordered_map<std::string, bool> removed;
    for (const std::string& path : paths)
    {
        removed[PathKey(std::filesystem::u8path(path))] = true;
    }
    paths_.erase(std::remove_if(paths_.begin(), paths_.end(), [&](const std::string& path)
    {
        return removed.find(PathKey(std::filesystem::u8path(path))) != removed.end();
    }), paths_.end());
    for (const auto& [key, unused] : removed)
    {
        references_.erase(key);
        thumbnails_.erase(key);
    }
    RebuildImageList();
    RebuildList();
}

void BinPanel::SetLocked(bool locked)
{
    list_->Enable(!locked);
}

bool BinPanel::HasListFocus() const
{
    return list_->HasFocus();
}

void BinPanel::ApplySystemTheme()
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

void BinPanel::RebuildList()
{
    list_->DeleteAllItems();
    rows_.clear();
    for (size_t i = 0; i < paths_.size(); ++i)
    {
        const long row = list_->InsertItem(list_->GetItemCount(),
            DisplayPath(std::filesystem::u8path(paths_[i]).filename().u8string()),
            static_cast<int>(i));
        list_->SetItemData(row, static_cast<wxUIntPtr>(i));
        rows_[PathKey(std::filesystem::u8path(paths_[i]))] = row;
    }
}

void BinPanel::RebuildImageList()
{
    const int icon_size = FromDIP(48);
    wxImageList* images = new wxImageList(icon_size, icon_size, true,
        std::max<std::size_t>(1, paths_.size()));
    for (const std::string& path : paths_)
    {
        const std::string key = PathKey(std::filesystem::u8path(path));
        const auto thumbnail = thumbnails_.find(key);
        const auto references = references_.find(key);
        images->Add(ComposeThumbnailBitmap(
            thumbnail == thumbnails_.end() ? nullptr : &thumbnail->second,
            references == references_.end() ? 0 : references->second, icon_size));
    }
    list_->AssignImageList(images, wxIMAGE_LIST_SMALL);
    image_list_ = images;
}

void BinPanel::UpdateImage(const std::string& path)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    const auto row = rows_.find(key);
    if (row == rows_.end() || image_list_ == nullptr)
    {
        return;
    }
    const int image_index = static_cast<int>(list_->GetItemData(row->second));
    const auto thumbnail = thumbnails_.find(key);
    const auto references = references_.find(key);
    image_list_->Replace(image_index, ComposeThumbnailBitmap(
        thumbnail == thumbnails_.end() ? nullptr : &thumbnail->second,
        references == references_.end() ? 0 : references->second, FromDIP(48)));
    list_->SetItemImage(row->second, image_index);
}

void BinPanel::OnBeginDrag(wxListEvent&)
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

QueuePanel::QueuePanel(wxWindow* parent, int number)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        QueueBorderStyle()),
      number_(number)
{
    ApplySystemTheme();
    SetMinSize(wxSize(-1, FromDIP(140)));

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
    RebuildImageList();
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

void QueuePanel::SetHandlers(
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

void QueuePanel::SetActiveHandler(std::function<void()> handler)
{
    active_handler_ = std::move(handler);
    list_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event)
    {
        if (active_handler_)
        {
            active_handler_();
        }
        event.Skip();
    });
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& event)
    {
        if (active_handler_)
        {
            active_handler_();
        }
        event.Skip();
    });
}

bool QueuePanel::AddPath(const std::string& path)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    if (keys_.find(key) != keys_.end())
    {
        return false;
    }

    const long row = list_->InsertItem(list_->GetItemCount(),
        DisplayPath(std::filesystem::u8path(path).filename().u8string()), -1);
    list_->SetItemData(row, static_cast<wxUIntPtr>(paths_.size()));
    paths_.push_back(path);
    keys_[key] = path;
    const int image_index = image_list_->Add(
        ComposeThumbnailBitmap(nullptr, 0, FromDIP(48)));
    list_->SetItemImage(row, image_index);
    return true;
}

void QueuePanel::SetThumbnail(const std::string& path, const wxImage& image)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    if (keys_.find(key) == keys_.end() || !image.IsOk())
    {
        return;
    }
    thumbnails_[key] = image;
    UpdateImage(path);
}

bool QueuePanel::RemovePath(const std::string& path)
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
    thumbnails_.erase(key);
    RebuildImageList();
    RebuildList();
    return true;
}

const std::vector<std::string>& QueuePanel::Paths() const
{
    return paths_;
}

std::vector<std::string> QueuePanel::SelectedPaths() const
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

int QueuePanel::Number() const
{
    return number_;
}

void QueuePanel::SetNumber(int number)
{
    number_ = number;
    title_->SetLabel(wxString::Format("QUEUE %d", number_));
}

void QueuePanel::Clear()
{
    paths_.clear();
    keys_.clear();
    thumbnails_.clear();
    list_->DeleteAllItems();
    RebuildImageList();
}

void QueuePanel::SetLocked(bool locked)
{
    list_->Enable(!locked);
    add_button_->Enable(!locked);
    close_button_->Enable(!locked);
}

bool QueuePanel::HasListFocus() const
{
    return list_->HasFocus();
}

void QueuePanel::ApplySystemTheme()
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
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

void QueuePanel::RebuildList()
{
    list_->DeleteAllItems();
    for (size_t i = 0; i < paths_.size(); ++i)
    {
        const long row = list_->InsertItem(list_->GetItemCount(),
            DisplayPath(std::filesystem::u8path(paths_[i]).filename().u8string()),
            static_cast<int>(i));
        list_->SetItemData(row, static_cast<wxUIntPtr>(i));
    }
}

void QueuePanel::RebuildImageList()
{
    const int icon_size = FromDIP(48);
    wxImageList* images = new wxImageList(icon_size, icon_size, true,
        std::max<std::size_t>(1, paths_.size()));
    for (const std::string& path : paths_)
    {
        const auto thumbnail = thumbnails_.find(PathKey(std::filesystem::u8path(path)));
        images->Add(ComposeThumbnailBitmap(
            thumbnail == thumbnails_.end() ? nullptr : &thumbnail->second, 0, icon_size));
    }
    list_->AssignImageList(images, wxIMAGE_LIST_SMALL);
    image_list_ = images;
}

void QueuePanel::UpdateImage(const std::string& path)
{
    const std::string key = PathKey(std::filesystem::u8path(path));
    const auto position = std::find_if(paths_.begin(), paths_.end(), [&](const std::string& value)
    {
        return PathKey(std::filesystem::u8path(value)) == key;
    });
    if (position == paths_.end() || image_list_ == nullptr)
    {
        return;
    }
    const int image_index = static_cast<int>(std::distance(paths_.begin(), position));
    const auto thumbnail = thumbnails_.find(key);
    image_list_->Replace(image_index, ComposeThumbnailBitmap(
        thumbnail == thumbnails_.end() ? nullptr : &thumbnail->second, 0, FromDIP(48)));
    long row = -1;
    while ((row = list_->GetNextItem(row, wxLIST_NEXT_ALL)) != -1)
    {
        if (static_cast<int>(list_->GetItemData(row)) == image_index)
        {
            list_->SetItemImage(row, image_index);
            break;
        }
    }
}

void QueuePanel::OnContextMenu(wxContextMenuEvent& event)
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

OptionsPanel::OptionsPanel(wxWindow* parent)
    : wxPanel(parent)
{
    SetMinSize(FromDIP(wxSize(345, 500)));
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    wxNotebook* notebook = new wxNotebook(this, wxID_ANY);
    notebook->AddPage(CreatePipelinePage(notebook), "Pipeline");
    notebook->AddPage(CreateMergePage(notebook), "Merge");
    notebook->AddPage(CreateAlignPage(notebook), "Align");
    notebook->AddPage(CreateExposurePage(notebook), "Exposure");
    notebook->AddPage(CreateSuperResPage(notebook), "Super Res");
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
super_resolution_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&)
    {
        UpdateEnabledState();
    });
    super_resolution_subpixel_align_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&)
    {
        UpdateEnabledState();
    });
    super_resolution_align_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
    {
        UpdateEnabledState();
    });
    UpdateEnabledState();
}

burstmerge::BackendType OptionsPanel::Backend() const
{
    return backend_->GetSelection() == 1
        ? burstmerge::BackendType::Vulkan
        : burstmerge::BackendType::CPU;
}

burstmerge::Settings OptionsPanel::Settings() const
{
    burstmerge::Settings settings;
    settings.gpu_device_index = gpu_device_->GetValue();
    settings.tile_size = tile_size_->GetValue();
    settings.bit_depth = std::stoi(bit_depth_->GetStringSelection().ToStdString());
    switch (preprocess_interpolation_->GetSelection())
    {
        case 1: settings.preprocess_interpolation = burstmerge::PreprocessInterpolation::Nearest; break;
        case 2: settings.preprocess_interpolation = burstmerge::PreprocessInterpolation::Bilinear; break;
        case 3: settings.preprocess_interpolation = burstmerge::PreprocessInterpolation::MalvarHeCutler; break;
        default: settings.preprocess_interpolation = burstmerge::PreprocessInterpolation::Off; break;
    }
    settings.super_resolution = super_resolution_->GetValue()
        ? burstmerge::SuperResolutionMode::TwoX
        : burstmerge::SuperResolutionMode::Off;
settings.super_resolution_interpolation = super_resolution_interpolation_->GetSelection() == 1
        ? burstmerge::SuperResolutionInterpolation::Bicubic
        : burstmerge::SuperResolutionInterpolation::Bilinear;
    settings.super_resolution_subpixel_align = super_resolution_subpixel_align_->GetValue();
    settings.super_resolution_align_frequency = super_resolution_align_->GetSelection() == 0;
    settings.super_resolution_tile_size = super_resolution_tile_size_->GetValue();
    settings.super_resolution_fourier_grid = super_resolution_fourier_grid_->GetValue();
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
    double align_gamma = 0.0;
    if (!align_gamma_value_->GetTextValue().ToDouble(&align_gamma))
    {
        align_gamma = align_gamma_value_->GetValue();
    }
    settings.align_gamma = static_cast<float>(std::clamp(align_gamma, 0.1, 2.0));
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
    double noise_reduction = 0.0;
    if (!noise_reduction_value_->GetTextValue().ToDouble(&noise_reduction))
    {
        noise_reduction = noise_reduction_value_->GetValue();
    }
    settings.noise_reduction = static_cast<float>(std::clamp(noise_reduction, 0.0, 30.0));
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

std::optional<std::string> OptionsPanel::OutputRoot() const
{
    wxString value = output_dir_->GetValue();
    if (value.empty())
    {
        value = "./out";
    }
    return NormalizePath(value);
}

std::string OptionsPanel::OutputStem(const std::string& first_path, const burstmerge::Settings& settings) const
{
    std::string merge;
    switch (settings.merge_algo)
    {
        case burstmerge::MergeAlgorithm::Frequency:
            switch (settings.frequency_mode)
            {
                case burstmerge::FrequencyMode::WienerFft: merge = "frequency-wiener"; break;
                case burstmerge::FrequencyMode::WienerFftRobust: merge = "frequency-wiener-robust"; break;
                default: merge = "frequency-laplacian"; break;
            }
            break;
        case burstmerge::MergeAlgorithm::TemporalAverage: merge = "temporal-average"; break;
        case burstmerge::MergeAlgorithm::TemporalMedian: merge = "temporal-median"; break;
        case burstmerge::MergeAlgorithm::ExpBracketAverage: merge = "exposure-bracket-average"; break;
        default:
            merge = settings.spatial_mode == burstmerge::SpatialMergeMode::Linear
                ? "spatial-linear"
                : "spatial-standard";
            break;
    }

    std::string alignment;
    switch (settings.alignment_mode)
    {
        case burstmerge::AlignmentMode::DenseTile: alignment = "dense"; break;
        case burstmerge::AlignmentMode::Frequency: alignment = "frequency"; break;
        case burstmerge::AlignmentMode::Skip: alignment = "skip"; break;
        default: alignment = "standard"; break;
    }

    std::string exposure;
    switch (settings.exposure_mode)
    {
        case burstmerge::ExposureMode::Linear: exposure = "linear"; break;
        case burstmerge::ExposureMode::Curve:
            exposure = settings.exposure_curve_mode == burstmerge::ExposureCurveMode::LocalReinhard
                ? "curve-local"
                : "curve-global";
            break;
        default: exposure = "off"; break;
    }

    std::string parameters = std::string(Backend() == burstmerge::BackendType::Vulkan
            ? "vulkan"
            : "cpu") +
        "_" + merge + "_align-" + alignment +
        "_g" + FormatOptionNumber(settings.align_gamma, 2) +
        "_nr" + FormatOptionNumber(settings.noise_reduction, 1) +
        "_exp-" + exposure;
    if (settings.exposure_mode != burstmerge::ExposureMode::Off)
    {
        parameters += "-s" + FormatOptionNumber(settings.exposure_stops, 1);
    }
    if (settings.smooth_tile_field)
    {
        parameters += "_smooth";
    }
    parameters += settings.highlight_recovery ? "_hl" : "_nohl";
    switch (settings.preprocess_interpolation)
    {
        case burstmerge::PreprocessInterpolation::Nearest: parameters += "_rgb-nearest"; break;
        case burstmerge::PreprocessInterpolation::Bilinear: parameters += "_rgb-bilinear"; break;
        case burstmerge::PreprocessInterpolation::MalvarHeCutler: parameters += "_rgb-mhc"; break;
        default: break;
    }
    if (settings.super_resolution == burstmerge::SuperResolutionMode::TwoX)
    {
        parameters += settings.super_resolution_interpolation ==
            burstmerge::SuperResolutionInterpolation::Bicubic
            ? "_sr2x-bicubic"
            : "_sr2x-bilinear";
    }
    if (settings.hot_pixel_repair)
    {
        parameters += "_hotpix";
    }
    parameters +=
        "_t" + std::to_string(settings.tile_size) +
        "_b" + std::to_string(settings.bit_depth);
    if (file_naming_->GetSelection() == 1)
    {
        wxString first_stem = DisplayPath(
            std::filesystem::u8path(first_path).stem().u8string());
        first_stem = DisplayPath(SanitizeFileStem(first_stem.utf8_string()));
        while (!first_stem.empty() &&
            (first_stem.utf8_string().length() + parameters.length() + 1) > 220)
        {
            first_stem.RemoveLast();
        }
        parameters = (first_stem.empty() ? std::string("burst") : first_stem.utf8_string()) +
            "_" + parameters;
    }
    return SanitizeFileStem(parameters);
}

bool OptionsPanel::StopOnFirstError() const
{
    return stop_on_error_->GetValue();
}

bool OptionsPanel::Validate(std::string& error) const
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
    if (super_resolution_->GetValue() && preprocess_interpolation_->GetSelection() == 0)
    {
        error = "2x super-resolution requires enabling 'Interpolate Bayer first' (refusing to upscale a Bayer mosaic).";
        return false;
    }
    if (!dng_convert_dir_->GetValue().empty() && !NormalizePath(dng_convert_dir_->GetValue()))
    {
        error = "The DNG conversion cache path is invalid.";
        return false;
    }
    return true;
}

void OptionsPanel::SetLocked(bool locked)
{
    Enable(!locked);
}

void OptionsPanel::ApplySystemTheme()
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
    Refresh();
}

wxPanel* OptionsPanel::CreatePipelinePage(wxWindow* parent)
{
    wxPanel* panel = new wxPanel(parent);
    wxFlexGridSizer* grid = MakeGrid();
    backend_ = AddChoice(panel, grid, "Backend", {"CPU", "Vulkan"}, 0);
    gpu_device_ = AddSpin(panel, grid, "GPU device", -1, 64, -1);
    tile_size_ = AddSpin(panel, grid, "Tile size", 16, 256, 32);
    bit_depth_ = AddChoice(panel, grid, "Bit depth", {"8", "10", "12", "14", "16"}, 3);
    output_format_ = AddChoice(panel, grid, "Output format",
        {"Auto", "PNG", "JPEG", "BMP", "TIFF", "DNG"}, 0);
preprocess_interpolation_ = AddChoice(panel, grid, "Interpolate Bayer first",
        {"Off", "Nearest", "Bilinear", "Malvar-He-Cutler"}, 0);
    file_naming_ = AddChoice(panel, grid, "File naming",
        {"Processing parameters", "First frame + parameters"}, 1);
    file_naming_->SetToolTip(
        "Outputs directly to the selected directory. Existing names receive a numeric suffix.");

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
    wxBoxSizer* cache_row = new wxBoxSizer(wxHORIZONTAL);
    dng_convert_dir_ = new wxTextCtrl(panel, wxID_ANY);
    dng_convert_dir_->SetHint("Default: alongside output");
    wxButton* cache_browse = new wxButton(panel, ID_DNG_CACHE_BROWSE, "...",
        wxDefaultPosition, FromDIP(wxSize(36, -1)), wxBU_EXACTFIT);
    cache_row->Add(dng_convert_dir_, 1, wxRIGHT, FromDIP(4));
    cache_row->Add(cache_browse, 0);
    grid->Add(cache_row, 1, wxEXPAND);
    cache_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        wxDirDialog dialog(this, "Choose DNG conversion cache",
            dng_convert_dir_->GetValue(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK)
        {
            dng_convert_dir_->SetValue(dialog.GetPath());
        }
    });
    SetPageSizer(panel, grid);
    return panel;
}

wxPanel* OptionsPanel::CreateMergePage(wxWindow* parent)
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

wxPanel* OptionsPanel::CreateAlignPage(wxWindow* parent)
{
    wxPanel* panel = new wxPanel(parent);
    wxFlexGridSizer* grid = MakeGrid();
    alignment_mode_ = AddChoice(panel, grid, "Mode",
        {"Standard", "Dense tile", "Frequency", "Skip"}, 0);
    align_gamma_ = AddLinkedSlider(panel, grid, "Alignment gamma", 0.1, 2.0, 1.0,
        0.01, 0.05, 2, align_gamma_value_);
    smooth_tile_field_ = AddCheck(panel, grid, "Smooth tile field", false);
    SetPageSizer(panel, grid);
    return panel;
}

wxPanel* OptionsPanel::CreateExposurePage(wxWindow* parent)
{
    wxPanel* panel = new wxPanel(parent);
    wxFlexGridSizer* grid = MakeGrid();
    exposure_mode_ = AddChoice(panel, grid, "Mode", {"Off", "Linear", "Curve"}, 0);
    curve_mode_ = AddChoice(panel, grid, "Curve mode", {"Global", "Local Reinhard"}, 0);
    exposure_stops_ = AddSlider(panel, grid, "Stops", -30, 30, 0, "-3.0", "+3.0");
    SetPageSizer(panel, grid);
    return panel;
}

wxPanel* OptionsPanel::CreateSuperResPage(wxWindow* parent)
{
    wxPanel* panel = new wxPanel(parent);
    wxFlexGridSizer* grid = MakeGrid();
    super_resolution_ = AddCheck(panel, grid, "2x super-resolution", false);
    super_resolution_interpolation_ = AddChoice(panel, grid, "Fallback interpolation",
        {"Bilinear", "Bicubic"}, 0);
    super_resolution_subpixel_align_ = AddCheck(panel, grid,
        "Dedicated sub-pixel alignment", true);
    super_resolution_align_ = AddChoice(panel, grid, "Sub-pixel method",
        {"Frequency", "SAD parabola"}, 1);
    super_resolution_tile_size_ = AddSpin(panel, grid, "Alignment tile size",
        16, 256, 32);
    super_resolution_fourier_grid_ = AddSpin(panel, grid, "Fourier grid",
        3, 9, 5);
    SetPageSizer(panel, grid);
    return panel;
}

wxPanel* OptionsPanel::CreateCleanupPage(wxWindow* parent)
{
    wxPanel* panel = new wxPanel(parent);
    wxFlexGridSizer* grid = MakeGrid();
    highlight_recovery_ = AddCheck(panel, grid, "Highlight recovery", true);
    hot_pixel_repair_ = AddCheck(panel, grid, "Hot-pixel repair", false);
    noise_reduction_ = AddLinkedSlider(panel, grid, "Noise reduction", 0.0, 30.0, 13.0,
        0.1, 0.5, 1, noise_reduction_value_);
    stop_on_error_ = AddCheck(panel, grid, "Stop on first error", true);
    SetPageSizer(panel, grid);
    return panel;
}

void OptionsPanel::SetPageSizer(wxPanel* panel, wxSizer* contents)
{
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    root->Add(contents, 1, wxEXPAND | wxALL, FromDIP(12));
    panel->SetSizer(root);
}

wxFlexGridSizer* OptionsPanel::MakeGrid()
{
    wxFlexGridSizer* grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1, 1);
    grid->SetFlexibleDirection(wxHORIZONTAL);
    grid->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
    return grid;
}

wxChoice* OptionsPanel::AddChoice(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
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

wxSpinCtrl* OptionsPanel::AddSpin(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
    int minimum, int maximum, int value)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    wxSpinCtrl* spin = new wxSpinCtrl(parent, wxID_ANY);
    spin->SetRange(minimum, maximum);
    spin->SetValue(value);
    grid->Add(spin, 1, wxEXPAND);
    return spin;
}

wxSlider* OptionsPanel::AddSlider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
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

wxSlider* OptionsPanel::AddLinkedSlider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
    double minimum, double maximum, double value, double slider_step,
    double input_increment, int digits,
    wxSpinCtrlDouble*& numeric)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    const int steps = static_cast<int>(std::lround((maximum - minimum) / slider_step));
    const int initial = static_cast<int>(std::lround((value - minimum) / slider_step));
    wxSlider* slider = new wxSlider(parent, wxID_ANY, initial, 0, steps);
    numeric = new wxSpinCtrlDouble(parent, wxID_ANY, wxEmptyString,
        wxDefaultPosition, FromDIP(wxSize(82, -1)), wxSP_ARROW_KEYS,
        minimum, maximum, value, input_increment);
    numeric->SetDigits(digits);
    numeric->SetToolTip("Enter a value directly or adjust the linked slider.");
    row->Add(slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(numeric, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(row, 1, wxEXPAND);

    slider->Bind(wxEVT_SLIDER, [numeric, minimum, slider_step](wxCommandEvent& event)
    {
        numeric->SetValue(minimum + static_cast<double>(event.GetInt()) * slider_step);
    });
    numeric->Bind(wxEVT_SPINCTRLDOUBLE, [slider, minimum, slider_step](wxSpinDoubleEvent& event)
    {
        slider->SetValue(static_cast<int>(std::lround(
            (event.GetValue() - minimum) / slider_step)));
    });
    numeric->Bind(wxEVT_TEXT,
        [slider, numeric, minimum, maximum, slider_step](wxCommandEvent&)
    {
        double entered = 0.0;
        if (numeric->GetTextValue().ToDouble(&entered))
        {
            entered = std::clamp(entered, minimum, maximum);
            slider->SetValue(static_cast<int>(std::lround(
                (entered - minimum) / slider_step)));
        }
    });
    return slider;
}

wxCheckBox* OptionsPanel::AddCheck(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, bool value)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    wxCheckBox* check = new wxCheckBox(parent, wxID_ANY, "Enabled");
    check->SetValue(value);
    grid->Add(check, 1, wxEXPAND);
    return check;
}

void OptionsPanel::UpdateEnabledState()
{
    gpu_device_->Enable(backend_->GetSelection() == 1);
    spatial_mode_->Enable(merge_algorithm_->GetSelection() == 0);
    frequency_mode_->Enable(merge_algorithm_->GetSelection() == 1);
    noise_reduction_->Enable(merge_algorithm_->GetSelection() <= 1);
    noise_reduction_value_->Enable(merge_algorithm_->GetSelection() <= 1);
    curve_mode_->Enable(exposure_mode_->GetSelection() == 2);
    exposure_stops_->Enable(exposure_mode_->GetSelection() != 0);
super_resolution_interpolation_->Enable(super_resolution_->GetValue());
    const bool sr_on = super_resolution_->GetValue();
    super_resolution_subpixel_align_->Enable(sr_on);
    super_resolution_align_->Enable(sr_on && super_resolution_subpixel_align_->GetValue());
    super_resolution_tile_size_->Enable(sr_on);
    super_resolution_fourier_grid_->Enable(
        sr_on && super_resolution_subpixel_align_->GetValue() &&
        super_resolution_align_->GetSelection() == 0);
}

} // namespace burstmerge::gui
