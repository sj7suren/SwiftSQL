// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DialectProfile.cpp — the Template-Method shells for ADR-014's dialect field
// editor. This unit owns the whole-ALTER assembly (looping a TableEdit, wrapping
// clauses in ALTER TABLE) and the non-virtual metadata helpers; the per-clause
// bodies and the type catalogs stay in the concrete FooProfile.cpp files. Nothing
// here touches a connection handle or a vendor header.
#include "db/DialectProfile.h"

namespace db {

// ---- ColumnModel ----

wxString ColumnModel::Attr(const wxString& id) const
{
    for (const auto& kv : attrs)
        if (kv.first == id) return kv.second;
    return wxString();
}

// ---- DialectProfile: shared metadata + naming helpers ----

std::vector<AttrDescriptor> DialectProfile::ApplicableAttrs(const wxString& typeName) const
{
    // Resolve the selected type's ColKind (case-insensitive on the type name).
    ColKind kind = ColKind::Other;
    bool    knownType = false;
    for (const auto& t : Types())
        if (t.name.IsSameAs(typeName, /*caseSensitive=*/false)) {
            kind = t.kind;
            knownType = true;
            break;
        }

    std::vector<AttrDescriptor> out;
    for (const auto& a : Attributes()) {
        if (a.appliesTo.empty()) {          // universal — applies to every type
            out.push_back(a);
            continue;
        }
        if (!knownType) continue;           // gated attr but type unknown → skip
        for (ColKind k : a.appliesTo)
            if (k == kind) { out.push_back(a); break; }
    }
    return out;
}

// ---- cross-engine type mapping (A3) — base defaults ----

// Generic passthrough: no dialect-specific unsigned/timezone/enum parsing, just
// what the NormColumn already carries. Concrete profiles that DO have real
// cross-engine semantics (MySQL/Postgres) override this with SyncTypeMap's
// Interpret*Column, which additionally parses UNSIGNED / TIMESTAMP-vs-DATETIME
// / ENUM member lists out of rawType.
CanonicalType DialectProfile::Interpret(const NormColumn& c) const
{
    CanonicalType t;
    t.kind    = c.kind;
    t.length  = c.length;
    t.scale   = c.scale;
    t.rawType = c.rawType;
    return t;
}

// Honest gate: a dialect that hasn't opted in (no override) can never render a
// cross-engine type, so it can never be treated as auto-alterable either —
// MayAutoAlter alone isn't enough downstream if Render always refuses.
bool DialectProfile::Render(const CanonicalType&, wxString&, wxString& why) const
{
    why = L"该方言尚未实现跨引擎类型映射";
    return false;
}

wxString DialectProfile::QualifiedTable(const TableEdit& edit) const
{
    if (edit.db.IsEmpty()) return Q(edit.table);
    return Q(edit.db) + L"." + Q(edit.table);
}

wxString DialectProfile::RenderFkBody(const ForeignKeyModel& fk) const
{
    auto joinQ = [this](const std::vector<wxString>& ids) {
        wxString out;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) out += L", ";
            out += Q(ids[i]);
        }
        return out;
    };

    wxString s;
    if (!fk.name.IsEmpty())
        s = L"CONSTRAINT " + Q(fk.name) + L" ";

    const wxString refTbl = fk.refDb.IsEmpty()
        ? Q(fk.refTable)
        : Q(fk.refDb) + L"." + Q(fk.refTable);

    s += L"FOREIGN KEY (" + joinQ(fk.columns) + L") REFERENCES " +
         refTbl + L" (" + joinQ(fk.refColumns) + L")";
    if (!fk.onDelete.IsEmpty()) s += L" ON DELETE " + fk.onDelete;
    if (!fk.onUpdate.IsEmpty()) s += L" ON UPDATE " + fk.onUpdate;
    return s;
}

// Whole-table singleton edits, shared by both RenderAlter shells. Options first
// (structure), then comment (metadata) — a stable order the golden tests rely on.
void DialectProfile::RenderTableExtras(const TableEdit& edit,
                                       std::vector<wxString>& stmts) const
{
    const wxString qt = QualifiedTable(edit);
    if (edit.hasOptions) RenderOptionsChange(qt, edit.options, stmts);
    if (edit.hasComment) RenderCommentChange(qt, edit.comment, stmts);
    for (const auto& te : edit.triggers)
        RenderTriggerChange(qt, te, stmts);
}

// Base defaults: no-op for options/comment. A dialect emits table options / a table
// comment only if it overrides these — so an unsupported dialect stays silent rather
// than emitting wrong DDL.
void DialectProfile::RenderOptionsChange(const wxString&, const TableOptions&,
                                         std::vector<wxString>&) const {}
void DialectProfile::RenderCommentChange(const wxString&, const wxString&,
                                         std::vector<wxString>&) const {}

// Base trigger rendering: the MySQL/SQLite FOR EACH ROW form. Correct for MySQL,
// MariaDB, OceanBase and SQLite; the dialects whose trigger model differs
// (SupportsTriggers()=false) override this with an honest skip note.
void DialectProfile::RenderTriggerChange(const wxString& qualifiedTable,
                                         const TriggerEdit& te,
                                         std::vector<wxString>& stmts) const
{
    if (te.op == TriggerEdit::Drop) {
        stmts.push_back(L"DROP TRIGGER " + Q(te.model.name));
        return;
    }
    const TriggerModel& m = te.model;
    stmts.push_back(L"CREATE TRIGGER " + Q(m.name) + L" " + m.timing + L" " +
                    m.event + L" ON " + qualifiedTable + L" FOR EACH ROW " + m.body);
}

// ---- InlineAlterProfile: one ALTER TABLE, comma-joined clauses (MySQL family) ----

bool InlineAlterProfile::RenderAlter(const TableEdit& edit,
                                     std::vector<wxString>& stmts, wxString& err) const
{
    (void)err;   // the shell never fails; concrete clause builders own their errors

    std::vector<wxString> clauses;
    for (const auto& ce : edit.columns) {
        switch (ce.op) {
        case ColumnEdit::Add: {
            wxString c = L"ADD COLUMN " + RenderColumnBody(ce.model);
            if (!ce.afterColumn.IsEmpty())     // positional hint (MySQL supports AFTER)
                c += L" AFTER " + Q(ce.afterColumn);
            clauses.push_back(c);
            break;
        }
        case ColumnEdit::Modify: {
            wxString c = ModifyClause(ce.model);   // MODIFY vs CHANGE, rename inline
            if (!c.IsEmpty()) {
                if (ce.positioned)                 // column reorder: FIRST / AFTER `col`
                    c += ce.afterColumn.IsEmpty() ? L" FIRST"
                                                  : (L" AFTER " + Q(ce.afterColumn));
                clauses.push_back(c);
            }
            break;
        }
        case ColumnEdit::Drop:
            clauses.push_back(L"DROP COLUMN " + Q(ce.model.name));
            break;
        }
    }

    // Index Add/Drop fold into the SAME ALTER TABLE (MySQL family). The concrete
    // owns the clause shape; "" means "this profile emits no index clause".
    for (const auto& ie : edit.indexes) {
        const wxString c = IndexClause(ie);
        if (!c.IsEmpty()) clauses.push_back(c);
    }

    // FK Add/Drop likewise fold into the one ALTER TABLE.
    for (const auto& fe : edit.fks) {
        const wxString c = FkClause(fe);
        if (!c.IsEmpty()) clauses.push_back(c);
    }

    // Column/index/FK clauses (if any) become one comma-joined ALTER TABLE.
    if (!clauses.empty()) {
        wxString sql = L"ALTER TABLE " + QualifiedTable(edit) + L" ";
        for (size_t i = 0; i < clauses.size(); ++i) {
            if (i) sql += L", ";
            sql += clauses[i];
        }
        stmts.push_back(sql);
    }

    // Whole-table options / comment are separate statements (even for MySQL), so
    // they render after — and independently of — the column/index/FK ALTER.
    RenderTableExtras(edit, stmts);
    return true;
}

// MySQL-family FK clause: ADD <body> / DROP FOREIGN KEY <name>, folded into the one
// ALTER TABLE. Uniform across MySQL/MariaDB/OceanBase, so the base owns it.
wxString InlineAlterProfile::FkClause(const ForeignKeyEdit& fe) const
{
    if (fe.op == ForeignKeyEdit::Drop)
        return L"DROP FOREIGN KEY " + Q(fe.model.name);
    return L"ADD " + RenderFkBody(fe.model);
}

// ---- SeparateAlterProfile: one ALTER TABLE per change (PG/SQLServer/Oracle/SQLite) ----

// Default ADD verb: "ADD COLUMN <body>" (PG/SQLite). SQL Server / Oracle override.
wxString SeparateAlterProfile::AddColumnClause(const ColumnModel& c) const
{
    return L"ADD COLUMN " + RenderColumnBody(c);
}

bool SeparateAlterProfile::RenderAlter(const TableEdit& edit,
                                       std::vector<wxString>& stmts, wxString& err) const
{
    (void)err;

    const wxString qt = QualifiedTable(edit);
    for (const auto& ce : edit.columns) {
        switch (ce.op) {
        case ColumnEdit::Add:   // verb varies (ADD COLUMN vs ADD); delegate the clause
            stmts.push_back(L"ALTER TABLE " + qt + L" " + AddColumnClause(ce.model));
            break;
        case ColumnEdit::Modify:   // wholesale to the concrete: rename + type/attr,
            RenderColumnChange(qt, ce.model, stmts);   // whole statements, unwrapped
            break;
        case ColumnEdit::Drop:
            stmts.push_back(L"ALTER TABLE " + qt + L" DROP COLUMN " + Q(ce.model.name));
            break;
        }
    }

    // Indexes are standalone statements here (CREATE INDEX / DROP INDEX), not ALTER
    // TABLE clauses — delegate each to the concrete (or the ANSI default below).
    for (const auto& ie : edit.indexes)
        RenderIndexChange(qt, ie, stmts);

    // FKs are standalone ALTER TABLE ADD/DROP CONSTRAINT statements here.
    for (const auto& fe : edit.fks)
        RenderFkChange(qt, fe, stmts);

    // Whole-table options / comment (COMMENT ON TABLE …) as separate statements.
    RenderTableExtras(edit, stmts);
    return true;
}

// ANSI default index rendering for the standalone-ALTER dialects. Handles the two
// portable shapes (plain + UNIQUE); dialect-only kinds (FULLTEXT/SPATIAL), storage
// methods (PG `USING gin`), and SQL Server's `DROP INDEX <name> ON <t>` are left to
// concrete overrides — this base stays strictly standard SQL.
void SeparateAlterProfile::RenderIndexChange(const wxString& qualifiedTable,
                                             const IndexEdit& ie,
                                             std::vector<wxString>& stmts) const
{
    if (ie.op == IndexEdit::Drop) {
        stmts.push_back(L"DROP INDEX " + Q(ie.model.name));
        return;
    }

    wxString cols;
    for (size_t i = 0; i < ie.model.columns.size(); ++i) {
        if (i) cols += L", ";
        cols += Q(ie.model.columns[i]);
    }

    const bool unique = ie.model.type.IsSameAs(L"UNIQUE", /*caseSensitive=*/false);
    stmts.push_back(wxString(L"CREATE ") + (unique ? L"UNIQUE " : L"") +
                    L"INDEX " + Q(ie.model.name) + L" ON " + qualifiedTable +
                    L" (" + cols + L")");
}

// ANSI default FK rendering for the standalone-ALTER dialects: standard
// ADD/DROP CONSTRAINT. Correct for PG / SQL Server / Oracle; SQLite (no ALTER-add
// of FKs) overrides this to an honest skip note.
void SeparateAlterProfile::RenderFkChange(const wxString& qualifiedTable,
                                          const ForeignKeyEdit& fe,
                                          std::vector<wxString>& stmts) const
{
    if (fe.op == ForeignKeyEdit::Drop) {
        stmts.push_back(L"ALTER TABLE " + qualifiedTable +
                        L" DROP CONSTRAINT " + Q(fe.model.name));
        return;
    }
    stmts.push_back(L"ALTER TABLE " + qualifiedTable + L" ADD " +
                    RenderFkBody(fe.model));
}

// Generic CREATE TABLE for the standalone-ALTER dialects: columns + PRIMARY KEY +
// inline FK constraints inside the CREATE, then secondary indexes / table comment /
// triggers as separate follow-on statements (reusing the RenderAlter hooks — so a
// dialect's index/comment/trigger quirks and honest-skips apply uniformly here too).
bool SeparateAlterProfile::RenderCreate(const TableModel& model,
                                        std::vector<wxString>& stmts, wxString& err) const
{
    (void)err;
    const wxString qt = model.db.IsEmpty()
        ? Q(model.table)
        : Q(model.db) + L"." + Q(model.table);

    std::vector<wxString> lines;
    for (const auto& c : model.columns)
        lines.push_back(RenderColumnBody(c));

    if (!model.primaryKey.empty()) {
        wxString pk;
        for (size_t i = 0; i < model.primaryKey.size(); ++i) {
            if (i) pk += L", ";
            pk += Q(model.primaryKey[i]);
        }
        lines.push_back(L"PRIMARY KEY (" + pk + L")");
    }

    for (const auto& fk : model.fks)
        lines.push_back(RenderFkBody(fk));

    wxString sql = L"CREATE TABLE " + qt + L" (\n";
    for (size_t i = 0; i < lines.size(); ++i)
        sql += L"  " + lines[i] + (i + 1 < lines.size() ? L",\n" : L"\n");
    sql += L")";
    stmts.push_back(sql);

    // Follow-on statements: secondary indexes (standalone CREATE INDEX), the table
    // comment, then triggers — each via the same per-dialect hook used by RenderAlter.
    for (const auto& idx : model.indexes) {
        IndexEdit ie; ie.op = IndexEdit::Add; ie.model = idx;
        RenderIndexChange(qt, ie, stmts);
    }
    if (!model.comment.IsEmpty())
        RenderCommentChange(qt, model.comment, stmts);
    for (const auto& tg : model.triggers) {
        TriggerEdit te; te.op = TriggerEdit::Add; te.model = tg;
        RenderTriggerChange(qt, te, stmts);
    }
    return true;
}

} // namespace db
