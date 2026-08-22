// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_exec.h — shared harness for the LIVE EXECUTE/WRITE-PATH suite.
//
// The cross-engine compare suite (mysql_pg_live.h + its two TUs) proved the READ
// half against real servers: 70 checks, MySQL 8.0.36 → PostgreSQL 15.17. It
// never wrote a row. db::sync::SyncEngine::Execute, db::sync::ExecuteDataSync
// and IConnection::ExecuteBatch had, until this suite, never put a single byte
// into a real target — which is why data sync was held at Beta.
//
// THE RULE THIS SUITE IS BUILT AROUND: an assertion here reads the TARGET BACK
// and compares its actual contents. `Execute() returned true` is not evidence
// that anything was written, that it was written once, or that it was written
// correctly — those are three separate failure modes and only the target's own
// rows can distinguish them. Every helper below therefore exists to make
// "read the target back" cheap enough that no test is tempted to skip it.
//
// SAFETY: identical contract to mysql_pg_live.h — nothing hard-coded, everything
// from SWIFTSQL_MYTEST_* / SWIFTSQL_PGTEST_*, skip cleanly when unset, and only
// ever touch databases carrying the `swiftsql_xexec_` prefix (dropped on the way
// in AND out).
#pragma once

#include "mysql_pg_live.h"        // counters, Env/Profile/Exec/ScalarOf, Observe

#include "db/DataSync.h"
#include "db/DataSyncExec.h"
#include "db/SyncEngine.h"

#include <atomic>
#include <vector>

namespace mpexec {

using namespace db;
using namespace db::sync;
using mplive::g_checks;
using mplive::g_fails;

inline const std::atomic<bool> g_never{false};   // never-cancelled stop token

// ---------------------------------------------------------------------------
// String assertion. mplive has ExpectTrue/ExpectEq; the whole point of this
// suite is comparing READ-BACK TABLE CONTENTS, which are text, and a bool
// collapse of that ("contents matched: false") is useless in a failure report.
// ---------------------------------------------------------------------------
inline void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n       want=[%s]\n        got=[%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---------------------------------------------------------------------------
// Reading the target back
// ---------------------------------------------------------------------------

// Flatten a result set to "c1|c2;c1|c2" — row separator ';', column separator
// '|'. Deliberately ONE string so an assertion pins row count, row order and
// every cell at once: asserting only COUNT(*) would pass for a table that has
// the right number of completely wrong rows.
//
// NOTE ON NULL. IConnection::QueryResult renders a SQL NULL as the four
// characters "NULL", which is indistinguishable from a column legitimately
// containing the text 'NULL'. That ambiguity is unacceptable in a suite whose
// job is proving NULL was not confused with the empty string, so callers must
// disambiguate IN SQL (CASE WHEN c IS NULL THEN '#NULL#' ...) rather than trust
// this layer. See ItemB's read-back queries.
inline wxString Dump(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err))
        return L"<QUERY-FAILED: " + err + L">";
    wxString out;
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += L";";
        for (size_t j = 0; j < r.rows[i].size(); ++j) {
            if (j) out += L"|";
            out += r.rows[i][j];
        }
    }
    return out;
}

// Scalar read-back that reports the failure rather than silently answering ""
// (mplive::ScalarOf swallows the error, which is right for an observation probe
// and wrong for an assertion's input).
inline wxString Scalar(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return L"<QUERY-FAILED: " + err + L">";
    if (r.rows.empty() || r.rows[0].empty()) return L"<NO-ROWS>";
    return r.rows[0][0];
}

inline long long ScalarInt(IConnection& c, const wxString& sql)
{
    long long v = -1;
    if (!Scalar(c, sql).ToLongLong(&v)) return -1;
    return v;
}

// ---------------------------------------------------------------------------
// Building an execute job the way production does
// ---------------------------------------------------------------------------

// Fetch both sides' schemas and assemble the spec the execute pass replays.
// Returns false (with the reason printed) if either introspection fails — a
// setup failure, not a product defect, and it must not be mistaken for one.
inline bool MakeSpec(IConnection& src, IConnection& tgt, const wxString& srcDb,
                     const wxString& tgtDb, const wxString& table,
                     DataDiffSpec& out)
{
    wxString e1, e2;
    if (!src.GetTableSchema(srcDb, table, out.srcSchema, e1)) {
        std::printf("  ERR  GetTableSchema(src,%s): %s\n",
                    (const char*)table.utf8_str(), (const char*)e1.utf8_str());
        return false;
    }
    if (!tgt.GetTableSchema(tgtDb, table, out.tgtSchema, e2)) {
        std::printf("  ERR  GetTableSchema(tgt,%s): %s\n",
                    (const char*)table.utf8_str(), (const char*)e2.utf8_str());
        return false;
    }
    out.srcDb = srcDb;
    out.tgtDb = tgtDb;
    return true;
}

// What one table is authorized to do, in a form a test can write inline.
// `del` goes through the real AuthorizeDeletes token — there is deliberately no
// way to set TableDataSpec::deletes_ without one, and this helper does not
// invent a second one.
struct Allow {
    bool ins = false, upd = false, del = false;
    std::vector<wxString> excludeRows;
};

inline TableDataSpec MakeAllow(const wxString& table, const Allow& a,
                               bool deleteMasterSwitch)
{
    TableDataSpec s(table);
    if (a.ins) s.EnableInserts();
    if (a.upd) s.EnableUpdates();
    if (a.del) {
        // The master switch is the outer gate; the token proves it was consulted.
        // When it is off this branch CANNOT enable deletes, which is item E's
        // whole subject — so the test passes the switch explicitly rather than
        // letting a helper decide.
        if (auto tok = AuthorizeDeletes(deleteMasterSwitch)) s.EnableDeletes(*tok);
    }
    for (const wxString& k : a.excludeRows) s.ExcludeRow(k);
    return s;
}

// ---------------------------------------------------------------------------
// The compare pass, for tests that need the counts the user would have seen
// before deciding to execute (items D and H compare predicted vs applied).
// ---------------------------------------------------------------------------
struct CompareOutcome {
    bool          ran = false;
    RowChangeStat stat;
    bool          truncated = false;
    bool          executable = false;
    wxString      err;
    std::vector<wxString> sampleRowKeys;   // identities, for row-level exclusion
};

inline CompareOutcome Compare1(IConnection& src, IConnection& tgt,
                               const DataDiffSpec& spec)
{
    CompareOutcome o;
    DataSyncOptions opt;
    opt.insert = opt.update = opt.deleteMissing = true;   // compare sees everything

    RowLayout layout;
    RowChangeSet set;
    o.ran = DiffRowChanges(src, tgt, spec, opt, layout, set, o.err, g_never);
    o.stat       = set.Stat();
    o.truncated  = set.Truncated();
    o.executable = set.Executable();
    for (const RowChange& rc : set.Sample()) o.sampleRowKeys.push_back(rc.RowKey());
    return o;
}

// ---------------------------------------------------------------------------
// The execute pass. One entry point, used by every item, so no test can
// accidentally exercise a path production does not take.
// ---------------------------------------------------------------------------
struct ExecOutcome {
    bool           ok = false;
    wxString       err;
    DataExecResult result;
    long long      progressCalls = 0;   // item H: did the gauge actually move?
};

inline ExecOutcome RunExec(IConnection& src, IConnection& tgt,
                           const std::vector<TableExecJob>& jobs)
{
    ExecOutcome o;
    DataSyncOptions base;   // per-sweep category flags are the executor's business
    o.ok = ExecuteDataSync(src, tgt, jobs, base, o.result, o.err, g_never,
        [&o](const wxString&, const wxString&, long long) { ++o.progressCalls; });
    return o;
}

// Print the whole per-table account. Called on every execute, pass or fail —
// on a partial failure this IS the deliverable ("which tables committed?").
inline void ShowResult(const DataExecResult& r, const char* tag)
{
    for (const TableExecReport& t : r.tables) {
        std::printf("  OBS  %s [%s] ins=%lld upd=%lld del=%lld excl=%lld "
                    "attempted=%d committed=%d\n",
                    tag, (const char*)t.table.utf8_str(), t.inserts, t.updates,
                    t.deletes, t.excluded, (int)t.attempted, (int)t.committed);
        if (!t.error.IsEmpty())
            std::printf("  ERR  %s [%s] %s\n", tag,
                        (const char*)t.table.utf8_str(),
                        (const char*)t.error.utf8_str());
        for (const wxString& w : t.warnings)
            std::printf("  WARN %s [%s] %s\n", tag,
                        (const char*)t.table.utf8_str(),
                        (const char*)w.utf8_str());
    }
}

// Find one table's report by NAME. Never by position — DataExecResult is seeded
// in plan order but a caller that indexes it is one reorder away from asserting
// on the wrong table, which is the class of bug ui/SyncSelection.h documents.
inline const TableExecReport* ReportOf(const DataExecResult& r, const wxString& table)
{
    for (const TableExecReport& t : r.tables) if (t.table == table) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Items C-H live in mysql_pg_live_exec2.cpp — one more TU purely to stay under
// the charter's 1000-line ceiling, same reason mysql_pg_live_routines.cpp exists.
// ---------------------------------------------------------------------------
void ItemB2_WideBatch(IConnection& my, IConnection& pg,
                      const wxString& myDb, const wxString& pgDb);
// …and mysql_pg_live_exec3.cpp, which drives the REVERSE direction (PG→MySQL,
// so the MySQL driver's own bind path finally executes) and the delete-sweep
// partial-apply window.
void ItemB3_MySqlTargetBinding(IConnection& my, IConnection& pg,
                               const wxString& myDb, const wxString& pgDb);
void ItemC3_DeleteSweepWindow(IConnection& my, IConnection& pg,
                              const wxString& myDb, const wxString& pgDb);
// …and the ORDINARY case every earlier fixture dodged: a target that already
// holds rows, so the merge's target read is still open when the first write goes
// out. Not behind the scale gate — it is a correctness test, and the collision it
// covers needs no scale.
void ItemD3_PopulatedTarget(IConnection& my, IConnection& pg,
                            const wxString& myDb, const wxString& pgDb);
void ItemC_PartialFailure(IConnection& my, IConnection& pg,
                          const wxString& myDb, const wxString& pgDb);
void ItemD_DriftWindow(IConnection& my, IConnection& pg,
                       const wxString& myDb, const wxString& pgDb);
void ItemE_Selection(IConnection& my, IConnection& pg,
                     const wxString& myDb, const wxString& pgDb);
void ItemH_Scale(IConnection& my, IConnection& pg,
                 const wxString& myDb, const wxString& pgDb);

} // namespace mpexec
