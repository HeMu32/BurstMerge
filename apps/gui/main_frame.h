#pragma once

#include <wx/aui/aui.h>
#include <wx/colour.h>
#include <wx/frame.h>
#include <wx/image.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class wxBoxSizer;
class wxButton;
class wxCloseEvent;
class wxCommandEvent;
class wxGauge;
class wxKeyEvent;
class wxScrolledWindow;
class wxSplitterWindow;
class wxStatusBar;
class wxTextCtrl;
class wxThreadEvent;
class wxToolBar;

namespace burstmerge::gui
{

class BinPanel;
class OptionsPanel;
class ProcessThread;
class QueuePanel;
class ThumbnailLoader;

enum class ActiveList
{
    Bin,
    Queue
};
class MainFrame final : public wxFrame
{

public:
    MainFrame();
    ~MainFrame() override;

private:
    void BuildMenu();
    void BuildToolbar();
    void BuildWorkspace();
    void BuildStatusArea();
    void BindEvents();
    void AddQueue();
    void RemoveQueue(QueuePanel& queue);
    void AddToBin(const std::vector<std::string>& paths);
    void AddPathsToQueue(QueuePanel& queue, const std::vector<std::string>& paths, bool external);
    void RemovePathFromQueue(QueuePanel& queue, const std::string& path);
    void OnRemoveSelection(wxCommandEvent&);
    void OnCharHook(wxKeyEvent& event);
    void RemoveActiveSelection();
    void DecrementReference(const std::string& path);
    void OnAddFiles(wxCommandEvent&);
    void OnAddFolder(wxCommandEvent&);
    void OnClearBin(wxCommandEvent&);
    void OnToggleOptions(wxCommandEvent&);
    void OnStart(wxCommandEvent&);
    void OnProgress(wxThreadEvent& event);
    void OnQueueDone(wxThreadEvent& event);
    void OnProcessDone(wxThreadEvent& event);
    void OnThumbnailReady(wxThreadEvent& event);
    void OnAbout(wxCommandEvent&);
    void OnClose(wxCloseEvent& event);
    void SetProcessingState(bool processing);
    void PositionGauge();
    void ApplySystemTheme();
    void AppendLog(const std::string& text, const wxColour& colour = wxNullColour);
    wxAuiManager aui_;
    wxToolBar* toolbar_ = nullptr;
    wxSplitterWindow* workspace_splitter_ = nullptr;
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
    std::unordered_map<std::string, wxImage> thumbnails_;
    std::unique_ptr<ThumbnailLoader> thumbnail_loader_;
    ActiveList active_list_ = ActiveList::Bin;
    QueuePanel* active_queue_ = nullptr;
    ProcessThread* worker_ = nullptr;
    bool processing_ = false;
    std::string last_progress_key_;
    int last_progress_percent_ = -1;
};

} // namespace burstmerge::gui
