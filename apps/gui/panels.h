#pragma once

#include "burstmerge/api.h"

#include <wx/panel.h>
#include <wx/spinctrl.h>

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class wxBoxSizer;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxContextMenuEvent;
class wxFlexGridSizer;
class wxImageList;
class wxListCtrl;
class wxListEvent;
class wxSlider;
class wxSizer;
class wxStaticText;
class wxTextCtrl;

namespace burstmerge::gui
{
class BinPanel final : public wxPanel
{

public:
    BinPanel(wxWindow* parent);
    void SetFileHandler(std::function<void(const std::vector<std::string>&)> handler);
    void SetActiveHandler(std::function<void()> handler);
    void AddPath(const std::string& path);
    void SetReferences(const std::string& path, int references);
    std::vector<std::string> SelectedPaths() const;
    void Clear();
    void RemovePaths(const std::vector<std::string>& paths);
    void SetLocked(bool locked);
    bool HasListFocus() const;
    void ApplySystemTheme();

private:
    void RebuildList();
    void RebuildImageList();
    void OnBeginDrag(wxListEvent&);
    wxListCtrl* list_ = nullptr;
    wxImageList* image_list_ = nullptr;
    std::vector<std::string> paths_;
    std::unordered_map<std::string, long> rows_;
    std::function<void(const std::vector<std::string>&)> file_handler_;
    std::function<void()> active_handler_;
};

class QueuePanel final : public wxPanel
{

public:
    QueuePanel(wxWindow* parent, int number);
    void SetHandlers(
        std::function<void(QueuePanel&)> on_add_selected,
        std::function<void(QueuePanel&)> on_close,
        std::function<void(QueuePanel&, const std::vector<std::string>&, bool)> on_drop,
        std::function<void(QueuePanel&, const std::string&)> on_remove);
    void SetActiveHandler(std::function<void()> handler);
    bool AddPath(const std::string& path);
    bool RemovePath(const std::string& path);
    const std::vector<std::string>& Paths() const;
    std::vector<std::string> SelectedPaths() const;
    int Number() const;
    void SetNumber(int number);
    void Clear();
    void SetLocked(bool locked);
    bool HasListFocus() const;
    void ApplySystemTheme();

private:
    void RebuildList();
    void OnContextMenu(wxContextMenuEvent& event);
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
    std::function<void()> active_handler_;
};

class OptionsPanel final : public wxPanel
{

public:
    OptionsPanel(wxWindow* parent);
    burstmerge::BackendType Backend() const;
    burstmerge::Settings Settings() const;
    std::optional<std::string> OutputRoot() const;
    std::string OutputStem(const std::string& first_path, const burstmerge::Settings& settings) const;
    bool StopOnFirstError() const;
    bool Validate(std::string& error) const;
    void SetLocked(bool locked);
    void ApplySystemTheme();

private:
    wxPanel* CreatePipelinePage(wxWindow* parent);
    wxPanel* CreateMergePage(wxWindow* parent);
    wxPanel* CreateAlignPage(wxWindow* parent);
    wxPanel* CreateExposurePage(wxWindow* parent);
    wxPanel* CreateCleanupPage(wxWindow* parent);
    void SetPageSizer(wxPanel* panel, wxSizer* contents);
    wxFlexGridSizer* MakeGrid();
    wxChoice* AddChoice(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        std::initializer_list<wxString> choices, int selection);
    wxSpinCtrl* AddSpin(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        int minimum, int maximum, int value);
    wxSlider* AddSlider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        int minimum, int maximum, int value, const wxString& min_text, const wxString& max_text);
    wxSlider* AddLinkedSlider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
        double minimum, double maximum, double value, double slider_step,
        double input_increment, int digits,
        wxSpinCtrlDouble*& numeric);
    wxCheckBox* AddCheck(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, bool value);
    void UpdateEnabledState();
    wxChoice* backend_ = nullptr;
    wxSpinCtrl* gpu_device_ = nullptr;
    wxSpinCtrl* tile_size_ = nullptr;
    wxChoice* bit_depth_ = nullptr;
    wxChoice* output_format_ = nullptr;
    wxChoice* file_naming_ = nullptr;
    wxTextCtrl* output_dir_ = nullptr;
    wxTextCtrl* dng_convert_dir_ = nullptr;
    wxChoice* merge_algorithm_ = nullptr;
    wxChoice* spatial_mode_ = nullptr;
    wxChoice* frequency_mode_ = nullptr;
    wxChoice* alignment_mode_ = nullptr;
    wxSlider* align_gamma_ = nullptr;
    wxSpinCtrlDouble* align_gamma_value_ = nullptr;
    wxCheckBox* smooth_tile_field_ = nullptr;
    wxChoice* exposure_mode_ = nullptr;
    wxChoice* curve_mode_ = nullptr;
    wxSlider* exposure_stops_ = nullptr;
    wxCheckBox* highlight_recovery_ = nullptr;
    wxCheckBox* hot_pixel_repair_ = nullptr;
    wxSlider* noise_reduction_ = nullptr;
    wxSpinCtrlDouble* noise_reduction_value_ = nullptr;
    wxCheckBox* stop_on_error_ = nullptr;
};

} // namespace burstmerge::gui
