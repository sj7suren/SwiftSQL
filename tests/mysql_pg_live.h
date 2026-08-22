// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live.h — shared harness for the live MySQL → PostgreSQL cross-engine
// integration test. Split out (with mysql_pg_live_routines.cpp) purely to keep
// every TU under the charter's 1000-line ceiling; there is no second consumer.
//
// SAFETY: no credential is ever hard-coded here or in either TU. Everything
// comes from the environment, and both TUs skip cleanly when it is unset — see
// mysql_pg_live_test.cpp's header comment for the variable list.
#pragma once

#include "db/DbDriver.h"
#include "db/SchemaDelta.h"   // db::sync::Finding
#include "core/ConnectionProfile.h"

#include <cstdio>
#include <cstdlib>
#include <wx/string.h>

namespace mplive {

// ---- check counters (shared across both TUs) -------------------------------
inline int g_checks = 0;
inline int g_fails  = 0;

inline void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

inline void ExpectEq(const char* name, long long got, long long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%lld got=%lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// An OBSERVATION, not an assertion. Several of this suite's questions ("what
// text does each driver actually return for a DATETIME?") have no pre-agreed
// right answer — the whole point is to record what the servers really do. These
// lines are prefixed so they are greppable and can never be mistaken for a
// passing check.
inline void Observe(const char* label, const wxString& value)
{
    std::printf("  OBS  %s = [%s]\n", label, (const char*)value.utf8_str());
}

// ---- env -------------------------------------------------------------------
inline wxString Env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? wxString::FromUTF8(v) : wxString();
}

inline int EnvInt(const char* name, int dflt)
{
    long v = 0;
    if (Env(name).ToLong(&v) && v > 0) return (int)v;
    return dflt;
}

// ---- connection helpers ----------------------------------------------------
inline core::ConnectionProfile Profile(db::DbType type, const wxString& host,
                                       int port, const wxString& user,
                                       const wxString& pass, const wxString& database)
{
    core::ConnectionProfile p;
    p.type     = type;
    p.host     = host;
    p.port     = port;
    p.user     = user;
    p.password = pass;
    p.database = database;
    return p;
}

// Run one statement; on failure print the server's error and return false.
inline bool Exec(db::IConnection& c, const wxString& sql, const char* label)
{
    db::QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) {
        std::printf("  ERR  %s: %s\n", label, (const char*)err.utf8_str());
        return false;
    }
    return true;
}

// Run one statement, expecting it MAY fail; report the outcome without failing
// the suite. Used for optional setup (e.g. relaxing sql_mode on a server that
// does not allow it).
inline bool TryExec(db::IConnection& c, const wxString& sql, const char* label,
                    wxString& err)
{
    db::QueryResult r;
    if (!c.Execute(sql, r, err)) {
        std::printf("  note %s failed: %s\n", label, (const char*)err.utf8_str());
        return false;
    }
    return true;
}

// First cell of a single-row query, or "" on any failure.
inline wxString ScalarOf(db::IConnection& c, const wxString& sql)
{
    db::QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return wxString();
    if (r.rows.empty() || r.rows[0].empty()) return wxString();
    return r.rows[0][0];
}

inline const char* CellKindName(db::CellKind k)
{
    switch (k) {
    case db::CellKind::Null:    return "Null";
    case db::CellKind::Numeric: return "Numeric";
    case db::CellKind::Text:    return "Text";
    case db::CellKind::Binary:  return "Binary";
    }
    return "?";
}

// Print every Finding a refused table produced — the "with a reason" half of
// several hazards. Returns the count.
inline int ShowFindings(const std::vector<db::sync::Finding>& fs, const char* tag)
{
    for (const auto& f : fs)
        std::printf("  RSN  %s [%s.%s] %s\n", tag,
                    (const char*)f.table.utf8_str(),
                    (const char*)f.column.utf8_str(),
                    (const char*)f.reason.utf8_str());
    return (int)fs.size();
}

// Hazard 7 lives in its own TU (mysql_pg_live_routines.cpp). `myDb` is the
// MySQL test database; `pgDb` the PostgreSQL one. The MySQL host/port/user/pass
// are needed again because the restricted-account probe dials its OWN second
// MySQL connection as a freshly created least-privilege user.
void RunRoutineHazards(db::IConnection& my, db::IConnection& pg,
                       const wxString& myDb, const wxString& pgDb,
                       const wxString& host, int port,
                       const wxString& rootUser, const wxString& rootPass);

} // namespace mplive
