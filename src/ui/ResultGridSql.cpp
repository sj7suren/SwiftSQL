// ResultGridSql.cpp — implementation of the pure result-grid SQL generator.
// Identifiers are quoted through db::QuoteIdent ONLY; read the note at the top
// of ResultGridSql.h (single quoting regime + identifier provenance) first.
#include "ui/ResultGridSql.h"

namespace ui::gridsql {

namespace {
// Sole identifier quoter for this module. A one-line shim over db::QuoteIdent so
// the argument order reads naturally at the ~10 call sites below and so there is
// exactly one place a future dialect subtlety would land.
inline wxString Q(const wxString& ident, db::Dialect d)
{
    return db::QuoteIdent(ident, d);
}
} // namespace

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
wxString RenderValue(const wxString& v)
{
    if (v == L"NULL") return L"NULL";              // driver's null marker
    wxString e = v;
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}

wxString QualifyForDml(const DmlContext& ctx)
{
    // Quote the two parts SEPARATELY — quoting "db.table" as one identifier
    // would produce a single name containing a dot, not a qualified reference.
    const bool mysql = (ctx.dialect == db::Dialect::MySQL);
    if (mysql && !ctx.db.IsEmpty())
        return Q(ctx.db, ctx.dialect) + L"." + Q(ctx.table, ctx.dialect);
    return Q(ctx.table, ctx.dialect);
}

std::vector<int> MapPkIndices(const DmlContext& ctx)
{
    std::vector<int> pkIdx;
    for (const wxString& pk : ctx.pkColumns)
        for (size_t c = 0; c < ctx.columns.size(); ++c)
            if (ctx.columns[c] == pk) { pkIdx.push_back(static_cast<int>(c)); break; }
    return pkIdx;
}

std::vector<wxString> MissingPkColumns(const DmlContext& ctx)
{
    std::vector<wxString> missing;
    for (const wxString& pk : ctx.pkColumns) {
        bool found = false;
        for (const wxString& col : ctx.columns)
            if (col == pk) { found = true; break; }
        if (!found) missing.push_back(pk);
    }
    return missing;
}

wxString BuildPkWhere(const DmlContext& ctx, const std::vector<int>& pkIdx,
                      const CellRow& row)
{
    // All-or-nothing: a partial key would silently widen the WHERE. Refusing
    // here means a caller that ignores its gate emits `WHERE ;` — rejected by
    // the server — instead of quietly updating rows the user never touched.
    if (pkIdx.size() != ctx.pkColumns.size()) return wxString();

    wxString w;
    for (size_t i = 0; i < pkIdx.size(); ++i) {
        const size_t ci = static_cast<size_t>(pkIdx[i]);
        if (ci >= row.size()) return wxString();      // short row: same rule
        if (i) w += L" AND ";
        w += Q(ctx.pkColumns[i], ctx.dialect) + L"=" + RenderValue(row[ci]);
    }
    return w;
}

// ---------------------------------------------------------------------------
// The edit → DML diff
// ---------------------------------------------------------------------------
bool HasPendingEdits(const DmlContext& ctx,
                     const CellGrid& origRows,
                     const std::vector<int>& rowOrig,
                     const CellGrid& deleted,
                     const CellGrid& current)
{
    if (!deleted.empty()) return true;

    const int cols = static_cast<int>(ctx.columns.size());
    for (size_t r = 0; r < current.size(); ++r) {
        const int oi = (r < rowOrig.size()) ? rowOrig[r] : -1;
        if (oi < 0 || static_cast<size_t>(oi) >= origRows.size()) {
            // A new row counts only once it has content — an empty added row is
            // not an edit (and generates no INSERT), so Save stays grey.
            for (int c = 0; c < cols && c < static_cast<int>(current[r].size()); ++c)
                if (!current[r][c].IsEmpty()) return true;
        } else {
            const CellRow& orig = origRows[oi];
            for (int c = 0; c < cols && c < static_cast<int>(current[r].size()) &&
                            c < static_cast<int>(orig.size()); ++c)
                if (current[r][c] != orig[c]) return true;
        }
    }
    return false;
}

EditDml BuildEditDml(const DmlContext& ctx,
                     const CellGrid& origRows,
                     const std::vector<int>& rowOrig,
                     const CellGrid& deleted,
                     const CellGrid& current)
{
    EditDml out;
    out.dirty = HasPendingEdits(ctx, origRows, rowOrig, deleted, current);

    // THE GATE. Classified before a single statement is built, so the
    // under-constrained state is unreachable rather than merely bounded.
    if (ctx.pkColumns.empty()) { out.gate = PkGate::NoPrimaryKey; return out; }
    out.missingPk = MissingPkColumns(ctx);
    if (!out.missingPk.empty()) { out.gate = PkGate::IncompleteKey; return out; }

    const std::vector<int> pkIdx = MapPkIndices(ctx);   // now provably complete
    const wxString fq = QualifyForDml(ctx);
    wxString dml;

    // deletes
    for (const CellRow& row : deleted)
        dml += L"DELETE FROM " + fq + L" WHERE " + BuildPkWhere(ctx, pkIdx, row) + L";\n";

    // updates + inserts
    const int cols = static_cast<int>(ctx.columns.size());
    for (size_t r = 0; r < current.size(); ++r) {
        const int oi = (r < rowOrig.size()) ? rowOrig[r] : -1;
        if (oi < 0 || static_cast<size_t>(oi) >= origRows.size()) {
            // INSERT — only non-empty cells (let defaults / auto-increment apply).
            // An out-of-range oi (rowOrig desynced from origRows) is treated as a
            // new row rather than indexed into origRows out of bounds.
            wxString cList, vList;
            for (int c = 0; c < cols && c < static_cast<int>(current[r].size()); ++c) {
                const wxString& v = current[r][c];
                if (v.IsEmpty()) continue;
                if (!cList.IsEmpty()) { cList += L", "; vList += L", "; }
                cList += Q(ctx.columns[c], ctx.dialect);
                vList += RenderValue(v);
            }
            if (!cList.IsEmpty())
                dml += L"INSERT INTO " + fq + L" (" + cList + L") VALUES ("
                     + vList + L");\n";
        } else {
            // UPDATE — only changed columns
            const CellRow& orig = origRows[oi];
            wxString sets;
            for (int c = 0; c < cols && c < static_cast<int>(current[r].size()) &&
                            c < static_cast<int>(orig.size()); ++c) {
                const wxString& cur = current[r][c];
                if (cur == orig[c]) continue;
                if (!sets.IsEmpty()) sets += L", ";
                sets += Q(ctx.columns[c], ctx.dialect) + L"=" + RenderValue(cur);
            }
            if (!sets.IsEmpty())
                dml += L"UPDATE " + fq + L" SET " + sets + L" WHERE "
                     + BuildPkWhere(ctx, pkIdx, orig) + L";\n";
        }
    }
    out.sql = dml;
    return out;
}

// ---------------------------------------------------------------------------
// Scripted edit (SWIFTSQL_AUTOEDIT)
// ---------------------------------------------------------------------------
// One token per outcome. Deliberately NOT error-shaped for the benign cases:
// UNCHANGED and NO_ROWS are findings, not failures, and a CI reader that greps
// for ERR must not trip over them. A REFUSED:* prefix marks the two cases where
// a write was actively declined, which is the pair worth alerting on.
wxString ScriptedEditToken(const ScriptedEditResult& r)
{
    switch (r.status) {
        case ScriptedEditStatus::Ok:           return L"OK";
        case ScriptedEditStatus::NoChange:     return L"UNCHANGED";
        case ScriptedEditStatus::NoRows:       return L"NO_ROWS";
        case ScriptedEditStatus::NoSuchColumn: return L"NO_COLUMN:" + r.detail;
        case ScriptedEditStatus::NotEditable:
            return r.detail.IsEmpty() ? wxString(L"REFUSED:NOT_EDITABLE")
                                      : L"REFUSED:NOT_EDITABLE:" + r.detail;
        case ScriptedEditStatus::PkRefused:
            // detail carries the missing columns; empty ⇔ the table has no PK.
            return r.detail.IsEmpty() ? wxString(L"REFUSED:NO_PRIMARY_KEY")
                                      : L"REFUSED:INCOMPLETE_KEY:" + r.detail;
    }
    return L"REFUSED:NOT_EDITABLE";   // unreachable; never fall through to OK
}

// ---------------------------------------------------------------------------
// 复制为 SQL
// ---------------------------------------------------------------------------
wxString BuildInsertStatements(const DmlContext& ctx, const CellGrid& rows)
{
    if (ctx.columns.empty()) return wxString();
    const wxString tbl = ctx.table.IsEmpty() ? wxString(L"table_name") : ctx.table;
    const wxString fq  = Q(tbl, ctx.dialect);

    wxString out;
    const int nc = static_cast<int>(ctx.columns.size());
    for (const CellRow& row : rows) {
        wxString cList, vList;
        for (int c = 0; c < nc; ++c) {
            if (c) { cList += L", "; vList += L", "; }
            cList += Q(ctx.columns[c], ctx.dialect);
            vList += RenderValue(c < static_cast<int>(row.size()) ? row[c] : wxString());
        }
        out += L"INSERT INTO " + fq + L" (" + cList + L") VALUES (" + vList + L");\n";
    }
    return out;
}

wxString BuildRowDmlStatements(const DmlContext& ctx, const CellGrid& rows, bool asUpdate)
{
    if (ctx.columns.empty() || ctx.pkColumns.empty()) return wxString();
    const std::vector<int> pkIdx = MapPkIndices(ctx);
    if (pkIdx.size() != ctx.pkColumns.size()) return wxString();  // a pk col isn't in the result

    const wxString fq = QualifyForDml(ctx);
    wxString out;
    const int nc = static_cast<int>(ctx.columns.size());
    for (const CellRow& row : rows) {
        if (asUpdate) {
            wxString sets;
            for (int c = 0; c < nc; ++c) {
                bool isPk = false;
                for (int pi : pkIdx) if (pi == c) { isPk = true; break; }
                if (isPk) continue;                        // pk goes in WHERE, not SET
                if (!sets.IsEmpty()) sets += L", ";
                sets += Q(ctx.columns[c], ctx.dialect) + L"=" +
                        RenderValue(c < static_cast<int>(row.size()) ? row[c] : wxString());
            }
            out += L"UPDATE " + fq + L" SET " + sets + L" WHERE "
                 + BuildPkWhere(ctx, pkIdx, row) + L";\n";
        } else {
            out += L"DELETE FROM " + fq + L" WHERE " + BuildPkWhere(ctx, pkIdx, row) + L";\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Pager / filter
// ---------------------------------------------------------------------------
wxString QualifyForPaging(db::Dialect d, const wxString& db, const wxString& table)
{
    // Quote each identifier through db::QuoteIdent so an embedded quote in a db /
    // table name is doubled (not bare-spliced). MySQL needs `db`.`table` so the
    // SELECT works with no default schema selected ("No database selected");
    // other dialects are already inside a database.
    if (d == db::Dialect::MySQL && !db.IsEmpty())
        return db::QuoteIdent(db, d) + L"." + db::QuoteIdent(table, d);
    return db::QuoteIdent(table, d);
}

wxString BuildOrderByBody(const std::vector<wxString>& columns,
                          const std::vector<SortKey>& keys, db::Dialect d)
{
    wxString orderBy;
    for (const SortKey& k : keys) {
        if (k.col < 0 || k.col >= static_cast<int>(columns.size())) continue;
        if (!orderBy.IsEmpty()) orderBy += L", ";
        orderBy += db::QuoteIdent(columns[k.col], d) + (k.asc ? L" ASC" : L" DESC");
    }
    return orderBy;
}

wxString BuildPageSelect(db::Dialect d, const wxString& db, const wxString& table,
                         const wxString& where, const std::vector<wxString>& columns,
                         const std::vector<SortKey>& keys, int pageSize, int page)
{
    wxString sql = L"SELECT * FROM " + QualifyForPaging(d, db, table);
    if (!where.IsEmpty()) sql += L"\nWHERE " + where;
    const wxString orderBy = BuildOrderByBody(columns, keys, d);
    if (!orderBy.IsEmpty()) sql += L"\nORDER BY " + orderBy;
    sql += L"\n" + db::LimitOffsetClause(d, pageSize,
               static_cast<long long>(page) * pageSize).Trim(false) + L";";
    return sql;
}

wxString BuildCellFilterClause(db::Dialect d, const wxString& column,
                               const wxString& value, bool exclude)
{
    wxString clause = Q(column, d);
    if (value == L"NULL") {
        clause += exclude ? L" IS NOT NULL" : L" IS NULL";
    } else {
        wxString e = value;
        e.Replace(L"'", L"''");
        clause += (exclude ? L" <> '" : L" = '") + e + L"'";
    }
    return clause;
}

} // namespace ui::gridsql
