#include "process_thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace burstmerge::gui
{

wxDEFINE_EVENT(wxEVT_BM_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_BM_QUEUE_DONE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_BM_PROCESS_DONE, wxThreadEvent);

ProcessThread::ProcessThread(wxEvtHandler* target, std::vector<ProcessJob> jobs,
    burstmerge::BackendType backend, burstmerge::Settings settings, bool stop_on_error)
    : wxThread(wxTHREAD_JOINABLE),
      target_(target),
      jobs_(std::move(jobs)),
      backend_(backend),
      settings_(std::move(settings)),
      stop_on_error_(stop_on_error)
{
}

wxThread::ExitCode ProcessThread::Entry()
{
    ProcessSummary summary;
    for (const ProcessJob& job : jobs_)
    {
        const auto started = std::chrono::steady_clock::now();
        QueueResult queue_result;
        queue_result.queue_number = job.queue_number;

        try
        {
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

            const burstmerge::Result result = merge.Process(job.output_path);
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

} // namespace burstmerge::gui
