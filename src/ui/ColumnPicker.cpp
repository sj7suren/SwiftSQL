// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ColumnPicker.cpp — see header. A small modal wxDialog: a search box + a
// live-filtered column list. Mouse (single-click highlights, double-click / 确定
// picks) and keyboard (type to filter, ↑↓ to move, Enter to pick, Esc to cancel)
// all work reliably — unlike the previous wxPopupTransientWindow, whose mouse
// capture ate clicks meant for the inner list.
#include "ui/ColumnPicker.h"

#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/listbox.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/utils.h>    // wxBell

#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

ColumnPicker::ColumnPicker(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, tr(L"选择列"), wxDefaultPosition, wxDefaultSize,
               wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(theme::kWhite);
    auto* v = new wxBoxSizer(wxVERTICAL);

    search_ = new wxSearchCtrl(this, wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    search_->SetFont(ui::Ui(9));
    search_->ShowCancelButton(false);
    search_->Bind(wxEVT_TEXT,       [this](wxCommandEvent&) { Rebuild(search_->GetValue()); });
    search_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Confirm(); });
    v->Add(search_, 0, wxEXPAND | wxALL, 6);

    list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(240, 220),
                          0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);
    list_->SetFont(ui::Ui(9));
    // Double-click picks; single click just highlights (the native list behaviour).
    list_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { Confirm(); });
    v->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);

    auto* hint = new wxStaticText(this, wxID_ANY, tr(L"双击或回车选择 · Esc 取消"));
    hint->SetFont(ui::Mono(8));
    hint->SetForegroundColour(theme::kTextMuted);
    v->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, 6);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* cancel = new wxButton(this, wxID_CANCEL, tr(L"取消"));
    auto* ok     = new wxButton(this, wxID_ANY,    tr(L"确定"));
    ok->SetDefault();
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Confirm(); });
    btns->Add(cancel, 0, wxRIGHT, 8);
    btns->Add(ok, 0);
    v->Add(btns, 0, wxEXPAND | wxALL, 8);

    SetSizerAndFit(v);
    SetSize(wxSize(280, 360));

    // Keyboard nav regardless of whether the search box or the list owns focus.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        switch (e.GetKeyCode()) {
        case WXK_DOWN:                            MoveSelection(+1); return;
        case WXK_UP:                              MoveSelection(-1); return;
        case WXK_RETURN: case WXK_NUMPAD_ENTER:   Confirm();         return;
        default:                                  e.Skip();          return;   // Esc → wxID_CANCEL
        }
    });
}

void ColumnPicker::PopupAt(wxWindow* anchor, const std::vector<wxString>& cols,
                           const std::vector<int>& disabled,
                           std::function<void(int)> onPick)
{
    cols_   = cols;
    onPick_ = std::move(onPick);
    disabled_.assign(cols_.size(), false);
    for (int d : disabled)
        if (d >= 0 && d < static_cast<int>(disabled_.size())) disabled_[d] = true;

    search_->ChangeValue(wxEmptyString);
    Rebuild(wxEmptyString);
    chosen_ = -1;

    // Drop under the anchor button (clamped implicitly by the WM).
    if (anchor) {
        wxPoint pos = anchor->GetScreenPosition();
        pos.y += anchor->GetSize().GetHeight();
        SetPosition(pos);
    }
    CallAfter([this]() { if (search_) search_->SetFocus(); });   // focus once shown

    if (ShowModal() == wxID_OK && chosen_ >= 0 && onPick_)
        onPick_(chosen_);
}

void ColumnPicker::Rebuild(const wxString& filter)
{
    list_->Clear();
    shown_.clear();
    const wxString f = filter.Lower();
    for (size_t i = 0; i < cols_.size(); ++i) {
        if (!f.IsEmpty() && !cols_[i].Lower().Contains(f)) continue;
        wxString label = cols_[i];
        if (disabled_[i]) label += L"   " + tr(L"（已用）");
        list_->Append(label);
        shown_.push_back(disabled_[i] ? -1 : static_cast<int>(i));
    }
    // Select the first pickable row so Enter / 确定 works without a manual click.
    for (size_t r = 0; r < shown_.size(); ++r)
        if (shown_[r] >= 0) { list_->SetSelection(static_cast<int>(r)); break; }
}

void ColumnPicker::MoveSelection(int delta)
{
    const int n = static_cast<int>(list_->GetCount());
    if (n == 0) return;
    int sel = list_->GetSelection();
    if (sel == wxNOT_FOUND) sel = 0;
    sel = ((sel + delta) % n + n) % n;
    list_->SetSelection(sel);
}

void ColumnPicker::Confirm()
{
    const int sel = list_->GetSelection();
    if (sel == wxNOT_FOUND || sel >= static_cast<int>(shown_.size())) return;  // nothing → stay
    const int col = shown_[sel];
    if (col < 0) { wxBell(); return; }        // an already-used column: not pickable
    chosen_ = col;
    EndModal(wxID_OK);
}

} // namespace ui
