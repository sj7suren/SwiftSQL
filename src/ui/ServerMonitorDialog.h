// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ServerMonitorDialog.h — the 服务器监控 (server monitor) tool window.
//
// Opened from 工具 ▸ 服务器监控 for the active connection. Shows every live server
// session in a report-style list (Server / ID / User / Host / DB / Command / Time /
// State / Info) and offers a small toolbar: 刷新 (re-query), 结束进程 (kill the
// selected session), 升序 / 降序 (sort the current column). The per-engine session
// SQL + kill primitive live behind db::IConnection::ListProcesses / KillProcess, so
// this dialog is fully dialect-agnostic — it only renders ProcessInfo rows.
//
// Sessions are queried synchronously on the caller's connection (the same thread that
// owns it) — a process list is a cheap catalog read, and running it on the owning
// thread sidesteps the "connections aren't thread-safe" hazard entirely.
#pragma once

#include "ui/CenteredDialog.h"
#include "db/DbDriver.h"

#include <wx/listctrl.h>
#include <vector>

namespace ui {

class ServerMonitorDialog : public CenteredDialog {
public:
    ServerMonitorDialog(wxWindow* parent, db::IConnection* conn,
                        const wxString& connName);

private:
    void ReloadData();                 // query ListProcesses → rows_, then Populate
    void Populate();                   // fill the list from rows_ in the current order
    void KillSelected();               // confirm + KillProcess the selected row, reload
    void SortBy(int col, bool asc);    // set sort column/direction and repopulate
    wxString Field(const db::ProcessInfo& p, int col) const;  // per-column display text

    db::IConnection*             conn_ = nullptr;
    wxString                     connName_;
    wxListCtrl*                  list_    = nullptr;
    std::vector<db::ProcessInfo> rows_;
    int                          sortCol_ = 6;      // default sort: Time…
    bool                         sortAsc_ = false;  // …descending (longest-lived first)
};

} // namespace ui
