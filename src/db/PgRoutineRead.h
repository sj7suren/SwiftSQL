// PgRoutineRead.h — PostgreSQL routine introspection for the compare-only
// functions & stored procedures diff (ADR-013 Q2).
//
// Free functions over the public db::IConnection surface only — never a PGconn*
// — mirroring PgSchemaRead.h, and for the same charter reason: PgDriver.cpp is
// at 940/1000 lines, so new capability gets its own translation unit.
//
// ===========================================================================
// WHY prosrc AND NOT pg_get_functiondef()
// ===========================================================================
// PgDriver.cpp already has a routine reader — GetRoutineDdl(), which calls
// pg_get_functiondef(). That function is deliberately NOT reused here, and the
// reason is the whole point of ADR-013 Q2.
//
// pg_get_functiondef() is a server-side RENDERER. It reconstructs a CREATE
// statement from the catalog, and its output depends on the server's minor
// version, the session's search_path, and the server's own quoting choices. Two
// servers holding the SAME routine — a dump/restore pair, which is the single
// most common thing anyone compares — can therefore return different TEXT for
// it. Diffing that text manufactures false positives, against a release bar of
// zero false positives. It also THROWS on aggregates and window functions,
// which would abort the read.
//
// So this reader takes the structured columns instead — prosrc for the body,
// plus prokind / prorettype / proargtypes / prolang / prosecdef / provolatile
// for the metadata — and composes what it needs. prosrc is STORED text, not
// rendered text: the server hands back the same characters that were fed to
// CREATE FUNCTION, so two servers holding the same routine return the same
// string.
//
// The filters are part of the same argument, not housekeeping:
//   * prokind IN ('f','p') — plain functions and procedures. This also excludes
//     aggregates ('a') and window functions ('w'), which is exactly the set
//     pg_get_functiondef() would have thrown on. The failure mode disappears
//     instead of being caught.
//   * prolang IN ('sql','plpgsql') — internal and C-language functions have no
//     meaningful body to compare (prosrc holds a C symbol name), so including
//     them would produce body verdicts about link-time symbols.
//   * nspname NOT IN ('pg_catalog','information_schema') — the server's own
//     thousands of built-ins are not the user's routines.
//
// READS ONLY. No DDL is rendered here — see RoutineDiff.h.
#pragma once

#include <wx/string.h>
#include <vector>

#include "db/DbDriver.h"      // IConnection
#include "db/RoutineDiff.h"   // RoutineDef / RoutineReadStatus

namespace db {
namespace pgroutine {

// Read every sql/plpgsql function and procedure visible in the CURRENT
// database. The caller (a driver override, or the sync orchestrator) is
// responsible for having pointed `conn` at the right database first — this
// function only issues Execute() against whatever it is already bound to.
//
// When `schemaFilter` is non-empty only that schema is read; empty means every
// non-system schema, with each RoutineDef carrying its own `schema`.
//
// PG < 11 has no `prokind` column (and no procedures at all). That is detected
// and handled with a fallback query rather than being allowed to surface as an
// unreadable catalog — an old server must degrade to "functions only", not to
// "不可见".
sync::RoutineReadStatus ReadRoutines(IConnection& conn, const wxString& schemaFilter,
                               std::vector<sync::RoutineDef>& out, wxString& detail);

} // namespace pgroutine
} // namespace db
