#include "main_frame.h"

#include <wx/app.h>
#include <wx/image.h>

namespace burstmerge::gui
{

class BurstMergeApp final : public wxApp
{
public:
    bool OnInit() override
    {
        SetAppName("BurstMerge GUI");
        wxInitAllImageHandlers();
        MainFrame* frame = new MainFrame;
        frame->Show();
        return true;
    }
};

} // namespace burstmerge::gui

wxIMPLEMENT_APP(burstmerge::gui::BurstMergeApp);
