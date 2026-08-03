#include "thumbnail_loader.h"

#include <wx/mstream.h>
#include <wx/wfstream.h>
#include <wx/image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace burstmerge::gui
{

wxDEFINE_EVENT(wxEVT_BM_THUMBNAIL_READY, wxThreadEvent);

namespace
{

constexpr std::uint64_t kMaximumPreviewBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumPreviewPixels = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaximumPreviewDimension = 8192;
constexpr std::uint32_t kMaximumIfdEntries = 512;
constexpr std::uint32_t kMaximumEntryValues = 256;
constexpr std::size_t kMaximumIfds = 64;
constexpr std::size_t kMaximumTotalEntries = 4096;

class TiffReader
{
public:
    explicit TiffReader(const std::string& path)
        : stream_(std::filesystem::u8path(path), std::ios::binary)
    {
        if (!stream_)
        {
            return;
        }
        stream_.seekg(0, std::ios::end);
        const std::streamoff end = stream_.tellg();
        if (end > 0)
        {
            size_ = static_cast<std::uint64_t>(end);
        }
    }

    bool IsOpen() const
    {
        return stream_.is_open() && size_ >= 8;
    }

    std::uint64_t Size() const
    {
        return size_;
    }

    bool Read(std::uint64_t offset, void* output, std::size_t bytes)
    {
        if (offset > size_ || bytes > size_ - offset ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        {
            return false;
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream_.read(static_cast<char*>(output), static_cast<std::streamsize>(bytes));
        return stream_.good() || stream_.gcount() == static_cast<std::streamsize>(bytes);
    }

private:
    std::ifstream stream_;
    std::uint64_t size_ = 0;
};

struct IfdValues
{
    std::uint32_t new_subfile_type = 0;
    std::uint32_t old_subfile_type = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t compression = 0;
    std::uint32_t photometric = 0;
    std::uint32_t samples = 0;
    std::vector<std::uint32_t> strip_offsets;
    std::vector<std::uint32_t> strip_bytes;
    std::vector<std::uint32_t> tile_offsets;
    std::vector<std::uint32_t> tile_bytes;
    std::vector<std::uint32_t> jpeg_offset;
    std::vector<std::uint32_t> jpeg_bytes;
    std::vector<std::uint32_t> sub_ifds;
};

std::uint16_t Read16(const unsigned char* bytes, bool little)
{
    return little
        ? static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8))
        : static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::uint32_t Read32(const unsigned char* bytes, bool little)
{
    if (little)
    {
        return static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
    }
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) |
        static_cast<std::uint32_t>(bytes[3]);
}

bool ReadUnsignedValues(TiffReader& reader, const std::array<unsigned char, 12>& entry,
    bool little, std::vector<std::uint32_t>& values)
{
    const std::uint16_t type = Read16(entry.data() + 2, little);
    const std::uint32_t count = Read32(entry.data() + 4, little);
    const std::uint32_t element_size = type == 3 ? 2U : type == 4 ? 4U : 0U;
    if (element_size == 0 || count == 0 || count > kMaximumEntryValues)
    {
        return false;
    }
    const std::uint64_t byte_count = static_cast<std::uint64_t>(count) * element_size;
    std::vector<unsigned char> storage(static_cast<std::size_t>(byte_count));
    if (byte_count <= 4)
    {
        std::copy_n(entry.data() + 8, static_cast<std::size_t>(byte_count), storage.data());
    }
    else if (!reader.Read(Read32(entry.data() + 8, little), storage.data(), storage.size()))
    {
        return false;
    }
    values.resize(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        values[i] = type == 3
            ? Read16(storage.data() + static_cast<std::size_t>(i) * 2, little)
            : Read32(storage.data() + static_cast<std::size_t>(i) * 4, little);
    }
    return true;
}

void AssignFirst(const std::vector<std::uint32_t>& source, std::uint32_t& target)
{
    if (!source.empty())
    {
        target = source.front();
    }
}

bool IsReduced(const IfdValues& ifd)
{
    return (ifd.new_subfile_type & 1U) != 0 || ifd.old_subfile_type == 2;
}

bool SelectRange(const IfdValues& ifd, std::uint64_t& offset, std::uint64_t& bytes)
{
    const auto select_single = [&](const std::vector<std::uint32_t>& offsets,
                                   const std::vector<std::uint32_t>& lengths)
    {
        if (offsets.size() != 1 || lengths.size() != 1)
        {
            return false;
        }
        offset = offsets.front();
        bytes = lengths.front();
        return true;
    };
    return select_single(ifd.jpeg_offset, ifd.jpeg_bytes) ||
        select_single(ifd.strip_offsets, ifd.strip_bytes) ||
        select_single(ifd.tile_offsets, ifd.tile_bytes);
}

bool IsSafeJpegIfd(const IfdValues& ifd)
{
    // TIFF photometric 2 is RGB and 6 is YCbCr. CFA (32803) and LinearRaw
    // (34892) are deliberately excluded even when their compression is JPEG.
    const std::uint64_t pixels = static_cast<std::uint64_t>(ifd.width) * ifd.height;
    return IsReduced(ifd) && ifd.width > 0 && ifd.height > 0 &&
        ifd.width <= kMaximumPreviewDimension && ifd.height <= kMaximumPreviewDimension &&
        pixels <= kMaximumPreviewPixels &&
        (ifd.compression == 6 || ifd.compression == 7) &&
        (ifd.photometric == 2 || ifd.photometric == 6) && ifd.samples >= 3;
}

bool ReadJpegDimensions(const std::vector<unsigned char>& jpeg,
    std::uint32_t& width, std::uint32_t& height)
{
    width = 0;
    height = 0;
    if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
    {
        return false;
    }
    std::size_t offset = 2;
    while (offset + 4 <= jpeg.size())
    {
        while (offset < jpeg.size() && jpeg[offset] == 0xff)
        {
            ++offset;
        }
        if (offset >= jpeg.size())
        {
            return false;
        }
        const unsigned char marker = jpeg[offset++];
        if (marker == 0xd9 || marker == 0xda)
        {
            return false;
        }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
        {
            continue;
        }
        if (offset + 2 > jpeg.size())
        {
            return false;
        }
        const std::size_t length = (static_cast<std::size_t>(jpeg[offset]) << 8) |
            jpeg[offset + 1];
        if (length < 2 || length > jpeg.size() - offset)
        {
            return false;
        }
        const bool is_sof = (marker >= 0xc0 && marker <= 0xc3) ||
            (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) ||
            (marker >= 0xcd && marker <= 0xcf);
        if (is_sof)
        {
            if (length < 8)
            {
                return false;
            }
            height = (static_cast<std::uint32_t>(jpeg[offset + 3]) << 8) |
                jpeg[offset + 4];
            width = (static_cast<std::uint32_t>(jpeg[offset + 5]) << 8) |
                jpeg[offset + 6];
            return width > 0 && height > 0;
        }
        offset += length;
    }
    return false;
}

enum class FileSignature
{
    Unknown,
    Tiff,
    Jpeg,
    Png,
    Bmp
};

FileSignature DetectSignature(const std::string& path)
{
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    std::array<unsigned char, 8> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    const std::streamsize count = stream.gcount();
    if (count >= 4 && ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 42 && bytes[3] == 0) ||
        (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == 42)))
    {
        return FileSignature::Tiff;
    }
    if (count >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8)
    {
        return FileSignature::Jpeg;
    }
    const std::array<unsigned char, 8> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (count >= 8 && bytes == png)
    {
        return FileSignature::Png;
    }
    if (count >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
    {
        return FileSignature::Bmp;
    }
    return FileSignature::Unknown;
}

bool HasSafeRasterDimensions(const std::string& path, FileSignature signature)
{
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    std::array<unsigned char, 26> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (stream.gcount() < static_cast<std::streamsize>(bytes.size()))
    {
        return false;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (signature == FileSignature::Png)
    {
        width = Read32(bytes.data() + 16, false);
        height = Read32(bytes.data() + 20, false);
    }
    else if (signature == FileSignature::Bmp)
    {
        const std::int32_t signed_width = static_cast<std::int32_t>(Read32(bytes.data() + 18, true));
        const std::int32_t signed_height = static_cast<std::int32_t>(Read32(bytes.data() + 22, true));
        if (signed_width <= 0 || signed_height == 0 ||
            signed_height == std::numeric_limits<std::int32_t>::min())
        {
            return false;
        }
        width = static_cast<std::uint32_t>(signed_width);
        height = static_cast<std::uint32_t>(signed_height < 0 ? -signed_height : signed_height);
    }
    return width > 0 && height > 0 &&
        width <= kMaximumPreviewDimension && height <= kMaximumPreviewDimension &&
        static_cast<std::uint64_t>(width) * height <= kMaximumPreviewPixels;
}

} // namespace

bool ExtractEmbeddedJpegPreview(const std::string& path, std::vector<unsigned char>& jpeg,
    EmbeddedJpegPreviewInfo& info, std::string& error)
{
    jpeg.clear();
    info = {};
    error.clear();
    TiffReader reader(path);
    if (!reader.IsOpen())
    {
        error = "cannot open TIFF/DNG";
        return false;
    }

    std::array<unsigned char, 8> header{};
    if (!reader.Read(0, header.data(), header.size()))
    {
        error = "truncated TIFF header";
        return false;
    }
    const bool little = header[0] == 'I' && header[1] == 'I';
    const bool big = header[0] == 'M' && header[1] == 'M';
    if ((!little && !big) || Read16(header.data() + 2, little) != 42)
    {
        error = "not a classic TIFF file";
        return false;
    }

    std::deque<std::uint32_t> pending;
    pending.push_back(Read32(header.data() + 4, little));
    std::unordered_set<std::uint32_t> visited;
    std::uint64_t best_area = 0;
    std::uint64_t best_offset = 0;
    std::uint64_t best_bytes = 0;
    EmbeddedJpegPreviewInfo best_info;
    std::size_t total_entries = 0;

    while (!pending.empty() && visited.size() < kMaximumIfds)
    {
        const std::uint32_t ifd_offset = pending.front();
        pending.pop_front();
        if (ifd_offset == 0 || !visited.insert(ifd_offset).second)
        {
            continue;
        }
        std::array<unsigned char, 2> count_bytes{};
        if (!reader.Read(ifd_offset, count_bytes.data(), count_bytes.size()))
        {
            continue;
        }
        const std::uint32_t count = Read16(count_bytes.data(), little);
        if (count > kMaximumIfdEntries || count > kMaximumTotalEntries - total_entries)
        {
            continue;
        }
        total_entries += count;

        IfdValues ifd;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            std::array<unsigned char, 12> entry{};
            const std::uint64_t entry_offset = static_cast<std::uint64_t>(ifd_offset) + 2 +
                static_cast<std::uint64_t>(index) * entry.size();
            if (!reader.Read(entry_offset, entry.data(), entry.size()))
            {
                break;
            }
            const std::uint16_t tag = Read16(entry.data(), little);
            std::vector<std::uint32_t> values;
            switch (tag)
            {
                case 254: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.new_subfile_type); break;
                case 255: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.old_subfile_type); break;
                case 256: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.width); break;
                case 257: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.height); break;
                case 259: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.compression); break;
                case 262: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.photometric); break;
                case 273: ReadUnsignedValues(reader, entry, little, ifd.strip_offsets); break;
                case 277: if (ReadUnsignedValues(reader, entry, little, values)) AssignFirst(values, ifd.samples); break;
                case 279: ReadUnsignedValues(reader, entry, little, ifd.strip_bytes); break;
                case 324: ReadUnsignedValues(reader, entry, little, ifd.tile_offsets); break;
                case 325: ReadUnsignedValues(reader, entry, little, ifd.tile_bytes); break;
                case 330: ReadUnsignedValues(reader, entry, little, ifd.sub_ifds); break;
                case 513: ReadUnsignedValues(reader, entry, little, ifd.jpeg_offset); break;
                case 514: ReadUnsignedValues(reader, entry, little, ifd.jpeg_bytes); break;
                default: break;
            }
        }
        for (std::uint32_t child : ifd.sub_ifds)
        {
            pending.push_back(child);
        }
        std::array<unsigned char, 4> next_bytes{};
        const std::uint64_t next_offset = static_cast<std::uint64_t>(ifd_offset) + 2 +
            static_cast<std::uint64_t>(count) * 12;
        if (reader.Read(next_offset, next_bytes.data(), next_bytes.size()))
        {
            pending.push_back(Read32(next_bytes.data(), little));
        }

        std::uint64_t range_offset = 0;
        std::uint64_t range_bytes = 0;
        if (!IsSafeJpegIfd(ifd) || !SelectRange(ifd, range_offset, range_bytes) ||
            range_bytes < 2 || range_bytes > kMaximumPreviewBytes ||
            range_offset > reader.Size() || range_bytes > reader.Size() - range_offset)
        {
            continue;
        }
        std::array<unsigned char, 2> soi{};
        if (!reader.Read(range_offset, soi.data(), soi.size()) || soi[0] != 0xff || soi[1] != 0xd8)
        {
            continue;
        }
        const std::uint64_t area = static_cast<std::uint64_t>(ifd.width) * ifd.height;
        if (area > best_area)
        {
            best_area = area;
            best_offset = range_offset;
            best_bytes = range_bytes;
            best_info.width = ifd.width;
            best_info.height = ifd.height;
            best_info.offset = range_offset;
            best_info.bytes = static_cast<std::size_t>(range_bytes);
        }
    }

    if (best_area == 0)
    {
        error = "no safe reduced-resolution RGB/YCbCr JPEG preview";
        return false;
    }
    jpeg.resize(static_cast<std::size_t>(best_bytes));
    if (!reader.Read(best_offset, jpeg.data(), jpeg.size()))
    {
        jpeg.clear();
        error = "truncated embedded JPEG preview";
        return false;
    }
    std::uint32_t jpeg_width = 0;
    std::uint32_t jpeg_height = 0;
    if (!ReadJpegDimensions(jpeg, jpeg_width, jpeg_height) ||
        jpeg_width != best_info.width || jpeg_height != best_info.height ||
        jpeg_width > kMaximumPreviewDimension || jpeg_height > kMaximumPreviewDimension ||
        static_cast<std::uint64_t>(jpeg_width) * jpeg_height > kMaximumPreviewPixels)
    {
        jpeg.clear();
        error = "embedded JPEG dimensions are invalid or do not match the preview IFD";
        return false;
    }
    info = best_info;
    return true;
}

ThumbnailLoader::ThumbnailLoader(wxEvtHandler* target)
    : target_(target),
      worker_(&ThumbnailLoader::WorkerMain, this)
{
}

ThumbnailLoader::~ThumbnailLoader()
{
    Stop();
}

void ThumbnailLoader::Request(const std::string& path, int width, int height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !requested_.insert(path).second)
    {
        return;
    }
    requests_.push_back({path, std::max(1, width), std::max(1, height)});
    condition_.notify_one();
}

void ThumbnailLoader::Cancel(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto first = std::remove_if(requests_.begin(), requests_.end(), [&](const RequestItem& item)
    {
        return item.path == path;
    });
    if (first != requests_.end())
    {
        requests_.erase(first, requests_.end());
        requested_.erase(path);
    }
}

void ThumbnailLoader::ClearPending()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const RequestItem& request : requests_)
    {
        requested_.erase(request.path);
    }
    requests_.clear();
}

void ThumbnailLoader::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
        {
            return;
        }
        stopping_ = true;
        target_ = nullptr;
        requests_.clear();
    }
    condition_.notify_one();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void ThumbnailLoader::WorkerMain()
{
    for (;;)
    {
        RequestItem request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !requests_.empty(); });
            if (stopping_)
            {
                return;
            }
            request = std::move(requests_.front());
            requests_.pop_front();
        }

        ThumbnailResult result;
        try
        {
            result = Load(request);
        }
        catch (...)
        {
            result.path = request.path;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        requested_.erase(request.path);
        if (!stopping_ && target_ != nullptr)
        {
            wxThreadEvent* event = new wxThreadEvent(wxEVT_BM_THUMBNAIL_READY);
            event->SetPayload(result);
            wxQueueEvent(target_, event);
        }
    }
}

ThumbnailResult ThumbnailLoader::Load(const RequestItem& request) const
{
    ThumbnailResult result;
    result.path = request.path;
    std::string extension = std::filesystem::u8path(request.path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });

    const FileSignature signature = DetectSignature(request.path);
    wxImage image;
    if (signature == FileSignature::Tiff)
    {
        std::vector<unsigned char> jpeg;
        EmbeddedJpegPreviewInfo info;
        std::string error;
        if (!ExtractEmbeddedJpegPreview(request.path, jpeg, info, error))
        {
            return result;
        }
        wxMemoryInputStream stream(jpeg.data(), jpeg.size());
        image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, request.width);
        image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, request.height);
        image.LoadFile(stream, wxBITMAP_TYPE_JPEG);
    }
    else if ((extension == ".jpg" || extension == ".jpeg") && signature == FileSignature::Jpeg)
    {
        wxFileInputStream stream(wxString::FromUTF8(request.path));
        if (stream.IsOk())
        {
            image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, request.width);
            image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, request.height);
            image.LoadFile(stream, wxBITMAP_TYPE_JPEG);
        }
    }
    else if (extension == ".png" && signature == FileSignature::Png)
    {
        if (!HasSafeRasterDimensions(request.path, signature))
        {
            return result;
        }
        wxFileInputStream stream(wxString::FromUTF8(request.path));
        if (stream.IsOk())
        {
            image.LoadFile(stream, wxBITMAP_TYPE_PNG);
        }
    }
    else if (extension == ".bmp" && signature == FileSignature::Bmp)
    {
        if (!HasSafeRasterDimensions(request.path, signature))
        {
            return result;
        }
        wxFileInputStream stream(wxString::FromUTF8(request.path));
        if (stream.IsOk())
        {
            image.LoadFile(stream, wxBITMAP_TYPE_BMP);
        }
    }
    // Other RAW formats are intentionally never handed to a decoder or converter.
    if (!image.IsOk())
    {
        return result;
    }
    const double scale = std::min(
        static_cast<double>(request.width) / image.GetWidth(),
        static_cast<double>(request.height) / image.GetHeight());
    if (scale < 1.0)
    {
        image = image.Scale(std::max(1, static_cast<int>(image.GetWidth() * scale)),
            std::max(1, static_cast<int>(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
    }
    result.width = image.GetWidth();
    result.height = image.GetHeight();
    const std::size_t bytes = static_cast<std::size_t>(result.width) * result.height * 3;
    result.rgb.assign(image.GetData(), image.GetData() + bytes);
    return result;
}

} // namespace burstmerge::gui
