// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/AiContextBuilder.h"

#include "db/DbDriver.h"
#include "ui/I18n.h"

namespace ui {

namespace {

// Table-count budget: beyond this we send names only (a full signature for
// hundreds of tables would dominate the prompt and risk a context overflow).
constexpr size_t kMaxTablesFull = 200;

// "varchar" + "80" → "varchar(80)"; length "" → just the base type. Length may
// already carry precision+scale ("10,2") — we keep it verbatim inside the parens.
wxString TypeText(const db::ColumnInfo& c)
{
    if (c.length.IsEmpty()) return c.type;
    return c.type + L"(" + c.length + L")";
}

} // namespace

wxString BuildSchemaSignature(db::IConnection* conn, const wxString& database,
                              wxString& err)
{
    if (!conn) {
        err = tr(L"数据库连接不可用");
        return {};
    }

    std::vector<db::TableInfo> tables;
    if (!conn->ListTables(database, tables, err)) return {};

    wxString out;

    // Over budget → bare names + a truncation note, so the prompt stays bounded.
    if (tables.size() > kMaxTablesFull) {
        out << tr(L"（结构过大已截断，仅列出表名）") << L"\n";
        for (const db::TableInfo& t : tables) out << t.name << L" ";
        out.Trim(true);
        return out;
    }

    for (const db::TableInfo& t : tables) {
        std::vector<db::ColumnInfo> cols;
        wxString colErr;
        if (!conn->GetColumns(database, t.name, cols, colErr)) continue;  // skip, best-effort

        std::vector<db::ForeignKey> fks;
        wxString fkErr;
        conn->GetForeignKeys(database, t.name, fks, fkErr);   // best-effort

        wxString line = t.name + L"(";
        bool first = true;
        for (const db::ColumnInfo& c : cols) {
            if (!first) line << L", ";
            first = false;
            line << c.name << L" " << TypeText(c);
            if (c.key == L"PK") line << L" PK";
        }
        for (const db::ForeignKey& fk : fks) {
            if (!first) line << L", ";
            first = false;
            line << fk.fromColumn << L" →" << fk.toTable << L"." << fk.toColumn;
        }
        line << L")";
        out << line << L"\n";
    }

    out.Trim(true);
    return out;
}

} // namespace ui
