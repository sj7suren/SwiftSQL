// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/MainFrame.h"
#include "core/CrashLog.h"

#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/aui/framemanager.h>
#include <wx/aui/dockart.h>
#include <wx/busyinfo.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/grid.h>
#include <wx/imaglist.h>
#include <wx/notebook.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/activityindicator.h>
#include <wx/srchctrl.h>
#include <wx/simplebook.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/bmpbuttn.h>
#include <wx/statbmp.h>
#include <wx/accel.h>

#ifdef __WXMSW__
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <uxtheme.h>   // MARGINS
#  include <dwmapi.h>    // DwmExtendFrameIntoClientArea
#  pragma comment(lib, "dwmapi.lib")
#  undef DrawText        // windows.h maps DrawText→DrawTextW; we call wxDC/gc DrawText
#endif
#include <wx/stopwatch.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <vector>
#include <wx/srchctrl.h>
#include <wx/stc/stc.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>   // connTree_->widget() is a wxGenericTreeCtrl

#include "core/ConnectionStore.h"
#include "core/FavoriteStore.h"
#include "db/SqlScript.h"          // db::SplitSqlScript (dialect-aware statement splitter, R2)
#include "ui/CenteredDialog.h"
#include "ui/ConnectionDialog.h"
#include "ui/ConnectionTree.h"
#include "ui/EditorPage.h"
#include "ui/ObjectTemplates.h"
#include "ui/ExportDialog.h"
#include "ui/ImportDialog.h"
#include "ui/ResultGridPanel.h"
#include "ui/TableDataPage.h"
#include "ui/ErDiagramView.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/PreferencesDialog.h"
#include "ui/TableDesignView.h"
#include "ui/NewTableView.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/MainFrameInternal.h"
#include "ui/QuerySource.h"        // ui::qsrc — base-table derivation (pure, tested)

namespace ui {

// The naive statement splitter that used to live here was removed (R2 / S1-03):
// the editor's multi-statement run now uses db::SplitSqlScript — the same
// dialect-aware splitter the script runner uses — so DELIMITER, PostgreSQL
// $$…$$ bodies and comment-embedded ';' are handled identically in both paths.
// It is pinned by tests/sqlscript_test.cpp.

// Map a refusal to the text shown under the result. NotSimpleSelect is the
// ordinary "this is a join / an aggregate" case and stays SILENT — a message on
// every non-trivial query would be noise. The other three mean "we could not
// establish which table this is", which the user must be told, because the grid
// looking read-only for no stated reason is indistinguishable from a bug.
static wxString RefusalText(qsrc::BindRefusal r, const wxString& qualifier)
{
    switch (r) {
    case qsrc::BindRefusal::Unparsable:
        return tr(L"⚠ 无法解析此语句的目标表（引号或注释未闭合），结果为只读。");
    case qsrc::BindRefusal::UnknownQuoting:
        return tr(L"⚠ 语句使用了本程序不解析的标识符引用形式，结果为只读。");
    case qsrc::BindRefusal::ForeignQualifier:
        return wxString::Format(
            tr(L"⚠ 语句指定了库/模式限定名「%s」，编辑写回无法保证落到同一张表，结果为只读。"
               L"请用「打开表」编辑该表。"), qualifier);
    case qsrc::BindRefusal::None:
    case qsrc::BindRefusal::NotSimpleSelect:
        break;
    }
    return wxString();
}

// Decide whether a query result can be edited in-place: only a plain
// single-table SELECT whose table has a primary key. Any doubt → not editable
// (the grid stays read-only, which is the safe default).
//
// The base table is derived by ui::qsrc (pure, headless-tested — see
// tests/QuerySourceTests.cpp), NOT by substring-searching the raw SQL and
// deleting quote characters, which is how this function used to hand the write
// path a name like `weird` for a table actually called ``we`ird``. Everything
// here is now wiring: parse → policy → catalog lookup.
ui::EditableSpec DeriveEditable(db::IConnection* conn, const wxString& db,
                                const wxString& sql)
{
    ui::EditableSpec s;
    if (!conn) return s;

    const db::Dialect dialect = conn->GetDialect();
    const qsrc::SelectSource src = qsrc::ParseSelectSource(sql, dialect);
    const qsrc::BindRefusal  why = qsrc::ClassifyBind(src, dialect, db);
    if (why != qsrc::BindRefusal::None) {
        s.notEditableReason = RefusalText(why, src.qualifier);
        return s;
    }

    // RAW name: unquoted, with any doubled quote char collapsed. GetColumns and
    // db::QuoteIdent both want it in exactly this shape.
    const wxString table = src.table;

    std::vector<db::ColumnInfo> cols;
    wxString err;
    if (!conn->GetColumns(db, table, cols, err) || cols.empty()) return s;
    for (const auto& c : cols) {
        if (c.key == L"PK") s.pkColumns.push_back(c.name);
        // Column types in catalog (ordinal) order — matches SELECT * column order,
        // so ResultGridPanel can map each grid column back to its declared type.
        s.colTypes.push_back(c.type);
    }
    if (s.pkColumns.empty()) return s;          // no PK → can't target rows safely

    s.table = table;
    s.dialect = dialect;
    s.editable = true;
    return s;
}

void MainFrame::JoinWorker(bool cancelInFlight)
{
    if (worker_.joinable()) {
        // On shutdown/disconnect, interrupt a still-running query so we don't
        // block on join() until a slow SELECT finishes on its own. Cancel the conn
        // the worker ACTUALLY holds (workerConn_), not connTree_->active(): the two
        // diverge when a query runs on one connection while another is selected, and
        // cancelling the wrong conn would leave us blocked on join().
        if (cancelInFlight && workerConn_)
            workerConn_->Cancel();
        worker_.join();
    }
    workerConn_ = nullptr;   // worker no longer bound to any conn
}

// Cancel any in-flight overview query and join the loader thread. Called before
// starting a new load (the connection is single-threaded) and before a
// connection closes / the frame is destroyed, so the worker never touches a
// freed connection.
void MainFrame::JoinDbWorker()
{
    if (dbWorker_.joinable()) {
        if (dbLoadConn_) dbLoadConn_->Cancel();   // interrupt a slow metadata query
        dbWorker_.join();
    }
    dbLoadConn_ = nullptr;
}

// Count rows for the data-grid pager, asynchronously. A big-table COUNT(*) can take
// seconds, so running it synchronously on the GUI thread froze the whole app; instead
// the Execute runs on a dedicated worker (mirroring dbWorker_) and the total is
// delivered back on the GUI thread via CallAfter. Reports the total via `done`, or -1
// on any failure so the pager degrades to unbounded next/prev. `owner` is the notebook
// page hosting the pager: the result is dropped if that tab is gone when it lands.
//
// Concurrency: the connection is single-threaded, so the count must never run while
// another Execute touches the same conn. Two orderings guarantee this: (1) the launch
// is deferred via CallAfter, so ShowResult's own synchronous GetTableDetail (which runs
// immediately after RequestCount) completes first; (2) every GUI-thread path that then
// issues an Execute (RunSql / RunTxn / ExecuteDml / ApplyDataChangesInTxn /
// ShowConnectionOverview / close / destroy) calls JoinCountWorker() first, cancelling
// and joining any in-flight count. The generation counter drops any superseded result.
void MainFrame::CountRowsForPager(const wxString& table, const wxString& where,
                                  wxWindow* owner, std::function<void(long long)> done)
{
    ConnEntry* a = connTree_->active();
    if (!a || !a->IsConnected() || table.IsEmpty()) { done(-1); return; }
    const db::Dialect dialect = a->conn->GetDialect();
    // Qualify with the current database so COUNT works even when the connection has no
    // default schema selected (MySQL: "No database selected"). Route every name fragment
    // through QuoteIdent so an embedded quote char is doubled (no bare-quote splicing) —
    // consistent with the rest of the codebase.
    wxString from;
    if (dialect == db::Dialect::MySQL && !a->currentDb.IsEmpty())
        from = db::QuoteIdent(a->currentDb, dialect) + L"." + db::QuoteIdent(table, dialect);
    else
        from = db::QuoteIdent(table, dialect);
    wxString sql = L"SELECT COUNT(*) FROM " + from;
    if (!where.IsEmpty()) sql += L" WHERE " + where;

    JoinCountWorker();                       // cancel + join any prior count (bumps countGen_)
    const int gen = countGen_;               // this count's ticket
    db::IConnection* conn = a->conn.get();
    // Defer the launch: let the synchronous GetTableDetail that ShowResult runs right
    // after RequestCount finish first, so the two never hit the conn at the same time.
    CallAfter([this, gen, conn, sql, owner, done = std::move(done)]() mutable {
        if (gen != countGen_) return;        // superseded before we even started
        countConn_ = conn;
        countWorker_ = core::CrashLog::GuardedThread(L"分页行数统计", [this, gen, conn, sql, owner,
                                    done = std::move(done)]() mutable {
            db::QueryResult r; wxString err;
            long long n = -1;
            if (conn->Execute(sql, r, err) && !r.rows.empty() && !r.rows[0].empty())
                r.rows[0][0].ToLongLong(&n);
            CallAfter([this, gen, n, owner, done = std::move(done)]() mutable {
                if (gen != countGen_) return;                    // superseded / closing
                countConn_ = nullptr;
                if (owner && editors_->GetPageIndex(owner) == wxNOT_FOUND) return;  // tab gone
                done(n);
            });
        });
    });
}

// Cancel any in-flight pager COUNT and join the counter thread. Called before any new
// count and before a GUI-thread Execute / connection close / frame destroy, so the
// worker never runs concurrently with another use of the single-threaded connection and
// never touches a freed connection. Bumps countGen_ so a CallAfter the joined worker may
// already have queued is dropped rather than delivered to a stale pager.
void MainFrame::JoinCountWorker()
{
    if (countWorker_.joinable()) {
        if (countConn_) countConn_->Cancel();   // interrupt a slow COUNT(*)
        countWorker_.join();
    }
    countConn_ = nullptr;
    ++countGen_;
}

// ---------------------------------------------------------------------------
// Execute a DML batch atomically (BEGIN → each statement → COMMIT/ROLLBACK).
// No UI; returns false + err on failure. Statements run one per Execute call
// because the drivers are single-statement.
bool MainFrame::ExecuteDml(const wxString& dml, wxString& err)
{
    ConnEntry* active = connTree_->active();
    if (!active || !active->IsConnected()) { err = tr(L"未连接"); return false; }
    JoinWorker(true);    // serialize with any in-flight async query on the same single-threaded conn
    JoinCountWorker();   // serialize with any in-flight pager COUNT on the same conn
    db::IConnection* conn = active->conn.get();
    auto run = [&](const wxString& s) { db::QueryResult r; return conn->Execute(s, r, err); };

    if (!run(L"BEGIN")) return false;
    for (const wxString& raw : wxSplit(dml, '\n')) {
        wxString s = raw; s.Trim().Trim(false);
        if (s.IsEmpty()) continue;
        if (!run(s)) { run(L"ROLLBACK"); return false; }
    }
    run(L"COMMIT");
    return true;
}

// Run a transaction control statement on the active connection (synchronous).
void MainFrame::RunTxn(const wxString& sql, const wxString& okMsg)
{
    ConnEntry* a = connTree_->active();
    if (!a || !a->IsConnected()) { SetStatusText(tr(L"尚未连接")); return; }
    if (queryRunning_) return;   // don't interleave with a running query
    JoinCountWorker();           // ...nor with an in-flight pager COUNT on the same conn
    db::QueryResult r; wxString err;
    if (a->conn->Execute(sql, r, err)) SetStatusText(okMsg);
    else SetStatusText(tr(L"事务操作失败:") + err);
}

// Apply in-grid data edits with NO manual transaction (design §4): the edits are
// applied atomically (ExecuteDml wraps its own BEGIN/COMMIT) and committed. No
// confirmation dialog — the data browser's 取消 button is the safety net (§2).
void MainFrame::ApplyDataChanges(const wxString& dml)
{
    ConnEntry* active = connTree_->active();
    if (!active || !active->IsConnected() || dml.IsEmpty()) return;

    wxString err;
    if (!ExecuteDml(dml, err)) {
        wxMessageBox(tr(L"执行失败,已回滚:") + L"\n\n" + err, tr(L"保存数据修改"),
                     wxOK | wxICON_ERROR, this);
        return;
    }
    SetStatusText(tr(L"数据已保存"));
    wxCommandEvent d;
    OnRunQuery(d);   // reload
}

// Apply in-grid data edits INSIDE an already-open manual transaction (design §4):
// run each statement directly — no nested BEGIN (which on MySQL implicitly commits
// the manual txn) and crucially NO COMMIT. The applied edits sit in the open
// transaction until the user hits 提交事务 (persist) or 回滚事务 (undo). On error
// the transaction is left open so the user can 回滚事务 themselves. No confirm.
void MainFrame::ApplyDataChangesInTxn(const wxString& dml)
{
    ConnEntry* active = connTree_->active();
    if (!active || !active->IsConnected() || dml.IsEmpty()) return;

    JoinWorker(true);    // serialize with any in-flight async query on the same single-threaded conn
    JoinCountWorker();   // serialize with any in-flight pager COUNT on the same conn
    db::IConnection* conn = active->conn.get();
    wxString err;
    auto run = [&](const wxString& s) { db::QueryResult r; return conn->Execute(s, r, err); };

    bool ok = true;
    for (const wxString& raw : wxSplit(dml, '\n')) {
        wxString s = raw; s.Trim().Trim(false);
        if (s.IsEmpty()) continue;
        if (!run(s)) { ok = false; break; }          // leave the txn open for 回滚事务
    }
    if (!ok)
        wxMessageBox(tr(L"执行失败(事务仍开启,可回滚):") + L"\n\n" + err,
                     tr(L"保存数据修改"), wxOK | wxICON_ERROR, this);
    else
        SetStatusText(tr(L"已应用到事务(未提交)"));
    wxCommandEvent d;
    OnRunQuery(d);   // reload to show the in-transaction state
}

// Reflect the run state on the toolbar: 运行 is disabled while a query runs (one at a
// time); 停止 is enabled only while there's actually something to stop.
void MainFrame::UpdateRunStopButtons()
{
    if (wxWindow* r = FindWindow(ID_RUN_QUERY))  r->Enable(!queryRunning_);
    if (wxWindow* s = FindWindow(ID_STOP_QUERY)) s->Enable(queryRunning_);
}

// ---------------------------------------------------------------------------
void MainFrame::OnRunQuery(wxCommandEvent&)
{
    if (IQueryTab* tab = ActiveTab()) RunSql(tab, tab->QueryText());
}

void MainFrame::OnExplain(wxCommandEvent&)
{
    EditorPage* page = ActivePage();
    if (!page) return;
    wxString q = page->QueryText().Strip(wxString::both);
    if (q.EndsWith(L";")) q.RemoveLast();
    if (!q.IsEmpty()) RunSql(page, L"EXPLAIN " + q);
}

// Run one or more ';'-separated statements on the active connection (async).
// Shows the last SELECT result (or the last statement's outcome).
// Reload everything that lists TABLES for (connection, database), after a script
// that created / dropped / altered one. Both halves are no-ops when they don't
// apply, so this is safe to call unconditionally on a table-DDL run:
//
//   * the sidebar's 表 folder — RefreshTablesUi re-validates the connection and
//     does nothing if that database (or that folder) was never opened;
//   * the 表信息 grid — only when it is ALREADY showing this exact database.
//     Reloading it otherwise would yank the fixed tab away from whatever the
//     user is actually looking at.
//
// MUST run on the GUI thread.
void MainFrame::RefreshAfterTableDdl(ConnEntry* e, const wxString& db)
{
    if (!e || !connTree_) return;
    connTree_->RefreshTablesUi(e, db);
    if (dbovEntry_ != e || dbovDb_ != db) return;

    // Preserve an active 分组 filter across the reload. ShowDatabaseOverview
    // clears it by design (clicking a database means "show the whole database"),
    // but this is not a click — the user is still looking at their group, and a
    // DDL run must not silently drop them back to the full list. Set after the
    // call so the reload's async render reads it, exactly as
    // ShowTableGroupOverview does.
    const wxString keepGroup = dbovGroup_;
    ShowDatabaseOverview(e, db, /*force*/ true);
    dbovGroup_ = keepGroup;
    RefreshOverviewTabTitle();
}

void MainFrame::RunSql(IQueryTab* tab, const wxString& sql)
{
    // An object editor carries a 连接/数据库 selector; commit its choice to the run
    // target (active conn + currentDb) before we read them below. A dead selection
    // aborts the run with a red status strip rather than running on the wrong conn.
    if (auto* ep = dynamic_cast<EditorPage*>(tab)) {
        wxString terr;
        if (!ep->CommitRunTarget(terr)) {
            // Selected connection is dead / nothing selected → abort with a visible
            // error. Object editors show their bottom save-status strip; plain SQL
            // editors show the red error in the results grid (ShowError no-ops the
            // strip for non-object editors, so route each to its own surface).
            if (ep->IsObjectEditor()) ep->ShowObjectSaveStatus(false, terr);
            else                       ep->ShowError(terr);
            return;
        }
    }
    ConnEntry* active = connTree_->active();
    if (!active || !active->IsConnected()) {
        tab->ShowError(tr(L"尚未连接数据库 — 双击左侧连接节点,或点「新建连接」"));
        return;
    }
    if (queryRunning_.exchange(true)) return;   // one query at a time

    tab->SetRunning(true);
    UpdateRunStopButtons();          // running → 运行 disabled, 停止 enabled
    SetStatusText(tr(L"正在执行查询…"));

    JoinWorker();
    JoinCountWorker();   // the pager COUNT shares this single-threaded conn — serialize
    db::IConnection* conn = active->conn.get();
    workerConn_ = conn;  // record the conn the worker holds so a disconnect of THIS
                         // connection (even when it isn't active_) joins us before freeing it
    // The tab is also a window (EditorPage / TableDataPage) — keep a wxWindow*
    // to check it still lives in the notebook when the async result lands.
    wxWindow* win = dynamic_cast<wxWindow*>(tab);
    const wxString curDb = active->currentDb;
    const wxString target = curDb.IsEmpty() ? active->profile.host : curDb;
    // Read the dialect on the UI thread; the worker splits with the dialect-aware
    // splitter so the editor path handles DELIMITER / $$…$$ / comments exactly like
    // the script runner (RunScriptDialog) — one splitter, no drift (R2 / S1-03).
    const db::Dialect dialect = conn->GetDialect();
    worker_ = core::CrashLog::GuardedThread(L"SQL 执行", [this, conn, active, tab, win, sql, curDb, target, dialect]() {
        const std::vector<wxString> stmts = db::SplitSqlScript(sql, dialect);
        // Decided HERE, off the already-split statements, rather than by
        // re-scanning the raw text on the GUI thread: the splitter has already
        // stripped the comments and delimiter games that a naive scan of `sql`
        // would trip over.
        const bool tableDdl = db::ScriptTouchesTables(stmts);
        db::QueryResult shown; bool haveSelect = false, haveAny = false;
        wxString err; bool ok = true; int nrun = 0;
        for (const wxString& st : stmts) {
            db::QueryResult r;
            if (!conn->Execute(st, r, err)) { ok = false; break; }
            ++nrun;
            if (r.isSelect) { shown = std::move(r); haveSelect = true; }
            else if (!haveSelect) { shown = std::move(r); }
            haveAny = true;
        }
        // editable only for a single plain SELECT statement
        ui::EditableSpec spec;
        if (ok && stmts.size() == 1 && shown.isSelect)
            spec = DeriveEditable(conn, curDb, stmts.front());

        CallAfter([this, active, curDb, tab, win, shown = std::move(shown), err, ok, target, spec, nrun, haveAny, sql, tableDdl]() {
            queryRunning_ = false;
            UpdateRunStopButtons();      // idle → 运行 enabled, 停止 disabled
            if (!win || editors_->GetPageIndex(win) == wxNOT_FOUND) return;
            tab->SetRunning(false);
            // Object editors (视图/函数/存储过程/包) run a CREATE that returns no rows:
            // show a bottom "保存成功" / error strip in the editor rather than the
            // results grid, and rename the tab to the created object on success.
            auto* objEp = dynamic_cast<EditorPage*>(win);
            if (objEp && !objEp->IsObjectEditor()) objEp = nullptr;
            if (ok && haveAny) {
                if (objEp) {
                    objEp->ShowObjectSaveStatus(/*ok*/ true, wxString());
                    const wxString name = ui::ParseObjectName(sql);
                    const int idx = editors_->GetPageIndex(win);
                    if (!name.IsEmpty() && idx != wxNOT_FOUND)
                        editors_->SetPageText(idx, name);
                    // Reload the tree's matching category folder so a newly created /
                    // renamed 视图/函数/存储过程 appears on the left without a manual
                    // refresh. RefreshCategory re-validates the connection is still live
                    // and no-ops if the db/folder isn't open in the tree. `active`+`curDb`
                    // are the actual run target (CommitRunTarget), not the tree's active_.
                    connTree_->RefreshCategory(active, curDb, objEp->ObjectEditorKind());
                } else {
                    tab->ShowResult(shown, target, spec);
                    // A PLAIN EDITOR CREATES TABLES TOO, and until this branch
                    // existed nothing reloaded after it did. Only the object-editor
                    // branch above refreshed anything, so the 新建表 flow — which
                    // opens a PLAIN tab prefilled with a CREATE TABLE template
                    // (ConnectionTree's newQueryWith hook) — ran the DDL
                    // successfully and then showed the user nothing: no new table
                    // on the left, no new row in 表信息, and no error either,
                    // because there was no error. That is the whole reported
                    // symptom, and it is engine-independent.
                    if (tableDdl) RefreshAfterTableDdl(active, curDb);
                }
                SetStatusText(wxString::Format(tr(L"完成 · %d 条语句 · %llu 行 · %ld ms"),
                              nrun, shown.affected, shown.elapsedMs));
            } else if (ok) {
                SetStatusText(tr(L"无可执行语句"));
            } else {
                if (objEp) objEp->ShowObjectSaveStatus(/*ok*/ false, err);
                else       tab->ShowError(err);
                SetStatusText(tr(L"查询失败"));
            }
        });
    });
}

} // namespace ui
