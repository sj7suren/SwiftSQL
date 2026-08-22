// PgSchemaRead.h — real PostgreSQL table-structure introspection for the
// cross-database synchronization suite (A2 of docs/design/cross-db-sync.md).
// Free functions (not a PgConnection method) so PgDriver.cpp only needs a
// thin delegating override — see the "keep PgDriver.cpp near the 1000-line
// charter ceiling" note in PgDriver.cpp. Operates purely through the public
// db::IConnection surface (Execute), mirroring the shape of MySqlDriver.cpp's
// GetTableSchema (MySqlSql.cpp's detail::MapColKind counterpart) but querying
// PostgreSQL's information_schema/pg_catalog instead. Never touches a PGconn*.
//
// ---------------------------------------------------------------------------
// SCHEMA QUALIFICATION — why this file stopped delegating
// ---------------------------------------------------------------------------
// This function used to hard-code `table_schema='public'` and fetch the PK,
// indexes and foreign keys by delegating to IConnection::GetPrimaryKey /
// GetIndexes / GetForeignKeys — whose PostgreSQL overrides ALSO hard-code
// 'public'. Measured against a real server, the consequence was:
//
//   GetTableSchema(db, "only_here")  ->  returns TRUE, with ZERO COLUMNS
//
// for a table living in the `archive` schema. Zero columns is DiffSchema's
// convention for "this table does not exist", so a table the picker had just
// listed was introspected as absent — a silent misread, not an error.
//
// It now issues every catalog query itself, filtered on the schema it was ASKED
// for. Delegating was the actual defect, so the delegation is gone rather than
// patched: the three IConnection overrides remain 'public'-scoped for the
// general UI paths that call them with a bare name, and the sync path — the one
// that must be exact — no longer routes through them.
#pragma once

#include <wx/string.h>

#include "db/DbDriver.h"   // IConnection / TableSchema / QualifiedName

namespace db {
namespace pgschema {

// Normalized structure for `table` in the CURRENT database, read from the schema
// `table` names. The caller (PgDriver.cpp's thin override) is responsible for
// ensureDb()-ing to the right database first — this function only issues
// Execute() calls against whatever database `conn` is currently bound to.
//
// An UNQUALIFIED `table` (no schema half) resolves through the session's
// search_path, via pg_catalog's own `::regclass` resolution rather than a
// guessed 'public' — so a caller that genuinely does not care gets the schema
// PostgreSQL itself would have picked, and `out.name` reports which one that
// turned out to be. A qualified name is used exactly as given.
bool GetTableSchema(IConnection& conn, const QualifiedName& table,
                    TableSchema& out, wxString& err);

} // namespace pgschema
} // namespace db
