// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SqliteProfile.cpp — the SQLite DialectProfile (ADR-014 T5). Inherits
// SeparateAlterProfile: Add/Drop are base-owned (uniform ALTER TABLE … ADD/DROP
// COLUMN). SQLite's ALTER is deliberately minimal — it supports only ADD COLUMN and
// RENAME COLUMN, NOT changing a column's type or constraints — so RenderColumnChange
// is HONEST: it emits RENAME COLUMN when the name changed and a "-- SQLite …" skip
// note for any type/constraint change (mirrors the honest degradation in
// SqliteDriver.cpp:616's RenderSchemaChange). It never emits invalid ALTER COLUMN.
// SQLite is dynamically typed (column affinity), so type names are cosmetic and are
// emitted verbatim rather than alias-resolved. No connection handle here.
#include "db/DialectProfile.h"

namespace db {
namespace {

class SqliteProfile : public SeparateAlterProfile {
public:
    Dialect GetDialect() const override { return Dialect::Sqlite; }
    const std::vector<TypeDescriptor>& Types() const override;
    const std::vector<AttrDescriptor>& Attributes() const override;
    wxString RenderTypeSpec(const ColumnModel& c) const override;
    wxString RenderColumnBody(const ColumnModel& c) const override;

protected:
    void RenderColumnChange(const wxString& qualifiedTable, const ColumnModel& c,
                            std::vector<wxString>& stmts) const override;
    void RenderFkChange(const wxString& qualifiedTable, const ForeignKeyEdit& fe,
                        std::vector<wxString>& stmts) const override;

private:
    const TypeDescriptor* FindType(const wxString& name) const;
};

// ---- type catalog: {name, kind, takesLength, takesScale, lengthRequired, category, aliasOf} ----
// The five storage classes / affinity canonicals, plus the common cross-engine
// synonyms SQLite recognizes via its affinity rules. takesLength is mostly false
// (SQLite ignores length), but the character/decimal spellings allow writing one so
// a pasted "varchar(80)" round-trips. aliasOf points at the affinity canonical.
const std::vector<TypeDescriptor>& SqliteProfile::Types() const
{
    static const std::vector<TypeDescriptor> kTypes = {
        // affinity canonicals
        { L"INTEGER", ColKind::Integer, false, false, false, L"数值",   L"" },
        { L"REAL",    ColKind::Float,   false, false, false, L"数值",   L"" },
        { L"NUMERIC", ColKind::Decimal, true,  true,  false, L"数值",   L"" },
        { L"TEXT",    ColKind::Text,    false, false, false, L"字符",   L"" },
        { L"BLOB",    ColKind::Blob,    false, false, false, L"二进制", L"" },
        // integer synonyms → INTEGER affinity
        { L"int",       ColKind::Integer, false, false, false, L"数值", L"INTEGER" },
        { L"integer",   ColKind::Integer, false, false, false, L"数值", L"INTEGER" },
        { L"tinyint",   ColKind::Integer, false, false, false, L"数值", L"INTEGER" },
        { L"smallint",  ColKind::Integer, false, false, false, L"数值", L"INTEGER" },
        { L"mediumint", ColKind::Integer, false, false, false, L"数值", L"INTEGER" },
        { L"bigint",    ColKind::Integer, false, false, false, L"数值", L"INTEGER" },
        // real/decimal synonyms
        { L"double",    ColKind::Float,   false, false, false, L"数值", L"REAL" },
        { L"float",     ColKind::Float,   false, false, false, L"数值", L"REAL" },
        { L"decimal",   ColKind::Decimal, true,  true,  false, L"数值", L"NUMERIC" },
        // boolean has no storage class → NUMERIC affinity (0/1)
        { L"boolean",   ColKind::Boolean, false, false, false, L"布尔", L"NUMERIC" },
        { L"bool",      ColKind::Boolean, false, false, false, L"布尔", L"NUMERIC" },
        // character synonyms → TEXT affinity
        { L"varchar",   ColKind::Varchar, true,  false, false, L"字符", L"TEXT" },
        { L"nvarchar",  ColKind::Varchar, true,  false, false, L"字符", L"TEXT" },
        { L"char",      ColKind::Char,    true,  false, false, L"字符", L"TEXT" },
        { L"clob",      ColKind::Text,    false, false, false, L"字符", L"TEXT" },
        // date/time have no storage class → NUMERIC affinity (stored as text/num)
        { L"date",      ColKind::Date,      false, false, false, L"日期时间", L"NUMERIC" },
        { L"datetime",  ColKind::Timestamp, false, false, false, L"日期时间", L"NUMERIC" },
        { L"timestamp", ColKind::Timestamp, false, false, false, L"日期时间", L"NUMERIC" },
    };
    return kTypes;
}

// ---- attribute catalog: {id, label, editor, choices, appliesTo, core} ----
// SQLite has very few column properties: no unsigned/charset/collation-as-attr, and
// NO native column comment (omitted rather than offering a control that does
// nothing). autoIncrement is meaningful only on an INTEGER PRIMARY KEY.
const std::vector<AttrDescriptor>& SqliteProfile::Attributes() const
{
    using K = ColKind;
    static const std::vector<AttrDescriptor> kAttrs = {
        { L"autoIncrement", L"自增", AttrDescriptor::Bool, {}, { K::Integer }, true },
        { L"generated",     L"生成列", AttrDescriptor::Expr, {}, {}, true },
    };
    return kAttrs;
}

const TypeDescriptor* SqliteProfile::FindType(const wxString& name) const
{
    for (const auto& t : Types())
        if (t.name.IsSameAs(name, /*caseSensitive=*/false)) return &t;
    return nullptr;
}

// SQLite is dynamically typed: the declared type is just an affinity hint, so we
// emit exactly what the user typed (no alias resolution) + optional length.
wxString SqliteProfile::RenderTypeSpec(const ColumnModel& c) const
{
    wxString s = c.type;
    const TypeDescriptor* td = FindType(c.type);
    if (td && td->takesLength && !c.length.IsEmpty()) {
        if (td->takesScale && !c.scale.IsEmpty())
            s += L"(" + c.length + L"," + c.scale + L")";
        else
            s += L"(" + c.length + L")";
    }
    return s;
}

// "name" <typespec> [GENERATED ALWAYS AS (expr) STORED|VIRTUAL] [NOT NULL]
// [DEFAULT …]. AUTOINCREMENT is intentionally absent — it is legal only as part of a
// table-level INTEGER PRIMARY KEY, not in an ADD COLUMN body.
wxString SqliteProfile::RenderColumnBody(const ColumnModel& c) const
{
    wxString s = Q(c.name) + L" " + RenderTypeSpec(c);

    const bool generated = !c.generatedExpr.IsEmpty();
    if (generated)
        s += L" GENERATED ALWAYS AS (" + c.generatedExpr + L") " +
             (c.generatedStored ? L"STORED" : L"VIRTUAL");

    if (c.notNull)                            s += L" NOT NULL";
    if (!c.defaultVal.IsEmpty() && !generated)
        s += L" DEFAULT " + c.defaultVal;
    return s;
}

// SQLite ALTER can rename a column but cannot change its type or constraints. Emit
// the RENAME when the name changed; then a single honest "-- SQLite …" note that any
// type/constraint change requires rebuilding the table. Never emit ALTER COLUMN.
void SqliteProfile::RenderColumnChange(const wxString& qt, const ColumnModel& c,
                                       std::vector<wxString>& stmts) const
{
    if (!c.origName.IsEmpty() && c.origName != c.name)
        stmts.push_back(L"ALTER TABLE " + qt + L" RENAME COLUMN " +
                        Q(c.origName) + L" TO " + Q(c.name));

    // Honest degradation: SQLite has no ALTER COLUMN. Any type/constraint edit is
    // impossible without a full table rebuild, so we surface it as a comment rather
    // than emit invalid SQL (mirrors SqliteDriver.cpp RenderSchemaChange).
    stmts.push_back(L"-- SQLite 不支持修改列 " + Q(c.name) +
                    L" 的类型/约束（仅支持 ADD/RENAME COLUMN），如需变更请重建表");
}

// SQLite can DECLARE foreign keys only inside CREATE TABLE — it cannot ADD or DROP a
// FK on an existing table via ALTER. Honest degradation: a "-- " note, never invalid
// SQL. (The full CREATE path in slice 4 will emit inline FOREIGN KEY clauses.)
void SqliteProfile::RenderFkChange(const wxString& qt, const ForeignKeyEdit& fe,
                                   std::vector<wxString>& stmts) const
{
    (void)qt;
    const wxString verb = (fe.op == ForeignKeyEdit::Drop) ? L"删除" : L"新增";
    stmts.push_back(L"-- SQLite 不支持在既有表上" + verb + L"外键 " + Q(fe.model.name) +
                    L"（外键只能在 CREATE TABLE 时声明），如需变更请重建表");
}

} // namespace

// Per-DbType singleton accessor (wired by DialectRegistry.cpp).
const DialectProfile& SqliteDialectProfile()
{
    static const SqliteProfile inst;
    return inst;
}

} // namespace db
