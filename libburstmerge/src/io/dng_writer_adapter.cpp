#include "burstmerge/internal/io/image_writer.h"
#include "burstmerge/internal/io/dng_io.h"
#include "dng_sdk_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace burstmerge
{
namespace io
{

class DngWriterAdapter : public ImageWriter
{
public:
    bool CanWrite(OutputFormat fmt, uint32_t bit_depth) override
    {
        return fmt == OutputFormat::DNG;
    }

    void Write(const std::string& path,
               const FloatImage& image,
               const WriteParams& params) override
    {
        (void)params;
        // DNG writing must be called via the dedicated RAW path which has
        // access to the original DngNegativeHolder and metadata.
        // This adapter should not be reached directly.
        throw std::runtime_error(
            "DngWriterAdapter: use the dedicated RAW pipeline for DNG output");
    }
};

std::unique_ptr<ImageWriter> CreateDngWriter()
{
    return std::make_unique<DngWriterAdapter>();
}

} // namespace io
} // namespace burstmerge
