// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// PgProfile.cpp — the PostgreSQL-family DialectProfile (ADR-014 T4), shared by
// PostgreSQL / KingBase. Inherits SeparateAlterProfile, so each column change is
// its own statement: Add/Drop are base-owned (uniform ALTER TABLE … ADD/DROP
// COLUMN); a Modify is emitted here as the PG sequence RENAME COLUMN + ALTER COLUMN
// TYPE / SET|DROP NOT NULL / SET|DROP DEFAULT + COMMENT ON COLUMN. Column bodies
// emit PG defaults verbatim, matching detail::PgDefault in PgSql.cpp. No handle here.
#include "db/DialectProfile.h"
#include "db/SyncTypeMap.h"   // InterpretPgColumn/RenderPgType (A1, cross-db sync)

namespace db {
namespace {

class PgProfile : public SeparateAlterProfile {
public:
    Dialect GetDialect() const override { return Dialect::Postgres; }
    const std::vector<TypeDescriptor>& Types() const override;
    const std::vector<AttrDescriptor>& Attributes() const override;
    wxString RenderTypeSpec(const ColumnModel& c) const override;
    wxString RenderColumnBody(const ColumnModel& c) const override;
    // PG triggers are function-based (CREATE FUNCTION … + CREATE TRIGGER … EXECUTE
    // FUNCTION), which the design view's timing/event/body model does not capture —
    // hide the tab rather than emit a wrong single-statement trigger.
    bool SupportsTriggers() const override { return false; }
    // A3 cross-engine type mapping — delegates to SyncTypeMap (A1); see
    // DialectProfile.h for the contract.
    CanonicalType Interpret(const NormColumn& c) const override
    { return InterpretPgColumn(c); }
    bool Render(const CanonicalType& t, wxString& out, wxString& why) const override
    { return RenderPgType(t, out, why); }

protected:
    void RenderColumnChange(const wxString& qualifiedTable, const ColumnModel& c,
                            std::vector<wxString>& stmts) const override;
    void RenderCommentChange(const wxString& qt, const wxString& comment,
                             std::vector<wxString>& stmts) const override;
    void RenderTriggerChange(const wxString& qt, const TriggerEdit& te,
                             std::vector<wxString>& stmts) const override;

private:
    const TypeDescriptor* FindType(const wxString& name) const;
};

// ---- type catalog: {name, kind, takesLength, takesScale, lengthRequired, category, aliasOf} ----
const std::vector<TypeDescriptor>& PgProfile::Types() const
{
    static const std::vector<TypeDescriptor> kTypes = {
        // integers + serial pseudo-types
        { L"smallint",         ColKind::Integer, false, false, false, L"数值",   L"" },
        { L"integer",          ColKind::Integer, false, false, false, L"数值",   L"" },
        { L"bigint",           ColKind::Integer, false, false, false, L"数值",   L"" },
        { L"int",              ColKind::Integer, false, false, false, L"数值",   L"integer" },
        { L"int2",             ColKind::Integer, false, false, false, L"数值",   L"smallint" },
        { L"int4",             ColKind::Integer, false, false, false, L"数值",   L"integer" },
        { L"int8",             ColKind::Integer, false, false, false, L"数值",   L"bigint" },
        { L"smallserial",      ColKind::Integer, false, false, false, L"数值",   L"" },
        { L"serial",           ColKind::Integer, false, false, false, L"数值",   L"" },
        { L"bigserial",        ColKind::Integer, false, false, false, L"数值",   L"" },
        // numeric / floating point
        { L"numeric",          ColKind::Decimal, true,  true,  false, L"数值",   L"" },
        { L"decimal",          ColKind::Decimal, true,  true,  false, L"数值",   L"numeric" },
        { L"real",             ColKind::Float,   false, false, false, L"数值",   L"" },
        { L"double precision", ColKind::Float,   false, false, false, L"数值",   L"" },
        { L"float4",           ColKind::Float,   false, false, false, L"数值",   L"real" },
        { L"float8",           ColKind::Float,   false, false, false, L"数值",   L"double precision" },
        // boolean
        { L"boolean",          ColKind::Boolean, false, false, false, L"布尔",   L"" },
        { L"bool",             ColKind::Boolean, false, false, false, L"布尔",   L"boolean" },
        // character
        { L"varchar",          ColKind::Varchar, true,  false, false, L"字符",   L"" },
        { L"character varying",ColKind::Varchar, true,  false, false, L"字符",   L"varchar" },
        { L"char",             ColKind::Char,    true,  false, false, L"字符",   L"" },
        { L"character",        ColKind::Char,    true,  false, false, L"字符",   L"char" },
        { L"text",             ColKind::Text,    false, false, false, L"字符",   L"" },
        // structured / identity
        { L"uuid",             ColKind::Uuid,    false, false, false, L"结构化", L"" },
        { L"json",             ColKind::Json,    false, false, false, L"结构化", L"" },
        { L"jsonb",            ColKind::Json,    false, false, false, L"结构化", L"" },
        { L"bytea",            ColKind::Binary,  false, false, false, L"二进制", L"" },
        // date / time (length = fractional-seconds precision, optional)
        { L"date",             ColKind::Date,      false, false, false, L"日期时间", L"" },
        { L"time",             ColKind::Time,      true,  false, false, L"日期时间", L"" },
        { L"timetz",           ColKind::Time,      true,  false, false, L"日期时间", L"" },
        { L"timestamp",        ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
        { L"timestamptz",      ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
    };
    return kTypes;
}

// ---- attribute catalog: {id, label, editor, choices, appliesTo, core} ----
// PG has no per-column unsigned/charset; comment is a separate COMMENT ON COLUMN.
const std::vector<AttrDescriptor>& PgProfile::Attributes() const
{
    using K = ColKind;
    static const std::vector<AttrDescriptor> kAttrs = {
        { L"comment",       L"注释",     AttrDescriptor::Text, {}, {}, true },
        { L"autoIncrement", L"标识/自增", AttrDescriptor::Bool, {}, { K::Integer }, true },
        { L"collation",     L"排序规则", AttrDescriptor::Text, {},
          { K::Char, K::Varchar, K::Text }, false },
        { L"generated",     L"生成列",   AttrDescriptor::Expr, {}, {}, true },
    };
    return kAttrs;
}

const TypeDescriptor* PgProfile::FindType(const wxString& name) const
{
    for (const auto& t : Types())
        if (t.name.IsSameAs(name, /*caseSensitive=*/false)) return &t;
    return nullptr;
}

// Resolve any alias to its canonical PG type name, then append (len[,scale]).
wxString PgProfile::RenderTypeSpec(const ColumnModel& c) const
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

// `name` <typespec> [COLLATE "…"] [IDENTITY | GENERATED … STORED] [NOT NULL]
// [DEFAULT …]. COMMENT is intentionally absent — PG carries it in a separate
// COMMENT ON COLUMN statement (emitted by RenderColumnChange on modify).
wxString PgProfile::RenderColumnBody(const ColumnModel& c) const
{
    wxString s = Q(c.name) + L" " + RenderTypeSpec(c);
    if (!c.collation.IsEmpty())              s += L" COLLATE " + Q(c.collation);

    const bool generated = !c.generatedExpr.IsEmpty();
    if (c.autoIncrement)                     s += L" GENERATED BY DEFAULT AS IDENTITY";
    else if (generated)                      s += L" GENERATED ALWAYS AS (" +
                                                   c.generatedExpr + L") STORED";

    if (c.notNull)                            s += L" NOT NULL";
    // Identity/generated columns can't carry a plain DEFAULT; PG defaults are
    // complete expressions, emitted verbatim (matches detail::PgDefault).
    if (!c.defaultVal.IsEmpty() && !c.autoIncrement && !generated)
        s += L" DEFAULT " + c.defaultVal;
    return s;
}

// A Modify becomes the PG sequence: RENAME first (if the name changed), then the
// type change, NOT NULL toggle, DEFAULT set/drop, and finally COMMENT ON COLUMN.
void PgProfile::RenderColumnChange(const wxString& qt, const ColumnModel& c,
                                   std::vector<wxString>& stmts) const
{
    if (!c.origName.IsEmpty() && c.origName != c.name)
        stmts.push_back(L"ALTER TABLE " + qt + L" RENAME COLUMN " +
                        Q(c.origName) + L" TO " + Q(c.name));

    const wxString colRef = L"ALTER TABLE " + qt + L" ALTER COLUMN " + Q(c.name);

    stmts.push_back(colRef + L" TYPE " + RenderTypeSpec(c));
    stmts.push_back(colRef + (c.notNull ? L" SET NOT NULL" : L" DROP NOT NULL"));
    // Only SET a default when one is supplied. Do NOT auto-emit DROP DEFAULT: with
    // no baseline (R3) an empty defaultVal means "untouched", not "clear it", so an
    // unconditional DROP DEFAULT would silently delete a default the user never
    // edited (report P2). A deliberate clear will be a distinct signal post-R3.
    if (!c.defaultVal.IsEmpty())
        stmts.push_back(colRef + L" SET DEFAULT " + c.defaultVal);

    if (!c.comment.IsEmpty()) {
        wxString e = c.comment; e.Replace(L"'", L"''");
        stmts.push_back(L"COMMENT ON COLUMN " + qt + L"." + Q(c.name) +
                        L" IS '" + e + L"'");
    }
}

// PostgreSQL table comment is a standalone COMMENT ON TABLE … IS '…' statement
// (same shape used for column comments above).
void PgProfile::RenderCommentChange(const wxString& qt, const wxString& comment,
                                    std::vector<wxString>& stmts) const
{
    wxString e = comment; e.Replace(L"'", L"''");
    stmts.push_back(L"COMMENT ON TABLE " + qt + L" IS '" + e + L"'");
}

// Honest skip: PG triggers need a companion trigger function, so the simple
// single-statement model can't render valid DDL. (Reached only defensively —
// SupportsTriggers()=false hides the tab.)
void PgProfile::RenderTriggerChange(const wxString&, const TriggerEdit& te,
                                    std::vector<wxString>& stmts) const
{
    const wxString verb = (te.op == TriggerEdit::Drop) ? L"删除" : L"新增";
    stmts.push_back(L"-- PostgreSQL 触发器需配套触发器函数，设计视图暂不支持" + verb +
                    L"触发器 " + Q(te.model.name) + L"，请用 SQL 编辑器");
}

} // namespace

// Per-DbType singleton accessor (wired by DialectRegistry.cpp).
const DialectProfile& PgDialectProfile()
{
    static const PgProfile inst;
    return inst;
}

} // namespace db
