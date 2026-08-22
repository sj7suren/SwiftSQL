// SqlLiteral.cpp — RenderLiteral: one typed Cell → dialect SQL literal.
// The escaping rules are lifted verbatim from MySqlDriver::DumpTableData so the
// data-sync DML path and the dump path stay byte-for-byte compatible.
#include "db/SyncTypes.h"
#include "db/DbDriver.h"   // db::Dialect (full definition)

namespace db {

wxString RenderLiteral(const Cell& c, Dialect d)
{
    switch (c.kind) {
    case CellKind::Null:
        return L"NULL";

    case CellKind::Numeric:
        // Bare, unquoted (matches DumpTableData's NUM_FLAG branch). Empty text
        // shouldn't happen for a numeric, but guard rather than emit a bad token.
        return c.text.IsEmpty() ? L"NULL" : c.text;

    case CellKind::Binary: {
        // text = raw hex digits, no prefix. Empty blob → '' (0x needs digits).
        if (c.text.IsEmpty()) return L"''";
        if (d == Dialect::Postgres) return L"'\\x" + c.text + L"'";   // bytea escape
        if (d == Dialect::Oracle)  return L"HEXTORAW('" + c.text + L"')"; // Oracle / DM RAW
        if (d == Dialect::Sqlite)  return L"X'" + c.text + L"'";       // SQLite blob (0x parses as integer!)
        return L"0x" + c.text;                                         // MySQL / SQL Server
    }

    case CellKind::Text:
    default: {
        wxString e = c.text;
        if (d == Dialect::MySQL) {
            e.Replace(L"\\", L"\\\\");   // MySQL treats backslash as an escape char
            e.Replace(L"'", L"''");
        } else {
            e.Replace(L"'", L"''");      // standard SQL: only the quote is doubled
        }
        return L"'" + e + L"'";
    }
    }
}

} // namespace db
