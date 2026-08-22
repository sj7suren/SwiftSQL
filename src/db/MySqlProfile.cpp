// MySqlProfile.cpp — the MySQL-family DialectProfile (ADR-014 T3), shared by
// MySQL / MariaDB / OceanBase. Inherits InlineAlterProfile, so a whole TableEdit
// renders to ONE ALTER TABLE with comma-joined ADD/MODIFY/CHANGE/DROP COLUMN
// clauses. Type catalog + attribute catalog are data tables; column-body rendering
// reuses the detail:: fragment primitives (RenderDefault) shared with the sync
// path so the two never drift (ADR-014 guardrail). No connection handle here.
#include "db/DialectProfile.h"
#include "db/MySqlSql.h"      // detail::RenderDefault (shared DEFAULT-clause primitive)
#include "db/SyncTypeMap.h"   // InterpretMySqlColumn/RenderMySqlType (A1, cross-db sync)

namespace db {
namespace {

// A bag attribute read as a boolean flag ("1"/"true"/… = on; ""/"0" = off).
bool AttrOn(const ColumnModel& c, const wxString& id)
{
    const wxString v = c.Attr(id);
    return !v.IsEmpty() && v != L"0" && !v.IsSameAs(L"false", false);
}

class MySqlProfile : public InlineAlterProfile {
public:
    Dialect GetDialect() const override { return Dialect::MySQL; }
    const std::vector<TypeDescriptor>& Types() const override;
    const std::vector<AttrDescriptor>& Attributes() const override;
    wxString RenderTypeSpec(const ColumnModel& c) const override;
    wxString RenderColumnBody(const ColumnModel& c) const override;
    bool SupportsColumnReorder() const override { return true; }   // MODIFY … AFTER/FIRST
    std::vector<wxString> IndexTypes() const override;
    std::vector<wxString> IndexMethods() const override;
    std::vector<DbCreateOption> TableOptionSpecs() const override;
    bool RenderCreate(const TableModel& model,
                      std::vector<wxString>& stmts, wxString& err) const override;
    // A3 cross-engine type mapping — delegates to SyncTypeMap (A1); see
    // DialectProfile.h for the contract.
    CanonicalType Interpret(const NormColumn& c) const override
    { return InterpretMySqlColumn(c); }
    bool Render(const CanonicalType& t, wxString& out, wxString& why) const override
    { return RenderMySqlType(t, out, why); }

protected:
    wxString ModifyClause(const ColumnModel& c) const override;
    wxString IndexClause(const IndexEdit& ie) const override;
    void RenderOptionsChange(const wxString& qt, const TableOptions& o,
                             std::vector<wxString>& stmts) const override;
    void RenderCommentChange(const wxString& qt, const wxString& comment,
                             std::vector<wxString>& stmts) const override;

private:
    const TypeDescriptor* FindType(const wxString& name) const;
    // "[UNIQUE|FULLTEXT|SPATIAL ]INDEX `name` (`cols`) [USING m] [COMMENT '…']" — the
    // index definition shared by IndexClause (prefixed "ADD " for ALTER) and
    // RenderCreate (spliced inline into the CREATE TABLE body).
    wxString IndexInlineDef(const IndexModel& m) const;
    // MySQL table-option suffix ("ENGINE=… DEFAULT CHARSET=… … COMMENT='…'") for the
    // tail of CREATE TABLE; "" when nothing is set.
    wxString OptionSuffix(const TableOptions& o, const wxString& comment) const;
};

// ---- type catalog: {name, kind, takesLength, takesScale, lengthRequired, category, aliasOf} ----
const std::vector<TypeDescriptor>& MySqlProfile::Types() const
{
    static const std::vector<TypeDescriptor> kTypes = {
        // integers (length = legacy display width, optional)
        { L"tinyint",   ColKind::Integer, true,  false, false, L"数值",   L"" },
        { L"smallint",  ColKind::Integer, true,  false, false, L"数值",   L"" },
        { L"mediumint", ColKind::Integer, true,  false, false, L"数值",   L"" },
        { L"int",       ColKind::Integer, true,  false, false, L"数值",   L"" },
        { L"integer",   ColKind::Integer, true,  false, false, L"数值",   L"int" },
        { L"bigint",    ColKind::Integer, true,  false, false, L"数值",   L"" },
        { L"bit",       ColKind::Integer, true,  false, false, L"数值",   L"" },
        { L"year",      ColKind::Integer, false, false, false, L"数值",   L"" },
        // fixed / floating point
        { L"decimal",   ColKind::Decimal, true,  true,  false, L"数值",   L"" },
        { L"numeric",   ColKind::Decimal, true,  true,  false, L"数值",   L"decimal" },
        { L"float",     ColKind::Float,   true,  true,  false, L"数值",   L"" },
        { L"double",    ColKind::Float,   true,  true,  false, L"数值",   L"" },
        { L"real",      ColKind::Float,   true,  true,  false, L"数值",   L"double" },
        // character
        { L"char",      ColKind::Char,    true,  false, false, L"字符",   L"" },
        { L"varchar",   ColKind::Varchar, true,  false, true,  L"字符",   L"" },
        { L"tinytext",  ColKind::Text,    false, false, false, L"字符",   L"" },
        { L"text",      ColKind::Text,    false, false, false, L"字符",   L"" },
        { L"mediumtext",ColKind::Text,    false, false, false, L"字符",   L"" },
        { L"longtext",  ColKind::Text,    false, false, false, L"字符",   L"" },
        // binary / blob
        { L"binary",    ColKind::Binary,  true,  false, false, L"二进制", L"" },
        { L"varbinary", ColKind::Binary,  true,  false, true,  L"二进制", L"" },
        { L"tinyblob",  ColKind::Blob,    false, false, false, L"二进制", L"" },
        { L"blob",      ColKind::Blob,    false, false, false, L"二进制", L"" },
        { L"mediumblob",ColKind::Blob,    false, false, false, L"二进制", L"" },
        { L"longblob",  ColKind::Blob,    false, false, false, L"二进制", L"" },
        // date / time (length = fractional-seconds precision, optional)
        { L"date",      ColKind::Date,      false, false, false, L"日期时间", L"" },
        { L"datetime",  ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
        { L"timestamp", ColKind::Timestamp, true,  false, false, L"日期时间", L"" },
        { L"time",      ColKind::Time,      true,  false, false, L"日期时间", L"" },
        // structured / enumerated (length cell carries the value list for enum/set)
        { L"json",      ColKind::Json,    false, false, false, L"结构化", L"" },
        { L"enum",      ColKind::Enum,    true,  false, true,  L"枚举",   L"" },
        { L"set",       ColKind::Enum,    true,  false, true,  L"枚举",   L"" },
    };
    return kTypes;
}

// ---- attribute catalog: {id, label, editor, choices, appliesTo, core} ----
const std::vector<AttrDescriptor>& MySqlProfile::Attributes() const
{
    using K = ColKind;
    static const std::vector<AttrDescriptor> kAttrs = {
        { L"unsigned",      L"无符号",       AttrDescriptor::Bool,   {},
          { K::Integer, K::Decimal, K::Float }, false },
        { L"binary",        L"BINARY",       AttrDescriptor::Bool,   {},
          { K::Char, K::Varchar, K::Text }, false },
        { L"charset",       L"字符集",       AttrDescriptor::Text,   {},
          { K::Char, K::Varchar, K::Text, K::Enum }, false },
        { L"collation",     L"排序规则",     AttrDescriptor::Text,   {},
          { K::Char, K::Varchar, K::Text, K::Enum }, false },
        { L"comment",       L"注释",         AttrDescriptor::Text,   {}, {}, true },
        { L"autoIncrement", L"自增",         AttrDescriptor::Bool,   {},
          { K::Integer }, true },
        { L"onUpdate",      L"更新时",       AttrDescriptor::Choice, { L"CURRENT_TIMESTAMP" },
          { K::Timestamp }, false },
        { L"keyLength",     L"索引前缀长度", AttrDescriptor::Int,    {},
          { K::Varchar, K::Text, K::Binary, K::Blob }, false },
        { L"generated",     L"生成列",       AttrDescriptor::Expr,   {}, {}, true },
    };
    return kAttrs;
}

const TypeDescriptor* MySqlProfile::FindType(const wxString& name) const
{
    for (const auto& t : Types())
        if (t.name.IsSameAs(name, /*caseSensitive=*/false)) return &t;
    return nullptr;
}

// base + (len[,scale]) + [ unsigned] — just the type portion.
wxString MySqlProfile::RenderTypeSpec(const ColumnModel& c) const
{
    wxString s = c.type;
    const TypeDescriptor* td = FindType(c.type);
    if (td && td->takesLength && !c.length.IsEmpty()) {
        if (td->takesScale && !c.scale.IsEmpty())
            s += L"(" + c.length + L"," + c.scale + L")";
        else
            s += L"(" + c.length + L")";
    }
    if (AttrOn(c, L"unsigned")) s += L" unsigned";
    return s;
}

// `name` <typespec> [CHARACTER SET…] [COLLATE…] [GENERATED…] [NOT NULL]
// [AUTO_INCREMENT] [DEFAULT …] [ON UPDATE …] [COMMENT '…'].
wxString MySqlProfile::RenderColumnBody(const ColumnModel& c) const
{
    wxString s = Q(c.name) + L" " + RenderTypeSpec(c);

    if (AttrOn(c, L"binary"))                s += L" BINARY";
    const wxString cs = c.Attr(L"charset");
    if (!cs.IsEmpty())                        s += L" CHARACTER SET " + cs;
    const wxString col = !c.collation.IsEmpty() ? c.collation : c.Attr(L"collation");
    if (!col.IsEmpty())                       s += L" COLLATE " + col;

    const bool generated = !c.generatedExpr.IsEmpty();
    if (generated)
        s += L" GENERATED ALWAYS AS (" + c.generatedExpr + L") " +
             (c.generatedStored ? L"STORED" : L"VIRTUAL");

    if (c.notNull)                            s += L" NOT NULL";
    if (c.autoIncrement && !generated)        s += L" AUTO_INCREMENT";
    // Generated columns can't carry a DEFAULT; guard it.
    if (!c.defaultVal.IsEmpty() && !generated)
        s += L" DEFAULT " + detail::RenderDefault(c.defaultVal);

    const wxString onUpd = c.Attr(L"onUpdate");
    if (!onUpd.IsEmpty())                      s += L" ON UPDATE " + onUpd;

    if (!c.comment.IsEmpty()) {
        wxString e = c.comment; e.Replace(L"'", L"''");
        s += L" COMMENT '" + e + L"'";
    }
    return s;
}

// Rename ⇒ CHANGE COLUMN `old` <new body>; otherwise MODIFY COLUMN <body>.
wxString MySqlProfile::ModifyClause(const ColumnModel& c) const
{
    if (!c.origName.IsEmpty() && c.origName != c.name)
        return L"CHANGE COLUMN " + Q(c.origName) + L" " + RenderColumnBody(c);
    return L"MODIFY COLUMN " + RenderColumnBody(c);
}

// ---- index metadata (drives the design view's type / method dropdowns) ----

// "" (a plain index) is implicit and not listed. MySQL's extra kinds, in the order
// Navicat surfaces them.
std::vector<wxString> MySqlProfile::IndexTypes() const
{
    return { L"UNIQUE", L"FULLTEXT", L"SPATIAL" };
}

std::vector<wxString> MySqlProfile::IndexMethods() const
{
    return { L"BTREE", L"HASH" };
}

// [UNIQUE|FULLTEXT|SPATIAL ]INDEX `name` (`c1`, `c2`) [USING BTREE] [COMMENT '…'] —
// the index definition, shared by IndexClause (ALTER) and RenderCreate (inline).
wxString MySqlProfile::IndexInlineDef(const IndexModel& m) const
{
    // Kind keyword prefixes INDEX: "" → plain, else UNIQUE / FULLTEXT / SPATIAL.
    wxString kind;
    if (!m.type.IsEmpty()) kind = m.type.Upper() + L" ";

    wxString cols;
    for (size_t i = 0; i < m.columns.size(); ++i) {
        if (i) cols += L", ";
        cols += Q(m.columns[i]);
    }

    wxString c = kind + L"INDEX " + Q(m.name) + L" (" + cols + L")";

    // USING <method> is only valid for BTREE/HASH indexes — FULLTEXT/SPATIAL reject it.
    const bool ftOrSpatial = m.type.IsSameAs(L"FULLTEXT", /*caseSensitive=*/false) ||
                             m.type.IsSameAs(L"SPATIAL",  /*caseSensitive=*/false);
    if (!m.method.IsEmpty() && !ftOrSpatial)
        c += L" USING " + m.method.Upper();

    if (!m.comment.IsEmpty()) {
        wxString e = m.comment; e.Replace(L"'", L"''");
        c += L" COMMENT '" + e + L"'";
    }
    return c;
}

// ADD <index def>, or DROP INDEX `name`. Folded into the one ALTER TABLE by
// InlineAlterProfile.
wxString MySqlProfile::IndexClause(const IndexEdit& ie) const
{
    if (ie.op == IndexEdit::Drop)
        return L"DROP INDEX " + Q(ie.model.name);
    return L"ADD " + IndexInlineDef(ie.model);
}

// ---- table options (选项 tab) ----

std::vector<DbCreateOption> MySqlProfile::TableOptionSpecs() const
{
    using K = DbCreateOption::Kind;
    std::vector<DbCreateOption> out;
    { DbCreateOption o; o.id = L"engine";    o.label = L"引擎";     o.kind = K::Choice;
      o.choices = { L"InnoDB", L"MyISAM", L"MEMORY", L"ARCHIVE", L"CSV" };
      o.defaultVal = L"InnoDB"; out.push_back(o); }
    { DbCreateOption o; o.id = L"charset";   o.label = L"字符集";   o.kind = K::Choice;
      o.choices = { L"utf8mb4", L"utf8", L"latin1", L"gbk", L"ascii" }; out.push_back(o); }
    // Collation is a Choice filtered by the selected charset (groupSource): choices
    // lists every candidate, choiceGroup marks the charset each collation belongs to.
    { DbCreateOption o; o.id = L"collation"; o.label = L"排序规则"; o.kind = K::Choice;
      o.groupSource = L"charset";
      o.choices = { L"utf8mb4_general_ci", L"utf8mb4_unicode_ci", L"utf8mb4_bin",
                    L"utf8_general_ci", L"utf8_bin",
                    L"latin1_swedish_ci", L"latin1_bin",
                    L"gbk_chinese_ci", L"gbk_bin",
                    L"ascii_general_ci", L"ascii_bin" };
      o.choiceGroup = { L"utf8mb4", L"utf8mb4", L"utf8mb4",
                        L"utf8", L"utf8",
                        L"latin1", L"latin1",
                        L"gbk", L"gbk",
                        L"ascii", L"ascii" };
      out.push_back(o); }
    { DbCreateOption o; o.id = L"rowFormat"; o.label = L"行格式";   o.kind = K::Choice;
      o.choices = { L"DEFAULT", L"DYNAMIC", L"COMPRESSED", L"REDUNDANT", L"COMPACT" };
      out.push_back(o); }
    { DbCreateOption o; o.id = L"maxRows";       o.label = L"最大行数"; o.kind = K::Int;
      o.defaultVal = L"0"; out.push_back(o); }
    { DbCreateOption o; o.id = L"autoIncrement"; o.label = L"自增起始"; o.kind = K::Int;
      out.push_back(o); }
    { DbCreateOption o; o.id = L"avgRowLength";  o.label = L"平均行长"; o.kind = K::Int;
      out.push_back(o); }
    { DbCreateOption o; o.id = L"checksum";      o.label = L"校验和";   o.kind = K::Choice;
      o.choices = { L"0", L"1" }; out.push_back(o); }
    return out;
}

// ALTER TABLE `db`.`t` ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=… ROW_FORMAT=…
// — only the fields the user actually set; empty request → no statement.
void MySqlProfile::RenderOptionsChange(const wxString& qt, const TableOptions& o,
                                       std::vector<wxString>& stmts) const
{
    wxString s;
    auto add = [&s](const wxString& clause) {
        if (!s.IsEmpty()) s += L" ";
        s += clause;
    };
    if (!o.engine.IsEmpty())    add(L"ENGINE=" + o.engine);
    if (!o.charset.IsEmpty())   add(L"DEFAULT CHARSET=" + o.charset);
    if (!o.collation.IsEmpty()) add(L"COLLATE=" + o.collation);
    if (!o.rowFormat.IsEmpty()) add(L"ROW_FORMAT=" + o.rowFormat);
    // Numeric storage hints — "" or "0" means unlimited/default, so omit them.
    if (!o.maxRows.IsEmpty()       && o.maxRows != L"0")       add(L"MAX_ROWS=" + o.maxRows);
    if (!o.autoIncrement.IsEmpty() && o.autoIncrement != L"0") add(L"AUTO_INCREMENT=" + o.autoIncrement);
    if (!o.avgRowLength.IsEmpty()  && o.avgRowLength != L"0")  add(L"AVG_ROW_LENGTH=" + o.avgRowLength);
    // CHECKSUM is a 0/1 flag: empty omits, but 0 is a meaningful explicit value.
    if (!o.checksum.IsEmpty())  add(L"CHECKSUM=" + o.checksum);
    if (s.IsEmpty()) return;
    stmts.push_back(L"ALTER TABLE " + qt + L" " + s);
}

// MySQL keeps the table comment as a table option: ALTER TABLE … COMMENT = '…'.
void MySqlProfile::RenderCommentChange(const wxString& qt, const wxString& comment,
                                       std::vector<wxString>& stmts) const
{
    wxString e = comment; e.Replace(L"'", L"''");
    stmts.push_back(L"ALTER TABLE " + qt + L" COMMENT = '" + e + L"'");
}

// ---- full CREATE TABLE (TABLE DDL tab) ----

// The trailing " ENGINE=… DEFAULT CHARSET=… COLLATE=… ROW_FORMAT=… COMMENT='…'" for
// a CREATE TABLE; "" when neither options nor a comment are set. Leading space so it
// splices directly after the closing ')'.
wxString MySqlProfile::OptionSuffix(const TableOptions& o, const wxString& comment) const
{
    wxString s;
    auto add = [&s](const wxString& clause) {
        if (!s.IsEmpty()) s += L" ";
        s += clause;
    };
    if (!o.engine.IsEmpty())    add(L"ENGINE=" + o.engine);
    if (!o.charset.IsEmpty())   add(L"DEFAULT CHARSET=" + o.charset);
    if (!o.collation.IsEmpty()) add(L"COLLATE=" + o.collation);
    if (!o.rowFormat.IsEmpty()) add(L"ROW_FORMAT=" + o.rowFormat);
    // Numeric storage hints — "" or "0" means unlimited/default, so omit them.
    if (!o.maxRows.IsEmpty()       && o.maxRows != L"0")       add(L"MAX_ROWS=" + o.maxRows);
    if (!o.autoIncrement.IsEmpty() && o.autoIncrement != L"0") add(L"AUTO_INCREMENT=" + o.autoIncrement);
    if (!o.avgRowLength.IsEmpty()  && o.avgRowLength != L"0")  add(L"AVG_ROW_LENGTH=" + o.avgRowLength);
    // CHECKSUM is a 0/1 flag: empty omits, but 0 is a meaningful explicit value.
    if (!o.checksum.IsEmpty())  add(L"CHECKSUM=" + o.checksum);
    if (!comment.IsEmpty()) {
        wxString e = comment; e.Replace(L"'", L"''");
        add(L"COMMENT='" + e + L"'");
    }
    return s.IsEmpty() ? wxString() : (L" " + s);
}

// MySQL inlines everything into the CREATE TABLE body: columns, PRIMARY KEY, KEY/
// INDEX defs and CONSTRAINT … FOREIGN KEY …; engine/charset/comment go in the tail.
// Triggers are the one thing MySQL cannot inline — they follow as CREATE TRIGGER.
bool MySqlProfile::RenderCreate(const TableModel& model,
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

    for (const auto& idx : model.indexes)
        lines.push_back(IndexInlineDef(idx));
    for (const auto& fk : model.fks)
        lines.push_back(RenderFkBody(fk));

    wxString sql = L"CREATE TABLE " + qt + L" (\n";
    for (size_t i = 0; i < lines.size(); ++i)
        sql += L"  " + lines[i] + (i + 1 < lines.size() ? L",\n" : L"\n");
    sql += L")";
    sql += OptionSuffix(model.hasOptions ? model.options : TableOptions(), model.comment);
    stmts.push_back(sql);

    for (const auto& tg : model.triggers) {
        TriggerEdit te; te.op = TriggerEdit::Add; te.model = tg;
        RenderTriggerChange(qt, te, stmts);
    }
    return true;
}

} // namespace

// Per-DbType singleton accessor (wired by DialectRegistry.cpp).
const DialectProfile& MySqlDialectProfile()
{
    static const MySqlProfile inst;
    return inst;
}

} // namespace db
