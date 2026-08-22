// PgStream.h — PostgreSQL-side row streaming/writing helpers for the
// cross-database synchronization suite (T2/T4 of ADR-015).
//
// Free functions in their own TU, the same arrangement as PgSchemaRead.{h,cpp}
// and for the same reason (keep PgDriver.cpp under the 1000-line charter
// ceiling). Unlike PgSchemaRead these take the raw PGconn* — parameter binding
// has no equivalent on the public IConnection surface, and binding is the whole
// point of ExecuteBatch. Driver-private header: included only by PgDriver.cpp
// and PgStream.cpp, both inside swiftsql_db, the only target libpq's include
// dir is exposed to.
#pragma once

#include <wx/string.h>
#include <vector>

#include <libpq-fe.h>

#include "db/SyncTypes.h"   // Cell / StreamOptions

namespace db {
namespace pgstream {

// The ORDER BY clause for a structured row stream (leading space included; ""
// when opt.orderBy is empty).
//
// When opt.binaryOrder is set, every column named in opt.binaryOrderCols is
// ordered `col COLLATE "C"` — byte ordering, which for a UTF-8 database equals
// Unicode code-point order and therefore matches both MySQL's utf8mb4_bin and
// DataSync's CmpCell. This is the query-level half of ADR-015 T2: PostgreSQL's
// default collation (typically an ICU/libc locale) sorts a VARCHAR key by
// language rules — ignoring case and accents at the primary level — which is a
// different order from MySQL's, and the sorted-merge diff silently desyncs on
// the difference.
//
// Columns not listed in binaryOrderCols are ordered natively: COLLATE is a type
// error on non-collatable types ("collations are not supported by type
// integer"), and integer / uuid / timestamp / bytea keys have no collation to
// disagree about in the first place.
wxString OrderByClause(const StreamOptions& opt);

// Parameterized, batched INSERT — see IConnection::ExecuteBatch for the contract
// (`sql` is the statement head through VALUES, with no tuples). Sends each batch
// through PQexecParams with $n placeholders, so no value is ever rendered into
// SQL text; NULLs bind as a null pointer and Binary cells bind as bytea's
// `\xHEX` input form. Batches ~500 rows, capped so a batch never exceeds libpq's
// 65535-parameter limit. An empty `rows` is a no-op success.
bool ExecuteBatch(PGconn* conn, const wxString& sql,
                  const std::vector<std::vector<Cell>>& rows, wxString& err);

} // namespace pgstream
} // namespace db
