#include "burstmerge/internal/io/image_decoder.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <stdexcept>
#include <vector>

#ifdef BURSTMERGE_HAVE_JPEG
#include <jpeglib.h>
#endif

namespace burstmerge
{
namespace io
{

#ifdef BURSTMERGE_HAVE_JPEG

struct JpegErrorMgr
{
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void JpegErrorExit(j_common_ptr cinfo)
{
    JpegErrorMgr* myerr = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    (*cinfo->err->output_message)(cinfo);
    longjmp(myerr->setjmp_buffer, 1);
}

namespace
{
struct FileGuard
{
    FILE* fp;
    explicit FileGuard(FILE* f) : fp(f) {}
    ~FileGuard()
    {
        if (fp) std::fclose(fp);
    }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
};

struct JpegDecompressGuard
{
    jpeg_decompress_struct* cinfo = nullptr;
    void arm(jpeg_decompress_struct* c) { cinfo = c; }
    ~JpegDecompressGuard()
    {
        if (cinfo) jpeg_destroy_decompress(cinfo);
    }
    JpegDecompressGuard() = default;
    JpegDecompressGuard(const JpegDecompressGuard&) = delete;
    JpegDecompressGuard& operator=(const JpegDecompressGuard&) = delete;
};
} // namespace

class JpegDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".jpg" || e == ".jpeg";
    }

    DecodedImage Decode(const std::string& path) override
    {
        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp)
        {
            throw std::runtime_error("JpegDecoder: cannot open " + path);
        }
        FileGuard fp_guard(fp);

        jpeg_decompress_struct cinfo;
        JpegErrorMgr jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = JpegErrorExit;

        JpegDecompressGuard cinfo_guard;

        if (setjmp(jerr.setjmp_buffer))
        {
            throw std::runtime_error("JpegDecoder: decode error in " + path);
        }

        jpeg_create_decompress(&cinfo);
        cinfo_guard.arm(&cinfo);
        jpeg_stdio_src(&cinfo, fp);
        jpeg_read_header(&cinfo, TRUE);

        // Detect the actual color space for meaningful error reporting.
        // num_components comes from the original encoding (set by read_header).
        if (cinfo.num_components != 1 && cinfo.num_components != 3)
        {
            const char* cs_name = "unknown";
            switch (cinfo.jpeg_color_space)
            {
                case JCS_CMYK:  cs_name = "CMYK";  break;
                case JCS_YCCK:  cs_name = "YCCK";  break;
                case JCS_YCbCr: cs_name = "YCbCr"; break;
                default: break;
            }
            throw std::runtime_error(
                std::string("JpegDecoder: unsupported color space ") + cs_name
                + " (" + std::to_string(cinfo.num_components) + " components). "
                "Only 1-channel (grayscale) and 3-channel (RGB) JPEG are supported. "
                "Use an external tool to convert to sRGB first.");
        }

        jpeg_start_decompress(&cinfo);

        uint32_t w = cinfo.output_width;
        uint32_t h = cinfo.output_height;
        uint32_t channels = cinfo.output_components;

        DecodedImage result;
        result.info.width     = w;
        result.info.height    = h;
        result.info.pix_fmt   = (channels == 1) ? kPixelGray
                              : kPixelRGB;
        result.info.bit_depth = 8;
        result.info.is_raw    = false;
        result.info.white_level = 255.0f;

        result.pixels.resize(static_cast<size_t>(w) * h * channels);

        std::vector<uint8_t> row(static_cast<size_t>(w) * channels);
        for (uint32_t y = 0; y < h; ++y)
        {
            uint8_t* row_ptr = row.data();
            jpeg_read_scanlines(&cinfo, &row_ptr, 1);
            for (uint32_t x = 0; x < w; ++x)
            {
                size_t dst = (static_cast<size_t>(y) * w + x) * channels;
                for (uint32_t c = 0; c < channels; ++c)
                {
                    result.pixels[dst + c] = static_cast<float>(row[x * channels + c]);
                }
            }
        }

        jpeg_finish_decompress(&cinfo);

        return result;
    }
};

#else // !BURSTMERGE_HAVE_JPEG

class JpegDecoder : public ImageDecoder
{
public:
    bool CanDecode(const std::string& path) override
    {
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot == std::string::npos) return false;
        std::string e = ext.substr(dot);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".jpg" || e == ".jpeg";
    }

    DecodedImage Decode(const std::string& path) override
    {
        throw std::runtime_error("JPEG decoding not available (libjpeg-turbo not linked)");
    }
};

#endif // BURSTMERGE_HAVE_JPEG

std::unique_ptr<ImageDecoder> CreateJpegDecoder()
{
    return std::make_unique<JpegDecoder>();
}

} // namespace io
} // namespace burstmerge
