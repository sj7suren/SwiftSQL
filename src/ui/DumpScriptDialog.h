// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DumpScriptDialog.h — export a database's schema (+ optional data) to a .sql
// file with a live progress UI, so a large dump can't freeze the window.
//
// The whole dump — per-table CREATE DDL, streamed INSERT rows, then views /
// routines / triggers — runs on a worker thread (all IConnection calls and file
// I/O are blocking). Progress is marshalled back to the GUI thread with
// CallAfter: a gauge over tables, the current table name, and a throttled
// row counter. 停止 flips an atomic the worker checks between tables (and calls
// IConnection::Cancel() to interrupt an in-flight fetch). The serialization
// format is identical to the old synchronous dump, so the dump→import loop with
// RunScriptDialog stays closed.
#pragma once

#include <wx/string.h>
#include <atomic>
#include <thread>
#include <vector>
#include "db/DbDriver.h"
#include "ui/CenteredDialog.h"

class wxGauge;
class wxStaticText;
class wxTextCtrl;
class wxButton;

namespace ui {

class DumpScriptDialog : public CenteredDialog {
public:
    // `conn` must outlive the dialog. `tables` is the pre-listed table set (the
    // caller already resolved it, so the dialog doesn't re-query). `path` is the
    // chosen output file; `withData` toggles INSERT emission.
    DumpScriptDialog(wxWindow* parent, db::IConnection* conn,
                     const wxString& connName, const wxString& engineName,
                     const wxString& db, bool withData,
                     std::vector<db::TableInfo> tables, const wxString& path);
    ~DumpScriptDialog() override;

    bool     Succeeded() const { return succeeded_; }
    wxString Summary() const   { return summary_; }

private:
    wxWindow* BuildUi();
    void      StartWorker();
    void      OnStopOrClose(wxCommandEvent&);
    void      OnCloseWindow(wxCloseEvent&);
    void      JoinWorker();

    // worker → UI (always via CallAfter, on the GUI thread)
    void SetTable(int done, const wxString& table);
    void SetRows(long rows);
    void Warn(const wxString& line);
    void Finish(bool cancelled, int okTables, long totalRows, bool writeErr,
                long elapsedMs);

    db::IConnection*           conn_ = nullptr;
    wxString                   connName_, engineName_, db_, path_;
    bool                       withData_ = false;
    std::vector<db::TableInfo> tables_;

    wxStaticText* title_    = nullptr;
    wxGauge*      gauge_    = nullptr;
    wxStaticText* statusTxt_= nullptr;
    wxStaticText* rowTxt_   = nullptr;
    wxTextCtrl*   log_      = nullptr;
    wxButton*     primaryBtn_ = nullptr;   // 停止 while running → 关闭 when done

    std::thread       worker_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> running_{false};
    int               total_ = 0;

    bool     succeeded_ = false;
    wxString summary_;
};

} // namespace ui
