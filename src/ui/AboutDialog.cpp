// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AboutDialog.cpp — see AboutDialog.h. Built from wxDialog + vertical sizers,
// white background, ui::Ui() fonts, in the CrashDialog visual style. The donate
// path opens a small secondary DonateDialog showing the
// Alipay QR loaded from the RC PNG resource, with a text fallback if the bitmap
// fails to load (never a blank image, never a crash).
#include "ui/AboutDialog.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/button.h>
#include <wx/bitmap.h>
#include <wx/statbox.h>
#include <wx/hyperlink.h>
#include <wx/mstream.h>
#include <wx/utils.h>
#include <wx/image.h>

#include "core/AppVersion.h"
#include "ui/IconFactory.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {

// Load the embedded Alipay QR (RC: `alipay_qr PNG "assets\\alipay_qr.png"`) and
// scale it to `targetW` keeping aspect ratio. Returns a not-Ok bitmap if the
// resource / PNG handler is missing — callers must check IsOk() and degrade to a
// text notice rather than showing an empty image.
wxBitmap LoadAlipayQr(int targetW)
{
    // Read the raw bytes of the embedded RC resource (type "PNG", name
    // "alipay_qr") and decode with the PNG handler. This is more reliable across
    // wx versions than wxBITMAP_TYPE_PNG_RESOURCE, which mishandles custom PNG
    // resource types on some builds (→ silent not-Ok bitmap, "load failed").
    const void* data = nullptr;
    size_t len = 0;
    if (!wxLoadUserResource(&data, &len, L"alipay_qr", L"PNG") || !data || len == 0)
        return wxBitmap();
    wxMemoryInputStream mis(data, len);
    wxImage img(mis, wxBITMAP_TYPE_PNG);
    if (!img.IsOk() || img.GetWidth() <= 0)
        return wxBitmap();
    const int w = img.GetWidth(), h = img.GetHeight();
    const int targetH = static_cast<int>(static_cast<double>(h) * targetW / w + 0.5);
    img.Rescale(targetW, targetH, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(img);
}

// Secondary dialog: Alipay donation. Kept as its own modal (simplest, most
// stable choice — no dynamic relayout/resize of the About dialog, no shared
// mutable state) so the About dialog stays a plain, predictable card.
class DonateDialog : public wxDialog {
public:
    explicit DonateDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, tr(L"捐赠支持"),
                   wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        SetBackgroundColour(theme::kWhite);
        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- QR code (or graceful text fallback) ----
        const wxBitmap qr = LoadAlipayQr(220);
        if (qr.IsOk()) {
            auto* pic = new wxStaticBitmap(this, wxID_ANY, qr);
            root->Add(pic, 0, wxALIGN_CENTRE | wxTOP, 24);
        } else {
            auto* fail = new wxStaticText(this, wxID_ANY, tr(L"收款码加载失败"));
            fail->SetFont(Ui(10));
            fail->SetForegroundColour(theme::kTextMuted);
            root->Add(fail, 0, wxALIGN_CENTRE | wxTOP, 40);
            root->AddSpacer(40);
        }

        // ---- guidance line ----
        auto* guide = new wxStaticText(this, wxID_ANY,
            tr(L"打开支付宝「扫一扫」，支持开发者 ☕"));
        guide->SetFont(Ui(10));
        guide->SetForegroundColour(theme::kTextStrong);
        root->Add(guide, 0, wxALIGN_CENTRE | wxTOP, 14);

        // ---- suggested amounts (plain text, no payment logic) ----
        auto* amounts = new wxStaticText(this, wxID_ANY, L"¥6    ¥18    ¥66    " + tr(L"随心"));
        amounts->SetFont(Ui(11, /*bold*/ true));
        amounts->SetForegroundColour(theme::kPrimary);
        root->Add(amounts, 0, wxALIGN_CENTRE | wxTOP, 10);

        // ---- thanks ----
        auto* thanks = new wxStaticText(this, wxID_ANY,
            tr(L"感谢每一位支持者 ❤️ —— ") + L"SwiftSQL Contributors");
        thanks->SetFont(Ui(9));
        thanks->SetForegroundColour(theme::kTextSecondary);
        root->Add(thanks, 0, wxALIGN_CENTRE | wxTOP, 16);

        // ---- close button ----
        auto* closeBtn = new wxButton(this, wxID_OK, tr(L"关闭"));
        closeBtn->SetDefault();
        root->Add(closeBtn, 0, wxALIGN_CENTRE | wxTOP | wxBOTTOM, 20);

        auto* pad = new wxBoxSizer(wxHORIZONTAL);
        pad->Add(root, 1, wxLEFT | wxRIGHT | wxEXPAND, 28);
        SetSizerAndFit(pad);
        CentreOnParent();
    }
};

} // namespace

AboutDialog::AboutDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, tr(L"关于 SwiftSQL"),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(theme::kWhite);
    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- logo (centred) ----
    auto* logo = new wxStaticBitmap(this, wxID_ANY, icons::AppLogo(64));
    root->Add(logo, 0, wxALIGN_CENTRE | wxTOP, 26);

    // ---- title: SwiftSQL <version> ----
    auto* title = new wxStaticText(this, wxID_ANY,
        wxString(L"SwiftSQL  ") + core::kAppVersionText);
    title->SetFont(Ui(13, /*bold*/ true));
    title->SetForegroundColour(theme::kTextStrong);
    root->Add(title, 0, wxALIGN_CENTRE | wxTOP, 14);

    // ---- description (original tagline, tr'd) ----
    auto* desc = new wxStaticText(this, wxID_ANY,
        tr(L"轻量、跨平台的 SQL 客户端\n"
           L"支持 MySQL · PostgreSQL · OceanBase\n\n"
           L"Query fast. Ship faster."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    desc->SetFont(Ui(9.5));
    desc->SetForegroundColour(theme::kTextSecondary);
    root->Add(desc, 0, wxALIGN_CENTRE | wxTOP, 12);

    // ---- copyright / licence ----
    auto* copy = new wxStaticText(this, wxID_ANY,
        wxString(core::kCopyrightText) + L"\n" + core::kLicenseText,
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    copy->SetFont(Ui(8.5));
    copy->SetForegroundColour(theme::kTextMuted);
    root->Add(copy, 0, wxALIGN_CENTRE | wxTOP, 18);

    // ---- source code (GPLv3 s.6) ----
    // A GPL binary has to come with its source, or with an offer telling the
    // holder where to get it. The installer's licence page says so, but whoever
    // is running the program may never have seen that page -- this is the copy
    // that travels with the application itself. Clickable on purpose: an offer
    // nobody can act on is not much of an offer.
    auto* src = new wxHyperlinkCtrl(this, wxID_ANY,
        tr(L"源代码：") + core::kSourceUrl, core::kSourceUrl,
        wxDefaultPosition, wxDefaultSize, wxHL_ALIGN_CENTRE | wxNO_BORDER);
    src->SetFont(Ui(8.5));
    src->SetNormalColour(theme::kPrimary);
    src->SetHoverColour(theme::kPrimary);
    src->SetVisitedColour(theme::kPrimary);
    src->SetBackgroundColour(GetBackgroundColour());
    root->Add(src, 0, wxALIGN_CENTRE | wxTOP, 6);

    // ---- QQ community group ----
    auto* qq = new wxStaticText(this, wxID_ANY,
        tr(L"QQ 交流群：") + L"1058103238",
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    qq->SetFont(Ui(9));
    qq->SetForegroundColour(theme::kPrimary);
    root->Add(qq, 0, wxALIGN_CENTRE | wxTOP, 12);

    // ---- buttons ----
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto* donateBtn = new wxButton(this, wxID_ANY, tr(L"☕ 捐赠支持"));
    donateBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // Close the About dialog when opening Donate (don't stack them):
        // hide About, show Donate parented to the main window, then end
        // About's modal loop once Donate is dismissed.
        Hide();
        DonateDialog dlg(GetParent());
        dlg.ShowModal();
        EndModal(wxID_OK);
    });
    btns->Add(donateBtn, 0, wxRIGHT, 10);
    auto* okBtn = new wxButton(this, wxID_OK, tr(L"确定"));
    okBtn->SetDefault();
    btns->Add(okBtn, 0);
    root->Add(btns, 0, wxALIGN_CENTRE | wxTOP | wxBOTTOM, 22);

    auto* pad = new wxBoxSizer(wxHORIZONTAL);
    pad->Add(root, 1, wxLEFT | wxRIGHT | wxEXPAND, 36);
    SetSizerAndFit(pad);
    SetMinSize(wxSize(380, -1));
    Fit();
    CentreOnParent();
}

} // namespace ui
