#pragma once

#include <wx/event.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace burstmerge::gui
{

wxDECLARE_EVENT(wxEVT_BM_THUMBNAIL_READY, wxThreadEvent);

struct EmbeddedJpegPreviewInfo
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
};

struct ThumbnailResult
{
    std::string path;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
};

// Reads only TIFF metadata and one validated, reduced-resolution JPEG range.
bool ExtractEmbeddedJpegPreview(const std::string& path, std::vector<unsigned char>& jpeg,
    EmbeddedJpegPreviewInfo& info, std::string& error);
ThumbnailResult DecodeThumbnail(const std::string& path, int width, int height);

class ThumbnailLoader final
{
public:
    explicit ThumbnailLoader(wxEvtHandler* target);
    ~ThumbnailLoader();
    void Request(const std::string& path, int width, int height);
    void Cancel(const std::string& path);
    void ClearPending();
    void Stop();

private:
    struct RequestItem
    {
        std::string path;
        int width = 0;
        int height = 0;
    };

    void WorkerMain();
    wxEvtHandler* target_ = nullptr;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<RequestItem> requests_;
    std::unordered_set<std::string> requested_;
    std::thread worker_;
    bool stopping_ = false;
};

} // namespace burstmerge::gui
