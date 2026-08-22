// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MySqlSql.cpp — implementations of the connection-free MySQL SQL-rendering
// helpers declared in MySqlSql.h. Moved verbatim out of MySqlDriver.cpp; behavior
// is unchanged.
#include "db/MySqlSql.h"

namespace db {
namespace detail {

ColKind MapColKind(const wxString& dataType, const wxString& columnType)
{
    const wxString t = dataType.Lower();
    if (t == L"tinyint")
        return columnType.Lower().StartsWith(L"tinyint(1)")
                   ? ColKind::Boolean : ColKind::Integer;
    if (t == L"smallint" || t == L"mediumint" || t == L"int" ||
        t == L"integer"  || t == L"bigint"    || t == L"bit"  || t == L"year")
        return ColKind::Integer;
    if (t == L"decimal" || t == L"numeric" || t == L"dec" || t == L"fixed")
        return ColKind::Decimal;
    if (t == L"float" || t == L"double" || t == L"real")
        return ColKind::Float;
    if (t == L"bool" || t == L"boolean")   return ColKind::Boolean;
    if (t == L"char")                       return ColKind::Char;
    if (t == L"varchar")                    return ColKind::Varchar;
    if (t == L"tinytext" || t == L"text" || t == L"mediumtext" || t == L"longtext")
        return ColKind::Text;
    if (t == L"binary" || t == L"varbinary") return ColKind::Binary;
    if (t == L"tinyblob" || t == L"blob" || t == L"mediumblob" || t == L"longblob")
        return ColKind::Blob;
    if (t == L"date")                       return ColKind::Date;
    if (t == L"time")                       return ColKind::Time;
    if (t == L"datetime" || t == L"timestamp") return ColKind::Timestamp;
    if (t == L"json")                       return ColKind::Json;
    if (t == L"enum" || t == L"set")        return ColKind::Enum;
    return ColKind::Other;
}

std::vector<wxString> SplitCsv(const wxString& s)
{
    std::vector<wxString> out;
    wxString rest = s;
    while (true) {
        wxString part = rest.BeforeFirst(L',');
        part.Trim(true).Trim(false);
        if (!part.IsEmpty()) out.push_back(part);
        if (!rest.Contains(L",")) break;
        rest = rest.AfterFirst(L',');
    }
    return out;
}

wxString JoinIdents(const std::vector<wxString>& cols)
{
    wxString s;
    for (size_t i = 0; i < cols.size(); ++i)
        s += (i ? L", " : L"") + QuoteIdent(cols[i], Dialect::MySQL);
    return s;
}

wxString RenderDefault(const wxString& v)
{
    if (v.IsEmpty()) return L"''";
    const wxString up = v.Upper();
    double d;
    if (up == L"NULL" || up == L"CURRENT_TIMESTAMP" ||
        v.Contains(L"(") || v.ToDouble(&d))
        return v;
    wxString e = v;
    e.Replace(L"\\", L"\\\\");
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}

wxString ColumnDef(const NormColumn& c)
{
    wxString s = QuoteIdent(c.name, Dialect::MySQL) + L" " + c.rawType;
    if (c.notNull)       s += L" NOT NULL";
    if (c.autoIncrement) s += L" AUTO_INCREMENT";
    if (c.hasDefault)    s += L" DEFAULT " + RenderDefault(c.defaultExpr);
    return s;
}

wxString IndexDef(const NormIndex& idx)
{
    wxString s = (idx.unique ? L"UNIQUE INDEX " : L"INDEX ");
    s += QuoteIdent(idx.name, Dialect::MySQL) + L" (" +
         JoinIdents(idx.columns) + L")";
    return s;
}

wxString FkDef(const NormForeignKey& fk)
{
    wxString s;
    if (!fk.name.IsEmpty())
        s = L"CONSTRAINT " + QuoteIdent(fk.name, Dialect::MySQL) + L" ";
    s += L"FOREIGN KEY (" + JoinIdents(fk.columns) + L") REFERENCES " +
         QuoteIdent(fk.refTable, Dialect::MySQL) + L" (" +
         JoinIdents(fk.refColumns) + L")";
    if (!fk.onDelete.IsEmpty()) s += L" ON DELETE " + fk.onDelete;
    if (!fk.onUpdate.IsEmpty()) s += L" ON UPDATE " + fk.onUpdate;
    return s;
}

wxString RenderCreateTable(const TableSchema& s)
{
    std::vector<wxString> defs;
    for (const auto& c : s.columns) defs.push_back(L"  " + ColumnDef(c));
    if (!s.primaryKey.empty())
        defs.push_back(L"  PRIMARY KEY (" + JoinIdents(s.primaryKey) + L")");
    for (const auto& idx : s.indexes)
        if (!idx.primary) defs.push_back(L"  " + IndexDef(idx));
    for (const auto& fk : s.foreignKeys)
        defs.push_back(L"  " + FkDef(fk));

    wxString sql = L"CREATE TABLE IF NOT EXISTS " +
                   QuoteIdent(s.name.Name(), Dialect::MySQL) + L" (\n";
    for (size_t i = 0; i < defs.size(); ++i)
        sql += defs[i] + (i + 1 < defs.size() ? L",\n" : L"\n");
    sql += L")";
    if (!s.engine.IsEmpty())    sql += L" ENGINE=" + s.engine;
    if (!s.charset.IsEmpty())   sql += L" DEFAULT CHARSET=" + s.charset;
    if (!s.collation.IsEmpty()) sql += L" COLLATE=" + s.collation;
    if (!s.comment.IsEmpty()) {
        wxString e = s.comment; e.Replace(L"'", L"''");
        sql += L" COMMENT='" + e + L"'";
    }
    return sql;
}

} // namespace detail
} // namespace db
