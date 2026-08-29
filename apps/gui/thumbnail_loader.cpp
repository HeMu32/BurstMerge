#include "thumbnail_loader.h"

#include <wx/mstream.h>
#include <wx/wfstream.h>
#include <wx/image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

#ifdef BURSTMERGE_GUI_HAVE_TIFF
#include <tiffio.h>
#endif

namespace burstmerge::gui
{

wxDEFINE_EVENT(wxEVT_BM_THUMBNAIL_READY, wxThreadEvent);

namespace
{

constexpr std::uint64_t kMaximumPreviewBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumPreviewPixels = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumEmbeddedJpegPixels = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumFullTiffPixels = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumInputImageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumContainerBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumScanlineBytes = 1024 * 1024;
constexpr std::uint32_t kMaximumPreviewDimension = 16384;
constexpr std::uint32_t kMaximumIfdEntries = 512;
constexpr std::uint32_t kMaximumEntryValues = 256;
constexpr std::size_t kMaximumIfds = 64;
constexpr std::size_t kMaximumTotalEntries = 4096;
constexpr std::size_t kMaximumPendingRequests = 4096;

bool HasSafeInputSize(const std::string& path)
{
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(std::filesystem::u8path(path), error);
    return !error && bytes > 0 && bytes <= kMaximumInputImageBytes;
}

bool HasSafeContainerSize(const std::string& path)
{
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(std::filesystem::u8path(path), error);
    return !error && bytes > 0 && bytes <= kMaximumContainerBytes;
}

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

std::uint64_t Read64BigEndian(const unsigned char* bytes)
{
    return (static_cast<std::uint64_t>(Read32(bytes, false)) << 32) |
        Read32(bytes + 4, false);
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
        pixels <= kMaximumEmbeddedJpegPixels &&
        (ifd.compression == 6 || ifd.compression == 7) &&
        (ifd.photometric == 2 || ifd.photometric == 6) && ifd.samples >= 3;
}

bool ReadJpegDimensions(const std::vector<unsigned char>& jpeg,
    std::uint32_t& width, std::uint32_t& height, unsigned char* components = nullptr)
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
            if (components != nullptr)
            {
                *components = jpeg[offset + 7];
            }
            return width > 0 && height > 0;
        }
        offset += length;
    }
    return false;
}

bool ReadJpegFileDimensions(const std::string& path, std::uint32_t& width, std::uint32_t& height)
{
    width = 0;
    height = 0;
    if (!HasSafeInputSize(path))
    {
        return false;
    }
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream)
    {
        return false;
    }
    std::vector<unsigned char> header(1024 * 1024);
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(stream.gcount()));
    return ReadJpegDimensions(header, width, height);
}

bool ExtractRafJpegPreview(const std::string& path, std::vector<unsigned char>& jpeg,
    EmbeddedJpegPreviewInfo& info)
{
    TiffReader reader(path);
    std::array<unsigned char, 92> header{};
    if (!reader.IsOpen() || !reader.Read(0, header.data(), header.size()) ||
        std::memcmp(header.data(), "FUJIFILM", 8) != 0)
    {
        return false;
    }
    const std::uint64_t offset = Read32(header.data() + 84, false);
    const std::uint64_t bytes = Read32(header.data() + 88, false);
    if (offset < header.size() || bytes < 4 || bytes > kMaximumPreviewBytes ||
        offset > reader.Size() || bytes > reader.Size() - offset)
    {
        return false;
    }
    jpeg.resize(static_cast<std::size_t>(bytes));
    if (!reader.Read(offset, jpeg.data(), jpeg.size()))
    {
        jpeg.clear();
        return false;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!ReadJpegDimensions(jpeg, width, height) ||
        width > kMaximumPreviewDimension || height > kMaximumPreviewDimension ||
        static_cast<std::uint64_t>(width) * height > kMaximumEmbeddedJpegPixels)
    {
        jpeg.clear();
        return false;
    }
    info = {width, height, offset, jpeg.size()};
    return true;
}

bool ExtractCr3JpegPreview(const std::string& path, std::vector<unsigned char>& jpeg,
    EmbeddedJpegPreviewInfo& info)
{
    static constexpr std::array<unsigned char, 16> kCanonPreviewUuid = {
        0xea, 0xf4, 0x2b, 0x5e, 0x1c, 0x98, 0x4b, 0x88,
        0xb9, 0xfb, 0xb7, 0xdc, 0x40, 0x6e, 0x4d, 0x16
    };
    TiffReader reader(path);
    if (!reader.IsOpen())
    {
        return false;
    }
    std::array<unsigned char, 8> ftyp_header{};
    if (!reader.Read(0, ftyp_header.data(), ftyp_header.size()) ||
        std::memcmp(ftyp_header.data() + 4, "ftyp", 4) != 0)
    {
        return false;
    }
    const std::uint32_t ftyp_size = Read32(ftyp_header.data(), false);
    if (ftyp_size < 16 || ftyp_size > 4096 || ftyp_size > reader.Size())
    {
        return false;
    }
    std::vector<unsigned char> ftyp(ftyp_size);
    if (!reader.Read(0, ftyp.data(), ftyp.size()))
    {
        return false;
    }
    bool canon_brand = std::memcmp(ftyp.data() + 8, "crx ", 4) == 0;
    for (std::size_t offset = 16; !canon_brand && offset + 4 <= ftyp_size; offset += 4)
    {
        canon_brand = std::memcmp(ftyp.data() + offset, "crx ", 4) == 0;
    }
    if (!canon_brand)
    {
        return false;
    }

    std::uint64_t box_offset = ftyp_size;
    std::size_t box_count = 0;
    while (box_offset + 8 <= reader.Size() && box_count++ < kMaximumIfds)
    {
        std::array<unsigned char, 16> box_header{};
        if (!reader.Read(box_offset, box_header.data(), 8))
        {
            return false;
        }
        std::uint64_t box_size = Read32(box_header.data(), false);
        std::uint64_t header_size = 8;
        if (box_size == 1)
        {
            if (!reader.Read(box_offset + 8, box_header.data() + 8, 8))
            {
                return false;
            }
            box_size = Read64BigEndian(box_header.data() + 8);
            header_size = 16;
        }
        else if (box_size == 0)
        {
            box_size = reader.Size() - box_offset;
        }
        if (box_size < header_size || box_size > reader.Size() - box_offset)
        {
            return false;
        }
        if (std::memcmp(box_header.data() + 4, "uuid", 4) == 0 &&
            box_size >= header_size + 48 && box_size - header_size - 48 <= kMaximumPreviewBytes)
        {
            std::array<unsigned char, 48> preview_header{};
            if (reader.Read(box_offset + header_size, preview_header.data(), preview_header.size()) &&
                std::equal(kCanonPreviewUuid.begin(), kCanonPreviewUuid.end(),
                    preview_header.begin()) &&
                std::memcmp(preview_header.data() + 28, "PRVW", 4) == 0)
            {
                const std::uint64_t jpeg_offset = box_offset + header_size + 48;
                const std::uint64_t jpeg_bytes = box_size - header_size - 48;
                jpeg.resize(static_cast<std::size_t>(jpeg_bytes));
                if (!reader.Read(jpeg_offset, jpeg.data(), jpeg.size()))
                {
                    jpeg.clear();
                    return false;
                }
                std::uint32_t width = 0;
                std::uint32_t height = 0;
                if (!ReadJpegDimensions(jpeg, width, height) ||
                    width > kMaximumPreviewDimension || height > kMaximumPreviewDimension ||
                    static_cast<std::uint64_t>(width) * height > kMaximumEmbeddedJpegPixels)
                {
                    jpeg.clear();
                    return false;
                }
                info = {width, height, jpeg_offset, jpeg.size()};
                return true;
            }
        }
        box_offset += box_size;
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
    if (count >= 4 && ((bytes[0] == 'I' && bytes[1] == 'I' &&
        (bytes[2] == 42 || bytes[2] == 43) && bytes[3] == 0) ||
        (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 &&
        (bytes[3] == 42 || bytes[3] == 43))))
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
    if (!HasSafeInputSize(path))
    {
        return false;
    }
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

#ifdef BURSTMERGE_GUI_HAVE_TIFF
struct TiffHandle
{
    TIFF* value = nullptr;
    ~TiffHandle()
    {
        if (value != nullptr)
        {
            TIFFClose(value);
        }
    }
};

struct TiffOpenOptionsGuard
{
    TIFFOpenOptions* value = TIFFOpenOptionsAlloc();
    ~TiffOpenOptionsGuard()
    {
        TIFFOpenOptionsFree(value);
    }
};

int IgnoreTiffMessage(TIFF*, void*, const char*, const char*, va_list)
{
    return 1;
}

ThumbnailResult DecodeTiffThumbnail(const std::string& path, int target_width, int target_height)
{
    ThumbnailResult result;
    result.path = path;
    TiffOpenOptionsGuard options;
    if (options.value == nullptr)
    {
        return result;
    }
    TIFFOpenOptionsSetMaxSingleMemAlloc(options.value, 64 * 1024 * 1024);
    TIFFOpenOptionsSetErrorHandlerExtR(options.value, IgnoreTiffMessage, nullptr);
    TIFFOpenOptionsSetWarningHandlerExtR(options.value, IgnoreTiffMessage, nullptr);
#ifdef _WIN32
    const std::filesystem::path filesystem_path = std::filesystem::u8path(path);
    TiffHandle handle{TIFFOpenWExt(filesystem_path.c_str(), "r", options.value)};
#else
    TiffHandle handle{TIFFOpenExt(path.c_str(), "r", options.value)};
#endif
    if (handle.value == nullptr)
    {
        return result;
    }

    std::uint64_t best_offset = 0;
    std::uint64_t best_area = 0;
    bool best_is_reduced = false;
    std::deque<std::uint64_t> subdirectories;
    std::unordered_set<std::uint64_t> visited;
    auto consider_current = [&]()
    {
        const std::uint64_t current_offset = TIFFCurrentDirOffset(handle.value);
        if (!visited.insert(current_offset).second)
        {
            return;
        }
        std::uint16_t subdirectory_count = 0;
        std::uint64_t* subdirectory_offsets = nullptr;
        if (TIFFGetField(handle.value, TIFFTAG_SUBIFD, &subdirectory_count,
            &subdirectory_offsets) && subdirectory_offsets != nullptr)
        {
            for (std::uint16_t index = 0; index < subdirectory_count &&
                subdirectories.size() < kMaximumIfds; ++index)
            {
                subdirectories.push_back(subdirectory_offsets[index]);
            }
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t subfile_type = 0;
        std::uint16_t photometric = 0;
        TIFFGetField(handle.value, TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(handle.value, TIFFTAG_IMAGELENGTH, &height);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_SUBFILETYPE, &subfile_type);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_PHOTOMETRIC, &photometric);
        const bool rendered = photometric == PHOTOMETRIC_RGB ||
            photometric == PHOTOMETRIC_YCBCR || photometric == PHOTOMETRIC_MINISBLACK ||
            photometric == PHOTOMETRIC_MINISWHITE || photometric == PHOTOMETRIC_PALETTE;
        const std::uint64_t area = static_cast<std::uint64_t>(width) * height;
        const bool safe = width > 0 && height > 0 &&
            width <= kMaximumPreviewDimension && height <= kMaximumPreviewDimension;
        const bool reduced = (subfile_type & FILETYPE_REDUCEDIMAGE) != 0;
        const bool resource_safe = reduced
            ? area <= kMaximumPreviewPixels
            : area <= kMaximumEmbeddedJpegPixels;
        if (rendered && safe && resource_safe && ((reduced && !best_is_reduced) ||
            (reduced == best_is_reduced && area > best_area)))
        {
            best_offset = current_offset;
            best_area = area;
            best_is_reduced = reduced;
        }
    };
    do
    {
        consider_current();
    } while (visited.size() < kMaximumIfds && TIFFReadDirectory(handle.value));
    while (!subdirectories.empty() && visited.size() < kMaximumIfds)
    {
        const std::uint64_t offset = subdirectories.front();
        subdirectories.pop_front();
        if (visited.find(offset) == visited.end() && TIFFSetSubDirectory(handle.value, offset))
        {
            consider_current();
        }
    }

    if (best_area == 0 || !TIFFSetSubDirectory(handle.value, best_offset))
    {
        return result;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TIFFGetField(handle.value, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(handle.value, TIFFTAG_IMAGELENGTH, &height);
    if (!best_is_reduced && best_area > kMaximumFullTiffPixels)
    {
        std::uint16_t bits = 0;
        std::uint16_t samples = 0;
        std::uint16_t planar = 0;
        std::uint16_t photometric = 0;
        std::uint16_t sample_format = SAMPLEFORMAT_UINT;
        std::uint16_t orientation = ORIENTATION_TOPLEFT;
        std::uint32_t rows_per_strip = 0;
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_SAMPLESPERPIXEL, &samples);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_PLANARCONFIG, &planar);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_PHOTOMETRIC, &photometric);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_SAMPLEFORMAT, &sample_format);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_ORIENTATION, &orientation);
        TIFFGetFieldDefaulted(handle.value, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
        const bool rgb = photometric == PHOTOMETRIC_RGB && (samples == 3 || samples == 4);
        const bool grayscale = (photometric == PHOTOMETRIC_MINISBLACK ||
            photometric == PHOTOMETRIC_MINISWHITE) && samples == 1;
        const tmsize_t scanline_bytes = TIFFScanlineSize(handle.value);
        const std::uint64_t required_scanline_bytes = static_cast<std::uint64_t>(width) *
            samples * (bits / 8);
        if ((!rgb && !grayscale) || (bits != 8 && bits != 16) ||
            planar != PLANARCONFIG_CONTIG || sample_format != SAMPLEFORMAT_UINT ||
            orientation != ORIENTATION_TOPLEFT ||
            rows_per_strip == 0 || rows_per_strip > 16 || scanline_bytes <= 0 ||
            static_cast<std::size_t>(scanline_bytes) > kMaximumScanlineBytes ||
            required_scanline_bytes > static_cast<std::uint64_t>(scanline_bytes))
        {
            return result;
        }

        // Large linear TIFFs such as SLg2-1-48-lin.tif have no preview IFD but
        // store one LZW strip per row. Decode only the rows sampled by the 64px
        // thumbnail instead of materializing the full 16-bit RGB raster.
        const double scale = std::min(1.0, std::min(
            static_cast<double>(target_width) / width,
            static_cast<double>(target_height) / height));
        result.width = std::max(1, static_cast<int>(width * scale));
        result.height = std::max(1, static_cast<int>(height * scale));
        result.rgb.resize(static_cast<std::size_t>(result.width) * result.height * 3);
        std::vector<unsigned char> scanline(static_cast<std::size_t>(scanline_bytes));
        for (int y = 0; y < result.height; ++y)
        {
            const std::uint32_t source_y = std::min(height - 1,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * height) /
                    result.height));
            if (TIFFReadScanline(handle.value, scanline.data(), source_y) < 0)
            {
                ThumbnailResult failed;
                failed.path = path;
                return failed;
            }
            for (int x = 0; x < result.width; ++x)
            {
                const std::uint32_t source_x = std::min(width - 1,
                    static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * width) /
                        result.width));
                const std::size_t input = static_cast<std::size_t>(source_x) * samples;
                const std::size_t output = (static_cast<std::size_t>(y) * result.width + x) * 3;
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    const std::size_t source_channel = rgb ? channel : 0;
                    unsigned char value = 0;
                    if (bits == 8)
                    {
                        value = scanline[input + source_channel];
                    }
                    else
                    {
                        std::uint16_t sample = 0;
                        std::memcpy(&sample, scanline.data() +
                            (input + source_channel) * sizeof(sample), sizeof(sample));
                        value = static_cast<unsigned char>(sample >> 8);
                    }
                    if (photometric == PHOTOMETRIC_MINISWHITE)
                    {
                        value = static_cast<unsigned char>(255 - value);
                    }
                    result.rgb[output + channel] = value;
                }
            }
        }
        return result;
    }
    std::vector<std::uint32_t> raster(static_cast<std::size_t>(width) * height);
    if (!TIFFReadRGBAImageOriented(handle.value, width, height, raster.data(),
        ORIENTATION_TOPLEFT, 0))
    {
        return result;
    }

    const double scale = std::min(1.0, std::min(
        static_cast<double>(target_width) / width,
        static_cast<double>(target_height) / height));
    result.width = std::max(1, static_cast<int>(width * scale));
    result.height = std::max(1, static_cast<int>(height * scale));
    result.rgb.resize(static_cast<std::size_t>(result.width) * result.height * 3);
    for (int y = 0; y < result.height; ++y)
    {
        const std::uint32_t source_y = std::min(height - 1,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * height) / result.height));
        for (int x = 0; x < result.width; ++x)
        {
            const std::uint32_t source_x = std::min(width - 1,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * width) / result.width));
            const std::uint32_t pixel = raster[static_cast<std::size_t>(source_y) * width + source_x];
            const std::size_t output = (static_cast<std::size_t>(y) * result.width + x) * 3;
            result.rgb[output] = TIFFGetR(pixel);
            result.rgb[output + 1] = TIFFGetG(pixel);
            result.rgb[output + 2] = TIFFGetB(pixel);
        }
    }
    return result;
}
#endif

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
        const bool safe_rendered_ifd = IsSafeJpegIfd(ifd);
        // Sony NEX-5 generation ARW files expose only the IFD0 PreviewImage
        // through old-style TIFF tags 513/514. Unlike newer ILCE bodies, they
        // have no separate RGB/YCbCr JpgFromRaw IFD carrying dimensions.
        const bool legacy_sony_preview = IsReduced(ifd) &&
            (ifd.compression == 6 || ifd.compression == 7) &&
            ifd.jpeg_offset.size() == 1 && ifd.jpeg_bytes.size() == 1;
        if ((!safe_rendered_ifd && !legacy_sony_preview) ||
            !SelectRange(ifd, range_offset, range_bytes) ||
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
        std::uint32_t candidate_width = ifd.width;
        std::uint32_t candidate_height = ifd.height;
        if (!safe_rendered_ifd)
        {
            std::vector<unsigned char> candidate(static_cast<std::size_t>(range_bytes));
            unsigned char components = 0;
            if (!reader.Read(range_offset, candidate.data(), candidate.size()) ||
                !ReadJpegDimensions(candidate, candidate_width, candidate_height, &components) ||
                components != 3 || candidate_width > kMaximumPreviewDimension ||
                candidate_height > kMaximumPreviewDimension ||
                static_cast<std::uint64_t>(candidate_width) * candidate_height >
                    kMaximumEmbeddedJpegPixels)
            {
                continue;
            }
        }
        const std::uint64_t area = static_cast<std::uint64_t>(candidate_width) * candidate_height;
        if (area > best_area)
        {
            best_area = area;
            best_offset = range_offset;
            best_bytes = range_bytes;
            best_info.width = candidate_width;
            best_info.height = candidate_height;
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
        static_cast<std::uint64_t>(jpeg_width) * jpeg_height > kMaximumEmbeddedJpegPixels)
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
    if (stopping_ || requests_.size() >= kMaximumPendingRequests || !requested_.insert(path).second)
    {
        return;
    }
    const std::uint64_t generation = ++next_generation_;
    generations_[path] = generation;
    requests_.push_back({path, std::max(1, width), std::max(1, height), epoch_, generation});
    condition_.notify_one();
}

void ThumbnailLoader::Cancel(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto first = std::remove_if(requests_.begin(), requests_.end(), [&](const RequestItem& item)
    {
        return item.path == path;
    });
    requests_.erase(first, requests_.end());
    requested_.erase(path);
    generations_.erase(path);
}

void ThumbnailLoader::ClearPending()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    requested_.clear();
    requests_.clear();
    generations_.clear();
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
        bool deliver = false;
        try
        {
            result = DecodeThumbnail(request.path, request.width, request.height);
        }
        catch (...)
        {
            result.path = request.path;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto generation = generations_.find(request.path);
        const bool is_current = generation != generations_.end() &&
            generation->second == request.generation;
        if (is_current)
        {
            requested_.erase(request.path);
            generations_.erase(generation);
        }
        deliver = !stopping_ && target_ != nullptr && request.epoch == epoch_ && is_current;
        if (deliver)
        {
            wxThreadEvent* event = new wxThreadEvent(wxEVT_BM_THUMBNAIL_READY);
            event->SetPayload(result);
            wxQueueEvent(target_, event);
        }
    }
}

ThumbnailResult DecodeThumbnail(const std::string& path, int width, int height)
{
    ThumbnailResult result;
    result.path = path;
    width = std::max(1, width);
    height = std::max(1, height);
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });

    const FileSignature signature = DetectSignature(path);
    wxImage image;
    if (signature == FileSignature::Tiff && (extension == ".tif" || extension == ".tiff"))
    {
        if (!HasSafeContainerSize(path))
        {
            return result;
        }
#ifdef BURSTMERGE_GUI_HAVE_TIFF
        return DecodeTiffThumbnail(path, width, height);
#else
        return result;
#endif
    }
    else if (signature == FileSignature::Tiff)
    {
        if (!HasSafeContainerSize(path))
        {
            return result;
        }
        std::vector<unsigned char> jpeg;
        EmbeddedJpegPreviewInfo info;
        std::string error;
        if (!ExtractEmbeddedJpegPreview(path, jpeg, info, error))
        {
            return result;
        }
        else
        {
            wxMemoryInputStream stream(jpeg.data(), jpeg.size());
            image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, width);
            image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, height);
            image.LoadFile(stream, wxBITMAP_TYPE_JPEG);
        }
    }
    else if (extension == ".raf")
    {
        if (!HasSafeContainerSize(path))
        {
            return result;
        }
        std::vector<unsigned char> jpeg;
        EmbeddedJpegPreviewInfo info;
        if (!ExtractRafJpegPreview(path, jpeg, info))
        {
            return result;
        }
        wxMemoryInputStream stream(jpeg.data(), jpeg.size());
        image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, width);
        image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, height);
        image.LoadFile(stream, wxBITMAP_TYPE_JPEG);
    }
    else if (extension == ".cr3")
    {
        if (!HasSafeContainerSize(path))
        {
            return result;
        }
        std::vector<unsigned char> jpeg;
        EmbeddedJpegPreviewInfo info;
        if (!ExtractCr3JpegPreview(path, jpeg, info))
        {
            return result;
        }
        wxMemoryInputStream stream(jpeg.data(), jpeg.size());
        image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, width);
        image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, height);
        image.LoadFile(stream, wxBITMAP_TYPE_JPEG);
    }
    else if ((extension == ".jpg" || extension == ".jpeg") && signature == FileSignature::Jpeg)
    {
        std::uint32_t source_width = 0;
        std::uint32_t source_height = 0;
        if (!ReadJpegFileDimensions(path, source_width, source_height) ||
            source_width > kMaximumPreviewDimension || source_height > kMaximumPreviewDimension ||
            static_cast<std::uint64_t>(source_width) * source_height > kMaximumPreviewPixels)
        {
            return result;
        }
        wxFileInputStream stream(wxString::FromUTF8(path));
        if (stream.IsOk())
        {
            image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, width);
            image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, height);
            image.LoadFile(stream, wxBITMAP_TYPE_JPEG);
        }
    }
    else if (extension == ".png" && signature == FileSignature::Png)
    {
        if (!HasSafeRasterDimensions(path, signature))
        {
            return result;
        }
        wxFileInputStream stream(wxString::FromUTF8(path));
        if (stream.IsOk())
        {
            image.LoadFile(stream, wxBITMAP_TYPE_PNG);
        }
    }
    else if (extension == ".bmp" && signature == FileSignature::Bmp)
    {
        if (!HasSafeRasterDimensions(path, signature))
        {
            return result;
        }
        wxFileInputStream stream(wxString::FromUTF8(path));
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
        static_cast<double>(width) / image.GetWidth(),
        static_cast<double>(height) / image.GetHeight());
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
