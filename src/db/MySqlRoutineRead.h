// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MySqlRoutineRead.h — MySQL routine introspection for the compare-only
// functions & stored procedures diff (ADR-013).
//
// Free functions operating purely through the public db::IConnection surface
// (Execute), never a MYSQL* handle — the same pattern as PgSchemaRead /
// MySqlStream / DataSyncExec, and for the same reason: MySqlDriver.cpp is at
// 978/1000 lines against the charter's hard ceiling, so new cross-cutting
// capability goes in its own translation unit rather than into the driver class.
//
// READS ONLY. This unit issues SELECTs against information_schema and returns
// value objects. It renders no DDL and has no write path — see RoutineDiff.h's
// structural guarantee.
#pragma once

#include <wx/string.h>
#include <vector>

#include "db/DbDriver.h"      // IConnection
#include "db/RoutineDiff.h"   // RoutineDef / RoutineReadStatus

namespace db {
namespace mysqlroutine {

// Read every function and procedure in `database`.
//
// ===========================================================================
// TWO SERVER GENERATIONS, TWO DIFFERENT WAYS OF HIDING ROUTINES
// ===========================================================================
// This reader has to survive both, and they fail in opposite directions. Do not
// delete either defense on the grounds that the other one covers it.
//
// MySQL 8 — HIDES THE ROWS. Measured on 8.0.36 (tests/mysql_pg_live_routines.cpp,
// hazard 7d) with an account holding only `GRANT SELECT ON db.*`:
//
//   information_schema.ROUTINES    -> query SUCCEEDS, returns ZERO ROWS
//   information_schema.PARAMETERS  -> query SUCCEEDS, returns ZERO ROWS
//
// Not an error, and not a NULL body: the rows are filtered out, because 8.0
// backs information_schema with the data dictionary and applies privilege
// filtering per row. Every other defense in this file misses that: the
// blank-body heuristic has nothing to list, and the PARAMETERS degradation
// requires a FAILED query. The reader used to answer Ok + empty vector, i.e.
// "genuinely empty catalog" — so a restricted source compared against a
// privileged target reported every one of the target's routines as
// 仅目标端存在, confidently and silently.
//
// THE FIX: AN EMPTY LIST IS AMBIGUOUS, SO IT IS NOT ANSWERED FROM THE LIST.
// "No routines" and "no privilege to see routines" are the same response on
// MySQL 8, so when the list comes back empty this reader acquires a SECOND
// SIGNAL — SHOW GRANTS FOR CURRENT_USER(), scanned for EXECUTE / CREATE ROUTINE
// / ALTER ROUTINE / ALL PRIVILEGES on this database or globally — and answers
// from that:
//
//   empty + privileges demonstrably HELD    -> Ok. A real empty catalog. This
//       case must keep working: a legitimately empty database read by a
//       privileged account must never start reporting a permission problem.
//   empty + privileges demonstrably ABSENT  -> PermissionDenied, with a detail
//       naming the missing privileges.
//   empty + privileges INDETERMINATE        -> Unsupported, with a detail saying
//       so. NOT Ok — Ok is the silent false-positive state and is never the
//       fallback for "we could not tell". Not PermissionDenied either, because
//       that asserts something about the account we have not established. See
//       EmptyListVerdict() in the .cpp for the full argument.
//
// The probe is cheap and failure-tolerant by contract: ONE query, run only on
// the empty path, and every failure mode (query error, unparsable output, role
// grants, wildcard database patterns) resolves to Indeterminate rather than to
// an error or to a guess.
//
// MySQL 5.x — BLANKS THE BODY. 5.x does NOT fail and does NOT hide the row when
// the account lacks routine privileges. It lists the routines anyway and returns
// ROUTINE_DEFINITION as NULL. That path is UNCHANGED by the MySQL 8 fix above
// (which only touches the empty-list case) and remains load-bearing. So:
//
//   * a routine whose body came back empty is marked RoutineDef::bodyReadable =
//     false rather than being given an empty body — otherwise two routines with
//     hidden bodies would compare equal and be reported "相同";
//   * if EVERY routine in a non-empty list has a hidden body, that is not a
//     coincidence, it is a privilege state, and the whole side is reported
//     PermissionDenied so the UI collapses the category to its "不可见" label;
//   * an empty list returns Ok with an empty vector ONLY when the privilege
//     probe above confirms the account could have seen routines. "No routines"
//     and "cannot see routines" are different answers and never collapse — but
//     on MySQL 8 they are the same OBSERVATION, which is why they are now told
//     apart by a second query rather than read off the list.
//
// PARTIAL READS ARE REFUSED. The routine list and the parameter list are two
// queries, and information_schema.PARAMETERS can be denied independently of
// ROUTINES. When that happens this reader degrades the WHOLE SIDE (non-Ok,
// empty vector) rather than returning routines with empty argument lists. It
// has to: the matching key is name + argument types, so a side whose parameters
// were withheld matches nothing on a side whose were not, and every routine is
// then reported as both SourceOnly and TargetOnly. Returning less is the only
// way to avoid returning wrong.
//
// `detail` carries the server's error text when the status is not Ok, and is
// what the UI shows beside the degraded category.
sync::RoutineReadStatus ReadRoutines(IConnection& conn, const wxString& database,
                               std::vector<sync::RoutineDef>& out, wxString& detail);

} // namespace mysqlroutine
} // namespace db
