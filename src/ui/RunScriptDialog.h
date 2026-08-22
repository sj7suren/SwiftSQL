// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// RunScriptDialog.h — run a .sql script file against a live connection, with a
// config step (file / encoding / error-handling) and a live progress step
// (gauge, success/failure counters, a per-failure list you can copy or export,
// and a gated AI-diagnose placeholder).
//
// The script runs on a worker thread — IConnection::Execute() is blocking by
// design (see DbDriver.h) — and every UI update is marshalled back to the GUI
// thread with CallAfter. The 「停止」 button flips an atomic flag the worker
// checks between statements and calls IConnection::Cancel() to interrupt an
// in-flight statement. Statement splitting is delegated to db::SplitSqlScript,
// so DELIMITER / dollar-quote handling stays in one dialect-aware place.
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
class wxGauge;
class wxStaticText;
class wxListCtrl;
class wxButton;
class wxSimplebook;
class wxTextCtrl;

namespace ui {

class RunScriptDialog : public CenteredDialog {
public:
    // `conn` must outlive the dialog (the caller owns it). `connName`/`dbName`
    // are display-only header text. `aiConfigured` gates the AI-diagnose button
    // — combined with "only when there are failures", per product requirement.
    // `presetFile` (optional): when non-empty, the dialog skips the config step
    // and immediately runs that .sql file — used by "粘贴表", which generates a
    // CREATE+INSERT script and streams it into the target database with the same
    // progress/USE/failure machinery a manual run gets.
    RunScriptDialog(wxWindow* parent, db::IConnection* conn,
                    const wxString& connName, const wxString& dbName,
                    bool aiConfigured, const wxString& presetFile = wxEmptyString);
    ~RunScriptDialog() override;

    // True once a script actually executed (any outcome) — the caller refreshes
    // its tree, since the run may have changed structure or data.
    bool DidRun() const { return didRun_; }

private:
    struct Failure {
        int      index;       // 1-based statement number
        wxString statement;   // the SQL that failed (trimmed for display)
        wxString error;       // driver error text
    };

    // --- build ---
    wxWindow* BuildConfigPage();
    wxWindow* BuildRunPage();

    // --- events ---
    void OnStart(wxCommandEvent&);
    void OnStopOrClose(wxCommandEvent&);
    void OnBack(wxCommandEvent&);
    void OnCopyErrors(wxCommandEvent&);
    void OnExportErrors(wxCommandEvent&);
    void OnAiDiagnose(wxCommandEvent&);
    void OnCloseWindow(wxCloseEvent&);

    // --- worker plumbing ---
    wxString ReadScriptFile(const wxString& path, wxString& err) const; // honours encoding choice
    void     AppendFailure(const Failure& f);
    void     Tick(int done, int okCount, int failCount);
    void     Finish(bool cancelled, int okCount, long elapsedMs);
    void     JoinWorker();
    wxString AllFailuresText() const;

    db::IConnection* conn_ = nullptr;
    wxString         connName_, dbName_;
    bool             aiConfigured_ = false;
    bool             didRun_       = false;

    // config page
    wxSimplebook*     book_        = nullptr;
    wxFilePickerCtrl* filePicker_  = nullptr;
    wxChoice*         encoding_    = nullptr;
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
    wxButton*     aiBtn_       = nullptr;
    wxTextCtrl*   aiPanel_     = nullptr;
    wxButton*     backBtn_     = nullptr;
    wxButton*     primaryBtn_  = nullptr;   // 停止 while running → 关闭 when done

    // worker
    std::thread          worker_;
    std::atomic<bool>    stopFlag_{false};
    std::atomic<bool>    running_{false};
    int                  total_ = 0;
    std::vector<Failure> failures_;
};

} // namespace ui
