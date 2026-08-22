// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncRunnerDialog.cpp — see header. Applies a SyncPlan on a worker thread via
// db::sync::SyncEngine::Execute, mirroring DumpScriptDialog's progress/stop UI.
#include "ui/SyncRunnerDialog.h"
#include "core/CrashLog.h"
#include "db/ConnectionClone.h"

#include <memory>

#include <wx/wx.h>
#include <wx/gauge.h>
#include <wx/statline.h>
#include <wx/stopwatch.h>

#include "ui/CenteredDialog.h"   // GetTextCentered
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// A statement is destructive if it drops or wipes data. Uppercase substring
// match is enough for the gate — false positives only add a confirm step.
bool IsDangerStmt(const wxString& s)
{
    const wxString u = s.Upper();
    return u.Contains(L"DROP ") || u.Contains(L"DELETE ") || u.Contains(L"TRUNCATE");
}

// Would this run destroy anything? Two independent sources, because the two
// halves are now authorized separately:
//
//   * the DDL text, scanned for DROP/TRUNCATE as before;
//   * the DATA authorization, which is STRUCTURED — a table may delete rows iff
//     its TableDataSpec carries the delete token. That is read directly rather
//     than by scanning `u.dml` for the word DELETE, which would both miss the
//     deletes beyond the 500-row preview cap and fire on a table whose deletes
//     the user never approved.
// `deleteRows` is the caller's count of rows that will actually be deleted
// after per-row exclusions, or -1 when it was not supplied. It can only ever
// SUPPRESS the data half of the gate, and only on an exact zero: a table whose
// deletes are authorized but every one of whose delete rows the user
// individually unchecked destroys nothing, and demanding a typed database name
// to authorize nothing trains people to type it without reading. The DDL half
// is untouched by it — a DROP TABLE is destructive no matter how many rows were
// unchecked.
bool PlanHasDestructive(const db::sync::SyncPlan& p, const db::sync::DataExecPlan& d,
                        long long deleteRows)
{
    for (const auto& s : p.preamble)  if (IsDangerStmt(s)) return true;
    for (const auto& s : p.postamble) if (IsDangerStmt(s)) return true;
    for (const auto& u : p.units)
        for (const auto& s : u.ddl) if (IsDangerStmt(s)) return true;
    if (deleteRows == 0) return false;
    for (const auto& t : d.tables) if (t.Deletes()) return true;
    return false;
}

// One-line snippet for the log.
wxString Snippet(const wxString& sql)
{
    wxString s = sql;
    s.Replace(L"\r", L" ");
    s.Replace(L"\n", L" ");
    s.Replace(L"\t", L" ");
    while (s.Replace(L"  ", L" ")) {}
    s.Trim(true).Trim(false);
    if (s.length() > 200) s = s.Left(200) + L"…";
    return s;
}

} // namespace

SyncRunnerDialog::SyncRunnerDialog(wxWindow* parent, db::IConnection* src,
                                   db::IConnection* tgt, const wxString& srcDb,
                                   const wxString& tgtDb, const wxString& srcName,
                                   const wxString& tgtName, db::sync::SyncPlan plan,
                                   db::sync::DataExecPlan data, bool useTransaction,
                                   long long deleteRows)
    : CenteredDialog(parent, wxID_ANY, tr(L"执行同步"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , srcTemplate_(src)
    , tgtTemplate_(tgt)
    , srcDb_(srcDb)
    , tgtDb_(tgtDb)
    , srcName_(srcName)
    , tgtName_(tgtName)
    , plan_(std::move(plan))
    , data_(std::move(data))
    , useTransaction_(useTransaction)
    , deleteRows_(deleteRows)
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(BuildUi(), 1, wxEXPAND);
    SetSizerAndFit(root);
    SetSize(FromDIP(wxSize(600, 460)));

    Bind(wxEVT_CLOSE_WINDOW, &SyncRunnerDialog::OnCloseWindow, this);
    // Gate + start after the modal loop is running, so an instant finish can't
    // EndModal before ShowModal() has begun.
    CallAfter([this]() { Confirm(); });
}

SyncRunnerDialog::~SyncRunnerDialog()
{
    JoinWorker();
}

wxWindow* SyncRunnerDialog::BuildUi()
{
    auto* page = new wxPanel(this);
    auto* s = new wxBoxSizer(wxVERTICAL);

    title_ = new wxStaticText(page, wxID_ANY, tr(L"正在同步…"));
    title_->SetFont(Ui(13, /*bold*/ true));
    title_->SetForegroundColour(theme::kTextStrong);
    s->Add(title_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    auto* sub = new wxStaticText(page, wxID_ANY,
        srcName_ + L" / " + srcDb_ + L"   ──▶   " + tgtName_ + L" / " + tgtDb_);
    sub->SetFont(Ui(9));
    sub->SetForegroundColour(theme::kTextSecondary);
    s->Add(sub, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    gauge_ = new wxGauge(page, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 10),
                         wxGA_HORIZONTAL | wxGA_SMOOTH);
    s->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    statusTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    statusTxt_->SetFont(Ui(9));
    statusTxt_->SetForegroundColour(theme::kTextSecondary);
    s->Add(statusTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    counterTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    counterTxt_->SetFont(Ui(10, /*bold*/ true));
    counterTxt_->SetForegroundColour(theme::kPrimary);
    s->Add(counterTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    auto* logHdr = new wxStaticText(page, wxID_ANY, tr(L"日志"));
    logHdr->SetFont(Ui(9, /*bold*/ true));
    logHdr->SetForegroundColour(theme::kTextBody);
    s->Add(logHdr, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    log_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                          wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    log_->SetFont(Mono(9));
    s->Add(log_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    primaryBtn_ = new wxButton(page, wxID_ANY, tr(L"停止"));
    primaryBtn_->Bind(wxEVT_BUTTON, &SyncRunnerDialog::OnStopOrClose, this);
    btns->Add(primaryBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    page->SetSizer(s);
    return page;
}

// ---------------------------------------------------------------------------
// Destructive gate → start
// ---------------------------------------------------------------------------
void SyncRunnerDialog::Confirm()
{
    if (PlanHasDestructive(plan_, data_, deleteRows_)) {
        // The COUNT is stated when we have one. "包含删除语句" is a category;
        // 「将删除约 12 行」 is the thing the user is actually authorizing, and
        // it is the number their per-row unchecking just changed. 「约」 is not
        // hedging for its own sake: the execute pass re-derives the diff (see
        // DataSyncExec.h), so between compare and execute the real figure can
        // legitimately move, and promising an exact one would be the lie.
        wxString what = tr(L"此操作包含删除 / 清空等破坏性语句，可能造成目标数据不可恢复。");
        if (deleteRows_ > 0)
            what += L"\n" + wxString::Format(tr(L"按当前勾选，将删除目标中约 %lld 行。"),
                                             deleteRows_);

        const wxString v = GetTextCentered(
            this,
            what + L"\n" + tr(L"请键入目标数据库名以确认执行：") + L"\n\n" + tgtDb_,
            tr(L"危险操作确认"));
        if (v != tgtDb_) {
            AbortBeforeStart();
            return;
        }
    }
    StartWorker();
}

void SyncRunnerDialog::AbortBeforeStart()
{
    running_ = false;
    succeeded_ = false;
    title_->SetLabel(tr(L"已取消（未执行）"));
    title_->SetForegroundColour(theme::kDotAmber);
    statusTxt_->SetLabel(tr(L"库名不匹配或已取消，目标未改变。"));
    primaryBtn_->SetLabel(tr(L"关闭"));
    Layout();
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
void SyncRunnerDialog::StartWorker()
{
    if (running_.exchange(true)) return;
    stopFlag_ = false;
    gaugeSet_ = 0;

    // Snapshot the UI's connections BY POINTER — read-only templates for
    // CloneConnection(); the worker calls nothing else on them.
    db::IConnection* srcRaw = srcTemplate_;
    db::IConnection* tgtRaw = tgtTemplate_;

    worker_ = core::CrashLog::GuardedThread(L"同步执行", [this, srcRaw, tgtRaw]() {
        wxStopWatch sw;
        bool     ok = false;
        bool     cancelled = false;
        wxString err;
        try {
            // Dedicated connections for the execute — never the caller's:
            // db::IConnection is not thread-safe and those belong to the UI
            // thread (the heap-corruption pattern this mirrors the compare-phase
            // fix for). Connect latency (incl. re-dialing an SSH tunnel) happens
            // here, off the UI thread, on purpose.
            std::unique_ptr<db::IConnection> srcClone = db::CloneConnection(*srcRaw, err);
            std::unique_ptr<db::IConnection> tgtClone;
            if (srcClone) tgtClone = db::CloneConnection(*tgtRaw, err);

            if (!srcClone || !tgtClone) {
                // Abort BEFORE a single statement runs — the target is untouched.
                err = tr(L"无法建立独立的执行连接，未执行任何语句，目标未改变：") + err;
            } else {
                // Publish the clones as the UI thread's live Cancel() targets.
                // clearObservers is declared AFTER the clones, so it is destroyed
                // BEFORE them (reverse declaration order) on every exit path —
                // CancelRunning() can therefore never observe a clone that's
                // mid-destruction.
                {
                    std::lock_guard<std::mutex> lk(runConnMx_);
                    runSrcConn_ = srcClone.get();
                    runTgtConn_ = tgtClone.get();
                }
                struct ObserverGuard {
                    SyncRunnerDialog* self;
                    ~ObserverGuard()
                    {
                        std::lock_guard<std::mutex> lk(self->runConnMx_);
                        self->runSrcConn_ = nullptr;
                        self->runTgtConn_ = nullptr;
                    }
                } clearObservers{ this };

                // ONE engine over ONE target clone: preamble, per-table DDL/DML,
                // postamble and the BEGIN/COMMIT all go through eng's tgt_, so
                // session state (SET FOREIGN_KEY_CHECKS=0, the open transaction)
                // is continuous. Splitting phases across connections would
                // silently lose both.
                db::sync::SyncEngine eng(*srcClone, *tgtClone, srcDb_, tgtDb_);
                auto progress = [this](int done, int total, const wxString& sql) {
                    const wxString snip = Snippet(sql);
                    CallAfter([this, done, total, snip]() { Tick(done, total, snip); });
                };
                // The data-bearing overload. The structure-only one refuses,
                // loudly, any plan carrying data changes — it has no way to
                // express which categories the user approved, so it fails
                // closed. Passing data_ is what turns that refusal back into a
                // working data sync.
                ok = eng.Execute(plan_, data_, useTransaction_, dataResult_, err,
                                 stopFlag_, progress);
                cancelled = stopFlag_.load();
            }
            // clearObservers (if constructed) then tgtClone/srcClone unwind here —
            // torn down on THIS (worker) thread, strictly before the CallAfter
            // below hands anything to the UI thread.
        } catch (...) {
            // Guarantee the completion CallAfter still fires even if the above
            // throws: OnCloseWindow() Veto()s until running_ goes false, and only
            // Finish() clears it, so without this the dialog could never close.
            // Clones already unwound via stack unwinding before this handler runs.
            const long fms = sw.Time();
            CallAfter([this, fms]() {
                Finish(false, false, tr(L"执行线程发生未预期异常"), fms);
            });
            throw;   // rethrow so CrashLog::Guard still logs/notifies (defense in
                     // depth) — the CallAfter above already unblocks the dialog.
        }
        const long ms = sw.Time();
        CallAfter([this, ok, cancelled, err, ms]() {
            Finish(ok, cancelled, err, ms);
        });
    });
}

// ---------------------------------------------------------------------------
// Worker → UI
// ---------------------------------------------------------------------------
void SyncRunnerDialog::Tick(int done, int total, const wxString& sql)
{
    if (total > 0 && gauge_->GetRange() != total) gauge_->SetRange(total);
    gauge_->SetValue(done);
    gaugeSet_ = total;
    statusTxt_->SetLabel(wxString::Format(tr(L"正在执行  %d / %d"), done, total));
    counterTxt_->SetLabel(wxString::Format(tr(L"已执行 %d 条"), done));
    log_->AppendText(sql + L"\n");
}

void SyncRunnerDialog::Finish(bool ok, bool cancelled, const wxString& err,
                              long elapsedMs)
{
    running_ = false;
    if (gauge_->GetRange() > 0) gauge_->SetValue(gauge_->GetRange());

    // Before the verdict line, so the reader sees WHICH tables landed and only
    // then the overall outcome. Runs on failure and cancellation too: those are
    // exactly the cases where "which tables are already committed?" is the
    // user's first question, and DataExecResult is populated either way.
    ReportDataResult(dataResult_);

    if (cancelled) {
        title_->SetLabel(tr(L"已停止"));
        title_->SetForegroundColour(theme::kDotAmber);
        succeeded_ = false;
        statusTxt_->SetLabel(tr(L"已停止 — 按事务策略，未提交的更改已回滚或部分提交。"));
    } else if (ok) {
        title_->SetLabel(tr(L"同步完成"));
        title_->SetForegroundColour(theme::kGreen);
        succeeded_ = true;
        statusTxt_->SetLabel(wxString::Format(
            tr(L"全部语句执行成功 · 用时 %ld ms"), elapsedMs));
    } else {
        title_->SetLabel(tr(L"同步失败"));
        title_->SetForegroundColour(theme::kDotRed);
        succeeded_ = false;
        statusTxt_->SetLabel(tr(L"执行失败：") + err);
        log_->AppendText(L"\n-- " + tr(L"错误：") + err + L"\n");
    }
    primaryBtn_->SetLabel(tr(L"关闭"));
    primaryBtn_->Enable(true);
    Layout();
}

// The per-table account of the DATA pass.
//
// WHY THE COUNTS MAY DISAGREE WITH THE COMPARE SCREEN, AND WHY WE SAY SO
// The compare pass counts differences; the execute pass RE-RUNS the diff and
// streams it into the target, because replaying the compare pass's 500-row
// sample would apply 0.1% of a 500k-row diff and report success. The source can
// change between the two passes, so the rows actually applied are the execute
// pass's numbers, not the compare pass's. The data layer deliberately did not
// hide that; hiding it one layer up would be worse, because the user would then
// have a screen that says 12 and a database that got 11 with nothing anywhere
// admitting it. So the drift is named explicitly.
void SyncRunnerDialog::ReportDataResult(const db::sync::DataExecResult& result)
{
    if (result.tables.empty()) return;

    // Compare-pass counters, by table NAME — identity, never position. The two
    // collections are ordered independently (the plan is FK-topological, the
    // result is per-sweep), so a positional pairing would silently attribute one
    // table's drift to another.
    auto planned = [this](const wxString& table) -> db::sync::RowStat {
        for (const auto& u : plan_.units)
            if (u.table == table) return u.stat;
        return db::sync::RowStat{};
    };

    log_->AppendText(L"\n-- " + tr(L"数据同步结果（按表）") + L"\n");

    long long drifted = 0;
    for (const db::sync::TableExecReport& t : result.tables) {
        wxString line;
        if (!t.attempted) {
            line = wxString::Format(tr(L"  %s：未执行（前序表失败或已取消）"), t.table);
        } else if (!t.error.IsEmpty()) {
            line = wxString::Format(tr(L"  %s：失败 — %s"), t.table, t.error);
        } else {
            line = wxString::Format(
                tr(L"  %s：%s · 实际写入 +%lld ~%lld -%lld"),
                t.table,
                t.committed ? tr(L"已提交") : tr(L"未提交"),
                t.inserts, t.updates, t.deletes);
            if (t.excluded > 0)
                line += wxString::Format(tr(L" · 按逐行勾选跳过 %lld 行"), t.excluded);

            const db::sync::RowStat p = planned(t.table);
            if (p.inserts != t.inserts || p.updates != t.updates || p.deletes != t.deletes) {
                ++drifted;
                line += wxString::Format(
                    tr(L"\n      ⚠ 与比对时的 +%lld ~%lld -%lld 不一致：执行阶段会重新比对，"
                       L"期间源数据发生了变化，以上为实际写入数"),
                    p.inserts, p.updates, p.deletes);
            }
        }
        for (const wxString& w : t.warnings) line += L"\n      ⚠ " + w;
        log_->AppendText(line + L"\n");
    }

    if (drifted > 0) {
        log_->AppendText(wxString::Format(
            tr(L"-- 有 %lld 张表的实际写入行数与比对结果不同（源数据在比对与执行之间发生变化）。\n"),
            drifted));
    }
}

// ---------------------------------------------------------------------------
// Buttons / lifecycle
// ---------------------------------------------------------------------------
// Interrupt the statement actually in flight. Must target the worker's clones —
// Cancel()ing srcTemplate_/tgtTemplate_ would cancel an idle UI connection and
// silently do nothing to the running sync.
void SyncRunnerDialog::CancelRunning()
{
    stopFlag_ = true;
    std::lock_guard<std::mutex> lk(runConnMx_);
    if (runTgtConn_) runTgtConn_->Cancel();
    if (runSrcConn_) runSrcConn_->Cancel();
}

void SyncRunnerDialog::OnStopOrClose(wxCommandEvent&)
{
    if (running_.load()) {
        CancelRunning();
        primaryBtn_->Enable(false);
        primaryBtn_->SetLabel(tr(L"正在停止…"));
    } else {
        Close();
    }
}

void SyncRunnerDialog::OnCloseWindow(wxCloseEvent& ev)
{
    if (running_.load()) {
        CancelRunning();
        ev.Veto();      // let Finish() re-enable closing once the worker drains
        return;
    }
    JoinWorker();
    EndModal(wxID_OK);
}

void SyncRunnerDialog::JoinWorker()
{
    if (worker_.joinable()) {
        CancelRunning();
        worker_.join();
    }
}

} // namespace ui
