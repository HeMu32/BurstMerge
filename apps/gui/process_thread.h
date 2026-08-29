#pragma once

#include "burstmerge/api.h"

#include <wx/event.h>
#include <wx/thread.h>

#include <string>
#include <vector>

namespace burstmerge::gui
{

wxDECLARE_EVENT(wxEVT_BM_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_BM_QUEUE_DONE, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_BM_PROCESS_DONE, wxThreadEvent);
struct ProcessJob
{
    int queue_number = 0;
    std::vector<std::string> paths;
    std::string output_path;
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
        burstmerge::BackendType backend, burstmerge::Settings settings, bool stop_on_error);

protected:
    ExitCode Entry() override;

private:
    wxEvtHandler* target_;
    std::vector<ProcessJob> jobs_;
    burstmerge::BackendType backend_;
    burstmerge::Settings settings_;
    bool stop_on_error_;
};

} // namespace burstmerge::gui
