// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_exec2.cpp — second half of the LIVE EXECUTE/WRITE-PATH suite.
// See mysql_pg_live_exec.cpp's header for why this suite exists and what its
// standing rule is (every assertion reads the TARGET back). Split across two
// TUs purely to stay under the charter's 1000-line ceiling, same reason
// mysql_pg_live_routines.cpp exists.
//
// Items here:
//   C — per-table transactions, partial failure, MySQL DDL auto-commit
//   D — the two-pass consistency window (source changes between compare and
//       execute; the applied counts must be what was APPLIED, not predicted)
//   E — selection filtering end to end, asserted AT THE DATABASE. This is the
//       safety-critical one: the delete gate's six layers were verified at the
//       source, and "verified at the source" is precisely the kind of claim
//       that has been wrong before. Here the question is only ever "is the row
//       still in the target?".
//   H — scale: enough rows that the 500-row materialization cap and the batch
//       boundary both actually engage
#include "mysql_pg_live_exec.h"

#include <chrono>

using namespace mpexec;

namespace {

// ---------------------------------------------------------------------------
// Fixture helper: the same small table on both engines, with a target that
// differs from the source in one of each category.
//
//   source : (1,'s1') (2,'s2') (3,'s3') (4,'s4')
//   target : (1,'s1') (2,'OLD2') (3,'OLD3') (90,'x90') (91,'x91')
//
// So: 1 insert (id=4), 2 updates (id=2,3), 2 deletes (id=90,91), 1 untouched
// (id=1). Every selection question in item E is asked against this shape, so a
// read-back can be compared against a known string rather than a computed one.
bool MakeSelPair(IConnection& my, const wxString& myDb, IConnection& pg,
                 const wxString& table)
{
    const wxString q = QuoteIdent(myDb, Dialect::MySQL) + L"." +
                       QuoteIdent(table, Dialect::MySQL);
    const wxString p = QuoteIdent(table, Dialect::Postgres);
    return mplive::Exec(my, L"DROP TABLE IF EXISTS " + q, "drop my sel") &&
           mplive::Exec(my, L"CREATE TABLE " + q +
                        L" (id INT NOT NULL PRIMARY KEY, name VARCHAR(50) NOT NULL)"
                        L" ENGINE=InnoDB", "create my sel") &&
           mplive::Exec(my, L"INSERT INTO " + q + L" (id,name) VALUES "
                        L"(1,'s1'),(2,'s2'),(3,'s3'),(4,'s4')", "seed my sel") &&
           mplive::Exec(pg, L"DROP TABLE IF EXISTS " + p, "drop pg sel") &&
           mplive::Exec(pg, L"CREATE TABLE " + p +
                        L" (id integer NOT NULL PRIMARY KEY, name varchar(50) NOT NULL)",
                        "create pg sel") &&
           mplive::Exec(pg, L"INSERT INTO " + p + L" (id,name) VALUES "
                        L"(1,'s1'),(2,'OLD2'),(3,'OLD3'),(90,'x90'),(91,'x91')",
                        "seed pg sel");
}

wxString ReadSel(IConnection& pg, const wxString& table)
{
    return Dump(pg, L"SELECT id, name FROM " + QuoteIdent(table, Dialect::Postgres) +
                    L" ORDER BY id");
}

// The target's state before anything is executed — the baseline every
// "nothing was written" assertion compares against.
const wxChar* kSelBefore = L"1|s1;2|OLD2;3|OLD3;90|x90;91|x91";

// ---------------------------------------------------------------------------
// C — per-table transactions and partial failure
// ---------------------------------------------------------------------------
//
// The design claims per-table transactions rather than one giant one, and that
// the result reports which tables committed. Both halves are only observable on
// a run that FAILS in the middle, so one is forced: the target's t_c2 carries a
// CHECK constraint that the incoming data violates.
bool SetupPartial(IConnection& my, const wxString& myDb, IConnection& pg)
{
    const wxString q = QuoteIdent(myDb, Dialect::MySQL);
    for (const wxChar* t : { L"t_c1", L"t_c2", L"t_c3" }) {
        if (!mplive::Exec(my, wxString(L"DROP TABLE IF EXISTS ") + q + L"." + t, "drop c") ||
            !mplive::Exec(my, wxString(L"CREATE TABLE ") + q + L"." + t +
                          L" (id INT NOT NULL PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB",
                          "create c") ||
            !mplive::Exec(pg, wxString(L"DROP TABLE IF EXISTS ") + t, "drop pg c"))
            return false;
    }
    // t_c1 and t_c3 are ordinary. t_c2's target refuses v >= 100, and the source
    // holds a 500 — so the failure happens at APPLY time on the target, which is
    // the only place a rollback claim can be tested.
    return mplive::Exec(pg, L"CREATE TABLE t_c1 (id integer PRIMARY KEY, v integer NOT NULL)", "c1") &&
           mplive::Exec(pg, L"CREATE TABLE t_c2 (id integer PRIMARY KEY, v integer NOT NULL "
                            L"CHECK (v < 100))", "c2") &&
           mplive::Exec(pg, L"CREATE TABLE t_c3 (id integer PRIMARY KEY, v integer NOT NULL)", "c3") &&
           mplive::Exec(my, L"INSERT INTO " + q + L".t_c1 (id,v) VALUES (1,1),(2,2)", "seed c1") &&
           // Row 1 is legal and row 2 is not: if the table were NOT transactional
           // row 1 would be left behind, and that is what the read-back checks.
           mplive::Exec(my, L"INSERT INTO " + q + L".t_c2 (id,v) VALUES (1,5),(2,500)", "seed c2") &&
           mplive::Exec(my, L"INSERT INTO " + q + L".t_c3 (id,v) VALUES (1,7),(2,8)", "seed c3");
}

// The MySQL DDL half. MySQL auto-commits DDL, so a DDL phase wrapped in a
// transaction cannot roll back — the design acknowledges this and the honest
// question is whether the ACTUAL behaviour matches that documentation. Forced
// with a two-statement DDL plan whose second statement is invalid.
// Source and target are the SAME MySQL connection here on purpose: the subject
// is the target's DDL transaction semantics, not a cross-database comparison,
// and the throwaway source database is the only MySQL database this TU owns.
void CheckMySqlDdlAutoCommit(IConnection& my, const wxString& myDb)
{
    std::printf("\n  -- item C: MySQL DDL auto-commit interaction --\n");
    const wxString q = QuoteIdent(myDb, Dialect::MySQL);
    QueryResult r; wxString ig;
    my.Execute(L"DROP TABLE IF EXISTS " + q + L".t_ddl_a", r, ig);
    my.Execute(L"DROP TABLE IF EXISTS " + q + L".t_ddl_b", r, ig);

    SyncPlan plan;
    {
        SyncPlan::TableUnit a;
        a.table = L"t_ddl_a";
        a.ddl.push_back(L"CREATE TABLE " + q + L".t_ddl_a (id INT PRIMARY KEY)");
        plan.units.push_back(std::move(a));

        SyncPlan::TableUnit b;
        b.table = L"t_ddl_b";
        // Invalid on purpose: NO_SUCH_TYPE is not a MySQL column type.
        b.ddl.push_back(L"CREATE TABLE " + q + L".t_ddl_b (id NO_SUCH_TYPE)");
        plan.units.push_back(std::move(b));
    }

    SyncEngine eng(my, my, myDb, myDb);
    wxString err;
    DataExecResult dr;
    const bool ok = eng.Execute(plan, DataExecPlan{}, /*useTransaction=*/true, dr, err,
                                g_never, nullptr);
    std::printf("  OBS  DDL execute ok=%d err=[%s]\n", (int)ok, (const char*)err.utf8_str());
    mplive::ExpectTrue("itemC invalid DDL fails the run", !ok);

    // THE OBSERVATION THAT MATTERS: is the first table still there after the
    // "rollback"? On MySQL it must be — DDL auto-commits — and the product must
    // SAY so rather than implying the target was restored.
    const long long a = ScalarInt(my,
        L"SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='" +
        myDb + L"' AND TABLE_NAME='t_ddl_a'");
    const long long b = ScalarInt(my,
        L"SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='" +
        myDb + L"' AND TABLE_NAME='t_ddl_b'");
    std::printf("  OBS  after failed DDL run: t_ddl_a exists=%lld t_ddl_b exists=%lld\n", a, b);
    mplive::ExpectEq("itemC MySQL DDL did NOT roll back (t_ddl_a survives)", a, 1);
    mplive::ExpectEq("itemC the failing DDL created nothing", b, 0);
    mplive::ExpectTrue("itemC error warns that MySQL DDL cannot be rolled back",
                       err.Contains(L"MySQL DDL"));
}

} // namespace

namespace mpexec {

// ---------------------------------------------------------------------------
// B2 — the placeholder-ceiling chunk reduction
// ---------------------------------------------------------------------------
//
// DataSyncExec hands ExecuteBatch 500 rows at a time. Both drivers then chunk
// AGAIN, because 500 rows of a wide table exceeds each engine's 65535-parameter
// limit for one prepared statement / PQexecParams:
//
//     size_t chunk = 500;
//     if (ncols * chunk > kMaxParams) chunk = max(1, kMaxParams / ncols);
//
// That reduction had never executed. Every table in this suite so far is 2-4
// columns wide, where 500 * ncols stays far under the limit and the branch is
// dead. It is also the single most off-by-one-prone line in the write path: an
// inner loop that mis-slices produces a partial row, a duplicated row, or a
// parameter/tuple count mismatch that the server rejects — and the only way to
// see any of it is to make the branch fire.
//
// 140 columns x 500 rows = 70,000 placeholders, so the driver must reduce to
// 65535/140 = 468 rows per round trip. 600 source rows therefore go out as
// 500 + 100 from DataSyncExec, and the driver re-slices the first into 468 + 32.
// Every boundary in that arithmetic is asserted from the target's contents.
void ItemB2_WideBatch(IConnection& my, IConnection& pg,
                      const wxString& myDb, const wxString& pgDb)
{
    constexpr int kCols = 140;   // 140 * 500 = 70000 > 65535
    constexpr int kRows = 600;   // 500 + 100 from the executor; 468 + 32 in the driver
    std::printf("\n== item B2: wide table (%d cols) forces driver chunk reduction ==\n",
                kCols);

    const wxString q = QuoteIdent(myDb, Dialect::MySQL);
    wxString myCols, pgCols, colList;
    for (int c = 0; c < kCols; ++c) {
        const wxString name = wxString::Format(L"c%d", c);
        if (c) { myCols += L", "; pgCols += L", "; colList += L", "; }
        myCols += name + (c == 0 ? wxString(L" INT NOT NULL PRIMARY KEY")
                                 : wxString(L" INT NOT NULL"));
        pgCols += name + (c == 0 ? wxString(L" integer NOT NULL PRIMARY KEY")
                                 : wxString(L" integer NOT NULL"));
        colList += name;
    }

    bool ok = mplive::Exec(my, L"DROP TABLE IF EXISTS " + q + L".t_wide", "drop wide") &&
              mplive::Exec(my, L"CREATE TABLE " + q + L".t_wide (" + myCols +
                           L") ENGINE=InnoDB", "create wide") &&
              mplive::Exec(pg, L"DROP TABLE IF EXISTS t_wide", "drop pg wide") &&
              mplive::Exec(pg, L"CREATE TABLE t_wide (" + pgCols + L")", "create pg wide");
    if (!ok) { mplive::ExpectTrue("itemB2 fixture setup", false); return; }

    // Cell value is a pure function of (row, column): c0 = id, cN = id*1000 + N.
    // So a row assembled from the wrong slice, or a column shifted by one, is
    // arithmetically detectable rather than merely "looks different".
    for (int base = 1; base <= kRows && ok; base += 50) {
        wxString values;
        for (int id = base; id < base + 50 && id <= kRows; ++id) {
            if (!values.IsEmpty()) values += L",";
            values += L"(";
            for (int c = 0; c < kCols; ++c) {
                if (c) values += L",";
                values += wxString::Format(L"%d", c == 0 ? id : id * 1000 + c);
            }
            values += L")";
        }
        ok = mplive::Exec(my, L"INSERT INTO " + q + L".t_wide (" + colList +
                          L") VALUES " + values, "seed wide");
    }
    if (!ok) { mplive::ExpectTrue("itemB2 seed", false); return; }

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_wide", spec)) {
        mplive::ExpectTrue("itemB2 spec built", false);
        return;
    }
    mplive::ExpectEq("itemB2 source really is 140 columns wide",
                     (long long)spec.srcSchema.columns.size(), kCols);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_wide", Allow{true, true, false}, false)});
    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "B2");
    mplive::ExpectTrue("itemB2 execute ok", x.ok);
    if (!x.ok) std::printf("  ERR  B2: %s\n", (const char*)x.err.utf8_str());

    const TableExecReport* rep = ReportOf(x.result, L"t_wide");
    if (rep) mplive::ExpectEq("itemB2 reported inserts", rep->inserts, kRows);

    mplive::ExpectEq("itemB2 target row count",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_wide"), kRows);
    mplive::ExpectEq("itemB2 every id exactly once",
                     ScalarInt(pg, L"SELECT COUNT(DISTINCT c0) FROM t_wide"), kRows);
    // Rows straddling the DRIVER's reduced chunk boundary (468/469) and the
    // EXECUTOR's own (500/501) — the two do not coincide, which is the whole
    // point of asserting both.
    ExpectStr("itemB2 rows across the driver's 468-row chunk boundary",
              Dump(pg, L"SELECT c0 FROM t_wide WHERE c0 IN (467,468,469,470) ORDER BY c0"),
              L"467;468;469;470");
    ExpectStr("itemB2 rows across the executor's 500-row batch boundary",
              Dump(pg, L"SELECT c0 FROM t_wide WHERE c0 IN (499,500,501,502) ORDER BY c0"),
              L"499;500;501;502");
    // No column was shifted, dropped or duplicated anywhere in the table: every
    // cell must still satisfy cN = c0*1000 + N. Checked on the three columns
    // most likely to be damaged by a slicing bug (first, middle, last).
    for (int c : { 1, kCols / 2, kCols - 1 }) {
        const wxString col = wxString::Format(L"c%d", c);
        mplive::ExpectEq(
            "itemB2 no cell was shifted by the chunk reduction",
            ScalarInt(pg, L"SELECT COUNT(*) FROM t_wide WHERE " + col +
                          wxString::Format(L" <> c0 * 1000 + %d", c)), 0);
    }

    const CompareOutcome again = Compare1(my, pg, spec);
    std::printf("  OBS  B2 re-compare: ins=%lld upd=%lld del=%lld\n",
                again.stat.inserts, again.stat.updates, again.stat.deletes);
    mplive::ExpectEq("itemB2 re-compare finds no differences",
                     again.stat.inserts + again.stat.updates + again.stat.deletes, 0);
}

void ItemC_PartialFailure(IConnection& my, IConnection& pg,
                          const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item C: per-table transactions and partial failure ==\n");
    if (!SetupPartial(my, myDb, pg)) {
        mplive::ExpectTrue("itemC fixture setup", false);
        return;
    }

    std::vector<TableExecJob> jobs;
    for (const wxChar* t : { L"t_c1", L"t_c2", L"t_c3" }) {
        DataDiffSpec spec;
        if (!MakeSpec(my, pg, myDb, pgDb, t, spec)) {
            mplive::ExpectTrue("itemC spec built", false);
            return;
        }
        jobs.push_back(TableExecJob{spec, MakeAllow(t, Allow{true, true, false}, false)});
    }

    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "C");
    std::printf("  OBS  C error: [%s]\n", (const char*)x.err.utf8_str());
    mplive::ExpectTrue("itemC run fails", !x.ok);

    // ---- which tables committed, per table ---------------------------------
    const std::vector<wxString> committed = x.result.Committed();
    const std::vector<wxString> failed    = x.result.Failed();
    const std::vector<wxString> untouched = x.result.NotAttempted();
    for (const wxString& s : committed) std::printf("  OBS  committed: %s\n", (const char*)s.utf8_str());
    for (const wxString& s : failed)    std::printf("  OBS  failed:    %s\n", (const char*)s.utf8_str());
    for (const wxString& s : untouched) std::printf("  OBS  untouched: %s\n", (const char*)s.utf8_str());

    mplive::ExpectEq("itemC exactly one table committed", (long long)committed.size(), 1);
    mplive::ExpectTrue("itemC the committed table is t_c1",
                       committed.size() == 1 && committed[0] == L"t_c1");
    mplive::ExpectEq("itemC exactly one table failed", (long long)failed.size(), 1);
    mplive::ExpectTrue("itemC the failed table is t_c2",
                       failed.size() == 1 && failed[0] == L"t_c2");
    mplive::ExpectEq("itemC exactly one table was never attempted",
                     (long long)untouched.size(), 1);
    mplive::ExpectTrue("itemC the unattempted table is t_c3",
                       untouched.size() == 1 && untouched[0] == L"t_c3");
    mplive::ExpectTrue("itemC the error names the failing table",
                       x.err.Contains(L"t_c2"));

    // ---- and now the target itself -----------------------------------------
    // Previously-committed table stays committed.
    ExpectStr("itemC committed table's rows are PRESENT in the target",
              Dump(pg, L"SELECT id, v FROM t_c1 ORDER BY id"), L"1|1;2|2");
    // The failing table rolled back COMPLETELY — including the legal row that
    // preceded the illegal one inside the same transaction.
    ExpectStr("itemC failing table rolled back completely (no partial rows)",
              Dump(pg, L"SELECT id, v FROM t_c2 ORDER BY id"), wxString());
    // The unattempted table was never written.
    ExpectStr("itemC unattempted table is untouched",
              Dump(pg, L"SELECT id, v FROM t_c3 ORDER BY id"), wxString());

    // ---- the target table disappears between compare and execute -----------
    // The other half of the partial-failure question, and the one the two-pass
    // design makes reachable: the spec (including the target's schema) is
    // captured at compare time, but the ROWS are re-read at execute time. If
    // the table is gone by then, the executor is holding a schema for something
    // that no longer exists. It must fail loudly and leave nothing behind, not
    // half-create or half-write.
    {
        std::printf("\n  -- item C: target table dropped between the passes --\n");
        const wxString q = QuoteIdent(myDb, Dialect::MySQL);
        const bool setup =
            mplive::Exec(my, L"DROP TABLE IF EXISTS " + q + L".t_c4", "drop c4") &&
            mplive::Exec(my, L"CREATE TABLE " + q + L".t_c4 "
                         L"(id INT NOT NULL PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB",
                         "create c4") &&
            mplive::Exec(my, L"INSERT INTO " + q + L".t_c4 (id,v) VALUES (1,1),(2,2)",
                         "seed c4") &&
            mplive::Exec(pg, L"DROP TABLE IF EXISTS t_c4", "drop pg c4") &&
            mplive::Exec(pg, L"CREATE TABLE t_c4 (id integer PRIMARY KEY, v integer NOT NULL)",
                         "create pg c4");
        if (!setup) { mplive::ExpectTrue("itemC c4 fixture", false); return; }

        DataDiffSpec spec;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_c4", spec)) return;
        const CompareOutcome cmp = Compare1(my, pg, spec);
        mplive::ExpectEq("itemC c4 compare predicts 2 inserts", cmp.stat.inserts, 2);

        // …and now it is gone.
        mplive::Exec(pg, L"DROP TABLE t_c4", "drop target between passes");

        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{spec, MakeAllow(L"t_c4", Allow{true, true, false}, false)});
        const ExecOutcome x = RunExec(my, pg, jobs);
        ShowResult(x.result, "C-drop");
        std::printf("  OBS  C-drop error: [%s]\n", (const char*)x.err.utf8_str());
        mplive::ExpectTrue("itemC dropped target fails the run", !x.ok);
        mplive::ExpectTrue("itemC dropped-target error names the table",
                           x.err.Contains(L"t_c4"));

        const TableExecReport* rep = ReportOf(x.result, L"t_c4");
        mplive::ExpectTrue("itemC dropped target reported as attempted-and-failed",
                           rep && rep->attempted && !rep->committed &&
                           !rep->error.IsEmpty());
        // It must be listed as FAILED, not silently as "never attempted" — the
        // caller's whole recovery story is which bucket a table landed in.
        const std::vector<wxString> failed = x.result.Failed();
        mplive::ExpectTrue("itemC dropped target appears in Failed(), not NotAttempted()",
                           failed.size() == 1 && failed[0] == L"t_c4");
        // And the executor did not helpfully recreate anything.
        mplive::ExpectEq("itemC executor did NOT recreate the dropped table",
                         ScalarInt(pg, L"SELECT COUNT(*) FROM information_schema.tables "
                                       L"WHERE table_schema='public' AND table_name='t_c4'"), 0);
    }

    CheckMySqlDdlAutoCommit(my, myDb);
}

// ---------------------------------------------------------------------------
// D — the two-pass consistency window
// ---------------------------------------------------------------------------
//
// Compare materializes a bounded sample; Execute RE-RUNS the diff and streams.
// So the source can legitimately change in between, and the design's stated
// position is that the newer state is applied and the reported counts describe
// what was APPLIED, never what compare predicted. Nobody had ever watched that
// happen. This drives the drift on purpose.
void ItemD_DriftWindow(IConnection& my, IConnection& pg,
                       const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item D: the two-pass consistency window ==\n");
    const wxString q = QuoteIdent(myDb, Dialect::MySQL);

    const bool setup =
        mplive::Exec(my, L"DROP TABLE IF EXISTS " + q + L".t_drift", "drop drift") &&
        mplive::Exec(my, L"CREATE TABLE " + q + L".t_drift "
                     L"(id INT NOT NULL PRIMARY KEY, name VARCHAR(50) NOT NULL) ENGINE=InnoDB",
                     "create drift") &&
        mplive::Exec(my, L"INSERT INTO " + q + L".t_drift (id,name) VALUES (1,'a'),(2,'b')",
                     "seed drift") &&
        mplive::Exec(pg, L"DROP TABLE IF EXISTS t_drift", "drop pg drift") &&
        mplive::Exec(pg, L"CREATE TABLE t_drift "
                     L"(id integer NOT NULL PRIMARY KEY, name varchar(50) NOT NULL)",
                     "create pg drift");
    if (!setup) { mplive::ExpectTrue("itemD fixture setup", false); return; }

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_drift", spec)) {
        mplive::ExpectTrue("itemD spec built", false);
        return;
    }

    // PASS 1 — what the user sees and approves.
    const CompareOutcome cmp = Compare1(my, pg, spec);
    std::printf("  OBS  compare predicted: ins=%lld upd=%lld del=%lld\n",
                cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes);
    mplive::ExpectEq("itemD compare predicts 2 inserts", cmp.stat.inserts, 2);

    // ---- the source moves under us, between the passes ---------------------
    const bool drifted =
        mplive::Exec(my, L"UPDATE " + q + L".t_drift SET name='a-NEW' WHERE id=1", "drift upd") &&
        mplive::Exec(my, L"INSERT INTO " + q + L".t_drift (id,name) VALUES (3,'c-NEW')", "drift ins") &&
        mplive::Exec(my, L"DELETE FROM " + q + L".t_drift WHERE id=2", "drift del");
    mplive::ExpectTrue("itemD source drifted between the passes", drifted);

    // PASS 2 — execute. Note the spec is the one built BEFORE the drift; only
    // the ROWS are re-read, which is exactly the production situation.
    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_drift", Allow{true, true, false}, false)});
    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "D");
    mplive::ExpectTrue("itemD execute ok", x.ok);

    // The target must hold the NEWER state — id=1 as 'a-NEW', id=3 present,
    // id=2 never written at all (it was deleted on the source before pass 2 read
    // it, so it was never an insert candidate).
    const wxString got = Dump(pg, L"SELECT id, name FROM t_drift ORDER BY id");
    std::printf("  OBS  target after drifted execute: [%s]\n", (const char*)got.utf8_str());
    ExpectStr("itemD target holds the state at EXECUTE time, not at compare time",
              got, L"1|a-NEW;3|c-NEW");

    // And the counts describe what was APPLIED (2 rows: ids 1 and 3), not what
    // compare predicted (2 rows: ids 1 and 2). The numbers happen to coincide
    // here, so the row CONTENTS above are what actually carries the claim — this
    // assertion pins that the report is not simply echoing the compare pass.
    const TableExecReport* rep = ReportOf(x.result, L"t_drift");
    mplive::ExpectTrue("itemD table reported", rep != nullptr);
    if (rep) {
        mplive::ExpectEq("itemD applied inserts", rep->inserts, 2);
        mplive::ExpectEq("itemD applied updates", rep->updates, 0);
        mplive::ExpectTrue("itemD table committed", rep->committed);
    }

    // A second drift, this time one that makes the applied count DIFFER from the
    // predicted one — so "the report is not a copy of the prediction" is proved
    // by a disagreement, not by a coincidence.
    {
        const CompareOutcome c2 = Compare1(my, pg, spec);
        mplive::ExpectEq("itemD second compare predicts no work",
                         c2.stat.inserts + c2.stat.updates + c2.stat.deletes, 0);

        mplive::Exec(my, L"INSERT INTO " + q + L".t_drift (id,name) VALUES (4,'d'),(5,'e')",
                     "drift ins2");

        const ExecOutcome x2 = RunExec(my, pg, jobs);
        ShowResult(x2.result, "D2");
        const TableExecReport* r2 = ReportOf(x2.result, L"t_drift");
        std::printf("  OBS  predicted=0 applied=%lld\n", r2 ? r2->inserts : -1);
        mplive::ExpectTrue("itemD execute ok after second drift", x2.ok);
        mplive::ExpectTrue("itemD applied count reports what was APPLIED (2), "
                           "not what compare predicted (0)",
                           r2 && r2->inserts == 2);
        ExpectStr("itemD target holds the rows that appeared after compare",
                  Dump(pg, L"SELECT id, name FROM t_drift ORDER BY id"),
                  L"1|a-NEW;3|c-NEW;4|d;5|e");
    }
}

// ---------------------------------------------------------------------------
// E — selection filtering end to end, AT THE DATABASE
// ---------------------------------------------------------------------------
void ItemE_Selection(IConnection& my, IConnection& pg,
                     const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item E: selection filtering, asserted at the target ==\n");

    const bool setup =
        MakeSelPair(my, myDb, pg, L"t_sel_a") &&
        MakeSelPair(my, myDb, pg, L"t_sel_b") &&
        MakeSelPair(my, myDb, pg, L"t_sel_c") &&
        MakeSelPair(my, myDb, pg, L"t_sel_d") &&
        MakeSelPair(my, myDb, pg, L"t_sel_e");
    if (!setup) { mplive::ExpectTrue("itemE fixture setup", false); return; }

    // ---- E1: delete master switch OFF --------------------------------------
    // The switch is the outermost of the six layers. With it off, the extra
    // target rows must SURVIVE — asserted by reading them back, not by
    // inspecting a plan.
    {
        std::printf("\n  -- E1: delete master switch OFF --\n");
        DataDiffSpec spec;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_sel_a", spec)) return;

        const CompareOutcome cmp = Compare1(my, pg, spec);
        mplive::ExpectEq("itemE1 compare still SEES the 2 deletable rows",
                         cmp.stat.deletes, 2);

        // del=true requested, master switch FALSE -> AuthorizeDeletes yields
        // nullopt and the spec cannot carry deletes at all.
        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{
            spec, MakeAllow(L"t_sel_a", Allow{true, true, true}, /*master=*/false)});
        mplive::ExpectTrue("itemE1 spec carries no delete authorization",
                           !jobs[0].allow.Deletes());

        const ExecOutcome x = RunExec(my, pg, jobs);
        ShowResult(x.result, "E1");
        mplive::ExpectTrue("itemE1 execute ok", x.ok);

        const wxString got = ReadSel(pg, L"t_sel_a");
        std::printf("  OBS  t_sel_a: [%s]\n", (const char*)got.utf8_str());
        // Inserts and updates applied; BOTH extras still present.
        ExpectStr("itemE1 TARGET: extra rows SURVIVE with the master switch off",
                  got, L"1|s1;2|s2;3|s3;4|s4;90|x90;91|x91");
        mplive::ExpectEq("itemE1 target still holds both extra rows",
                         ScalarInt(pg, L"SELECT COUNT(*) FROM t_sel_a WHERE id IN (90,91)"), 2);
        const TableExecReport* rep = ReportOf(x.result, L"t_sel_a");
        if (rep) mplive::ExpectEq("itemE1 zero deletes applied", rep->deletes, 0);
    }

    // ---- E2: deletes armed for ONE table only ------------------------------
    {
        std::printf("\n  -- E2: deletes armed for t_sel_b only --\n");
        DataDiffSpec sb, sc;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_sel_b", sb)) return;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_sel_c", sc)) return;

        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{sb, MakeAllow(L"t_sel_b", Allow{true, true, true}, true)});
        jobs.push_back(TableExecJob{sc, MakeAllow(L"t_sel_c", Allow{true, true, false}, true)});
        mplive::ExpectTrue("itemE2 only t_sel_b is authorized to delete",
                           jobs[0].allow.Deletes() && !jobs[1].allow.Deletes());

        const ExecOutcome x = RunExec(my, pg, jobs);
        ShowResult(x.result, "E2");
        mplive::ExpectTrue("itemE2 execute ok", x.ok);

        const wxString gb = ReadSel(pg, L"t_sel_b");
        const wxString gc = ReadSel(pg, L"t_sel_c");
        std::printf("  OBS  t_sel_b: [%s]\n  OBS  t_sel_c: [%s]\n",
                    (const char*)gb.utf8_str(), (const char*)gc.utf8_str());
        ExpectStr("itemE2 TARGET: armed table's extras are GONE",
                  gb, L"1|s1;2|s2;3|s3;4|s4");
        ExpectStr("itemE2 TARGET: unarmed table's extras REMAIN",
                  gc, L"1|s1;2|s2;3|s3;4|s4;90|x90;91|x91");

        const TableExecReport* rb = ReportOf(x.result, L"t_sel_b");
        const TableExecReport* rc = ReportOf(x.result, L"t_sel_c");
        if (rb) mplive::ExpectEq("itemE2 armed table applied 2 deletes", rb->deletes, 2);
        if (rc) mplive::ExpectEq("itemE2 unarmed table applied 0 deletes", rc->deletes, 0);
    }

    // ---- E3: per-category — inserts only -----------------------------------
    {
        std::printf("\n  -- E3: inserts checked, updates NOT --\n");
        DataDiffSpec spec;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_sel_d", spec)) return;

        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{
            spec, MakeAllow(L"t_sel_d", Allow{/*ins=*/true, /*upd=*/false, /*del=*/false}, false)});

        const ExecOutcome x = RunExec(my, pg, jobs);
        ShowResult(x.result, "E3");
        mplive::ExpectTrue("itemE3 execute ok", x.ok);

        const wxString got = ReadSel(pg, L"t_sel_d");
        std::printf("  OBS  t_sel_d: [%s]\n", (const char*)got.utf8_str());
        // id=4 inserted; id=2 and id=3 must STILL carry their old values; the
        // extras untouched.
        ExpectStr("itemE3 TARGET: insert applied, updates did NOT",
                  got, L"1|s1;2|OLD2;3|OLD3;4|s4;90|x90;91|x91");
        mplive::ExpectEq("itemE3 no row was silently updated",
                         ScalarInt(pg, L"SELECT COUNT(*) FROM t_sel_d "
                                       L"WHERE id IN (2,3) AND name LIKE 'OLD%'"), 2);
        const TableExecReport* rep = ReportOf(x.result, L"t_sel_d");
        if (rep) {
            mplive::ExpectEq("itemE3 applied 1 insert", rep->inserts, 1);
            mplive::ExpectEq("itemE3 applied 0 updates", rep->updates, 0);
        }
    }

    // ---- E4: row-level exclusion -------------------------------------------
    {
        std::printf("\n  -- E4: two specific rows excluded by identity --\n");
        DataDiffSpec spec;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_sel_e", spec)) return;

        // The identities the UI would carry: EncodeRowKey over the RAW,
        // pre-bridge primary key. Built here the same way the merge builds them
        // — if the encodings disagreed, the exclusion would silently miss and
        // the row would sync anyway, which is the failure this pins.
        const wxString keyInsert =
            EncodeRowKey({ L"id" }, { Cell{ CellKind::Numeric, L"4" } });
        const wxString keyUpdate =
            EncodeRowKey({ L"id" }, { Cell{ CellKind::Numeric, L"2" } });
        std::printf("  OBS  excluded keys: [%s] [%s]\n",
                    (const char*)keyInsert.utf8_str(), (const char*)keyUpdate.utf8_str());

        Allow a{true, true, false};
        a.excludeRows = { keyInsert, keyUpdate };
        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{spec, MakeAllow(L"t_sel_e", a, false)});

        const ExecOutcome x = RunExec(my, pg, jobs);
        ShowResult(x.result, "E4");
        mplive::ExpectTrue("itemE4 execute ok", x.ok);

        const wxString got = ReadSel(pg, L"t_sel_e");
        std::printf("  OBS  t_sel_e: [%s]\n", (const char*)got.utf8_str());
        // id=4 must be ABSENT (its insert was excluded); id=2 must still be
        // OLD2 (its update was excluded); id=3 must have been updated.
        ExpectStr("itemE4 TARGET: exactly the two excluded rows were not applied",
                  got, L"1|s1;2|OLD2;3|s3;90|x90;91|x91");
        mplive::ExpectEq("itemE4 excluded insert is absent from the target",
                         ScalarInt(pg, L"SELECT COUNT(*) FROM t_sel_e WHERE id=4"), 0);
        mplive::ExpectEq("itemE4 excluded update did not change the target row",
                         ScalarInt(pg, L"SELECT COUNT(*) FROM t_sel_e "
                                       L"WHERE id=2 AND name='OLD2'"), 1);
        const TableExecReport* rep = ReportOf(x.result, L"t_sel_e");
        if (rep) {
            mplive::ExpectEq("itemE4 two rows reported excluded", rep->excluded, 2);
            mplive::ExpectEq("itemE4 no insert applied", rep->inserts, 0);
            mplive::ExpectEq("itemE4 one update applied", rep->updates, 1);
        }
    }

    // ---- E5: nothing checked at all ----------------------------------------
    // The degenerate case, and the one a customer hits by mis-clicking: an
    // authorization carrying no category must write NOTHING.
    {
        std::printf("\n  -- E5: no category checked --\n");
        const wxString before = ReadSel(pg, L"t_sel_c");
        DataDiffSpec spec;
        if (!MakeSpec(my, pg, myDb, pgDb, L"t_sel_c", spec)) return;
        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{spec, MakeAllow(L"t_sel_c", Allow{}, false)});
        const ExecOutcome x = RunExec(my, pg, jobs);
        mplive::ExpectTrue("itemE5 execute ok", x.ok);
        ExpectStr("itemE5 TARGET is byte-for-byte unchanged", ReadSel(pg, L"t_sel_c"), before);
        mplive::ExpectEq("itemE5 nothing was even attempted",
                         (long long)x.result.tables.size(), 0);
    }
}

// ---------------------------------------------------------------------------
// H — scale
// ---------------------------------------------------------------------------
//
// Everything above runs on 2-11 row tables except item B. This one is large
// enough that the 500-row materialization cap, Truncated(), the 500-row batch
// boundary and intra-table progress ALL engage at once, and small enough that
// the suite stays fast.
void ItemH_Scale(IConnection& my, IConnection& pg,
                 const wxString& myDb, const wxString& pgDb)
{
    constexpr int kRows = 3000;
    std::printf("\n== item H: scale (%d rows) ==\n", kRows);
    const wxString q = QuoteIdent(myDb, Dialect::MySQL);

    bool ok =
        mplive::Exec(my, L"DROP TABLE IF EXISTS " + q + L".t_scale", "drop scale") &&
        mplive::Exec(my, L"CREATE TABLE " + q + L".t_scale ("
                     L"  id INT NOT NULL PRIMARY KEY,"
                     L"  name VARCHAR(60) NOT NULL,"
                     L"  v INT NOT NULL) ENGINE=InnoDB", "create scale") &&
        mplive::Exec(pg, L"DROP TABLE IF EXISTS t_scale", "drop pg scale") &&
        mplive::Exec(pg, L"CREATE TABLE t_scale ("
                     L"  id integer NOT NULL PRIMARY KEY,"
                     L"  name varchar(60) NOT NULL,"
                     L"  v integer NOT NULL)", "create pg scale");
    if (ok) {
        wxString values;
        int inChunk = 0;
        for (int id = 1; id <= kRows && ok; ++id) {
            if (!values.IsEmpty()) values += L",";
            values += wxString::Format(L"(%d,'row-%d',%d)", id, id, id * 2);
            if (++inChunk >= 500 || id == kRows) {
                ok = mplive::Exec(my, L"INSERT INTO " + q +
                                  L".t_scale (id,name,v) VALUES " + values, "seed scale");
                values.Clear();
                inChunk = 0;
            }
        }
    }
    // A pre-existing target row that DIFFERS, so the run is not purely inserts:
    // an update crossing a batch flush is where an insert buffered behind an
    // update would land out of order.
    ok = ok && mplive::Exec(pg, L"INSERT INTO t_scale (id,name,v) VALUES "
                                L"(1200,'STALE',-1),(2500,'STALE',-1),(99999,'EXTRA',0)",
                            "seed pg scale");
    if (!ok) { mplive::ExpectTrue("itemH fixture setup", false); return; }

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_scale", spec)) {
        mplive::ExpectTrue("itemH spec built", false);
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const CompareOutcome cmp = Compare1(my, pg, spec);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("  OBS  compare: ins=%lld upd=%lld del=%lld truncated=%d (%lld ms)\n",
                cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes, (int)cmp.truncated,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    mplive::ExpectEq("itemH compare counts inserts exactly", cmp.stat.inserts, kRows - 2);
    mplive::ExpectEq("itemH compare counts updates exactly", cmp.stat.updates, 2);
    mplive::ExpectEq("itemH compare counts deletes exactly", cmp.stat.deletes, 1);
    // 2999 changes against a 500-row cap.
    mplive::ExpectTrue("itemH Truncated() is set past the 500-row cap", cmp.truncated);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_scale", Allow{true, true, true}, true)});
    const auto t2 = std::chrono::steady_clock::now();
    const ExecOutcome x = RunExec(my, pg, jobs);
    const auto t3 = std::chrono::steady_clock::now();
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    ShowResult(x.result, "H");
    std::printf("  OBS  execute took %lld ms; progress fired %lld times\n",
                ms, x.progressCalls);
    mplive::ExpectTrue("itemH execute ok", x.ok);
    if (!x.ok) std::printf("  ERR  H: %s\n", (const char*)x.err.utf8_str());

    const TableExecReport* rep = ReportOf(x.result, L"t_scale");
    if (rep) {
        mplive::ExpectEq("itemH applied inserts", rep->inserts, kRows - 2);
        mplive::ExpectEq("itemH applied updates", rep->updates, 2);
        mplive::ExpectEq("itemH applied deletes", rep->deletes, 1);
        mplive::ExpectTrue("itemH committed", rep->committed);
    }
    // Intra-table progress: a 3000-row table crossing six batch boundaries must
    // move the gauge more than once, or a long run shows a frozen dialog.
    mplive::ExpectTrue("itemH intra-table progress fired more than once",
                       x.progressCalls > 1);

    // ---- the target, exactly ------------------------------------------------
    mplive::ExpectEq("itemH target row count exact",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_scale"), kRows);
    mplive::ExpectEq("itemH every id present exactly once",
                     ScalarInt(pg, L"SELECT COUNT(DISTINCT id) FROM t_scale"), kRows);
    mplive::ExpectEq("itemH id checksum",
                     ScalarInt(pg, L"SELECT COALESCE(SUM(id),0) FROM t_scale"),
                     (long long)kRows * (kRows + 1) / 2);
    mplive::ExpectEq("itemH value checksum (every v is id*2)",
                     ScalarInt(pg, L"SELECT COALESCE(SUM(v),0) FROM t_scale"),
                     (long long)kRows * (kRows + 1));
    mplive::ExpectEq("itemH no stale row survived",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_scale WHERE name='STALE'"), 0);
    mplive::ExpectEq("itemH the extra target row was deleted",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_scale WHERE id=99999"), 0);
    mplive::ExpectEq("itemH no name is malformed",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_scale "
                                   L"WHERE name <> ('row-' || id::text)"), 0);
    // Rows straddling every batch boundary, by identity.
    ExpectStr("itemH rows across batch boundaries land",
              Dump(pg, L"SELECT id FROM t_scale WHERE id IN (500,501,1000,1001,"
                       L"1500,1501,2000,2001,2500,2501,3000) ORDER BY id"),
              L"500;501;1000;1001;1500;1501;2000;2001;2500;2501;3000");

    const CompareOutcome again = Compare1(my, pg, spec);
    std::printf("  OBS  re-compare: ins=%lld upd=%lld del=%lld\n",
                again.stat.inserts, again.stat.updates, again.stat.deletes);
    mplive::ExpectEq("itemH re-compare finds nothing left to do",
                     again.stat.inserts + again.stat.updates + again.stat.deletes, 0);
}

} // namespace mpexec
