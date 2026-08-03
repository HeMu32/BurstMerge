#include "main_frame.h"

#include "gui_utils.h"
#include "panels.h"
#include "process_thread.h"
#include "thumbnail_loader.h"

#include <wx/artprov.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/wx.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace burstmerge::gui
{

MainFrame::MainFrame()
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
    thumbnail_loader_ = std::make_unique<ThumbnailLoader>(this);
    AddQueue();
    ApplySystemTheme();
    Centre();
}

MainFrame::~MainFrame()
{
    // The loader must stop posting events before wx destroys this frame's children.
    thumbnail_loader_.reset();
    if (worker_ != nullptr)
    {
        worker_->Wait();
        delete worker_;
    }
    aui_.UnInit();
}

void MainFrame::BuildMenu()
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
    edit->Append(ID_REMOVE_SELECTION, "Remove Selection");
    edit->Append(ID_CLEAR_BIN, "Clear Bin");
    edit->AppendSeparator();
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

void MainFrame::BuildToolbar()
{
    wxToolBar* toolbar = CreateToolBar(wxTB_FLAT | wxTB_TEXT);
    toolbar->AddTool(ID_START, "Start", wxArtProvider::GetBitmap(wxART_GO_FORWARD));
    toolbar->AddSeparator();
    toolbar->AddTool(ID_NEW_QUEUE, "New Queue", wxArtProvider::GetBitmap(wxART_NEW));
    toolbar->AddTool(ID_REMOVE_SELECTION, "Remove Sel.",
        wxArtProvider::GetBitmap(wxART_MINUS));
    toolbar->AddTool(ID_CLEAR_BIN, "Clear Bin", wxArtProvider::GetBitmap(wxART_DELETE));
    toolbar->AddSeparator();
    toolbar->AddTool(ID_TOGGLE_OPTIONS, "Options", wxArtProvider::GetBitmap(wxART_LIST_VIEW));
    toolbar->AddTool(wxID_ABOUT, "About", wxArtProvider::GetBitmap(wxART_INFORMATION));
    toolbar->Realize();
    toolbar_ = toolbar;
}

void MainFrame::BuildWorkspace()
{
    workspace_splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
    workspace_splitter_->SetMinimumPaneSize(FromDIP(160));
    workspace_splitter_->SetSashGravity(0.22);

    bin_ = new BinPanel(workspace_splitter_);
    bin_->SetFileHandler([this](const std::vector<std::string>& paths)
    {
        AddToBin(paths);
    });
    bin_->SetActiveHandler([this]()
    {
        active_list_ = ActiveList::Bin;
        active_queue_ = nullptr;
    });

    queue_scroll_ = new wxScrolledWindow(workspace_splitter_, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_NONE);
    queue_scroll_->SetScrollRate(0, 12);
    queue_scroll_->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
    queue_sizer_ = new wxBoxSizer(wxVERTICAL);
    new_queue_button_ = new wxButton(queue_scroll_, ID_NEW_QUEUE, "+ New Queue",
        wxDefaultPosition, wxSize(-1, 38));
    queue_sizer_->Add(new_queue_button_, 0, wxEXPAND | wxALL, 4);
    queue_scroll_->SetSizer(queue_sizer_);
    workspace_splitter_->SplitVertically(bin_, queue_scroll_, FromDIP(230));

    options_ = new OptionsPanel(this);
    aui_.AddPane(workspace_splitter_,
        wxAuiPaneInfo().Name("workspace").CenterPane().PaneBorder(false));
    aui_.AddPane(options_, wxAuiPaneInfo().Name("options").Caption("Options")
        .Right().BestSize(FromDIP(365), FromDIP(640)).MinSize(FromDIP(330), FromDIP(400)).Floatable(true)
        .Dockable(true).CloseButton(true).Show(true));
    aui_.Update();
}

void MainFrame::BuildStatusArea()
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

void MainFrame::BindEvents()
{
    Bind(wxEVT_MENU, &MainFrame::OnAddFiles, this, ID_ADD_FILES);
    Bind(wxEVT_MENU, &MainFrame::OnAddFolder, this, ID_ADD_FOLDER);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { AddQueue(); }, ID_NEW_QUEUE);
    Bind(wxEVT_MENU, &MainFrame::OnRemoveSelection, this, ID_REMOVE_SELECTION);
    Bind(wxEVT_MENU, &MainFrame::OnClearBin, this, ID_CLEAR_BIN);
    Bind(wxEVT_MENU, &MainFrame::OnToggleOptions, this, ID_TOGGLE_OPTIONS);
    Bind(wxEVT_MENU, &MainFrame::OnStart, this, ID_START);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_TOOL, &MainFrame::OnStart, this, ID_START);
    Bind(wxEVT_TOOL, [this](wxCommandEvent&) { AddQueue(); }, ID_NEW_QUEUE);
    Bind(wxEVT_TOOL, &MainFrame::OnRemoveSelection, this, ID_REMOVE_SELECTION);
    Bind(wxEVT_TOOL, &MainFrame::OnClearBin, this, ID_CLEAR_BIN);
    Bind(wxEVT_TOOL, &MainFrame::OnToggleOptions, this, ID_TOGGLE_OPTIONS);
    Bind(wxEVT_TOOL, &MainFrame::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddQueue(); }, ID_NEW_QUEUE);
    Bind(wxEVT_CHAR_HOOK, &MainFrame::OnCharHook, this);
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
    Bind(wxEVT_BM_THUMBNAIL_READY, &MainFrame::OnThumbnailReady, this);
}

void MainFrame::AddQueue()
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
    queue->SetActiveHandler([this, queue]()
    {
        active_list_ = ActiveList::Queue;
        active_queue_ = queue;
    });
    queue_sizer_->Insert(queue_sizer_->GetItemCount() - 1, queue, 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    queues_.push_back(queue);
    queue_scroll_->FitInside();
    queue_scroll_->Layout();
}

void MainFrame::RemoveQueue(QueuePanel& queue)
{
    if (processing_)
    {
        return;
    }
    auto it = std::find(queues_.begin(), queues_.end(), &queue);
    if (it == queues_.end())
    {
        return;
    }
    for (const std::string& path : queue.Paths())
    {
        DecrementReference(path);
    }
    if (active_queue_ == &queue)
    {
        active_list_ = ActiveList::Bin;
        active_queue_ = nullptr;
    }
    queue_sizer_->Detach(&queue);
    queue.Destroy();
    queues_.erase(it);
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

void MainFrame::AddToBin(const std::vector<std::string>& paths)
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
            const auto thumbnail = thumbnails_.find(key);
            if (thumbnail != thumbnails_.end())
            {
                bin_->SetThumbnail(path, thumbnail->second);
            }
            else
            {
                thumbnail_loader_->Request(path, FromDIP(64), FromDIP(64));
            }
        }
    }
}

void MainFrame::AddPathsToQueue(QueuePanel& queue, const std::vector<std::string>& paths, bool external)
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
            const auto thumbnail = thumbnails_.find(key);
            if (thumbnail != thumbnails_.end())
            {
                queue.SetThumbnail(bin_paths_[key], thumbnail->second);
            }
            const int references = ++bin_refs_[key];
            bin_->SetReferences(bin_paths_[key], references);
        }
    }
}

void MainFrame::RemovePathFromQueue(QueuePanel& queue, const std::string& path)
{
    if (queue.RemovePath(path))
    {
        DecrementReference(path);
    }
}

void MainFrame::OnRemoveSelection(wxCommandEvent&)
{
    RemoveActiveSelection();
}

void MainFrame::OnCharHook(wxKeyEvent& event)
{
    if (event.GetKeyCode() != WXK_DELETE || processing_)
    {
        event.Skip();
        return;
    }

    const bool active_has_focus = active_list_ == ActiveList::Bin
        ? bin_->HasListFocus()
        : active_queue_ != nullptr && active_queue_->HasListFocus();
    if (!active_has_focus)
    {
        event.Skip();
        return;
    }

    RemoveActiveSelection();
}

void MainFrame::RemoveActiveSelection()
{
    if (processing_)
    {
        return;
    }

    if (active_list_ == ActiveList::Queue && active_queue_ != nullptr)
    {
        const std::vector<std::string> selected = active_queue_->SelectedPaths();
        for (const std::string& path : selected)
        {
            RemovePathFromQueue(*active_queue_, path);
        }
        return;
    }

    const std::vector<std::string> selected = bin_->SelectedPaths();
    for (const std::string& path : selected)
    {
        for (QueuePanel* queue : queues_)
        {
            queue->RemovePath(path);
        }
        thumbnail_loader_->Cancel(path);
        const std::string key = PathKey(std::filesystem::u8path(path));
        thumbnails_.erase(key);
        bin_refs_.erase(key);
        bin_paths_.erase(key);
    }
    bin_->RemovePaths(selected);
    for (const auto& [key, path] : bin_paths_)
    {
        bin_->SetReferences(path, bin_refs_[key]);
    }
}

void MainFrame::DecrementReference(const std::string& path)
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

void MainFrame::OnAddFiles(wxCommandEvent&)
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

void MainFrame::OnAddFolder(wxCommandEvent&)
{
    wxDirDialog dialog(this, "Stage files from folder", wxEmptyString,
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
    {
        return;
    }

    std::vector<std::string> paths;
    AppendFolderFiles(FileSystemPath(dialog.GetPath()), paths);
    AddToBin(paths);
}

void MainFrame::OnClearBin(wxCommandEvent&)
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
    thumbnail_loader_->ClearPending();
    bin_->Clear();
    bin_refs_.clear();
    bin_paths_.clear();
    thumbnails_.clear();
}

void MainFrame::OnToggleOptions(wxCommandEvent&)
{
    wxAuiPaneInfo& pane = aui_.GetPane("options");
    pane.Show(!pane.IsShown());
    aui_.Update();
}

void MainFrame::OnStart(wxCommandEvent&)
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

    std::vector<ProcessJob> jobs;
    std::unordered_map<std::string, int> reserved_names;
    for (QueuePanel* queue : non_empty)
    {
        ProcessJob job;
        job.queue_number = queue->Number();
        job.paths = queue->Paths();
        const bool has_raw = std::any_of(job.paths.begin(), job.paths.end(), IsRawPath);
        const std::string extension = OutputExtension(settings.output_format, has_raw);
        const std::string base_name = options_->OutputStem(job.paths.front(), settings);
        std::filesystem::path output_path;
        for (int suffix = 1; suffix < 1000; ++suffix)
        {
            const std::string stem = suffix == 1
                ? base_name
                : base_name + "_" + std::to_string(suffix);
            const std::string key = PathKey(std::filesystem::u8path(stem + extension));
            output_path = std::filesystem::u8path(output_root) / (stem + extension);
            error.clear();
            const bool exists = std::filesystem::exists(output_path, error);
            if (error)
            {
                output_path.clear();
                break;
            }
            if (reserved_names.find(key) == reserved_names.end() && !exists)
            {
                reserved_names[key] = 1;
                break;
            }
            output_path.clear();
        }
        if (output_path.empty())
        {
            wxMessageBox(wxString::Format("Cannot create a unique output filename for Queue %d.",
                job.queue_number), "Output error", wxOK | wxICON_ERROR, this);
            return;
        }
        job.output_path = output_path.u8string();
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

void MainFrame::OnProgress(wxThreadEvent& event)
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

void MainFrame::OnQueueDone(wxThreadEvent& event)
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

void MainFrame::OnProcessDone(wxThreadEvent& event)
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

void MainFrame::OnThumbnailReady(wxThreadEvent& event)
{
    const ThumbnailResult result = event.GetPayload<ThumbnailResult>();
    if (result.width <= 0 || result.height <= 0 ||
        result.rgb.size() != static_cast<std::size_t>(result.width) * result.height * 3)
    {
        return;
    }
    const std::string key = PathKey(std::filesystem::u8path(result.path));
    const auto path = bin_paths_.find(key);
    if (path == bin_paths_.end())
    {
        return;
    }
    wxImage image(result.width, result.height, false);
    std::copy(result.rgb.begin(), result.rgb.end(), image.GetData());
    thumbnails_[key] = image;
    bin_->SetThumbnail(path->second, image);
    for (QueuePanel* queue : queues_)
    {
        queue->SetThumbnail(path->second, image);
    }
}

void MainFrame::OnAbout(wxCommandEvent&)
{
    wxMessageBox(
        "BurstMerge GUI\n\nA queue-based desktop frontend for BurstMerge.\n"
        "Files remain staged until Start is pressed; each queue is processed independently.",
        "About BurstMerge GUI", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::OnClose(wxCloseEvent& event)
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

void MainFrame::SetProcessingState(bool processing)
{
    processing_ = processing;
    toolbar_->EnableTool(ID_START, !processing);
    toolbar_->EnableTool(ID_NEW_QUEUE, !processing);
    toolbar_->EnableTool(ID_REMOVE_SELECTION, !processing);
    toolbar_->EnableTool(ID_CLEAR_BIN, !processing);
    GetMenuBar()->Enable(ID_START, !processing);
    GetMenuBar()->Enable(ID_NEW_QUEUE, !processing);
    GetMenuBar()->Enable(ID_REMOVE_SELECTION, !processing);
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

void MainFrame::PositionGauge()
{
    if (status_bar_ == nullptr || gauge_ == nullptr)
    {
        return;
    }
    wxRect rect;
    status_bar_->GetFieldRect(1, rect);
    gauge_->SetSize(rect.Deflate(3));
}

void MainFrame::ApplySystemTheme()
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

void MainFrame::AppendLog(const std::string& text, const wxColour& colour)
{
    const wxColour actual = colour.IsOk()
        ? colour
        : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    log_->SetDefaultStyle(wxTextAttr(actual));
    log_->AppendText(DisplayPath(text) + "\n");
    log_->ShowPosition(log_->GetLastPosition());
}

} // namespace burstmerge::gui
