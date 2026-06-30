#include "burstmerge/internal/io/image_writer.h"

#include <algorithm>
#include <cmath>
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

struct JpegCompressGuard
{
    jpeg_compress_struct* cinfo = nullptr;
    explicit JpegCompressGuard(jpeg_compress_struct* c) : cinfo(c) {}
    ~JpegCompressGuard()
    {
        if (cinfo) jpeg_destroy_compress(cinfo);
    }
    JpegCompressGuard(const JpegCompressGuard&) = delete;
    JpegCompressGuard& operator=(const JpegCompressGuard&) = delete;
};
} // namespace

class JpegWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::JPEG) && (bit_depth <= 8);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        if (image.channels != 1 && image.channels != 3)
        {
            throw std::runtime_error("JpegWriter: only 1 or 3 channel images supported");
        }

        uint32_t channels = image.channels;

        jpeg_compress_struct cinfo;
        jpeg_error_mgr jerr;
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);
        JpegCompressGuard cinfo_guard(&cinfo);

        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp)
        {
            throw std::runtime_error("JpegWriter: cannot open " + path);
        }
        FileGuard fp_guard(fp);

        jpeg_stdio_dest(&cinfo, fp);

        cinfo.image_width = image.width;
        cinfo.image_height = image.height;
        cinfo.input_components = static_cast<int>(channels);
        cinfo.in_color_space = (channels == 3) ? JCS_RGB : JCS_GRAYSCALE;

        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, 96, TRUE);
        jpeg_start_compress(&cinfo, TRUE);

        float wl = (params.white_level > 1.0f) ? params.white_level : 255.0f;
        float scale = (wl > 1.0f) ? (255.0f / wl) : 1.0f;

        std::vector<uint8_t> row(static_cast<size_t>(image.width) * channels);
        while (cinfo.next_scanline < cinfo.image_height)
        {
            uint32_t y = cinfo.next_scanline;
            for (uint32_t x = 0; x < image.width; ++x)
            {
                size_t src = (static_cast<size_t>(y) * image.width + x) * channels;
                for (uint32_t c = 0; c < channels; ++c)
                {
                    row[x * channels + c] = Quantize(image.data[src + c], scale);
                }
            }
            uint8_t* row_ptr = row.data();
            jpeg_write_scanlines(&cinfo, &row_ptr, 1);
        }

        jpeg_finish_compress(&cinfo);
    }

private:
    static uint8_t Quantize(float v, float scale)
    {
        int i = static_cast<int>(std::round(v * scale));
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return static_cast<uint8_t>(i);
    }

};

#else // !BURSTMERGE_HAVE_JPEG

class JpegWriter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return (fmt == OutputFormat::JPEG) && (bit_depth <= 8);
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        throw std::runtime_error("JPEG writing not available (libjpeg-turbo not linked)");
    }
};

#endif // BURSTMERGE_HAVE_JPEG

std::unique_ptr<ImageWriter> CreateJpegWriter()
{
    return std::make_unique<JpegWriter>();
}

} // namespace io
} // namespace burstmerge
