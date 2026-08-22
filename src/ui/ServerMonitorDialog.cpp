// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ServerMonitorDialog.cpp — see ServerMonitorDialog.h for the overview.
#include "ui/ServerMonitorDialog.h"

#include "db/ProcessMonitor.h"   // ListServerProcesses / KillServerProcess

#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>

#include <algorithm>
#include <utility>

#include "ui/FlatControls.h"   // FlatButton toolbar buttons
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/I18n.h"

namespace ui {

namespace {

// The fixed cross-engine column layout. `numeric` columns (ID / Time) sort by value,
// not lexically. Widths are initial hints; the user can drag them.
struct ColDef { const wchar_t* title; int width; bool numeric; };
const ColDef kCols[] = {
    { L"Server",  120, false },
    { L"ID",       70, true  },
    { L"User",    110, false },
    { L"Host",    150, false },
    { L"DB",      120, false },
    { L"Command",  90, false },
    { L"Time",     70, true  },
    { L"State",   100, false },
    { L"Info",    360, false },
};
constexpr int kColCount = static_cast<int>(sizeof(kCols) / sizeof(kCols[0]));

} // namespace

ServerMonitorDialog::ServerMonitorDialog(wxWindow* parent, db::IConnection* conn,
                                         const wxString& connName)
    : CenteredDialog(parent, wxID_ANY,
                     tr(L"服务器监控") + L" — " + connName,
                     wxDefaultPosition, wxSize(1040, 560),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , conn_(conn)
    , connName_(connName)
{
    SetBackgroundColour(theme::kWhite);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- toolbar: 刷新 / 结束进程 / 升序 / 降序 -------------------------------
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto addBtn = [&](icons::Glyph g, const wxString& label, const wxColour& fg,
                      std::function<void()> fn) {
        auto* b = new FlatButton(this, g, label, fg, std::move(fn));
        bar->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    };
    addBtn(icons::Glyph::Refresh,   tr(L"刷新"),     theme::kPrimary,
           [this] { ReloadData(); });
    addBtn(icons::Glyph::Stop,      tr(L"结束进程"), theme::kDotRed,
           [this] { KillSelected(); });
    bar->AddSpacer(8);
    addBtn(icons::Glyph::ArrowUp,   tr(L"升序"),     theme::kText,
           [this] { SortBy(sortCol_, true);  });
    addBtn(icons::Glyph::ArrowDown, tr(L"降序"),     theme::kText,
           [this] { SortBy(sortCol_, false); });
    root->Add(bar, 0, wxEXPAND | wxALL, 8);

    root->Add(new wxStaticLine(this), 0, wxEXPAND);

    // ---- session list ------------------------------------------------------
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    list_->SetBackgroundColour(theme::kWhite);
    for (int c = 0; c < kColCount; ++c)
        list_->InsertColumn(c, kCols[c].title, wxLIST_FORMAT_LEFT, kCols[c].width);
    root->Add(list_, 1, wxEXPAND | wxALL, 8);

    SetSizer(root);

    // A header click sorts by that column (toggling direction if it's already active) —
    // the standard grid gesture, complementing the toolbar's explicit 升序/降序.
    list_->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
        const int c = e.GetColumn();
        SortBy(c, c == sortCol_ ? !sortAsc_ : true);
    });
    // Double-click a row = kill it (with the same confirm as the toolbar button).
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { KillSelected(); });

    ReloadData();
}

wxString ServerMonitorDialog::Field(const db::ProcessInfo& p, int col) const
{
    switch (col) {
        case 0:  return p.server.IsEmpty() ? connName_ : p.server;
        case 1:  return p.id;
        case 2:  return p.user;
        case 3:  return p.host;
        case 4:  return p.db;
        case 5:  return p.command;
        case 6:  return p.time;
        case 7:  return p.state;
        case 8:  return p.info;
        default: return wxString();
    }
}

void ServerMonitorDialog::ReloadData()
{
    if (!conn_) return;
    wxString err;
    std::vector<db::ProcessInfo> ps;
    if (!db::ListServerProcesses(*conn_, ps, err)) {
        wxMessageBox(err, tr(L"服务器监控"), wxOK | wxICON_WARNING, this);
        return;
    }
    rows_ = std::move(ps);
    Populate();
}

void ServerMonitorDialog::Populate()
{
    // Sort rows_ in place so the list row index stays 1:1 with rows_ (KillSelected
    // relies on that mapping). Numeric columns compare by value; the rest fold case.
    const int col = sortCol_;
    const bool numeric = (col >= 0 && col < kColCount) && kCols[col].numeric;
    const bool asc = sortAsc_;
    std::stable_sort(rows_.begin(), rows_.end(),
        [&](const db::ProcessInfo& a, const db::ProcessInfo& b) {
            const wxString fa = Field(a, col), fb = Field(b, col);
            int cmp;
            if (numeric) {
                long long na = 0, nb = 0;
                fa.ToLongLong(&na); fb.ToLongLong(&nb);   // non-numeric → 0
                cmp = (na < nb) ? -1 : (na > nb) ? 1 : 0;
            } else {
                cmp = fa.CmpNoCase(fb);
            }
            return asc ? (cmp < 0) : (cmp > 0);
        });

    list_->DeleteAllItems();
    for (size_t i = 0; i < rows_.size(); ++i) {
        const long idx = list_->InsertItem(static_cast<long>(i), Field(rows_[i], 0));
        for (int c = 1; c < kColCount; ++c)
            list_->SetItem(idx, c, Field(rows_[i], c));
    }
}

void ServerMonitorDialog::KillSelected()
{
    const long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= static_cast<long>(rows_.size())) {
        wxMessageBox(tr(L"请先选择要结束的会话"), tr(L"服务器监控"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const db::ProcessInfo& p = rows_[sel];
    if (wxMessageBox(wxString::Format(tr(L"确定结束会话 %s (%s) 吗？"), p.id, p.user),
                     tr(L"结束进程"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;

    wxString err;
    if (!db::KillServerProcess(*conn_, p.id, err))
        wxMessageBox(err, tr(L"结束进程"), wxOK | wxICON_WARNING, this);
    ReloadData();   // refresh either way — the row may already be gone
}

void ServerMonitorDialog::SortBy(int col, bool asc)
{
    if (col < 0 || col >= kColCount) return;
    sortCol_ = col;
    sortAsc_ = asc;
    Populate();
}

} // namespace ui
