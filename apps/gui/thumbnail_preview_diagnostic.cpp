#include "thumbnail_loader.h"

#include <wx/init.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    wxInitializer initializer;
    if (!initializer.IsOk())
    {
        std::cerr << "wxWidgets initialization failed\n";
        return 2;
    }
    wxInitAllImageHandlers();
    if (argc < 2)
    {
        std::cerr << "usage: thumbnail_preview_diagnostic <image-or-RAW> [...]\n";
        return 2;
    }

    bool success = true;
    for (int index = 1; index < argc; ++index)
    {
        const burstmerge::gui::ThumbnailResult thumbnail =
            burstmerge::gui::DecodeThumbnail(argv[index], 64, 64);
        if (thumbnail.rgb.empty())
        {
            std::cerr << argv[index] << ": thumbnail decode failed\n";
            success = false;
            continue;
        }

        std::vector<unsigned char> jpeg;
        burstmerge::gui::EmbeddedJpegPreviewInfo info;
        std::string error;
        if (!burstmerge::gui::ExtractEmbeddedJpegPreview(argv[index], jpeg, info, error))
        {
            std::cout << argv[index] << ": thumbnail=" << thumbnail.width << 'x'
                      << thumbnail.height << '\n';
            continue;
        }
        wxMemoryInputStream stream(jpeg.data(), jpeg.size());
        wxImage image;
        if (!image.LoadFile(stream, wxBITMAP_TYPE_JPEG))
        {
            std::cerr << argv[index] << ": extracted JPEG failed to decode\n";
            success = false;
            continue;
        }
        if (image.GetWidth() != static_cast<int>(info.width) ||
            image.GetHeight() != static_cast<int>(info.height))
        {
            std::cerr << argv[index] << ": decoded JPEG dimensions do not match preview IFD\n";
            success = false;
            continue;
        }
        std::cout << argv[index] << ": IFD=" << info.width << 'x' << info.height
                  << " decoded=" << image.GetWidth() << 'x' << image.GetHeight()
                  << " thumbnail=" << thumbnail.width << 'x' << thumbnail.height
                  << " offset=" << info.offset << " bytes=" << info.bytes << '\n';
    }
    return success ? 0 : 1;
}
