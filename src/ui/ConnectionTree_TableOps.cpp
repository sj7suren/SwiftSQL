// ConnectionTree_TableOps.cpp — table-leaf context-menu operations (drop / empty /
// rename / export / copy-name / copy-DDL / object-info / batch-drop) plus the name
// qualifier and category refresh, split out of ConnectionTree.cpp per the ≤1000-line
// charter (docs/CHARTER.md). The method bodies below are the exact originals;
// behavior is unchanged.
#include "ui/ConnectionTree.h"
#include "ui/ConnectionTreeInternal.h"

#include <wx/wx.h>
#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>
#include <algorithm>

#include "ui/CenteredDialog.h"
#include "ui/DumpScriptDialog.h"
#include "ui/ObjectTemplates.h"   // ObjectKind (DropObject)
#include "ui/I18n.h"

namespace ui {

// ---------------------------------------------------------------------------
// Table-leaf context-menu operations.
// ---------------------------------------------------------------------------

// MySQL's session database is fixed at connect time and is NOT the tree node's
// db, so a bare `t` in DROP/TRUNCATE/RENAME could hit the wrong schema — qualify
// as `db`.`table`. PG/SQLite/SqlServer connections are single-database (session
// already on the right db), so a plain quoted table name is correct there.
wxString ConnectionTree::QualifiedTable(db::IConnection* c, const wxString& db,
                                        const wxString& table) const
{
    const db::Dialect d = c->GetDialect();
    if (d == db::Dialect::MySQL)
        return db::QuoteIdent(db, d) + L"." + db::QuoteIdent(table, d);
    return db::QuoteIdent(table, d);
}

// Reload the category folder a table lives under, so the tree reflects reality
// after a drop / rename. `tableItem`'s parent is the category node.
void ConnectionTree::RefreshTableCategory(const wxTreeItemId& tableItem)
{
    if (!tableItem.IsOk()) return;
    wxTreeItemId parent = tree_->GetItemParent(tableItem);
    if (!parent.IsOk()) return;
    if (auto* pdata = dynamic_cast<NodeData*>(tree_->GetItemData(parent)))
        pdata->loaded = false;
    LoadCategoryMembers(parent);
    tree_->Expand(parent);
}

void ConnectionTree::DropTable(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;
    if (wxMessageBox(tr(L"确定删除表") + L" " + table + L"? " +
                     tr(L"此操作不可恢复。"), tr(L"删除表"),
                     wxYES_NO | wxICON_WARNING | wxNO_DEFAULT, dialogParent_) != wxYES)
        return;
    db::QueryResult r; wxString err;
    {
        wxBusyCursor busy;   // synchronous DROP on the GUI thread — show loading cursor
        if (e->conn->Execute(L"DROP TABLE " + QualifiedTable(e->conn.get(), db, table), r, err)) {
            GroupTrackDrop(e, db, Category::Tables, table);   // forget its 分组 membership
            hooks_.status(tr(L"已删除表") + L" — " + db + L"." + table);
            return;
        }
    }
    wxMessageBox(tr(L"删除表失败:") + L"\n\n" + err, tr(L"删除表"),
                 wxOK | wxICON_ERROR, dialogParent_);
}

// Drop a view / routine / package (context menu) — DROP <kind> <qualified name> with a
// confirm, mirroring DropTable. The keyword is per ObjectKind; the name is db-qualified
// (MySQL needs `db`.`name`) via the same QualifiedTable helper.
void ConnectionTree::DropObject(ConnEntry* e, const wxString& db, const wxString& name,
                                ObjectKind kind)
{
    if (!e || !e->IsConnected() || name.IsEmpty()) return;
    wxString keyword, title;
    switch (kind) {
    case ObjectKind::View:      keyword = L"VIEW";      title = tr(L"删除视图");     break;
    case ObjectKind::Function:  keyword = L"FUNCTION";  title = tr(L"删除函数");     break;
    case ObjectKind::Procedure: keyword = L"PROCEDURE"; title = tr(L"删除存储过程"); break;
    case ObjectKind::Package:   keyword = L"PACKAGE";   title = tr(L"删除包");       break;
    default: return;
    }
    if (wxMessageBox(tr(L"确定删除") + L" " + name + L"? " + tr(L"此操作不可恢复。"),
                     title, wxYES_NO | wxICON_WARNING | wxNO_DEFAULT, dialogParent_) != wxYES)
        return;
    db::QueryResult r; wxString err;
    {
        wxBusyCursor busy;   // synchronous DROP on the GUI thread — show loading cursor
        if (e->conn->Execute(L"DROP " + keyword + L" " +
                             QualifiedTable(e->conn.get(), db, name), r, err)) {
            GroupTrackDrop(e, db, kind, name);   // forget its 分组 membership
            hooks_.status(title + L" — " + db + L"." + name);
            return;
        }
    }
    wxMessageBox(err, title, wxOK | wxICON_ERROR, dialogParent_);
}

void ConnectionTree::EmptyTable(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;
    if (wxMessageBox(tr(L"确定清空表") + L" " + table + L" " +
                     tr(L"的所有数据?"), tr(L"清空表"),
                     wxYES_NO | wxICON_WARNING | wxNO_DEFAULT, dialogParent_) != wxYES)
        return;
    // SQLite has no TRUNCATE — DELETE FROM clears every row instead.
    const wxString qt = QualifiedTable(e->conn.get(), db, table);
    const wxString sql = (e->conn->GetDialect() == db::Dialect::Sqlite)
                             ? L"DELETE FROM " + qt
                             : L"TRUNCATE TABLE " + qt;
    db::QueryResult r; wxString err;
    {
        wxBusyCursor busy;   // TRUNCATE/DELETE can block on a large table — loading cursor
        if (e->conn->Execute(sql, r, err)) {
            hooks_.status(tr(L"已清空表") + L" — " + db + L"." + table);
            return;
        }
    }
    wxMessageBox(tr(L"清空表失败:") + L"\n\n" + err, tr(L"清空表"),
                 wxOK | wxICON_ERROR, dialogParent_);
}

void ConnectionTree::RenameTable(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;
    const wxString newName = GetTextCentered(dialogParent_, tr(L"输入新表名:"),
                                             tr(L"重命名"), table);
    if (newName.IsEmpty() || newName == table) return;

    const db::Dialect d = e->conn->GetDialect();
    wxString sql;
    switch (d) {
    case db::Dialect::MySQL:
        // RENAME TABLE `db`.`old` TO `db`.`new`
        sql = L"RENAME TABLE " + QualifiedTable(e->conn.get(), db, table) +
              L" TO " + db::QuoteIdent(db, d) + L"." + db::QuoteIdent(newName, d);
        break;
    case db::Dialect::SqlServer:
        // sp_rename takes the unquoted old/new names (new has no schema prefix).
        sql = L"EXEC sp_rename '" + table + L"', '" + newName + L"'";
        break;
    default:   // Postgres / SQLite: ALTER TABLE <old> RENAME TO <new>
        sql = L"ALTER TABLE " + QualifiedTable(e->conn.get(), db, table) +
              L" RENAME TO " + db::QuoteIdent(newName, d);
        break;
    }
    db::QueryResult r; wxString err;
    {
        wxBusyCursor busy;   // synchronous rename on the GUI thread — show loading cursor
        if (e->conn->Execute(sql, r, err)) {
            // Carry its 分组 membership, so a renamed table stays in the folder
            // the user put it in instead of quietly reappearing at top level.
            GroupTrackRename(e, db, Category::Tables, table, newName);
            hooks_.status(tr(L"已重命名表") + L" — " + table + L" → " + newName);
            return;
        }
    }
    wxMessageBox(tr(L"重命名失败:") + L"\n\n" + err, tr(L"重命名"),
                 wxOK | wxICON_ERROR, dialogParent_);
}

// Rename a view / function / procedure. Tables keep RenameTable (RENAME/ALTER TABLE);
// non-table objects can't use RENAME TABLE (MySQL errors), so they route here.
//
// Strategy per dialect × kind:
//   • PostgreSQL — native `ALTER {VIEW|FUNCTION|PROCEDURE} old RENAME TO new`. An
//     OVERLOADED routine needs its argument signature; PG then rejects the bare form
//     as ambiguous and that error is shown (we don't guess the signature).
//   • SQL Server — `EXEC sp_rename 'old','new'` (old may be schema.name; new is bare).
//   • MySQL (view/func/proc) & SQLite (view) — no native rename → REBUILD: fetch the
//     DDL, rewrite the name, CREATE the new object FIRST, then DROP the old one. The
//     original survives every failure path (CREATE-before-DROP), so a rename can
//     never lose the object.
//   • Oracle / DM — routines (PL/SQL END-label, '/' terminator, owner scope) aren't
//     safe to rebuild blind yet → politely declined, never mis-executed.
void ConnectionTree::RenameObject(ConnEntry* e, const wxString& db,
                                  const wxString& name, ObjectKind kind)
{
    if (!e || !e->IsConnected() || name.IsEmpty()) return;
    if (kind != ObjectKind::View && kind != ObjectKind::Function &&
        kind != ObjectKind::Procedure)
        return;   // packages / other kinds aren't renamed through this path

    const db::Dialect d = e->conn->GetDialect();

    // The prompt defaults to the BARE name (SQL Server stores objects as schema.name).
    wxString bare = name;
    if (bare.Contains(L".")) bare = bare.AfterLast('.');
    const wxString newName = GetTextCentered(dialogParent_, tr(L"输入新名称:"),
                                             tr(L"重命名"), bare);
    if (newName.IsEmpty() || newName == bare) return;

    const wxString title = tr(L"重命名");
    db::QueryResult r;
    wxString err;

    // ---- Native rename (PostgreSQL: ALTER … RENAME) ----
    if (d == db::Dialect::Postgres) {
        const wxString kw = (kind == ObjectKind::View)      ? L"VIEW"
                          : (kind == ObjectKind::Procedure) ? L"PROCEDURE"
                                                            : L"FUNCTION";
        const wxString sql = L"ALTER " + kw + L" " + db::QuoteIdent(bare, d) +
                             L" RENAME TO " + db::QuoteIdent(newName, d);
        if (!e->conn->Execute(sql, r, err)) {
            wxMessageBox(tr(L"重命名失败:") + L"\n\n" + err, title,
                         wxOK | wxICON_ERROR, dialogParent_);
            return;
        }
        GroupTrackRename(e, db, kind, bare, newName);   // 分组 follows the rename
        hooks_.status(tr(L"已重命名") + L" — " + bare + L" → " + newName);
        return;
    }

    // ---- Native rename (SQL Server: sp_rename) ----
    if (d == db::Dialect::SqlServer) {
        // Old operand may be schema-qualified (node stores schema.name); the new
        // operand must be the bare name — sp_rename keeps the existing schema.
        const wxString sql = L"EXEC sp_rename '" + name + L"', '" + newName + L"'";
        if (!e->conn->Execute(sql, r, err)) {
            wxMessageBox(tr(L"重命名失败:") + L"\n\n" + err, title,
                         wxOK | wxICON_ERROR, dialogParent_);
            return;
        }
        GroupTrackRename(e, db, kind, bare, newName);   // 分组 follows the rename
        hooks_.status(tr(L"已重命名") + L" — " + bare + L" → " + newName);
        return;
    }

    // ---- Rebuild (no native rename): MySQL view/func/proc, SQLite view ----
    const bool rebuildOk = (d == db::Dialect::MySQL) ||
                           (d == db::Dialect::Sqlite && kind == ObjectKind::View);
    if (!rebuildOk) {
        wxMessageBox(tr(L"该数据库暂不支持此类对象重命名"), title,
                     wxOK | wxICON_INFORMATION, dialogParent_);
        return;
    }

    // 1) Fetch the current definition.
    wxString ddl;
    bool got = false;
    if (kind == ObjectKind::View) {
        got = e->conn->GetViewDdl(db, bare, ddl, err);
    } else {
        db::RoutineInfo ri;
        ri.name = bare;
        ri.type = (kind == ObjectKind::Procedure) ? L"PROCEDURE" : L"FUNCTION";
        got = e->conn->GetRoutineDdl(db, ri, ddl, err);
    }
    if (!got || ddl.IsEmpty()) {
        wxMessageBox(tr(L"读取对象定义失败:") + L"\n\n" + err, title,
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }

    // 2) Rewrite the DDL to CREATE under the new (db-qualified) name. A failure to
    //    locate the object name aborts before touching anything.
    const wxString newQualified = QualifiedTable(e->conn.get(), db, newName);
    wxString createSql;
    if (!BuildRenamedCreateDdl(kind, d, ddl, newQualified, createSql)) {
        wxMessageBox(tr(L"该数据库暂不支持此类对象重命名"), title,
                     wxOK | wxICON_INFORMATION, dialogParent_);
        return;
    }

    // 3) CREATE-first (safe order). If this fails (e.g. the new name already exists),
    //    the original object is untouched → abort with the error.
    if (!e->conn->Execute(createSql, r, err)) {
        wxMessageBox(tr(L"重命名失败:") + L"\n\n" + err, title,
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }

    // 4) DROP the old object. If this fails the new object already exists (no data
    //    loss) — warn honestly rather than claim full success.
    const wxString kw = (kind == ObjectKind::View)      ? L"VIEW"
                      : (kind == ObjectKind::Procedure) ? L"PROCEDURE"
                                                        : L"FUNCTION";
    db::QueryResult r2;
    wxString err2;
    if (!e->conn->Execute(L"DROP " + kw + L" " + QualifiedTable(e->conn.get(), db, bare),
                          r2, err2)) {
        wxMessageBox(tr(L"新对象已创建，但删除旧对象失败:") + L"\n\n" + err2, title,
                     wxOK | wxICON_WARNING, dialogParent_);
        return;
    }
    hooks_.status(tr(L"已重命名") + L" — " + bare + L" → " + newName);
}

// Single-table dump — same async DumpScriptDialog as DumpDatabase, seeded with a
// one-element table list, so the progress bar / no-freeze come for free.
void ConnectionTree::ExportTable(ConnEntry* e, const wxString& db,
                                 const wxString& table, bool withData)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;

    std::vector<db::TableInfo> tables{ { table, wxString() } };
    wxFileDialog fd(dialogParent_, tr(L"导出表"), wxEmptyString, table + L".sql",
                    tr(L"SQL 文件 (*.sql)|*.sql|所有文件 (*.*)|*.*"),
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK) return;

    DumpScriptDialog dlg(dialogParent_, e->conn.get(), e->profile.name,
                         db::InfoOf(e->profile.type).name, db, withData,
                         std::move(tables), fd.GetPath());
    dlg.ShowModal();

    if (dlg.Succeeded())
        hooks_.status(tr(L"已导出表") + L" — " + fd.GetPath() + L"  (" + dlg.Summary() + L")");
    else
        hooks_.status(tr(L"导出结束") + L" — " + dlg.Summary());
}

void ConnectionTree::CopyTableName(const wxString& table)
{
    if (table.IsEmpty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(table));
        wxTheClipboard->Close();
        hooks_.status(tr(L"已复制表名") + L" — " + table);
    }
}

void ConnectionTree::CopyTableDdl(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;
    wxString ddl, err;
    if (!e->conn->GetCreateDdl(db, table, ddl, err)) {
        wxMessageBox(tr(L"读取表结构失败:") + L"\n\n" + err, tr(L"复制 CREATE 语句"),
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(ddl));
        wxTheClipboard->Close();
        hooks_.status(tr(L"已复制 CREATE 语句") + L" — " + table);
    }
}

// A lightweight, non-freezing summary: metadata only (no COUNT(*), which could
// scan a huge table). One quick GetColumns call fills field count + names.
void ConnectionTree::ObjectInfo(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;
    std::vector<db::ColumnInfo> cols; wxString err;
    if (!e->conn->GetColumns(db, table, cols, err)) {
        wxMessageBox(tr(L"读取表信息失败:") + L"\n\n" + err, tr(L"对象信息"),
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }
    wxString msg;
    msg << tr(L"表名") << L": " << table << L"\n"
        << tr(L"数据库") << L": " << db << L"\n"
        << tr(L"引擎") << L": " << db::InfoOf(e->profile.type).name << L"\n"
        << tr(L"字段数") << L": " << (int)cols.size();
    if (!cols.empty()) {
        msg << L"\n\n" << tr(L"字段") << L": ";
        const size_t shown = std::min<size_t>(cols.size(), 8);
        for (size_t i = 0; i < shown; ++i)
            msg << (i ? L", " : L"") << cols[i].name;
        if (cols.size() > shown) msg << L", …";
    }
    wxMessageBox(msg, tr(L"对象信息") + L" — " + table, wxOK | wxICON_INFORMATION,
                 dialogParent_);
}

// Batch DROP for the database-overview list's multi-select delete. One confirm
// for the whole set; each name is qualified (QualifiedTable) so MySQL targets the
// right database. Refreshes the tree afterwards.
bool ConnectionTree::DropTablesUi(ConnEntry* e, const wxString& db,
                                  const std::vector<wxString>& tables)
{
    if (!e || !e->IsConnected() || tables.empty()) return false;
    const wxString msg = (tables.size() == 1)
        ? tr(L"确定删除表 ") + tables.front() + tr(L"？此操作不可恢复。")
        : wxString::Format(tr(L"确定删除选中的 %zu 张表？此操作不可恢复。"), tables.size());
    if (wxMessageBox(msg, tr(L"删除表"), wxYES_NO | wxICON_WARNING | wxNO_DEFAULT,
                     dialogParent_) != wxYES)
        return false;

    wxString err; int ok = 0;
    for (const wxString& t : tables) {
        db::QueryResult r;
        if (e->conn->Execute(L"DROP TABLE " + QualifiedTable(e->conn.get(), db, t), r, err))
            ++ok;
    }
    hooks_.status(wxString::Format(tr(L"已删除 %d/%zu 张表"), ok, tables.size()));
    LoadDatabases(e);   // structure changed — refresh the tree
    return true;
}

} // namespace ui
