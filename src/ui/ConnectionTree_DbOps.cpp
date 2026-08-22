// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionTree_DbOps.cpp — database-level operations (create / edit / delete /
// dump / sync / run-script) plus the cross-connection table copy/paste, split out
// of ConnectionTree.cpp per the ≤1000-line charter (docs/CHARTER.md). The method
// bodies below are the exact originals; behavior is unchanged.
#include "ui/ConnectionTree.h"
#include "ui/ConnectionTreeInternal.h"

#include <wx/wx.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>

#include "ui/CenteredDialog.h"
#include "ui/I18n.h"
#include "ui/NewDatabaseDialog.h"
#include "ui/RunScriptDialog.h"
#include "ui/DumpScriptDialog.h"
#include "ui/SyncWizardDialog.h"

namespace ui {

void ConnectionTree::NewDatabase(ConnEntry* e)
{
    if (!e || !e->IsConnected()) return;
    // Engine-adaptive dialog: it queries the driver for supported options
    // (charset/collation, encoding/owner/locale/…) and builds the escaped SQL.
    NewDatabaseDialog dlg(dialogParent_, e->conn.get(),   // CenteredDialog → centred
                          db::InfoOf(e->profile.type).name);
    if (dlg.ShowModal() != wxID_OK) return;
    const wxString sql = dlg.Sql();
    if (sql.IsEmpty()) return;

    db::QueryResult r; wxString err;
    {
        // CREATE DATABASE physically copies the template database on PostgreSQL — a
        // synchronous call that can block the GUI thread for a moment; show the
        // loading cursor while it runs (and while the tree refreshes) so the app
        // reads as "working" rather than frozen.
        wxBusyCursor busy;
        if (e->conn->Execute(sql, r, err)) {   // one statement → PG autocommits
            hooks_.status(tr(L"已新建数据库") + L" — " + dlg.DatabaseName());
            LoadDatabases(e);   // refresh the tree
            return;
        }
    }
    wxMessageBox(tr(L"新建数据库失败:") + L"\n\n" + err, tr(L"新建数据库"),
                 wxOK | wxICON_ERROR, dialogParent_);
}

void ConnectionTree::EditDatabase(ConnEntry* e, const wxString& db)
{
    if (!e || !e->IsConnected()) return;
    // Only the MySQL family lets you re-set a database's default charset after
    // creation; a libpq (PG/KingBase/DM) database's encoding is fixed at CREATE.
    if (e->conn->GetDialect() != db::Dialect::MySQL) {
        wxMessageBox(tr(L"该数据库引擎不支持在创建后修改数据库字符集。"),
                     tr(L"编辑数据库"), wxOK | wxICON_INFORMATION, dialogParent_);
        return;
    }
    const wxString cs = GetTextCentered(
        dialogParent_, tr(L"字符集 (CHARACTER SET):"),
        tr(L"编辑数据库") + L" — " + db, e->profile.charset);
    if (cs.IsEmpty()) return;
    db::QueryResult r; wxString err;
    {
        wxBusyCursor busy;   // synchronous ALTER on the GUI thread — show loading cursor
        if (e->conn->Execute(L"ALTER DATABASE `" + db + L"` CHARACTER SET " + cs, r, err)) {
            hooks_.status(tr(L"已更新数据库字符集") + L" — " + db);
            return;
        }
    }
    wxMessageBox(tr(L"编辑数据库失败:") + L"\n\n" + err, tr(L"编辑数据库"),
                 wxOK | wxICON_ERROR, dialogParent_);
}

void ConnectionTree::DeleteDatabase(ConnEntry* e, const wxString& db)
{
    if (!e || !e->IsConnected()) return;
    if (wxMessageBox(tr(L"确定删除数据库?此操作不可恢复。") + L"\n\n" + db,
                     tr(L"删除数据库"), wxYES_NO | wxICON_WARNING | wxNO_DEFAULT,
                     dialogParent_) != wxYES)
        return;
    const wxString q = (e->conn->GetDialect() == db::Dialect::MySQL) ? L"`" : L"\"";
    db::QueryResult r; wxString err;
    // PG refuses to drop the database the session is attached to — that error is
    // surfaced verbatim rather than pre-guessed here.
    {
        wxBusyCursor busy;   // synchronous DROP + tree refresh on the GUI thread
        if (e->conn->Execute(L"DROP DATABASE " + q + db + q, r, err)) {
            if (e->currentDb == db) e->currentDb.clear();
            hooks_.status(tr(L"已删除数据库") + L" — " + db);
            LoadDatabases(e);   // refresh the tree
            return;
        }
    }
    wxMessageBox(tr(L"删除数据库失败:") + L"\n\n" + err, tr(L"删除数据库"),
                 wxOK | wxICON_ERROR, dialogParent_);
}

void ConnectionTree::DumpDatabase(ConnEntry* e, const wxString& db, bool withData)
{
    if (!e || !e->IsConnected()) return;
    wxString err;
    std::vector<db::TableInfo> tables;
    if (!e->conn->ListTables(db, tables, err)) {
        wxMessageBox(tr(L"读取表失败:") + L"\n\n" + err, tr(L"转储 SQL 文件"),
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }
    if (tables.empty()) {
        wxMessageBox(tr(L"该数据库没有可导出的表。"), tr(L"转储 SQL 文件"),
                     wxOK | wxICON_INFORMATION, dialogParent_);
        return;
    }

    wxFileDialog fd(dialogParent_, tr(L"转储 SQL 文件"), wxEmptyString, db + L".sql",
                    tr(L"SQL 文件 (*.sql)|*.sql|所有文件 (*.*)|*.*"),
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK) return;

    // The serialization (per-table DDL, streamed INSERT rows, views/routines/
    // triggers) runs on a worker thread inside the dialog with a live gauge, so a
    // large dump can't freeze the window. The output format is unchanged, so the
    // dump→import round-trip with RunScriptDialog stays closed.
    DumpScriptDialog dlg(dialogParent_, e->conn.get(), e->profile.name,
                         db::InfoOf(e->profile.type).name, db, withData,
                         std::move(tables), fd.GetPath());
    dlg.ShowModal();

    if (dlg.Succeeded())
        hooks_.status(tr(L"已转储") + L" — " + fd.GetPath() + L"  (" + dlg.Summary() + L")");
    else
        hooks_.status(tr(L"转储结束") + L" — " + dlg.Summary());
}

void ConnectionTree::OpenSyncForActive(int mode)
{
    // The menu-bar entry carries no right-clicked node, so it syncs whatever
    // database is currently active. Guide the user rather than opening a wizard
    // with an empty (locked) source.
    ConnEntry* e = active_;
    if (!e || !e->IsConnected() || e->currentDb.IsEmpty()) {
        hooks_.status(tr(L"请先在左侧打开一个数据库,再使用数据同步"));
        return;
    }
    SyncDatabase(e, e->currentDb, mode);
}

void ConnectionTree::SyncDatabase(ConnEntry* e, const wxString& db, int mode)
{
    if (!e || !e->IsConnected()) return;

    // Candidate targets = every currently-connected connection (the source
    // included — the wizard gates same-db and cross-engine picks itself).
    std::vector<ConnEntry*> targets;
    for (auto& up : conns_)
        if (up->IsConnected()) targets.push_back(up.get());

    SyncWizardDialog dlg(dialogParent_, e, db, std::move(targets),
                         static_cast<SyncWizardDialog::Mode>(mode));
    dlg.ShowModal();
    if (dlg.DidRun()) {
        hooks_.status(tr(L"同步结束") + L" — " + db);
        LoadDatabases(e);   // structure/data may have changed under the target
    }
}

void ConnectionTree::ExecuteSqlFile(ConnEntry* e, const wxString& db)
{
    if (!e || !e->IsConnected()) return;
    active_ = e; e->currentDb = db;

    // A full script runner: file + encoding + error-handling config, then a live
    // progress step off a worker thread (so a large import can't freeze the UI),
    // with per-failure copy/export and a gated AI-diagnose placeholder. File I/O,
    // dialect-aware splitting and threading all live in the dialog.
    // TODO: gate `aiConfigured` on a real AI settings key once one exists; the
    // app has no AI config store yet, so the placeholder is offered for now.
    // Pass the *real* database name (not a host fallback): the dialog issues a
    // USE on it before running so an unqualified dump imports into this database.
    RunScriptDialog dlg(dialogParent_, e->conn.get(), e->profile.name, db,
                        /*aiConfigured*/ true);
    dlg.ShowModal();

    if (dlg.DidRun()) {
        hooks_.status(tr(L"已执行 SQL 脚本") + L" — " + e->profile.name);
        LoadDatabases(e);   // structure/data may have changed — refresh the subtree
    }
}

// Capture a table into the cross-connection clipboard. Only coordinates are
// stored; the DDL and rows are re-fetched at paste time so nothing goes stale.
void ConnectionTree::CopyTable(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || table.IsEmpty()) return;
    tableClip_ = { e, db, table };
    hooks_.status(tr(L"已复制表") + L" — " + db + L"." + table);
}

// Recreate the clipboard table (structure + data) into `dstDb`, possibly across
// connections. Assembles a CREATE+INSERT script to a temp file, then runs it via
// the async RunScriptDialog (progress + USE dstDb + failure reporting).
void ConnectionTree::PasteTable(ConnEntry* dst, const wxString& dstDb)
{
    if (!dst || !dst->IsConnected() || tableClip_.table.IsEmpty()) return;

    // The source connection must still be open — the clipboard holds a raw
    // pointer, so validate it against the live list before touching it.
    ConnEntry* src = tableClip_.entry;
    bool srcOk = false;
    for (const auto& up : conns_)
        if (up.get() == src) { srcOk = src->IsConnected(); break; }
    if (!srcOk) {
        wxMessageBox(tr(L"源连接已关闭，剪贴板中的表不可用。"), tr(L"粘贴表"),
                     wxOK | wxICON_WARNING, dialogParent_);
        tableClip_ = {};
        return;
    }

    // Cross-dialect DDL is usually incompatible (MySQL ↔ PG syntax) — warn, don't block.
    if (src->conn->GetDialect() != dst->conn->GetDialect() &&
        wxMessageBox(tr(L"源库与目标库引擎不同，表结构语句可能不兼容。仍要继续?"),
                     tr(L"粘贴表"), wxYES_NO | wxICON_WARNING | wxNO_DEFAULT,
                     dialogParent_) != wxYES)
        return;

    wxString ddl, err;
    if (!src->conn->GetCreateDdl(tableClip_.db, tableClip_.table, ddl, err)) {
        wxMessageBox(tr(L"读取源表结构失败:") + L"\n\n" + err, tr(L"粘贴表"),
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }

    // Resolve a name collision in the target by prompting (default <name>_copy).
    std::vector<db::TableInfo> existing;
    dst->conn->ListTables(dstDb, existing, err);
    auto taken = [&](const wxString& n) {
        for (const auto& t : existing) if (t.name == n) return true;
        return false;
    };
    wxString target = tableClip_.table;
    if (taken(target)) {
        wxString sug = target + L"_copy";
        for (int i = 2; taken(sug); ++i) sug = target + wxString::Format(L"_copy%d", i);
        target = GetTextCentered(dialogParent_,
                     tr(L"目标数据库已存在同名表，请输入新表名:"), tr(L"粘贴表"), sug);
        if (target.IsEmpty()) return;
    }

    const db::Dialect dstD = dst->conn->GetDialect();
    const bool  mysql  = (dstD == db::Dialect::MySQL);
    const bool  rename = (target != tableClip_.table);
    const wxString qOrig = db::QuoteIdent(tableClip_.table, dstD);
    const wxString qNew  = db::QuoteIdent(target, dstD);

    // Build the paste script into a temp file (buffered/streamed → flat memory).
    const wxString path = wxFileName::CreateTempFileName(L"swiftsql_paste_");
    if (path.IsEmpty()) return;
    {
        wxFile f(path, wxFile::write);
        if (!f.IsOpened()) {
            wxMessageBox(tr(L"无法创建临时文件。"), tr(L"粘贴表"),
                         wxOK | wxICON_ERROR, dialogParent_);
            return;
        }
        wxString buf; bool werr = false;
        auto flush = [&](bool force) {
            if (werr || buf.IsEmpty()) return;
            if (force || buf.length() >= (1u << 18)) {
                if (!f.Write(buf, wxConvUTF8)) werr = true;
                buf.clear();
            }
        };
        auto emit = [&](const wxString& s) { buf += s; flush(false); };

        emit(L"-- SwiftSQL 粘贴表 — " + tableClip_.db + L"." + tableClip_.table +
             L"  →  " + dstDb + L"." + target + L"\n");
        if (mysql) emit(L"SET FOREIGN_KEY_CHECKS=0;\n");
        wxString ddl2 = ddl.Trim();
        if (rename) ddl2.Replace(qOrig, qNew);   // retarget the table name in the DDL
        emit(ddl2 + (mysql ? L";\n" : L"\n"));

        {   // stream the source rows as INSERTs (the read may take a moment)
            wxBusyCursor busy;
            auto rowEmit = [&](const wxString& s) {
                if (!rename) { emit(s); return; }
                wxString r = s; r.Replace(qOrig, qNew); emit(r);   // INSERT INTO <name> …
            };
            wxString derr;
            src->conn->DumpTableData(tableClip_.db, tableClip_.table, rowEmit, derr);
        }
        if (mysql) emit(L"\nSET FOREIGN_KEY_CHECKS=1;\n");
        flush(/*force*/ true);
        if (werr) {
            wxMessageBox(tr(L"写入临时文件出错。"), tr(L"粘贴表"),
                         wxOK | wxICON_ERROR, dialogParent_);
            wxRemoveFile(path);
            return;
        }
    }

    active_ = dst; dst->currentDb = dstDb;
    RunScriptDialog dlg(dialogParent_, dst->conn.get(), dst->profile.name, dstDb,
                        /*aiConfigured*/ true, path);
    dlg.ShowModal();
    wxRemoveFile(path);
    if (dlg.DidRun()) {
        hooks_.status(tr(L"已粘贴表") + L" — " + target);
        LoadDatabases(dst);   // refresh so the pasted table shows up
    }
}

} // namespace ui
