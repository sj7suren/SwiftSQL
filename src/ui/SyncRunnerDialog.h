// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncRunnerDialog.h — execute a pre-built cross-database SyncPlan against the
// target connection with a live progress UI, so a large sync can't freeze the
// window. Mirrors DumpScriptDialog's worker/progress/stop machinery.
//
// The plan (ordered preamble → per-table DDL/DML → postamble, all already
// materialized as statement strings by SyncEngine::BuildPlan) is applied on a
// worker thread — every IConnection call is blocking. db::sync::SyncEngine::
// Execute drives the target transaction (rollback-capable dialects wrap in
// BEGIN/COMMIT; MySQL DDL auto-commits and is reported honestly). Progress is
// marshalled back to the GUI thread with CallAfter: a gauge over statements, a
// per-statement log, and ok/fail counters. 停止 flips an atomic the engine
// checks between statements and calls Cancel() to interrupt an in-flight one.
//
// If the plan contains destructive statements (DROP / DELETE / TRUNCATE) the
// dialog demands the user type the target database name before it starts — the
// safety red line from the design (§2.5).
//
// Threading contract: the worker executes on its OWN cloned connections
// (db::CloneConnection), never the caller's — db::IConnection is not
// thread-safe, and the caller's pair belongs to the UI thread. See the member
// comments below.
#pragma once

#include <wx/string.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "db/DbDriver.h"
#include "db/SyncEngine.h"
#include "ui/CenteredDialog.h"

class wxGauge;
class wxStaticText;
class wxTextCtrl;
class wxButton;

namespace ui {

class SyncRunnerDialog : public CenteredDialog {
public:
    // src/tgt must outlive the dialog (the caller owns both connections). They
    // are used ONLY as templates for db::CloneConnection — the plan itself runs
    // on the worker's own clones, never on these. srcDb/tgtDb name the
    // databases; plan is consumed (moved in). useTransaction wraps
    // rollback-capable dialects; dbLabels are for the header.
    //
    // `data` is the DATA half's authorization — which categories of which
    // tables may write. It is a SEPARATE argument and not folded into the plan
    // on purpose: SyncEngine::Execute's data pass RE-DERIVES each table's diff
    // from the plan's dataSpec and applies it under this authorization, so the
    // plan says "what could change" and this says "what the user approved".
    //
    // `deleteRows` is how many rows the confirmed selection would actually
    // delete, AFTER the user's per-row unchecking. It is passed in rather than
    // derived here because this dialog does not hold the compare-time row
    // sample: DataExecPlan carries the per-table delete AUTHORIZATION, not a
    // count, and counting it here would mean re-deriving the diff. -1 means
    // "not supplied", which is treated exactly as the old behaviour (any
    // authorized delete prompts) so no caller can weaken the gate by omission.
    SyncRunnerDialog(wxWindow* parent, db::IConnection* src, db::IConnection* tgt,
                     const wxString& srcDb, const wxString& tgtDb,
                     const wxString& srcName, const wxString& tgtName,
                     db::sync::SyncPlan plan, db::sync::DataExecPlan data,
                     bool useTransaction, long long deleteRows = -1);
    ~SyncRunnerDialog() override;

    bool Succeeded() const { return succeeded_; }

private:
    wxWindow* BuildUi();
    void      Confirm();        // destructive gate, then StartWorker
    void      StartWorker();
    void      OnStopOrClose(wxCommandEvent&);
    void      OnCloseWindow(wxCloseEvent&);
    void      JoinWorker();
    void      CancelRunning();  // stopFlag_ + Cancel() the worker's live clones

    // worker → UI (always via CallAfter, on the GUI thread)
    void Tick(int done, int total, const wxString& sql);
    void Finish(bool ok, bool cancelled, const wxString& err, long elapsedMs);
    void AbortBeforeStart();

    // Append the per-table account of the data pass to the log: rows ACTUALLY
    // applied, which committed, which failed, which were never reached, and any
    // drift from the compare pass's counts. Called on the GUI thread from
    // Finish(). See the .cpp for why drift is shown rather than hidden.
    void ReportDataResult(const db::sync::DataExecResult& result);

    // Caller-owned, UI-thread-owned connections. Read on the worker ONLY by
    // db::CloneConnection (which touches nothing but the const accessors
    // IsConnected()/EffectiveProfile()); no statement is ever executed on them.
    db::IConnection*    srcTemplate_ = nullptr;
    db::IConnection*    tgtTemplate_ = nullptr;
    wxString            srcDb_, tgtDb_, srcName_, tgtName_;
    db::sync::SyncPlan      plan_;
    db::sync::DataExecPlan  data_;
    bool                    useTransaction_ = false;
    long long               deleteRows_ = -1;   // see ctor; -1 == not supplied

    // Filled by the worker, read by Finish() on the GUI thread. The handoff is
    // the same CallAfter that carries ok/err, so there is no concurrent access:
    // the worker writes it before posting, the UI reads it after.
    db::sync::DataExecResult dataResult_;

    wxStaticText* title_    = nullptr;
    wxGauge*      gauge_    = nullptr;
    wxStaticText* statusTxt_= nullptr;
    wxStaticText* counterTxt_ = nullptr;
    wxTextCtrl*   log_      = nullptr;
    wxButton*     primaryBtn_ = nullptr;   // 停止 while running → 关闭 when done

    // The worker owns its clones inside its lambda scope; runSrcConn_/runTgtConn_
    // are non-owning observers published only while those clones are alive, so
    // 停止 / window-close (UI thread) can Cancel() the query that is actually in
    // flight — cancelling srcTemplate_/tgtTemplate_ would be a no-op. runConnMx_
    // makes "read the pointer + Cancel() it" and "null it out before destroying
    // the clone" mutually exclusive, so Cancel() can never reach a half-destroyed
    // connection.
    std::thread       worker_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> running_{false};
    std::mutex        runConnMx_;
    db::IConnection*  runSrcConn_ = nullptr;
    db::IConnection*  runTgtConn_ = nullptr;
    int               gaugeSet_ = 0;

    bool succeeded_ = false;
};

} // namespace ui
