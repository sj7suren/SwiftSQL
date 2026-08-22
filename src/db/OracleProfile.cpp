// OracleProfile.cpp — the Oracle-family DialectProfile (ADR-014 T5), shared by
// Oracle and 达梦 DM (compatible DDL surface). Inherits SeparateAlterProfile:
// Add/Drop are base-owned (uniform ALTER TABLE … ADD/DROP COLUMN). Oracle diverges
// from PG/SQL Server on the modify shape: the type/constraint change uses the
// parenthesized `ALTER TABLE t MODIFY (c <typespec> [DEFAULT …] [NOT NULL])` form
// (NOT `ALTER COLUMN`), a rename is `ALTER TABLE t RENAME COLUMN old TO new`, and a
// comment is a separate `COMMENT ON COLUMN t.c IS '…'` (same shape as PG). Oracle
// has no native boolean (int → NUMBER) and no per-column unsigned. No handle here.
#include "db/DialectProfile.h"

namespace db {
namespace {

class OracleProfile : public SeparateAlterProfile {
public:
    Dialect GetDialect() const override { return Dialect::Oracle; }
    const std::vector<TypeDescriptor>& Types() const override;
    const std::vector<AttrDescriptor>& Attributes() const override;
    wxString RenderTypeSpec(const ColumnModel& c) const override;
    wxString RenderColumnBody(const ColumnModel& c) const override;
    // Oracle triggers use a PL/SQL block (…FOR EACH ROW BEGIN … END;) with its own
    // referencing/compound-trigger surface — not captured by the simple body model,
    // so the tab is hidden.
    bool SupportsTriggers() const override { return false; }

protected:
    // Oracle has no COLUMN keyword after ADD: "ALTER TABLE t ADD c …".
    wxString AddColumnClause(const ColumnModel& c) const override
    { return L"ADD " + RenderColumnBody(c); }
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
// Oracle character types require an explicit length (VARCHAR2(len) has no default),
// so lengthRequired=true there. NUMBER carries optional precision/scale. Legacy
// Oracle has no BOOLEAN — boolean/int synonyms alias onto NUMBER.
const std::vector<TypeDescriptor>& OracleProfile::Types() const
{
    static const std::vector<TypeDescriptor> kTypes = {
        // numeric
        { L"NUMBER",    ColKind::Decimal, true,  true,  false, L"数值", L"" },
        { L"FLOAT",     ColKind::Float,   true,  false, false, L"数值", L"" },
        { L"BINARY_FLOAT",  ColKind::Float, false, false, false, L"数值", L"" },
        { L"BINARY_DOUBLE", ColKind::Float, false, false, false, L"数值", L"" },
        { L"int",       ColKind::Integer, false, false, false, L"数值", L"NUMBER" },
        { L"integer",   ColKind::Integer, false, false, false, L"数值", L"NUMBER" },
        { L"smallint",  ColKind::Integer, false, false, false, L"数值", L"NUMBER" },
        { L"decimal",   ColKind::Decimal, true,  true,  false, L"数值", L"NUMBER" },
        { L"numeric",   ColKind::Decimal, true,  true,  false, L"数值", L"NUMBER" },
        // Oracle has no native boolean → NUMBER(1)
        { L"boolean",   ColKind::Boolean, false, false, false, L"布尔", L"NUMBER" },
        // character (length mandatory)
        { L"VARCHAR2",  ColKind::Varchar, true,  false, true,  L"字符", L"" },
        { L"NVARCHAR2", ColKind::Varchar, true,  false, true,  L"字符", L"" },
        { L"CHAR",      ColKind::Char,    true,  false, false, L"字符", L"" },
        { L"NCHAR",     ColKind::Char,    true,  false, false, L"字符", L"" },
        { L"varchar",   ColKind::Varchar, true,  false, true,  L"字符", L"VARCHAR2" },
        { L"CLOB",      ColKind::Text,    false, false, false, L"字符", L"" },
        { L"NCLOB",     ColKind::Text,    false, false, false, L"字符", L"" },
        // date / time
        { L"DATE",      ColKind::Date,      false, false, false, L"日期时间", L"" },
        { L"TIMESTAMP", ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
        // binary
        { L"BLOB",      ColKind::Blob,   false, false, false, L"二进制", L"" },
        { L"RAW",       ColKind::Binary, true,  false, false, L"二进制", L"" },
    };
    return kTypes;
}

// ---- attribute catalog: {id, label, editor, choices, appliesTo, core} ----
// Oracle carries the comment in a separate COMMENT ON COLUMN (same as PG); a
// generated column is GENERATED ALWAYS AS (expr) VIRTUAL. No per-column unsigned.
const std::vector<AttrDescriptor>& OracleProfile::Attributes() const
{
    static const std::vector<AttrDescriptor> kAttrs = {
        { L"comment",   L"注释",   AttrDescriptor::Text, {}, {}, true },
        { L"generated", L"生成列", AttrDescriptor::Expr, {}, {}, true },
    };
    return kAttrs;
}

const TypeDescriptor* OracleProfile::FindType(const wxString& name) const
{
    for (const auto& t : Types())
        if (t.name.IsSameAs(name, /*caseSensitive=*/false)) return &t;
    return nullptr;
}

// Resolve any alias to its canonical Oracle type, then append (len[,scale]).
wxString OracleProfile::RenderTypeSpec(const ColumnModel& c) const
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

// "name" <typespec> [DEFAULT …] [GENERATED ALWAYS AS (expr) VIRTUAL] [NOT NULL].
// COMMENT is intentionally absent — Oracle carries it in a separate COMMENT ON
// COLUMN statement (emitted by RenderColumnChange on modify).
wxString OracleProfile::RenderColumnBody(const ColumnModel& c) const
{
    wxString s = Q(c.name) + L" " + RenderTypeSpec(c);

    const bool generated = !c.generatedExpr.IsEmpty();
    // Virtual columns can't carry a plain DEFAULT; guard it.
    if (!c.defaultVal.IsEmpty() && !generated)
        s += L" DEFAULT " + c.defaultVal;
    if (generated)
        s += L" GENERATED ALWAYS AS (" + c.generatedExpr + L") VIRTUAL";
    if (c.notNull)                            s += L" NOT NULL";
    return s;
}

// A Modify: rename first (if changed), then the type/DEFAULT/nullability change via
// Oracle's parenthesized MODIFY (…) form, and finally COMMENT ON COLUMN.
void OracleProfile::RenderColumnChange(const wxString& qt, const ColumnModel& c,
                                       std::vector<wxString>& stmts) const
{
    if (!c.origName.IsEmpty() && c.origName != c.name)
        stmts.push_back(L"ALTER TABLE " + qt + L" RENAME COLUMN " +
                        Q(c.origName) + L" TO " + Q(c.name));

    // Oracle uses MODIFY (col <type> …), not ALTER COLUMN.
    wxString mod = Q(c.name) + L" " + RenderTypeSpec(c);
    if (!c.defaultVal.IsEmpty() && c.generatedExpr.IsEmpty())
        mod += L" DEFAULT " + c.defaultVal;
    mod += (c.notNull ? L" NOT NULL" : L" NULL");
    stmts.push_back(L"ALTER TABLE " + qt + L" MODIFY (" + mod + L")");

    if (!c.comment.IsEmpty()) {
        wxString e = c.comment; e.Replace(L"'", L"''");
        stmts.push_back(L"COMMENT ON COLUMN " + qt + L"." + Q(c.name) +
                        L" IS '" + e + L"'");
    }
}

// Oracle table comment is a standalone COMMENT ON TABLE … IS '…' statement (same
// shape as the column comment above).
void OracleProfile::RenderCommentChange(const wxString& qt, const wxString& comment,
                                        std::vector<wxString>& stmts) const
{
    wxString e = comment; e.Replace(L"'", L"''");
    stmts.push_back(L"COMMENT ON TABLE " + qt + L" IS '" + e + L"'");
}

// Honest skip: Oracle triggers are PL/SQL blocks; the simple body model can't render
// valid DDL. (Reached only defensively — SupportsTriggers()=false hides the tab.)
void OracleProfile::RenderTriggerChange(const wxString&, const TriggerEdit& te,
                                        std::vector<wxString>& stmts) const
{
    const wxString verb = (te.op == TriggerEdit::Drop) ? L"删除" : L"新增";
    stmts.push_back(L"-- Oracle 触发器为 PL/SQL 块，设计视图暂不支持" + verb +
                    L"触发器 " + Q(te.model.name) + L"，请用 SQL 编辑器");
}

} // namespace

// Per-DbType singleton accessor (wired by DialectRegistry.cpp). Shared by Oracle + DM.
const DialectProfile& OracleDialectProfile()
{
    static const OracleProfile inst;
    return inst;
}

} // namespace db
