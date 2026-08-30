#include "burstmerge/internal/core/image_resize.h"
#include "burstmerge/internal/core/chroma_effects.h"

#include <wx/wx.h>
#include <wx/statline.h>
#include <wx/spinctrl.h>
#include <wx/filepicker.h>
#include <wx/dnd.h>

#ifdef _WIN32
#include <windows.h>
#include <uxtheme.h>
#endif

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <thread>
#include <atomic>

namespace
{

void ApplySystemThemeToWindow(wxWindow* window,
                              const wxColour& face,
                              const wxColour& window_colour,
                              const wxColour& text)
{
    if (window == nullptr)
    {
        return;
    }

    window->SetForegroundColour(text);
    if (dynamic_cast<wxTextCtrl*>(window) != nullptr ||
        dynamic_cast<wxChoice*>(window) != nullptr ||
        dynamic_cast<wxSpinCtrl*>(window) != nullptr)
    {
        window->SetBackgroundColour(window_colour);
    }
    else
    {
        window->SetBackgroundColour(face);
    }

#ifdef _WIN32
    if (window->GetHandle() != nullptr)
    {
        SetWindowTheme(static_cast<HWND>(window->GetHandle()), L"Explorer", nullptr);
    }
#endif

    for (wxWindow* child : window->GetChildren())
    {
        ApplySystemThemeToWindow(child, face, window_colour, text);
    }
}

class FileDropTarget : public wxFileDropTarget
{
public:
    FileDropTarget(std::function<void(const wxString&)> callback)
        : callback_(callback) {}

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override
    {
        if (!filenames.IsEmpty() && callback_)
        {
            callback_(filenames[0]);
            return true;
        }
        return false;
    }

private:
    std::function<void(const wxString&)> callback_;
};

class RawResizeFrame : public wxFrame
{
public:
    RawResizeFrame()
        : wxFrame(nullptr, wxID_ANY, "BurstMerge RAW Resizer", wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_FRAME_STYLE | wxCLIP_CHILDREN)
    {
        SetSize(FromDIP(wxSize(700, 820)));
        SetMinSize(FromDIP(wxSize(580, 680)));
#ifdef _WIN32
        SetDoubleBuffered(true);
#endif
        InitUI();
        Bind(wxEVT_SYS_COLOUR_CHANGED, &RawResizeFrame::OnSystemColourChanged, this);
        SetDropTarget(new FileDropTarget([this](const wxString& path) { OnFileDropped(path); }));
        ApplySystemTheme();
        Centre();
    }

    ~RawResizeFrame()
    {
        if (worker_thread_.joinable())
        {
            worker_thread_.join();
        }
    }

private:
    wxFilePickerCtrl* input_picker_ = nullptr;
    wxFilePickerCtrl* output_picker_ = nullptr;

    wxRadioButton* rb_scale_ = nullptr;
    wxRadioButton* rb_dim_ = nullptr;
    wxTextCtrl* txt_scale_ = nullptr;
    wxSpinCtrl* spin_width_ = nullptr;
    wxSpinCtrl* spin_height_ = nullptr;

    wxChoice* choice_interp_ = nullptr;
    wxChoice* choice_bit_depth_ = nullptr;
    wxTextCtrl* txt_olpf_ = nullptr;
    wxTextCtrl* txt_dither_ = nullptr;
    wxCheckBox* chk_clear_hints_ = nullptr;

    wxCheckBox* chk_chroma_ = nullptr;
    wxChoice* choice_laca_color_ = nullptr;
    wxTextCtrl* txt_laca_width_ = nullptr;
    wxChoice* choice_loca_color_ = nullptr;
    wxTextCtrl* txt_loca_strength_ = nullptr;
    wxTextCtrl* txt_loca_width_ = nullptr;
    wxTextCtrl* txt_loca_minsensi_ = nullptr;

    wxGauge* progress_bar_ = nullptr;
    wxStaticText* status_lbl_ = nullptr;
    wxButton* btn_process_ = nullptr;

    std::thread worker_thread_;
    std::atomic<bool> is_running_{false};

    void InitUI()
    {
        auto* panel = new wxPanel(this, wxID_ANY);
        auto* main_sizer = new wxBoxSizer(wxVERTICAL);

        // ---- Input / Output ----
        auto* io_box = new wxStaticBoxSizer(wxVERTICAL, panel, "File Selection");
        auto* io_flex = new wxFlexGridSizer(2, 2, FromDIP(8), FromDIP(10));
        io_flex->AddGrowableCol(1, 1);

        io_flex->Add(new wxStaticText(panel, wxID_ANY, "Input RAW/DNG:"), 0, wxALIGN_CENTER_VERTICAL);
        input_picker_ = new wxFilePickerCtrl(panel, wxID_ANY, "", "Select Input RAW/DNG",
                                            "RAW/DNG Files (*.dng;*.arw;*.cr2;*.cr3;*.nef;*.orf;*.rw2)|*.dng;*.arw;*.cr2;*.cr3;*.nef;*.orf;*.rw2|All Files (*.*)|*.*",
                                             wxDefaultPosition, wxDefaultSize, wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
        io_flex->Add(input_picker_, 1, wxEXPAND);

        io_flex->Add(new wxStaticText(panel, wxID_ANY, "Output DNG:"), 0, wxALIGN_CENTER_VERTICAL);
        output_picker_ = new wxFilePickerCtrl(panel, wxID_ANY, "./out.dng", "Save Output DNG",
                                             "DNG Files (*.dng)|*.dng|All Files (*.*)|*.*",
                                             wxDefaultPosition, wxDefaultSize, wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
        io_flex->Add(output_picker_, 1, wxEXPAND);
        io_box->Add(io_flex, 1, wxEXPAND | wxALL, FromDIP(5));
        main_sizer->Add(io_box, 0, wxEXPAND | wxALL, FromDIP(8));

        // ---- Resize Dimension / Scale ----
        auto* size_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Resize Dimensions");
        auto* sizer_scale_row = new wxBoxSizer(wxHORIZONTAL);
        rb_scale_ = new wxRadioButton(panel, wxID_ANY, "Uniform Scale Factor:", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        txt_scale_ = new wxTextCtrl(panel, wxID_ANY, "0.5", wxDefaultPosition, wxSize(70, -1));
        sizer_scale_row->Add(rb_scale_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        sizer_scale_row->Add(txt_scale_, 0, wxALIGN_CENTER_VERTICAL);
        size_box->Add(sizer_scale_row, 0, wxEXPAND | wxALL, FromDIP(5));

        auto* sizer_dim_row = new wxBoxSizer(wxHORIZONTAL);
        rb_dim_ = new wxRadioButton(panel, wxID_ANY, "Exact Dimensions (px):");
        rb_dim_->SetValue(true);
        spin_width_ = new wxSpinCtrl(panel, wxID_ANY, "720", wxDefaultPosition, FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS, 2, 65534, 720);
        spin_height_ = new wxSpinCtrl(panel, wxID_ANY, "480", wxDefaultPosition, FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS, 2, 65534, 480);
        sizer_dim_row->Add(rb_dim_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        sizer_dim_row->Add(new wxStaticText(panel, wxID_ANY, "W:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        sizer_dim_row->Add(spin_width_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(15));
        sizer_dim_row->Add(new wxStaticText(panel, wxID_ANY, "H:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        sizer_dim_row->Add(spin_height_, 0, wxALIGN_CENTER_VERTICAL);
        size_box->Add(sizer_dim_row, 0, wxEXPAND | wxALL, FromDIP(5));
        main_sizer->Add(size_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        // ---- Processing & Algorithm Options ----
        auto* algo_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Algorithm Settings");
        auto* algo_grid = new wxFlexGridSizer(3, 4, FromDIP(8), FromDIP(10));
        algo_grid->AddGrowableCol(1, 1);
        algo_grid->AddGrowableCol(3, 1);

        algo_grid->Add(new wxStaticText(panel, wxID_ANY, "Interpolation:"), 0, wxALIGN_CENTER_VERTICAL);
        wxArrayString interp_choices;
        interp_choices.Add("Bicubic");
        interp_choices.Add("Bilinear");
        interp_choices.Add("Area Average (Recommended for heavy downscale)");
        interp_choices.Add("Gaussian Area (>=3x downscale)");
        interp_choices.Add("50% Half Sample (Default, >=2x downscale)");
        choice_interp_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, interp_choices);
        choice_interp_->SetSelection(4);
        algo_grid->Add(choice_interp_, 1, wxEXPAND);

        algo_grid->Add(new wxStaticText(panel, wxID_ANY, "Bit Depth:"), 0, wxALIGN_CENTER_VERTICAL);
        wxArrayString bit_choices;
        bit_choices.Add("16 bit (Default)");
        bit_choices.Add("14 bit");
        bit_choices.Add("12 bit");
        bit_choices.Add("10 bit");
        bit_choices.Add("8 bit");
        choice_bit_depth_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, bit_choices);
        choice_bit_depth_->SetSelection(0);
        algo_grid->Add(choice_bit_depth_, 1, wxEXPAND);

        algo_grid->Add(new wxStaticText(panel, wxID_ANY, "Pseudo-OLPF:"), 0, wxALIGN_CENTER_VERTICAL);
        txt_olpf_ = new wxTextCtrl(panel, wxID_ANY, "0.0");
        algo_grid->Add(txt_olpf_, 1, wxEXPAND);

        algo_grid->Add(new wxStaticText(panel, wxID_ANY, "TPDF Dither (LSB):"), 0, wxALIGN_CENTER_VERTICAL);
        txt_dither_ = new wxTextCtrl(panel, wxID_ANY, "0.0");
        algo_grid->Add(txt_dither_, 1, wxEXPAND);

        algo_box->Add(algo_grid, 1, wxEXPAND | wxALL, FromDIP(5));

        chk_clear_hints_ = new wxCheckBox(panel, wxID_ANY, "Clear Camera Metadata Hints (Anti-alias, noise profile, sharpness hints)");
        chk_clear_hints_->SetValue(true);
        algo_box->Add(chk_clear_hints_, 0, wxALL, FromDIP(5));
        main_sizer->Add(algo_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        // ---- Chromatic Aberration Lo-Fi Effects ----
        auto* chroma_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Chromatic Aberration (Lo-Fi Effects)");
        chk_chroma_ = new wxCheckBox(panel, wxID_ANY, "Enable Chromatic Aberration Effects");
        chroma_box->Add(chk_chroma_, 0, wxALL, FromDIP(5));

        wxArrayString color_choices;
        color_choices.Add("Red");
        color_choices.Add("Green");
        color_choices.Add("Blue");
        color_choices.Add("Cyan");
        color_choices.Add("Magenta");
        color_choices.Add("Yellow");

        auto* ca_grid = new wxFlexGridSizer(3, 4, FromDIP(6), FromDIP(10));
        ca_grid->AddGrowableCol(1, 1);
        ca_grid->AddGrowableCol(3, 1);

        ca_grid->Add(new wxStaticText(panel, wxID_ANY, "LaCA Color:"), 0, wxALIGN_CENTER_VERTICAL);
        choice_laca_color_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, color_choices);
        choice_laca_color_->SetSelection(0); // Red
        ca_grid->Add(choice_laca_color_, 1, wxEXPAND);

        ca_grid->Add(new wxStaticText(panel, wxID_ANY, "LaCA Width (%):"), 0, wxALIGN_CENTER_VERTICAL);
        txt_laca_width_ = new wxTextCtrl(panel, wxID_ANY, "0.0");
        ca_grid->Add(txt_laca_width_, 1, wxEXPAND);

        ca_grid->Add(new wxStaticText(panel, wxID_ANY, "LoCA Color:"), 0, wxALIGN_CENTER_VERTICAL);
        choice_loca_color_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, color_choices);
        choice_loca_color_->SetSelection(4); // Magenta
        ca_grid->Add(choice_loca_color_, 1, wxEXPAND);

        ca_grid->Add(new wxStaticText(panel, wxID_ANY, "LoCA Strength:"), 0, wxALIGN_CENTER_VERTICAL);
        txt_loca_strength_ = new wxTextCtrl(panel, wxID_ANY, "0.2");
        ca_grid->Add(txt_loca_strength_, 1, wxEXPAND);

        ca_grid->Add(new wxStaticText(panel, wxID_ANY, "LoCA Width (%):"), 0, wxALIGN_CENTER_VERTICAL);
        txt_loca_width_ = new wxTextCtrl(panel, wxID_ANY, "0.05");
        ca_grid->Add(txt_loca_width_, 1, wxEXPAND);

        ca_grid->Add(new wxStaticText(panel, wxID_ANY, "LoCA Min Sensi:"), 0, wxALIGN_CENTER_VERTICAL);
        txt_loca_minsensi_ = new wxTextCtrl(panel, wxID_ANY, "0.08");
        ca_grid->Add(txt_loca_minsensi_, 1, wxEXPAND);

        chroma_box->Add(ca_grid, 1, wxEXPAND | wxALL, FromDIP(5));
        main_sizer->Add(chroma_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        // ---- Progress & Action ----
        main_sizer->AddStretchSpacer(1);
        progress_bar_ = new wxGauge(panel, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(-1, 18)));
        main_sizer->Add(progress_bar_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

        status_lbl_ = new wxStaticText(panel, wxID_ANY, "Ready");
        main_sizer->Add(status_lbl_, 0, wxEXPAND | wxALL, FromDIP(8));

        btn_process_ = new wxButton(panel, wxID_ANY, "Start Resize", wxDefaultPosition, FromDIP(wxSize(-1, 36)));
        btn_process_->SetFont(btn_process_->GetFont().Bold());
        btn_process_->Bind(wxEVT_BUTTON, &RawResizeFrame::OnStartProcess, this);
        main_sizer->Add(btn_process_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        panel->SetSizer(main_sizer);
        main_sizer->Fit(this);
    }

    void ApplySystemTheme()
    {
        const wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
        const wxColour window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);

        SetBackgroundColour(face);
        for (wxWindow* child : GetChildren())
        {
            ApplySystemThemeToWindow(child, face, window, text);
        }
        Refresh();
    }

    void OnSystemColourChanged(wxSysColourChangedEvent& event)
    {
        ApplySystemTheme();
        event.Skip();
    }

    void OnFileDropped(const wxString& path)
    {
        input_picker_->SetPath(path);
        wxFileName fn(path);
        fn.SetName(fn.GetName() + "_resized");
        fn.SetExt("dng");
        output_picker_->SetPath(fn.GetFullPath());
    }

    burstmerge::LaCAColor GetSelectedLaCAColor()
    {
        switch (choice_laca_color_->GetSelection())
        {
            case 0: return burstmerge::LaCAColor_Red;
            case 1: return burstmerge::LaCAColor_Green;
            case 2: return burstmerge::LaCAColor_Blue;
            case 3: return burstmerge::LaCAColor_Cyan;
            case 4: return burstmerge::LaCAColor_Magenta;
            case 5: return burstmerge::LaCAColor_Yellow;
            default: return burstmerge::LaCAColor_Red;
        }
    }

    burstmerge::LoCAColor GetSelectedLoCAColor()
    {
        switch (choice_loca_color_->GetSelection())
        {
            case 0: return burstmerge::LoCAColor_Red;
            case 1: return burstmerge::LoCAColor_Green;
            case 2: return burstmerge::LoCAColor_Blue;
            case 3: return burstmerge::LoCAColor_Cyan;
            case 4: return burstmerge::LoCAColor_Magenta;
            case 5: return burstmerge::LoCAColor_Yellow;
            default: return burstmerge::LoCAColor_Magenta;
        }
    }

    burstmerge::InterpolationMethod GetSelectedInterp()
    {
        switch (choice_interp_->GetSelection())
        {
            case 0: return burstmerge::InterpolationMethod::Bicubic;
            case 1: return burstmerge::InterpolationMethod::Bilinear;
            case 2: return burstmerge::InterpolationMethod::AreaAverage;
            case 3: return burstmerge::InterpolationMethod::GaussianArea;
            case 4: return burstmerge::InterpolationMethod::HalfSample;
            default: return burstmerge::InterpolationMethod::Bicubic;
        }
    }

    int GetSelectedBitDepth()
    {
        switch (choice_bit_depth_->GetSelection())
        {
            case 0: return 16;
            case 1: return 14;
            case 2: return 12;
            case 3: return 10;
            case 4: return 8;
            default: return 16;
        }
    }

    void OnStartProcess(wxCommandEvent&)
    {
        if (is_running_) return;

        wxString input_path = input_picker_->GetPath();
        wxString output_path = output_picker_->GetPath();

        if (input_path.IsEmpty() || !wxFileExists(input_path))
        {
            wxMessageBox("Please choose a valid input RAW/DNG file.", "Input Missing", wxOK | wxICON_WARNING, this);
            return;
        }

        if (output_path.IsEmpty())
        {
            wxMessageBox("Please choose a valid output destination.", "Output Missing", wxOK | wxICON_WARNING, this);
            return;
        }

        burstmerge::RawResizeOptions opts;
        if (rb_scale_->GetValue())
        {
            double scale_val = 0.5;
            txt_scale_->GetValue().ToDouble(&scale_val);
            if (scale_val <= 0.0)
            {
                wxMessageBox("Scale factor must be > 0.", "Invalid Scale", wxOK | wxICON_ERROR, this);
                return;
            }
            opts.scale = scale_val;
        }
        else
        {
            opts.width = static_cast<uint32_t>(spin_width_->GetValue());
            opts.height = static_cast<uint32_t>(spin_height_->GetValue());
        }

        opts.interp = GetSelectedInterp();
        opts.bit_depth = GetSelectedBitDepth();

        double olpf = 0.0;
        txt_olpf_->GetValue().ToDouble(&olpf);
        opts.pseudo_olpf = static_cast<float>(olpf);

        double dither = 0.0;
        txt_dither_->GetValue().ToDouble(&dither);
        opts.dither = static_cast<float>(dither);

        opts.clear_camera_hints = chk_clear_hints_->GetValue();

        opts.chroma.enabled = chk_chroma_->GetValue();
        opts.chroma.laca_color = GetSelectedLaCAColor();
        double laca_w = 0.0;
        txt_laca_width_->GetValue().ToDouble(&laca_w);
        opts.chroma.laca_width = static_cast<float>(laca_w);

        opts.chroma.loca_color = GetSelectedLoCAColor();
        double loca_s = 0.2, loca_w = 0.05, loca_m = 0.08;
        txt_loca_strength_->GetValue().ToDouble(&loca_s);
        txt_loca_width_->GetValue().ToDouble(&loca_w);
        txt_loca_minsensi_->GetValue().ToDouble(&loca_m);
        opts.chroma.loca_strength = static_cast<float>(loca_s);
        opts.chroma.loca_width = static_cast<float>(loca_w);
        opts.chroma.loca_min_sensi = static_cast<float>(loca_m);

        is_running_ = true;
        btn_process_->Enable(false);
        progress_bar_->SetValue(0);
        status_lbl_->SetLabel("Starting process...");

        std::string in_str = input_path.ToStdString();
        std::string out_str = output_path.ToStdString();

        if (worker_thread_.joinable()) worker_thread_.join();

        worker_thread_ = std::thread([this, in_str, out_str, opts]()
        {
            auto cb = [this](float p, const std::string& status)
            {
                wxTheApp->CallAfter([this, p, status]()
                {
                    progress_bar_->SetValue(static_cast<int>(p * 100.0f));
                    status_lbl_->SetLabel(status);
                });
            };

            burstmerge::RawResizeResult res = burstmerge::ProcessRawResize(in_str, out_str, opts, cb);

            wxTheApp->CallAfter([this, res]()
            {
                is_running_ = false;
                btn_process_->Enable(true);
                if (res.success)
                {
                    progress_bar_->SetValue(100);
                    wxString msg = wxString::Format("Successfully resized RAW!\nOutput: %u x %u", res.dst_width, res.dst_height);
                    status_lbl_->SetLabel("Completed successfully.");
                    wxMessageBox(msg, "Resize Succeeded", wxOK | wxICON_INFORMATION, this);
                }
                else
                {
                    status_lbl_->SetLabel("Error: " + res.error_msg);
                    wxMessageBox("Failed to resize RAW:\n" + res.error_msg, "Error", wxOK | wxICON_ERROR, this);
                }
            });
        });
    }
};

class RawResizeApp : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit()) return false;
        auto* frame = new RawResizeFrame();
        frame->Show(true);
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(RawResizeApp);
