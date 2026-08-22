// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SqliteStream.h — parameterized batch INSERT for the SQLite driver (T4 of
// ADR-015), the piece that lets SQLite be a data-sync TARGET and not only a
// source.
//
// A free function rather than a SqliteConnection method, for the same reason
// MySqlStream / PgStream / PgSchemaRead exist: SqliteDriver.cpp is at 898 lines
// against the charter's 1000-line per-file ceiling, so new capability lands in
// its own TU and the driver keeps a one-line delegating override. Like
// MySqlStream (and unlike PgSchemaRead) this DOES take the raw handle —
// parameter binding has no equivalent on the public IConnection surface, and
// binding is the whole point. That makes this a driver-private header: included
// only by SqliteDriver.cpp and SqliteStream.cpp, both inside swiftsql_db, which
// is also the only target the sqlite3 include dir is exposed to.
#pragma once

#include <wx/string.h>
#include <vector>

#include <sqlite3.h>

#include "db/SyncTypes.h"   // Cell

namespace db {
namespace sqlitestream {

// Parameterized, batched INSERT — see IConnection::ExecuteBatch for the contract
// (`sql` is the statement head through VALUES, with NO tuples; the driver
// appends its own native placeholders). Every Cell is BOUND, so no value is ever
// rendered into SQL text and the escaping-bug class this API exists to eliminate
// cannot reappear here. An empty `rows` is a no-op success.
//
// PLACEHOLDER STRATEGY: one multi-row VALUES tuple set per chunk — the head plus
// `(?, ?, …), (?, ?, …), …` — prepared once with sqlite3_prepare_v2, bound with
// n*ncols parameters, and run with a single sqlite3_step. This is the same shape
// MySqlStream and PgStream use, so all three targets chunk and fail identically
// and there is one batching behaviour to reason about rather than three.
//
// The alternative — prepare ONE single-row tuple and sqlite3_reset between rows
// — was considered and rejected. It would sidestep the variable limit entirely,
// but it also changes what a "batch" means on this engine only: a partial
// failure would leave a different number of rows written than on MySQL/PG, and
// the caller (DataSyncExec) counts rep_.inserts per successful ExecuteBatch
// call. Matching the other two drivers is worth more than saving a prepare,
// especially on an embedded engine where a prepare is a local function call and
// not a round trip.
//
// BATCH SIZE: ~500 rows, lowered so a chunk never exceeds this connection's
// SQLITE_LIMIT_VARIABLE_NUMBER. That limit is READ FROM THE HANDLE at call time
// via sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1) rather than hardcoded,
// because it is a per-build/per-connection value: SQLite shipped 999 for years
// and raised the default to 32766 in 3.32, an application may lower it, and
// SQLITE_MAX_VARIABLE_NUMBER can be set at compile time. Hardcoding either
// number would mean silently over-running the cap on an old or hardened build
// ("too many SQL variables", the whole batch fails) or needlessly halving the
// batch on a modern one.
bool ExecuteBatch(sqlite3* db, const wxString& sql,
                  const std::vector<std::vector<Cell>>& rows, wxString& err);

} // namespace sqlitestream
} // namespace db
