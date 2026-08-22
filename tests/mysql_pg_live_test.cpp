// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_test.cpp — env-gated LIVE cross-engine integration test:
// a real MySQL server as the source, a real PostgreSQL server as the target.
//
// WHY THIS FILE EXISTS. The cross-database sync suite shipped three rounds of
// work — the sorted-merge data diff, the value-conversion gate, the routine
// compare — with zero observed runtime behaviour against real servers. Six
// silent data-corrupting defects were found and fixed by construction-time
// reasoning alone. Every one of those fixes rests on an ASSUMPTION about what a
// real MySQL or PostgreSQL server returns, and not one of those assumptions had
// ever been checked against a server. This file turns "verified by argument"
// into "verified by observation".
//
// It therefore does two different things, and the distinction matters when
// reading its output:
//   * `ok` / `FAIL` lines are ASSERTIONS — a stated expectation that a hazard is
//     handled. A FAIL is a defect in the product, never a reason to weaken the
//     assertion.
//   * `OBS` lines are OBSERVATIONS — the actual cell text, kind and count the
//     servers returned. Several of the fixes above depend on claims like "MySQL
//     returns tinyint(1) as Numeric 1/0 while PostgreSQL returns boolean as
//     Text t/f" which nobody had ever seen happen. These lines record the truth
//     either way so the next reader does not have to take it on faith.
//
// SAFETY: no password/host is hard-coded. All parameters come from environment
// variables; if the essential ones are unset the test prints SKIP and exits 0,
// so CI and other developers are never blocked. When configured it only ever
// touches databases carrying the `swiftsql_xsync_` prefix (and, for the routine
// privilege probe, a `swiftsql_xs_ro` MySQL account), all of which are dropped
// on the way in AND out. Nothing else on either server is read or written.
//
//   SWIFTSQL_MYTEST_HOST  (required)  MySQL host
//   SWIFTSQL_MYTEST_PORT  (optional)  default 3306
//   SWIFTSQL_MYTEST_USER  (required)  needs CREATE/DROP DATABASE (+ CREATE USER
//                                     for the hazard-7 privilege probe; that
//                                     probe self-skips if it cannot)
//   SWIFTSQL_MYTEST_PASS  (required)
//   SWIFTSQL_MYTEST_DB    (optional)  bootstrap DB, default "mysql"
//   SWIFTSQL_MYTEST_SRCDB (optional)  default "swiftsql_xsync_src"
//   SWIFTSQL_PGTEST_HOST  (required)  PostgreSQL host
//   SWIFTSQL_PGTEST_PORT  (optional)  default 5432
//   SWIFTSQL_PGTEST_USER  (required)  needs CREATEDB
//   SWIFTSQL_PGTEST_PASS  (required)
//   SWIFTSQL_PGTEST_DB    (optional)  bootstrap DB, default "postgres"
//   SWIFTSQL_XSYNC_TGTDB  (optional)  default "swiftsql_xsync_tgt"
//
// Hazards 1-6 live here; hazard 7 (routines + the MySQL restricted-account
// question) lives in mysql_pg_live_routines.cpp, so both TUs stay well under
// the charter's 1000-line ceiling.
#include "mysql_pg_live.h"

#include "db/DataSync.h"
#include "db/SchemaDiff.h"

#include <atomic>
#include <memory>
#include <vector>

using namespace db;
using namespace db::sync;
using namespace mplive;

namespace {

const std::atomic<bool> g_never{false};   // never-cancelled stop token

// ---------------------------------------------------------------------------
// Diff one table pair through the SAME structured path production uses
// (DiffRowChanges → BuildRowLayout → StreamRowChanges), and report everything a
// caller could want to assert on. Deliberately not DiffAndEmitData: that is the
// text-rendering legacy path, and what the wizard actually runs is this one.
// ---------------------------------------------------------------------------
struct DiffOutcome {
    bool          called   = false;   // the diff ran without an engine error
    bool          blocked  = false;   // the table was refused (set non-executable)
    RowChangeStat stat;               // inserts / updates / deletes / blocked / unchanged
    RowStat       raw;                // the merge's own counters, straight from StreamRowChanges
    long long     prodUnchanged = -1; // the same count as PRODUCTION reports it
    std::vector<Finding> findings;
    wxString      err;
};

// This is DiffRowChanges' body, re-expressed here so the merge's RowStat is
// reachable directly. It originally existed because DiffRowChanges filled a
// local RowStat and DISCARDED it, leaving no caller able to learn how many rows
// were UNCHANGED — and that count is exactly what hazards 1-3 assert on: "11
// identical rows produced zero differences" is only half the claim; the other
// half is that all 11 were actually seen and matched, which distinguishes a
// genuine clean compare from a merge that silently read nothing.
//
// DiffRowChanges now publishes that count (RowChangeStat::unchanged), so the
// shim is kept as an INDEPENDENT witness rather than as the only source: it
// reads the merge's raw RowStat, production reads the published field, and
// ExpectNoDifferences asserts the two agree. If the wiring in DataSync.cpp is
// ever dropped again, production would silently report 0 while the shim still
// reports the true number, and that mismatch is now a FAIL rather than an
// invisible regression. Everything else (BuildRowLayout, the findings fold,
// Accept as the single door) is identical to production.
DiffOutcome DiffTable(IConnection& src, IConnection& tgt,
                      const wxString& srcDb, const wxString& tgtDb,
                      const wxString& table)
{
    DiffOutcome o;

    TableSchema ss, ts;
    wxString e1, e2;
    if (!src.GetTableSchema(srcDb, table, ss, e1)) {
        o.err = L"GetTableSchema(src): " + e1;
        return o;
    }
    if (!tgt.GetTableSchema(tgtDb, table, ts, e2)) {
        o.err = L"GetTableSchema(tgt): " + e2;
        return o;
    }

    DataDiffSpec spec;
    spec.srcDb = srcDb;
    spec.tgtDb = tgtDb;
    spec.srcSchema = ss;
    spec.tgtSchema = ts;

    // Compare pass contract: run with every category enabled so the counts are
    // the real ones (the user's per-category choice is applied later, in pass 2).
    DataSyncOptions opt;
    opt.insert = opt.update = opt.deleteMissing = true;

    RowLayout layout;
    RowChangeSet set;
    std::vector<Finding> findings;
    const bool laidOut = BuildRowLayout(spec, src.GetDialect(), tgt.GetDialect(),
                                        layout, findings);
    for (Finding& f : findings) set.AddFinding(std::move(f));

    if (!laidOut) {
        o.called   = true;            // a per-table verdict, not an engine failure
        o.blocked  = !set.Executable();
        o.stat     = set.Stat();
        o.findings = set.Findings();
        return o;
    }

    o.called = StreamRowChanges(src, tgt, spec, layout, opt,
        [&](RowChangeBuilder&& b, const wxString&) -> bool {
            set.Accept(std::move(b));
            return true;
        }, o.raw, o.err, g_never);

    o.blocked  = !set.Executable();
    o.stat     = set.Stat();
    o.findings = set.Findings();

    // The production path, run over the same two tables, purely to check that
    // the unchanged count it publishes matches the one the merge actually
    // produced above.
    {
        RowLayout prodLayout;
        RowChangeSet prodSet;
        wxString prodErr;
        if (DiffRowChanges(src, tgt, spec, opt, prodLayout, prodSet, prodErr, g_never))
            o.prodUnchanged = prodSet.Stat().unchanged;
    }
    return o;
}

// Assert "the two sides are identical" — the shape hazards 1, 2 and 3 all take.
// A refused table counts as a failure here: refusing is not the same as finding
// no difference, and these hazards expect an actual clean compare.
void ExpectNoDifferences(const char* name, const DiffOutcome& o, long long unchangedWant)
{
    std::printf("  -- %s --\n", name);
    if (!o.called) {
        ExpectTrue(name, false);
        std::printf("  err: %s\n", (const char*)o.err.utf8_str());
        return;
    }
    ShowFindings(o.findings, name);
    std::printf("  OBS  stat: ins=%lld upd=%lld del=%lld unchanged=%lld blocked=%lld\n",
                o.stat.inserts, o.stat.updates, o.stat.deletes,
                o.raw.unchanged, o.stat.blocked);
    ExpectTrue("  ^ not refused", !o.blocked);
    ExpectEq("  ^ inserts", o.stat.inserts, 0);
    ExpectEq("  ^ updates", o.stat.updates, 0);
    ExpectEq("  ^ deletes", o.stat.deletes, 0);
    ExpectEq("  ^ blocked rows", o.stat.blocked, 0);
    ExpectEq("  ^ unchanged", o.raw.unchanged, unchangedWant);
    // DiffRowChanges must publish the same number the merge counted, or the
    // compare screen cannot honestly show "N rows identical".
    ExpectEq("  ^ unchanged as PRODUCTION reports it", o.prodUnchanged, unchangedWant);
}

// ---------------------------------------------------------------------------
// OBSERVATION probe: stream one table straight off a driver and print the kind
// + exact text of every cell. This is how hazards 2 and 3's "print what the
// engines actually return" requirement is met — no interpretation, no
// canonicalization, just the raw Cell the driver handed up.
// ---------------------------------------------------------------------------
void ProbeCells(IConnection& c, const wxString& db, const wxString& table,
                const std::vector<wxString>& columns, const char* engine)
{
    StreamOptions so;
    so.columns = columns;
    so.orderBy = { columns.empty() ? wxString(L"id") : columns[0] };

    int row = 0;
    wxString err;
    const bool ok = c.StreamRows(db, table, so,
        [&](const std::vector<Cell>& cells) {
            for (size_t i = 0; i < cells.size() && i < columns.size(); ++i) {
                std::printf("  OBS  %s %s.%s row%d kind=%-7s text=[%s]\n",
                            engine,
                            (const char*)table.utf8_str(),
                            (const char*)columns[i].utf8_str(),
                            row,
                            CellKindName(cells[i].kind),
                            (const char*)cells[i].text.utf8_str());
            }
            ++row;
            return true;
        }, err);
    if (!ok)
        std::printf("  ERR  probe %s %s: %s\n", engine,
                    (const char*)table.utf8_str(), (const char*)err.utf8_str());
}

// ===========================================================================
//  SETUP — every table both hazards need, created identically-in-meaning on
//  the two engines. Kept in one place so the hazard sections below read as
//  assertions rather than as DDL.
// ===========================================================================

// Hazard 1's row set. Mixed case and accents on a VARCHAR primary key: the
// exact input that made MySQL's utf8mb4_general_ci (case- AND accent-
// insensitive) and PostgreSQL's default collation return the same 11 rows in
// two different orders, desyncing the sorted merge into 10 spurious INSERTs and
// 10 spurious DELETEs. Both sides get the SAME rows, so the only correct answer
// is zero differences.
const wchar_t* const kTextKeys[] = {
    L"Apple", L"apple", L"APPLE", L"Banana", L"banana",
    L"ápple", L"Ápple", L"Zebra", L"zebra", L"Éclair", L"eclair",
};
const int kTextKeyCount = (int)(sizeof(kTextKeys) / sizeof(kTextKeys[0]));

bool SetupMySql(IConnection& my, const wxString& db)
{
    bool ok = true;
    (void)db;   // every statement below runs against the session's current DB

    // ---- hazard 1: text PK, mixed case + accents -------------------------
    //
    // The key column is explicitly utf8mb4_0900_as_cs, and that is a FINDING
    // rather than a preference. The obvious fixture — utf8mb4_general_ci, the
    // collation the ordering hazard is usually described against — cannot hold
    // this row set at all: general_ci is case- AND accent-insensitive, so
    // 'Apple', 'apple', 'APPLE', 'ápple' and 'Ápple' are ONE primary key value
    // and MySQL rejects the other four with "Duplicate entry 'apple' for key
    // PRIMARY". (Observed, not reasoned: that is exactly what the first live run
    // of this test did.)
    //
    // So the real-world shape of this hazard is narrower than the design notes
    // imply. A _ci text PK cannot contain case-variant rows in the first place,
    // which means the 11-identical-rows desync CANNOT be staged there. What
    // genuinely reproduces it is an accent- and case-SENSITIVE MySQL collation
    // (0900_as_cs, the MySQL 8 default family) against PostgreSQL's default
    // locale collation: both sides then hold all 11 distinct rows, and both sort
    // them by a linguistic order rather than by bytes — two different linguistic
    // orders, neither equal to the code-point order the merge compares with.
    // That is the configuration this table pins.
    ok &= Exec(my, L"CREATE TABLE t_textpk ("
                   L"k VARCHAR(64) COLLATE utf8mb4_0900_as_cs NOT NULL PRIMARY KEY, "
                   L"payload VARCHAR(50)"
                   L") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4", "my t_textpk");
    for (int i = 0; i < kTextKeyCount; ++i) {
        wxString k = kTextKeys[i];
        wxString e = k; e.Replace(L"'", L"''");
        ok &= Exec(my, L"INSERT INTO t_textpk(k,payload) VALUES ('" + e +
                       wxString::Format(L"','p%d')", i), "my t_textpk row");
    }

    // ---- hazard 2: tinyint(1) vs boolean ---------------------------------
    ok &= Exec(my, L"CREATE TABLE t_bool (id INT NOT NULL PRIMARY KEY, "
                   L"flag TINYINT(1), note VARCHAR(20))", "my t_bool");
    ok &= Exec(my, L"INSERT INTO t_bool(id,flag,note) VALUES "
                   L"(1,1,'true'),(2,0,'false'),(3,NULL,'null')", "my t_bool rows");
    // A tinyint(1) holding 2 — legal in MySQL, outside {0,1}, and therefore not
    // representable as a PostgreSQL boolean. Separate table so it cannot mask
    // hazard 2's clean case.
    ok &= Exec(my, L"CREATE TABLE t_bool2 (id INT NOT NULL PRIMARY KEY, "
                   L"flag TINYINT(1))", "my t_bool2");
    ok &= Exec(my, L"INSERT INTO t_bool2(id,flag) VALUES (1,1),(2,2)", "my t_bool2 rows");

    // ---- hazard 3: temporal ----------------------------------------------
    ok &= Exec(my, L"CREATE TABLE t_time (id INT NOT NULL PRIMARY KEY, "
                   L"d DATE, dt DATETIME, dt3 DATETIME(3), t TIME)", "my t_time");
    ok &= Exec(my, L"INSERT INTO t_time(id,d,dt,dt3,t) VALUES "
                   L"(1,'2024-01-15','2024-01-15 08:30:00','2024-01-15 08:30:00.123','08:30:00'),"
                   L"(2,'2024-02-29','2024-02-29 23:59:59','2024-02-29 23:59:59.500','23:59:59'),"
                   L"(3,'2024-03-01','2024-03-01 00:00:00','2024-03-01 00:00:00.000','00:00:00')",
                   "my t_time rows");
    // Row 3 differs in the DATETIME only → must be exactly ONE update, not a
    // delete+insert pair (a key-identity failure would produce the latter).
    ok &= Exec(my, L"CREATE TABLE t_timediff (id INT NOT NULL PRIMARY KEY, dt DATETIME)",
               "my t_timediff");
    ok &= Exec(my, L"INSERT INTO t_timediff(id,dt) VALUES "
                   L"(1,'2024-01-01 00:00:00'),(2,'2024-06-30 12:00:00')", "my t_timediff rows");

    // TIMESTAMP as the PRIMARY KEY — tz-ambiguous, must be refused.
    ok &= Exec(my, L"CREATE TABLE t_tspk (ts TIMESTAMP NOT NULL PRIMARY KEY, "
                   L"payload VARCHAR(20))", "my t_tspk");
    ok &= Exec(my, L"INSERT INTO t_tspk(ts,payload) VALUES "
                   L"('2024-01-01 10:00:00','a'),('2024-01-02 10:00:00','b')", "my t_tspk rows");

    // ---- hazard 4: zero date ---------------------------------------------
    // Needs a sql_mode without NO_ZERO_DATE/NO_ZERO_IN_DATE. Set for THIS
    // SESSION only — never globally; this is a shared server.
    wxString smErr;
    const bool relaxed = TryExec(my, L"SET SESSION sql_mode=''", "relax sql_mode", smErr);
    ok &= Exec(my, L"CREATE TABLE t_zerodate (id INT NOT NULL PRIMARY KEY, d DATE)",
               "my t_zerodate");
    if (relaxed) {
        ok &= Exec(my, L"INSERT INTO t_zerodate(id,d) VALUES (1,'2024-01-01')",
                   "my t_zerodate good row");
        wxString zErr;
        if (!TryExec(my, L"INSERT INTO t_zerodate(id,d) VALUES (2,'0000-00-00')",
                     "insert zero-date", zErr))
            std::printf("  note hazard 4 degraded: server refused a zero date "
                        "even with sql_mode='' — the hazard cannot be staged\n");
    }

    // ---- hazard 5: BIGINT UNSIGNED, per-value -----------------------------
    ok &= Exec(my, L"CREATE TABLE t_ubig (id INT NOT NULL PRIMARY KEY, "
                   L"n BIGINT UNSIGNED)", "my t_ubig");
    ok &= Exec(my, L"INSERT INTO t_ubig(id,n) VALUES "
                   L"(1,42),(2,9223372036854775807),(3,18446744073709551615)",
               "my t_ubig rows");

    // ---- hazard 6: no primary key ----------------------------------------
    ok &= Exec(my, L"CREATE TABLE t_nopk (a INT, b VARCHAR(20))", "my t_nopk");
    ok &= Exec(my, L"INSERT INTO t_nopk(a,b) VALUES (1,'x'),(2,'y')", "my t_nopk rows");

    // ---- control: a plain integer-PK table that must diff normally --------
    // Hazards 3, 4 and 6 all claim "the rest of the compare is unaffected";
    // that claim needs a table whose diff is expected to WORK.
    ok &= Exec(my, L"CREATE TABLE t_control (id INT NOT NULL PRIMARY KEY, "
                   L"name VARCHAR(50))", "my t_control");
    ok &= Exec(my, L"INSERT INTO t_control(id,name) VALUES "
                   L"(1,'same'),(2,'src-changed'),(3,'src-only')", "my t_control rows");
    return ok;
}

bool SetupPg(IConnection& pg, const wxString& db)
{
    bool ok = true;
    (void)db;

    ok &= Exec(pg, L"CREATE TABLE t_textpk (k varchar(64) PRIMARY KEY, payload varchar(50))",
               "pg t_textpk");
    for (int i = 0; i < kTextKeyCount; ++i) {
        wxString k = kTextKeys[i];
        wxString e = k; e.Replace(L"'", L"''");
        ok &= Exec(pg, L"INSERT INTO t_textpk(k,payload) VALUES ('" + e +
                       wxString::Format(L"','p%d')", i), "pg t_textpk row");
    }

    ok &= Exec(pg, L"CREATE TABLE t_bool (id integer PRIMARY KEY, flag boolean, "
                   L"note varchar(20))", "pg t_bool");
    ok &= Exec(pg, L"INSERT INTO t_bool(id,flag,note) VALUES "
                   L"(1,true,'true'),(2,false,'false'),(3,NULL,'null')", "pg t_bool rows");

    ok &= Exec(pg, L"CREATE TABLE t_bool2 (id integer PRIMARY KEY, flag boolean)",
               "pg t_bool2");
    ok &= Exec(pg, L"INSERT INTO t_bool2(id,flag) VALUES (1,true)", "pg t_bool2 rows");

    ok &= Exec(pg, L"CREATE TABLE t_time (id integer PRIMARY KEY, d date, "
                   L"dt timestamp, dt3 timestamp(3), t time)", "pg t_time");
    ok &= Exec(pg, L"INSERT INTO t_time(id,d,dt,dt3,t) VALUES "
                   L"(1,'2024-01-15','2024-01-15 08:30:00','2024-01-15 08:30:00.123','08:30:00'),"
                   L"(2,'2024-02-29','2024-02-29 23:59:59','2024-02-29 23:59:59.500','23:59:59'),"
                   L"(3,'2024-03-01','2024-03-01 00:00:00','2024-03-01 00:00:00.000','00:00:00')",
               "pg t_time rows");

    // id=2 holds a genuinely DIFFERENT instant from the MySQL side.
    ok &= Exec(pg, L"CREATE TABLE t_timediff (id integer PRIMARY KEY, dt timestamp)",
               "pg t_timediff");
    ok &= Exec(pg, L"INSERT INTO t_timediff(id,dt) VALUES "
                   L"(1,'2024-01-01 00:00:00'),(2,'2020-06-30 12:00:00')", "pg t_timediff rows");

    ok &= Exec(pg, L"CREATE TABLE t_tspk (ts timestamptz PRIMARY KEY, payload varchar(20))",
               "pg t_tspk");
    ok &= Exec(pg, L"INSERT INTO t_tspk(ts,payload) VALUES "
                   L"('2024-01-01 10:00:00+00','a'),('2024-01-02 10:00:00+00','b')",
               "pg t_tspk rows");

    ok &= Exec(pg, L"CREATE TABLE t_zerodate (id integer PRIMARY KEY, d date)",
               "pg t_zerodate");
    ok &= Exec(pg, L"INSERT INTO t_zerodate(id,d) VALUES (1,'2024-01-01')",
               "pg t_zerodate rows");

    ok &= Exec(pg, L"CREATE TABLE t_ubig (id integer PRIMARY KEY, n bigint)",
               "pg t_ubig");
    ok &= Exec(pg, L"INSERT INTO t_ubig(id,n) VALUES (1,42)", "pg t_ubig rows");

    ok &= Exec(pg, L"CREATE TABLE t_nopk (a integer, b varchar(20))", "pg t_nopk");
    ok &= Exec(pg, L"INSERT INTO t_nopk(a,b) VALUES (1,'x')", "pg t_nopk rows");

    ok &= Exec(pg, L"CREATE TABLE t_control (id integer PRIMARY KEY, name varchar(50))",
               "pg t_control");
    ok &= Exec(pg, L"INSERT INTO t_control(id,name) VALUES "
                   L"(1,'same'),(2,'tgt-changed'),(4,'tgt-only')", "pg t_control rows");
    return ok;
}

// ===========================================================================
//  HAZARDS
// ===========================================================================

// --- 1. text PK collation ordering — the worst defect ----------------------
void Hazard1(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 1: text PK collation ordering --\n");

    // What each server thinks the order is, natively. If these two lists differ
    // (they are expected to), the sorted merge WOULD desync without the forced
    // byte-order collation — which is the defect this hazard exists for.
    std::printf("  (native per-server ORDER BY k, for the record)\n");
    {
        QueryResult r; wxString e;
        if (my.Execute(L"SELECT GROUP_CONCAT(k ORDER BY k SEPARATOR '|') FROM t_textpk", r, e)
            && !r.rows.empty() && !r.rows[0].empty())
            Observe("mysql native order", r.rows[0][0]);
        if (pg.Execute(L"SELECT string_agg(k, '|' ORDER BY k) FROM t_textpk", r, e)
            && !r.rows.empty() && !r.rows[0].empty())
            Observe("pg native order", r.rows[0][0]);
        if (my.Execute(L"SELECT GROUP_CONCAT(k ORDER BY CONVERT(k USING utf8mb4) "
                       L"COLLATE utf8mb4_bin SEPARATOR '|') FROM t_textpk", r, e)
            && !r.rows.empty() && !r.rows[0].empty())
            Observe("mysql forced byte order", r.rows[0][0]);
        if (pg.Execute(L"SELECT string_agg(k, '|' ORDER BY k COLLATE \"C\") FROM t_textpk", r, e)
            && !r.rows.empty() && !r.rows[0].empty())
            Observe("pg forced byte order (C)", r.rows[0][0]);
    }

    // THE assertion of this file. 11 identical rows on both sides, text PK,
    // mixed case and accents → zero differences.
    const DiffOutcome o = DiffTable(my, pg, myDb, pgDb, L"t_textpk");
    ExpectNoDifferences("hazard1 t_textpk identical both sides", o, kTextKeyCount);
}

// --- 2. boolean coercion ---------------------------------------------------
void Hazard2(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 2: tinyint(1) vs boolean --\n");

    // The implementer's own highest-uncertainty claim: MySQL returns 1/0 as
    // CellKind::Numeric and PostgreSQL returns t/f as Text. Never observed
    // before this line ran.
    ProbeCells(my, myDb, L"t_bool", { L"id", L"flag", L"note" }, "mysql");
    ProbeCells(pg, pgDb, L"t_bool", { L"id", L"flag", L"note" }, "pg   ");

    const DiffOutcome o = DiffTable(my, pg, myDb, pgDb, L"t_bool");
    ExpectNoDifferences("hazard2 t_bool same logical values", o, 3);

    // tinyint(1) = 2: legal MySQL, not a PostgreSQL boolean. The row must be
    // refused per-value, not silently written as true.
    std::printf("  (tinyint(1) holding 2)\n");
    ProbeCells(my, myDb, L"t_bool2", { L"id", L"flag" }, "mysql");
    const DiffOutcome o2 = DiffTable(my, pg, myDb, pgDb, L"t_bool2");
    if (!o2.called) {
        ExpectTrue("hazard2 t_bool2 diff ran", false);
        std::printf("  err: %s\n", (const char*)o2.err.utf8_str());
    } else {
        ShowFindings(o2.findings, "t_bool2");
        std::printf("  OBS  t_bool2 stat: ins=%lld upd=%lld del=%lld unchanged=%lld blocked=%lld\n",
                    o2.stat.inserts, o2.stat.updates, o2.stat.deletes,
                    o2.raw.unchanged, o2.stat.blocked);
        ExpectTrue("hazard2 out-of-range tinyint(1) row is refused, not coerced",
                   o2.stat.blocked >= 1 || o2.blocked);
    }
}

// --- 3. temporal -----------------------------------------------------------
void Hazard3(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 3: temporal --\n");

    // Everything in the temporal canonicalization rests on the exact text each
    // driver returns per type. Record it.
    ProbeCells(my, myDb, L"t_time", { L"id", L"d", L"dt", L"dt3", L"t" }, "mysql");
    ProbeCells(pg, pgDb, L"t_time", { L"id", L"d", L"dt", L"dt3", L"t" }, "pg   ");

    const DiffOutcome o = DiffTable(my, pg, myDb, pgDb, L"t_time");
    ExpectNoDifferences("hazard3 t_time same instants incl. fractional seconds", o, 3);

    // A genuinely different instant → exactly one UPDATE, never DELETE+INSERT.
    const DiffOutcome d = DiffTable(my, pg, myDb, pgDb, L"t_timediff");
    if (!d.called) {
        ExpectTrue("hazard3 t_timediff diff ran", false);
        std::printf("  err: %s\n", (const char*)d.err.utf8_str());
    } else {
        ShowFindings(d.findings, "t_timediff");
        ExpectEq("hazard3 different instant = 1 update", d.stat.updates, 1);
        ExpectEq("hazard3 different instant = 0 inserts", d.stat.inserts, 0);
        ExpectEq("hazard3 different instant = 0 deletes", d.stat.deletes, 0);
        ExpectEq("hazard3 different instant = 1 unchanged", d.raw.unchanged, 1);
    }

    // TIMESTAMP primary key: tz-ambiguous, must be refused with a reason.
    ProbeCells(my, myDb, L"t_tspk", { L"ts", L"payload" }, "mysql");
    ProbeCells(pg, pgDb, L"t_tspk", { L"ts", L"payload" }, "pg   ");
    const DiffOutcome t = DiffTable(my, pg, myDb, pgDb, L"t_tspk");
    if (!t.called) {
        ExpectTrue("hazard3 t_tspk diff ran", false);
        std::printf("  err: %s\n", (const char*)t.err.utf8_str());
    } else {
        const int n = ShowFindings(t.findings, "t_tspk");
        ExpectTrue("hazard3 TIMESTAMP primary key is refused", t.blocked);
        ExpectTrue("hazard3 refusal carries a reason", n > 0);
        ExpectEq("hazard3 refused table emitted no changes",
                 t.stat.inserts + t.stat.updates + t.stat.deletes, 0);
    }

    // ...and the rest of the compare still works.
    const DiffOutcome c = DiffTable(my, pg, myDb, pgDb, L"t_control");
    ExpectEq("hazard3 control table still diffs (1 insert)", c.stat.inserts, 1);
    ExpectEq("hazard3 control table still diffs (1 update)", c.stat.updates, 1);
    ExpectEq("hazard3 control table still diffs (1 delete)", c.stat.deletes, 1);
    ExpectEq("hazard3 control table still diffs (1 unchanged)", c.raw.unchanged, 1);
}

// --- 4. zero date ----------------------------------------------------------
void Hazard4(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 4: MySQL zero date --\n");

    const wxString n = ScalarOf(my, L"SELECT COUNT(*) FROM t_zerodate WHERE d='0000-00-00'");
    Observe("mysql zero-date rows staged", n);
    if (n != L"1") {
        std::printf("  SKIP hazard 4: the server would not store a zero date "
                    "(sql_mode). Nothing to assert.\n");
        return;
    }
    ProbeCells(my, myDb, L"t_zerodate", { L"id", L"d" }, "mysql");

    const DiffOutcome o = DiffTable(my, pg, myDb, pgDb, L"t_zerodate");
    if (!o.called) {
        ExpectTrue("hazard4 diff ran", false);
        std::printf("  err: %s\n", (const char*)o.err.utf8_str());
        return;
    }
    const int nf = ShowFindings(o.findings, "t_zerodate");
    std::printf("  OBS  t_zerodate stat: ins=%lld upd=%lld del=%lld unchanged=%lld blocked=%lld\n",
                o.stat.inserts, o.stat.updates, o.stat.deletes,
                o.raw.unchanged, o.stat.blocked);
    // The zero-date row must be REFUSED. The failure this guards against is not
    // "it errored" but "it silently became NULL", so what is asserted is that
    // the row was blocked rather than turned into an emittable insert.
    ExpectTrue("hazard4 zero-date row refused (blocked), never written",
               o.stat.blocked >= 1);
    ExpectTrue("hazard4 refusal carries a reason", nf > 0 || o.blocked);
    ExpectEq("hazard4 zero-date row did not become an emittable INSERT",
             o.stat.inserts, 0);
}

// --- 5. unsigned overflow, PER VALUE ---------------------------------------
void Hazard5(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 5: BIGINT UNSIGNED, per-value --\n");
    ProbeCells(my, myDb, L"t_ubig", { L"id", L"n" }, "mysql");

    const DiffOutcome o = DiffTable(my, pg, myDb, pgDb, L"t_ubig");
    if (!o.called) {
        ExpectTrue("hazard5 diff ran", false);
        std::printf("  err: %s\n", (const char*)o.err.utf8_str());
        return;
    }
    ShowFindings(o.findings, "t_ubig");
    std::printf("  OBS  t_ubig stat: ins=%lld upd=%lld del=%lld unchanged=%lld blocked=%lld\n",
                o.stat.inserts, o.stat.updates, o.stat.deletes,
                o.raw.unchanged, o.stat.blocked);
    // id=1 (42) matches → unchanged. id=2 (LLONG_MAX) fits a PG bigint → insert.
    // id=3 (2^64-1) does not fit → blocked. The judgment is deliberately
    // per-value, so the fitting row must NOT be collateral damage.
    ExpectEq("hazard5 in-range row unchanged", o.raw.unchanged, 1);
    ExpectEq("hazard5 fitting row syncs (1 insert)", o.stat.inserts, 1);
    ExpectEq("hazard5 overflowing row refused (1 blocked)", o.stat.blocked, 1);
}

// --- 6. no primary key -----------------------------------------------------
void Hazard6(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 6: table without a primary key --\n");

    const DiffOutcome o = DiffTable(my, pg, myDb, pgDb, L"t_nopk");
    if (!o.called) {
        ExpectTrue("hazard6 diff ran", false);
        std::printf("  err: %s\n", (const char*)o.err.utf8_str());
        return;
    }
    const int nf = ShowFindings(o.findings, "t_nopk");
    ExpectTrue("hazard6 no-PK table excluded", o.blocked);
    ExpectTrue("hazard6 exclusion carries a reason", nf > 0);
    ExpectEq("hazard6 excluded table emitted no changes",
             o.stat.inserts + o.stat.updates + o.stat.deletes, 0);

    // The rest of the compare is unaffected.
    const DiffOutcome c = DiffTable(my, pg, myDb, pgDb, L"t_control");
    ExpectTrue("hazard6 control table unaffected", !c.blocked && c.stat.updates == 1);
}

} // namespace

int main()
{
    std::printf("== mysql_pg_live_test (MySQL -> PostgreSQL, live) ==\n");

    const wxString myHost = Env("SWIFTSQL_MYTEST_HOST");
    const wxString myUser = Env("SWIFTSQL_MYTEST_USER");
    const wxString myPass = Env("SWIFTSQL_MYTEST_PASS");
    const wxString pgHost = Env("SWIFTSQL_PGTEST_HOST");
    const wxString pgUser = Env("SWIFTSQL_PGTEST_USER");
    const wxString pgPass = Env("SWIFTSQL_PGTEST_PASS");
    if (myHost.IsEmpty() || myUser.IsEmpty() || myPass.IsEmpty() ||
        pgHost.IsEmpty() || pgUser.IsEmpty() || pgPass.IsEmpty()) {
        std::printf("SKIP: no live MySQL+PostgreSQL pair configured "
                    "(set SWIFTSQL_MYTEST_HOST/USER/PASS and "
                    "SWIFTSQL_PGTEST_HOST/USER/PASS)\n");
        return 0;
    }

    const int myPort = EnvInt("SWIFTSQL_MYTEST_PORT", 3306);
    const int pgPort = EnvInt("SWIFTSQL_PGTEST_PORT", 5432);
    wxString myBoot = Env("SWIFTSQL_MYTEST_DB");    if (myBoot.IsEmpty()) myBoot = L"mysql";
    wxString pgBoot = Env("SWIFTSQL_PGTEST_DB");    if (pgBoot.IsEmpty()) pgBoot = L"postgres";
    wxString myDb   = Env("SWIFTSQL_MYTEST_SRCDB"); if (myDb.IsEmpty())   myDb = L"swiftsql_xsync_src";
    wxString pgDb   = Env("SWIFTSQL_XSYNC_TGTDB");  if (pgDb.IsEmpty())   pgDb = L"swiftsql_xsync_tgt";

    const wxString qMyDb = QuoteIdent(myDb, Dialect::MySQL);
    const wxString qPgDb = QuoteIdent(pgDb, Dialect::Postgres);

    // ---- maintenance connections ----
    auto myMaint = CreateConnection(DbType::MySQL);
    auto pgMaint = CreateConnection(DbType::PostgreSQL);
    {
        wxString e1, e2;
        if (!myMaint->Connect(Profile(DbType::MySQL, myHost, myPort, myUser, myPass, myBoot), e1)) {
            std::printf("  FAIL connect MySQL bootstrap: %s\n", (const char*)e1.utf8_str());
            return 1;
        }
        if (!pgMaint->Connect(Profile(DbType::PostgreSQL, pgHost, pgPort, pgUser, pgPass, pgBoot), e2)) {
            std::printf("  FAIL connect PostgreSQL bootstrap: %s\n", (const char*)e2.utf8_str());
            return 1;
        }
        ExpectTrue("connect MySQL bootstrap", myMaint->IsConnected());
        ExpectTrue("connect PostgreSQL bootstrap", pgMaint->IsConnected());
        Observe("mysql server version", myMaint->ServerVersion());
        Observe("pg server version", pgMaint->ServerVersion());
    }

    // ---- leftover audit -------------------------------------------------
    // These servers are SHARED. Everything this test creates is dropped by the
    // guards below, but a hard kill (or a crash mid-run) can leave a `swiftsql_`
    // object behind, and silently reusing it would make the next run's results
    // meaningless as well as leaving litter on someone else's server. Report
    // anything found at startup; the DROP IF EXISTS statements that follow clean
    // up the ones this test owns.
    {
        std::printf("  -- leftover audit (pre-existing swiftsql_* objects) --\n");
        QueryResult r; wxString e;
        if (myMaint->Execute(L"SELECT SCHEMA_NAME FROM information_schema.SCHEMATA "
                             L"WHERE SCHEMA_NAME LIKE 'swiftsql\\_%'", r, e))
            for (const auto& row : r.rows)
                if (!row.empty())
                    std::printf("  OBS  leftover MySQL database: %s\n",
                                (const char*)row[0].utf8_str());
        if (myMaint->Execute(L"SELECT CONCAT(user,'@',host) FROM mysql.user "
                             L"WHERE user LIKE 'swiftsql\\_%'", r, e))
            for (const auto& row : r.rows)
                if (!row.empty())
                    std::printf("  OBS  leftover MySQL account: %s\n",
                                (const char*)row[0].utf8_str());
        if (pgMaint->Execute(L"SELECT datname FROM pg_database "
                             L"WHERE datname LIKE 'swiftsql\\_%'", r, e))
            for (const auto& row : r.rows)
                if (!row.empty())
                    std::printf("  OBS  leftover PG database: %s\n",
                                (const char*)row[0].utf8_str());
    }

    std::unique_ptr<IConnection> my, pg;
    struct Guard {
        IConnection *myMaint, *pgMaint, *my, *pg;
        wxString qMyDb, qPgDb;
        ~Guard()
        {
            if (my) my->Disconnect();
            if (pg) pg->Disconnect();
            QueryResult r; wxString e;
            if (myMaint) myMaint->Execute(L"DROP DATABASE IF EXISTS " + qMyDb, r, e);
            if (pgMaint) pgMaint->Execute(L"DROP DATABASE IF EXISTS " + qPgDb, r, e);
        }
    };

    if (!Exec(*myMaint, L"DROP DATABASE IF EXISTS " + qMyDb, "drop mysql db") ||
        !Exec(*myMaint, L"CREATE DATABASE " + qMyDb +
                        L" DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci",
              "create mysql db") ||
        !Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + qPgDb, "drop pg db") ||
        !Exec(*pgMaint, L"CREATE DATABASE " + qPgDb, "create pg db")) {
        std::printf("  (database setup failed — check CREATE DATABASE privilege)\n");
        Guard g{ myMaint.get(), pgMaint.get(), nullptr, nullptr, qMyDb, qPgDb };
        return 1;
    }

    my = CreateConnection(DbType::MySQL);
    pg = CreateConnection(DbType::PostgreSQL);
    Guard guard{ myMaint.get(), pgMaint.get(), my.get(), pg.get(), qMyDb, qPgDb };

    {
        wxString e1, e2;
        const bool s = my->Connect(Profile(DbType::MySQL, myHost, myPort, myUser, myPass, myDb), e1);
        const bool t = pg->Connect(Profile(DbType::PostgreSQL, pgHost, pgPort, pgUser, pgPass, pgDb), e2);
        ExpectTrue("connect MySQL test DB", s);
        ExpectTrue("connect PostgreSQL test DB", t);
        if (!s || !t) {
            std::printf("  mysql err: %s\n  pg err: %s\n",
                        (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
            std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
            return 1;
        }
    }

    Observe("mysql session collation",
            ScalarOf(*my, L"SELECT @@collation_database"));
    Observe("mysql session time_zone", ScalarOf(*my, L"SELECT @@session.time_zone"));
    Observe("pg default collation",
            ScalarOf(*pg, L"SELECT datcollate FROM pg_database WHERE datname=current_database()"));
    Observe("pg TimeZone", ScalarOf(*pg, L"SHOW TimeZone"));

    const bool setup = SetupMySql(*my, myDb) && SetupPg(*pg, pgDb);
    ExpectTrue("schema + data setup on both engines", setup);
    if (!setup) {
        std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
        return 1;
    }

    Hazard1(*my, *pg, myDb, pgDb);
    Hazard2(*my, *pg, myDb, pgDb);
    Hazard3(*my, *pg, myDb, pgDb);
    Hazard4(*my, *pg, myDb, pgDb);
    Hazard5(*my, *pg, myDb, pgDb);
    Hazard6(*my, *pg, myDb, pgDb);
    RunRoutineHazards(*my, *pg, myDb, pgDb, myHost, myPort, myUser, myPass);

    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;   // guard drops both test databases on the way out
}
