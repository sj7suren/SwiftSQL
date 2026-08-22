// SqlErrorDialog.cpp — see header.
#include "ui/SqlErrorDialog.h"

#include <wx/wx.h>
#include <wx/clipbrd.h>

#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

SqlErrorDialog::SqlErrorDialog(wxWindow* parent, const wxString& title,
                               const wxString& message)
    : CenteredDialog(parent, wxID_ANY, title, wxDefaultPosition,
                     wxSize(560, 360),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY, tr(L"错误信息"));
    header->SetForegroundColour(theme::kDiffDelFg);
    wxFont hf = header->GetFont(); hf.MakeBold(); header->SetFont(hf);
    root->Add(header, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* text = new wxTextCtrl(this, wxID_ANY, message, wxDefaultPosition,
                                wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    text->SetFont(Mono(10));
    text->SetForegroundColour(theme::kText);
    root->Add(text, 1, wxEXPAND | wxALL, 12);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    auto* copy = new wxButton(this, wxID_ANY, tr(L"复制"));
    copy->Bind(wxEVT_BUTTON, [message](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(message));
            wxTheClipboard->Close();
        }
    });
    btnRow->Add(copy, 0);
    btnRow->AddStretchSpacer(1);
    auto* close = new wxButton(this, wxID_OK, tr(L"关闭"));
    btnRow->Add(close, 0);
    root->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    SetSizer(root);
    close->SetFocus();
}

void ShowSqlError(wxWindow* parent, const wxString& title, const wxString& message)
{
    SqlErrorDialog dlg(parent, title, message);
    dlg.ShowModal();
}

} // namespace ui
