// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/CrashDialog.h"

#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/textctrl.h>
#include <wx/collpane.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include "core/CrashLog.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace ui {
namespace {

// Self-drawn warning badge (zero-bitmap, per CHARTER): a filled rounded disc with
// a centred "!". Amber for recoverable, red for fatal.
class WarnBadge : public wxPanel {
public:
    WarnBadge(wxWindow* parent, bool fatal)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(44, 44)), fatal_(fatal)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &WarnBadge::OnPaint, this);
    }
private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
        if (!gc) return;
        const wxColour fill = fatal_ ? theme::kDotRed : theme::kDotAmber;
        gc->SetBrush(wxBrush(fill));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawEllipse(2, 2, 40, 40);
        gc->SetFont(wxFontInfo(20).Bold(), theme::kWhite);
        double tw = 0, th = 0, dsc = 0, lead = 0;
        gc->GetTextExtent(L"!", &tw, &th, &dsc, &lead);
        gc->DrawText(L"!", (44 - tw) / 2, (44 - th) / 2);
        delete gc;
    }
    bool fatal_;
};

class CrashDialog : public wxDialog {
public:
    CrashDialog(bool fatal, const wxString& context, const wxString& details)
        : wxDialog(nullptr, wxID_ANY, L"SwiftSQL",
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE)
    {
        SetBackgroundColour(theme::kWhite);
        const wxString logPath = core::CrashLog::FilePath();

        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- headline row: badge + friendly copy ----
        auto* head = new wxBoxSizer(wxHORIZONTAL);
        head->Add(new WarnBadge(this, fatal), 0, wxRIGHT | wxTOP, 4);
        head->AddSpacer(14);

        auto* copy = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY,
            fatal ? tr(L"SwiftSQL 需要关闭") : tr(L"程序遇到一个问题"));
        wxFont tf = title->GetFont(); tf.SetPointSize(tf.GetPointSize() + 4); tf.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(tf);
        title->SetForegroundColour(theme::kTextStrong);

        auto* sub = new wxStaticText(this, wxID_ANY,
            fatal ? tr(L"程序遇到了一个严重错误，需要关闭。\n"
                       L"诊断信息已经保存下来，方便定位问题。")
                  : tr(L"别担心 —— SwiftSQL 已经拦下了这个错误，你的数据没有丢失。\n"
                       L"你可以继续使用；如果问题反复出现，请把下面的诊断信息发给我们。"));
        sub->SetForegroundColour(theme::kTextSecondary);
        sub->Wrap(400);

        copy->Add(title, 0, wxBOTTOM, 8);
        copy->Add(sub, 0);
        head->Add(copy, 1, wxEXPAND);
        root->Add(head, 0, wxALL | wxEXPAND, 20);

        // ---- always-visible diagnostics path ----
        auto* pathLine = new wxStaticText(this, wxID_ANY,
            tr(L"诊断日志：") + logPath);
        pathLine->SetForegroundColour(theme::kTextMuted);
        root->Add(pathLine, 0, wxLEFT | wxRIGHT | wxBOTTOM, 20);

        // ---- collapsible technical details (folded by default) ----
        auto* pane = new wxCollapsiblePane(this, wxID_ANY, tr(L"技术详情"));
        wxWindow* pw = pane->GetPane();
        auto* ps = new wxBoxSizer(wxVERTICAL);
        detailText_ =
            (!context.IsEmpty() ? tr(L"位置: ") + context + L"\n" : wxString()) +
            (!details.IsEmpty() ? details + L"\n" : wxString()) +
            tr(L"日志: ") + logPath;
        auto* box = new wxTextCtrl(pw, wxID_ANY, detailText_, wxDefaultPosition,
                                   wxSize(440, 120),
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        box->SetBackgroundColour(theme::kMenuBg);
        ps->Add(box, 1, wxEXPAND | wxTOP, 6);
        pw->SetSizer(ps);
        root->Add(pane, 0, wxLEFT | wxRIGHT | wxEXPAND, 20);
        pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
                   [this](wxCollapsiblePaneEvent&) { GetSizer()->Layout(); Fit(); });

        // ---- action buttons ----
        auto* btns = new wxBoxSizer(wxHORIZONTAL);
        auto* openBtn = new wxButton(this, wxID_ANY, tr(L"打开日志文件夹"));
        auto* copyBtn = new wxButton(this, wxID_ANY, tr(L"复制详情"));
        auto* okBtn   = new wxButton(this, wxID_OK,
                                     fatal ? tr(L"关闭程序") : tr(L"继续使用"));
        okBtn->SetDefault();

        openBtn->Bind(wxEVT_BUTTON, [logPath](wxCommandEvent&) {
            wxLaunchDefaultApplication(wxFileName(logPath).GetPath());
        });
        copyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(detailText_));
                wxTheClipboard->Close();
            }
        });

        btns->Add(openBtn, 0, wxRIGHT, 8);
        btns->Add(copyBtn, 0, wxRIGHT, 8);
        btns->AddStretchSpacer(1);
        btns->Add(okBtn, 0);
        root->Add(btns, 0, wxALL | wxEXPAND, 20);

        SetSizerAndFit(root);
        SetMinSize(wxSize(500, -1));
        Fit();
        CentreOnScreen();
    }
private:
    wxString detailText_;
};

}  // namespace

void ShowCrashDialog(bool fatal, const wxString& context, const wxString& details)
{
    // The dialog itself must never bring down the app — if wx is too far gone to
    // build it, we've already logged; just swallow.
    try {
        CrashDialog dlg(fatal, context, details);
        dlg.ShowModal();
    } catch (...) {}
}

}  // namespace ui
