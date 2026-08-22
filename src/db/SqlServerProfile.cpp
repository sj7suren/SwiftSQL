// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SqlServerProfile.cpp — the SQL Server DialectProfile (ADR-014 T5). Inherits
// SeparateAlterProfile: Add/Drop are base-owned (uniform ALTER TABLE … ADD/DROP
// COLUMN). A Modify is where T-SQL diverges most and is why ADR-014 #4 hands the
// whole modify to the concrete: a rename is NOT an ALTER TABLE at all but
// `EXEC sp_rename 'tbl.old','new','COLUMN'`, and a type/constraint change is
// `ALTER TABLE t ALTER COLUMN c <typespec> NULL|NOT NULL`. SQL Server has no native
// column comment (that would need sp_addextendedproperty — deferred, TODO) and keeps
// DEFAULT in a separate named constraint (deferred, TODO). No connection handle here.
#include "db/DialectProfile.h"

namespace db {
namespace {

class SqlServerProfile : public SeparateAlterProfile {
public:
    Dialect GetDialect() const override { return Dialect::SqlServer; }
    const std::vector<TypeDescriptor>& Types() const override;
    const std::vector<AttrDescriptor>& Attributes() const override;
    wxString RenderTypeSpec(const ColumnModel& c) const override;
    wxString RenderColumnBody(const ColumnModel& c) const override;
    // T-SQL triggers are set-based (no FOR EACH ROW; use inserted/deleted tables),
    // so the row-level body model does not fit — hide the tab.
    bool SupportsTriggers() const override { return false; }

protected:
    // T-SQL has no COLUMN keyword after ADD: "ALTER TABLE t ADD c …".
    wxString AddColumnClause(const ColumnModel& c) const override
    { return L"ADD " + RenderColumnBody(c); }
    void RenderColumnChange(const wxString& qualifiedTable, const ColumnModel& c,
                            std::vector<wxString>& stmts) const override;
    void RenderTriggerChange(const wxString& qt, const TriggerEdit& te,
                             std::vector<wxString>& stmts) const override;

private:
    const TypeDescriptor* FindType(const wxString& name) const;
};

// ---- type catalog: {name, kind, takesLength, takesScale, lengthRequired, category, aliasOf} ----
const std::vector<TypeDescriptor>& SqlServerProfile::Types() const
{
    static const std::vector<TypeDescriptor> kTypes = {
        // integers + bit
        { L"tinyint",          ColKind::Integer, false, false, false, L"数值", L"" },
        { L"smallint",         ColKind::Integer, false, false, false, L"数值", L"" },
        { L"int",              ColKind::Integer, false, false, false, L"数值", L"" },
        { L"bigint",           ColKind::Integer, false, false, false, L"数值", L"" },
        { L"bit",              ColKind::Boolean, false, false, false, L"布尔", L"" },
        // fixed / money
        { L"decimal",          ColKind::Decimal, true,  true,  false, L"数值", L"" },
        { L"numeric",          ColKind::Decimal, true,  true,  false, L"数值", L"decimal" },
        { L"money",            ColKind::Decimal, false, false, false, L"数值", L"" },
        { L"smallmoney",       ColKind::Decimal, false, false, false, L"数值", L"" },
        { L"float",            ColKind::Float,   true,  false, false, L"数值", L"" },
        { L"real",             ColKind::Float,   false, false, false, L"数值", L"" },
        // character (N* = double-byte / Unicode)
        { L"char",             ColKind::Char,    true,  false, false, L"字符", L"" },
        { L"varchar",          ColKind::Varchar, true,  false, false, L"字符", L"" },
        { L"nchar",            ColKind::Char,    true,  false, false, L"字符", L"" },
        { L"nvarchar",         ColKind::Varchar, true,  false, false, L"字符", L"" },
        { L"text",             ColKind::Text,    false, false, false, L"字符", L"" },
        { L"ntext",            ColKind::Text,    false, false, false, L"字符", L"" },
        // date / time
        { L"date",             ColKind::Date,      false, false, false, L"日期时间", L"" },
        { L"time",             ColKind::Time,      true,  false, false, L"日期时间", L"" },
        { L"datetime",         ColKind::Timestamp, false, false, false, L"日期时间", L"" },
        { L"datetime2",        ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
        { L"smalldatetime",    ColKind::Timestamp, false, false, false, L"日期时间", L"" },
        { L"datetimeoffset",   ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
        // structured / binary
        { L"uniqueidentifier", ColKind::Uuid,   false, false, false, L"结构化", L"" },
        { L"xml",              ColKind::Json,   false, false, false, L"结构化", L"" },
        { L"binary",           ColKind::Binary, true,  false, false, L"二进制", L"" },
        { L"varbinary",        ColKind::Binary, true,  false, false, L"二进制", L"" },
    };
    return kTypes;
}

// ---- attribute catalog: {id, label, editor, choices, appliesTo, core} ----
// collation is per-column on character types; identity == autoIncrement (core);
// generated == a computed column (AS (expr) PERSISTED). comment is intentionally
// absent — SQL Server has no native column comment (sp_addextendedproperty, TODO).
const std::vector<AttrDescriptor>& SqlServerProfile::Attributes() const
{
    using K = ColKind;
    static const std::vector<AttrDescriptor> kAttrs = {
        { L"collation",     L"排序规则", AttrDescriptor::Text, {},
          { K::Char, K::Varchar, K::Text }, false },
        { L"autoIncrement", L"标识(IDENTITY)", AttrDescriptor::Bool, {},
          { K::Integer }, true },
        { L"generated",     L"计算列", AttrDescriptor::Expr, {}, {}, true },
    };
    return kAttrs;
}

const TypeDescriptor* SqlServerProfile::FindType(const wxString& name) const
{
    for (const auto& t : Types())
        if (t.name.IsSameAs(name, /*caseSensitive=*/false)) return &t;
    return nullptr;
}

// Resolve any alias to its canonical T-SQL type, then append (len[,scale]).
wxString SqlServerProfile::RenderTypeSpec(const ColumnModel& c) const
{
    const TypeDescriptor* td = FindType(c.type);
    wxString s = (td && !td->aliasOf.IsEmpty()) ? td->aliasOf : c.type;
    if (td && td->takesLength && !c.length.IsEmpty()) {
        if (td->takesScale && !c.scale.IsEmpty())
            s += L"(" + c.length + L"," + c.scale + L")";
        else
            s += L"(" + c.length + L")";
    }
    return s;
}

// [name] <typespec> [COLLATE …] [IDENTITY(1,1)] [NOT NULL] [DEFAULT …], or a
// computed column: [name] AS (expr) [PERSISTED]. A computed column carries no type
// and no other clause.
wxString SqlServerProfile::RenderColumnBody(const ColumnModel& c) const
{
    const bool generated = !c.generatedExpr.IsEmpty();
    if (generated)
        return Q(c.name) + L" AS (" + c.generatedExpr + L")" +
               (c.generatedStored ? L" PERSISTED" : L"");

    wxString s = Q(c.name) + L" " + RenderTypeSpec(c);
    if (!c.collation.IsEmpty())              s += L" COLLATE " + c.collation;
    if (c.autoIncrement)                     s += L" IDENTITY(1,1)";
    if (c.notNull)                            s += L" NOT NULL";
    // Identity columns can't carry a plain DEFAULT; guard it.
    if (!c.defaultVal.IsEmpty() && !c.autoIncrement)
        s += L" DEFAULT " + c.defaultVal;
    return s;
}

// A Modify: rename first via sp_rename (NOT an ALTER TABLE — this is exactly the
// shape ADR-014 #4 locked the interface for), then a type/nullability change via
// ALTER COLUMN. DEFAULT is a separate named constraint in T-SQL and is deferred
// (TODO) — surfaced as a comment rather than folded into ALTER COLUMN where it is
// illegal.
void SqlServerProfile::RenderColumnChange(const wxString& qt, const ColumnModel& c,
                                          std::vector<wxString>& stmts) const
{
    if (!c.origName.IsEmpty() && c.origName != c.name) {
        // sp_rename parses the old-name string literal itself, so it must be the
        // idiomatic UNQUOTED 'schema.table.col' — embedding ANSI double quotes
        // ('"app"."users"."old"') is fragile / rejected. Strip the quotes the
        // qualified table carries, then append the bare column name. The NEW name
        // must be bare too. Escape embedded single quotes for the literal.
        wxString oldRef = qt;               // "app"."users"
        oldRef.Replace(L"\"", L"");         // app.users
        oldRef += L"." + c.origName;        // app.users.old_email
        wxString newName = c.name;
        oldRef.Replace(L"'", L"''");
        newName.Replace(L"'", L"''");
        stmts.push_back(L"EXEC sp_rename '" + oldRef + L"', '" + newName +
                        L"', 'COLUMN'");
    }

    // Computed columns can't be ALTER COLUMN'd (must be dropped + re-added); skip
    // honestly rather than emit invalid SQL.
    if (!c.generatedExpr.IsEmpty()) {
        stmts.push_back(L"-- SQL Server 不支持 ALTER COLUMN 修改计算列 " + Q(c.name) +
                        L"，需 DROP 后重新 ADD");
        return;
    }

    wxString alter = L"ALTER TABLE " + qt + L" ALTER COLUMN " + Q(c.name) + L" " +
                     RenderTypeSpec(c);
    if (!c.collation.IsEmpty())              alter += L" COLLATE " + c.collation;
    alter += (c.notNull ? L" NOT NULL" : L" NULL");
    stmts.push_back(alter);

    // DEFAULT is a standalone constraint (ADD CONSTRAINT … DEFAULT … FOR col) — out
    // of scope for this batch.
    if (!c.defaultVal.IsEmpty())
        stmts.push_back(L"-- TODO SQL Server 默认值为独立约束，需 ALTER TABLE " + qt +
                        L" ADD CONSTRAINT … DEFAULT " + c.defaultVal + L" FOR " +
                        Q(c.name));
}

// Honest skip: T-SQL triggers operate on the inserted/deleted pseudo-tables (set-
// based), so the row-level body model can't render valid DDL. (Reached only
// defensively — SupportsTriggers()=false hides the tab.)
void SqlServerProfile::RenderTriggerChange(const wxString&, const TriggerEdit& te,
                                           std::vector<wxString>& stmts) const
{
    const wxString verb = (te.op == TriggerEdit::Drop) ? L"删除" : L"新增";
    stmts.push_back(L"-- SQL Server 触发器为集合式(inserted/deleted)，设计视图暂不支持" +
                    verb + L"触发器 " + Q(te.model.name) + L"，请用 SQL 编辑器");
}

} // namespace

// Per-DbType singleton accessor (wired by DialectRegistry.cpp).
const DialectProfile& SqlServerDialectProfile()
{
    static const SqlServerProfile inst;
    return inst;
}

} // namespace db
