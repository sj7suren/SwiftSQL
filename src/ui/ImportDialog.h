// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ImportDialog.h — import a data file (CSV/TXT/JSON/XML) into a table, with a
// config step (file / encoding / target table / error strategy) and a live
// progress step (gauge, success/failure counters, a per-failure list you can
// copy or export). Parsing is delegated to db::ImportTable (TableImport.h);
// each parsed row becomes an INSERT run on a worker thread. A .sql file is not
// handled here — it is handed off to the existing RunScriptDialog.
#pragma once

#include <wx/string.h>
#include <atomic>
#include <thread>
#include <vector>

#include "db/DbDriver.h"
#include "ui/CenteredDialog.h"

class wxFilePickerCtrl;
class wxChoice;
class wxCheckBox;
class wxTextCtrl;
class wxGauge;
class wxStaticText;
class wxListCtrl;
class wxButton;
class wxSimplebook;

namespace ui {

class ImportDialog : public CenteredDialog {
public:
    // `conn` must outlive the dialog. `targetDb` / `targetTable` seed the target
    // (the table can be edited on the config page).
    ImportDialog(wxWindow* parent, db::IConnection* conn,
                 const wxString& targetDb, const wxString& targetTable);
    ~ImportDialog() override;

    bool DidImport() const { return didImport_; }

private:
    struct Failure {
        long long lineNo;    // 1-based source line (parse) or row index (insert)
        wxString  raw;       // offending row text
        wxString  error;     // reason
    };

    wxWindow* BuildConfigPage();
    wxWindow* BuildRunPage();

    void OnStart(wxCommandEvent&);
    void OnStopOrClose(wxCommandEvent&);
    void OnBack(wxCommandEvent&);
    void OnCopyErrors(wxCommandEvent&);
    void OnExportErrors(wxCommandEvent&);
    void OnCloseWindow(wxCloseEvent&);

    wxString ReadFile(const wxString& path, wxString& err) const;   // honours encoding
    wxString QualifiedTable() const;
    void     AppendFailure(const Failure& f);
    void     Tick(long long done, long long ok, long long fail);
    void     Finish(bool cancelled, long long ok, long elapsedMs);
    void     JoinWorker();
    wxString AllFailuresText() const;

    db::IConnection* conn_ = nullptr;
    wxString         targetDb_, targetTable_;
    bool             didImport_ = false;

    // config page
    wxSimplebook*     book_        = nullptr;
    wxFilePickerCtrl* filePicker_  = nullptr;
    wxChoice*         encoding_    = nullptr;
    wxTextCtrl*       tableCtrl_   = nullptr;
    wxCheckBox*       hasHeader_   = nullptr;
    wxChoice*         delimChoice_ = nullptr;
    wxCheckBox*       ignoreError_ = nullptr;
    wxCheckBox*       useTxn_      = nullptr;
    wxButton*         startBtn_    = nullptr;

    // run page
    wxStaticText* runTitle_    = nullptr;
    wxGauge*      gauge_       = nullptr;
    wxStaticText* progressTxt_ = nullptr;
    wxStaticText* counterTxt_  = nullptr;
    wxListCtrl*   failList_    = nullptr;
    wxButton*     copyBtn_     = nullptr;
    wxButton*     exportBtn_   = nullptr;
    wxButton*     backBtn_     = nullptr;
    wxButton*     primaryBtn_  = nullptr;

    std::thread          worker_;
    std::atomic<bool>    stopFlag_{false};
    std::atomic<bool>    running_{false};
    long long            total_ = 0;
    std::vector<Failure> failures_;
};

} // namespace ui
