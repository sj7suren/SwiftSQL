// MySqlStream.h — MySQL-side row streaming/writing helpers for the
// cross-database synchronization suite (T2/T4 of ADR-015).
//
// Free functions rather than MySqlConnection methods, for the same reason
// PgSchemaRead.{h,cpp} exists: MySqlDriver.cpp sits at the charter's 1000-line
// per-file ceiling with no headroom, so new capability lands in its own TU and
// the driver keeps only a one-line delegating override. Unlike PgSchemaRead
// these DO take the raw MYSQL* handle — parameter binding has no equivalent on
// the public IConnection surface, and binding is the whole point (see
// ExecuteBatch below). That makes this a driver-private header: it is included
// only by MySqlDriver.cpp and MySqlStream.cpp, both inside swiftsql_db, which
// is also the only target the libmariadb include dir is exposed to.
#pragma once

#include <wx/string.h>
#include <vector>

#include <mysql.h>

#include "db/SyncTypes.h"   // Cell / StreamOptions

namespace db {
namespace mysqlstream {

// ---- raw bytes -> hex ------------------------------------------------------
// Moved here verbatim from MySqlConnection's private statics: they are pure
// byte->text functions with no connection state, they are the encoding half of
// exactly the row streaming/writing this TU owns, and MySqlDriver.cpp needed the
// lines back (see the header banner).

// Raw bytes → uppercase hex digits, NO prefix (StreamRows' Binary cell text;
// RenderLiteral adds the dialect framing). Empty blob → "".
wxString HexDigits(const char* data, unsigned long len);

// Raw bytes → MySQL hex literal 0xDEADBEEF. Empty blob → '' (0x needs digits).
wxString HexLiteral(const char* data, unsigned long len);

// The ORDER BY clause for a structured row stream (leading space included; ""
// when opt.orderBy is empty).
//
// When opt.binaryOrder is set, every column named in opt.binaryOrderCols is
// ordered as `CONVERT(col USING utf8mb4) COLLATE utf8mb4_bin` — byte ordering of
// the UTF-8 encoding, which equals Unicode code-point order and therefore
// matches both PostgreSQL's `COLLATE "C"` and DataSync's CmpCell. This is the
// query-level half of ADR-015 T2: without it MySQL's stock utf8mb4_general_ci
// returns a VARCHAR key case- and accent-folded, in a different order from
// PostgreSQL, and the sorted-merge diff silently desyncs.
//
// CONVERT(...) rather than a bare COLLATE because utf8mb4_bin is only a legal
// collation for a utf8mb4 expression — a latin1 or utf8mb3 column would raise
// error 1253. Converting first normalizes any column charset to the one the
// client session already uses. Columns NOT listed in binaryOrderCols are
// ordered natively: COLLATE is invalid on non-string types, and integer / date /
// binary keys have no collation to disagree about.
wxString OrderByClause(const StreamOptions& opt);

// Parameterized, batched INSERT — see IConnection::ExecuteBatch for the contract
// (`sql` is the statement head through VALUES, with no tuples). Uses a prepared
// statement per batch and binds every Cell, so no value is ever rendered into
// SQL text; NULLs bind as NULL and Binary cells are hex-decoded and bound as raw
// bytes. Batches ~500 rows, capped so a batch never exceeds MySQL's 65535
// placeholder limit. An empty `rows` is a no-op success.
bool ExecuteBatch(MYSQL* conn, const wxString& sql,
                  const std::vector<std::vector<Cell>>& rows, wxString& err);

} // namespace mysqlstream
} // namespace db
