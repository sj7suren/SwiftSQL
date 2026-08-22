// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_scale3.cpp — phase D: isolating the target-side read/write
// collision the scale run exposed. Separate TU for the charter's 1000-line
// ceiling. See mysql_pg_live_scale.h.

#include "mysql_pg_live_scale.h"

#include <cstdio>

namespace mpscale {
namespace {

constexpr const wchar_t* kCollTable = L"t_coll";

// One trial of the experiment.
//
// `diffAt` is the load-bearing knob and the one an earlier draft of this file
// got wrong. Putting the single differing row at the LAST id means the merge has
// already consumed the target's entire result set by the time it has anything to
// write, so the write goes out on an idle connection and the trial passes no
// matter how large the table is - it measures nothing. The differing row must be
// EARLY for the write to be issued while the target stream is still open, which
// is the whole subject.
struct Trial {
    long long rows;        // rows on BOTH sides
    long long batchRows;   // DataSyncOptions::batchRows -> RowCursor queue cap
    long long diffAt;      // id of the single differing row
    bool      mysqlTarget; // false: MySQL -> PostgreSQL. true: PostgreSQL -> MySQL.
    const char* note;
};

// Every trial gets FRESH connections. That is not tidiness: the failure under
// study leaves the target connection unusable, so a trial that reused the
// previous one would be measuring the wreckage rather than its own subject, and
// the controls would fail for the wrong reason.
struct Pair {
    std::unique_ptr<IConnection> my, pg;
    bool Ok() const { return my && pg; }
};

Pair DialPair(Ctx& c)
{
    Pair p;
    wxString e1, e2;
    p.my = c.DialMy(e1);
    p.pg = c.DialPg(e2);
    if (!p.Ok())
        std::printf("  ERR  could not dial a fresh pair: %s / %s\n",
                    (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
    return p;
}

// Build both sides at `rows` rows, identical except ONE row whose value differs
// - so the sweep has exactly one UPDATE to apply. One is deliberate: the subject
// is whether a write can be issued AT ALL while the target stream is open, not
// how many writes succeed.
bool SetupTrial(Ctx& c, IConnection& my, IConnection& pg, long long rows,
                long long diffAt, bool mysqlTarget)
{
    const wxString qMy = QuoteIdent(c.myDb, Dialect::MySQL);
    const wxString myT = qMy + L"." + kCollTable;

    const bool built =
        mplive::Exec(my, L"DROP TABLE IF EXISTS " + myT, "coll drop my") &&
        mplive::Exec(my, L"CREATE TABLE " + myT + L" ("
                     L"  id INT NOT NULL PRIMARY KEY,"
                     L"  name VARCHAR(40) NOT NULL,"
                     L"  v BIGINT NOT NULL) ENGINE=InnoDB", "coll create my") &&
        mplive::Exec(pg, wxString(L"DROP TABLE IF EXISTS ") + kCollTable, "coll drop pg") &&
        mplive::Exec(pg, wxString(L"CREATE TABLE ") + kCollTable + L" ("
                     L"  id integer NOT NULL PRIMARY KEY,"
                     L"  name varchar(40) NOT NULL,"
                     L"  v bigint NOT NULL)", "coll create pg");
    if (!built) return false;

    if (!SeedTable(my, Dialect::MySQL, myT, rows)) return false;
    if (!SeedTable(pg, Dialect::Postgres, kCollTable, rows)) return false;

    // The single difference, wherever the trial wants it, applied to whichever
    // side is the TARGET - the diff must be a row the target has and the source
    // disagrees with, or the sweep has no UPDATE to issue at all.
    return mysqlTarget
        ? mplive::Exec(my, wxString::Format(
              L"UPDATE %s SET v = -1 WHERE id = %lld", myT, diffAt), "coll perturb my")
        : mplive::Exec(pg, wxString::Format(
              L"UPDATE %s SET v = -1 WHERE id = %lld", kCollTable, diffAt),
              "coll perturb pg");
}

struct TrialResult {
    bool      ran = false;
    wxString  err;
    long long applied = 0;
    bool      targetStillUsable = false;
};

TrialResult RunTrial(Ctx& c, const Trial& t)
{
    TrialResult tr;
    Pair p = DialPair(c);
    if (!p.Ok()) return tr;

    if (!SetupTrial(c, *p.my, *p.pg, t.rows, t.diffAt, t.mysqlTarget)) {
        std::printf("  ERR  trial fixture failed (rows=%lld)\n", t.rows);
        return tr;
    }

    // Which way round this trial runs. Both directions are worth asking about:
    // the collision is structural (one connection carries both the target read
    // and the target write), so if it is real it should not care which driver is
    // on the receiving end - libpq and libmysqlclient are each single-threaded
    // per connection and each keep a connection busy until its result set is
    // consumed.
    IConnection& src = t.mysqlTarget ? *p.pg : *p.my;
    IConnection& tgt = t.mysqlTarget ? *p.my : *p.pg;
    const wxString srcDb = t.mysqlTarget ? c.pgDb : c.myDb;
    const wxString tgtDb = t.mysqlTarget ? c.myDb : c.pgDb;

    DataDiffSpec spec;
    if (!MakeSpec(src, tgt, srcDb, tgtDb, kCollTable, spec)) return tr;

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec,
        MakeAllow(kCollTable, Allow{false, true, false}, false)});   // UPDATE only

    DataSyncOptions base;
    base.batchRows = t.batchRows;      // -> RowCursor's queue cap, both cursors

    DataExecResult res;
    tr.ran = ExecuteDataSync(src, tgt, jobs, base, res, tr.err, mpexec::g_never,
                             [](const wxString&, const wxString&, long long) {});
    // BY THE SPEC'S OWN KEY, never the bare table name. DataExecResult is keyed
    // by DataDiffSpec::SourceTable().Key(), which for a PostgreSQL source is
    // schema-qualified ("public.t_coll") and for a MySQL source is not
    // ("t_coll"). Looking it up as kCollTable therefore silently missed every
    // PostgreSQL-source run and reported applied=0 for a sweep that had in fact
    // applied its row - a test-side false negative that looked exactly like a
    // product defect until the two were told apart.
    if (const TableExecReport* r = ReportOf(res, spec.SourceTable().Key()))
        tr.applied = r->updates;

    // Is the target connection still usable AFTER this trial? A failed statement
    // should leave a working connection behind - in the real application this is
    // the ConnectionTree's own long-lived entry, so poisoning it does not just
    // fail one sync, it takes the user's session down with it.
    QueryResult qr; wxString e;
    tr.targetStillUsable = tgt.Execute(L"SELECT 1", qr, e);
    if (!tr.targetStillUsable)
        std::printf("  OBS  post-trial target connection is DEAD; error text=[%s]\n",
                    (const char*)e.utf8_str());
    return tr;
}

// Run one trial and print everything observable about it.
void RunAndReport(Ctx& c, const Trial& t)
{
    std::printf("\n-- trial: rows=%lld batchRows=%lld diffAt=%lld target=%s\n"
                "     %s --\n",
                t.rows, t.batchRows, t.diffAt,
                t.mysqlTarget ? "MySQL" : "PostgreSQL", t.note);

    const TrialResult r = RunTrial(c, t);
    std::printf("  OBS  ran=%d applied=%lld connection_usable_after=%d err=[%s]\n",
                (int)r.ran, r.applied, (int)r.targetStillUsable,
                (const char*)r.err.utf8_str());

    // ASSERTED AS THE DESIGN CLAIMS IT, NOT AS OBSERVED. DataSyncExec.h promises
    // peak memory of one batch "regardless of table size" and documents no
    // ceiling whatsoever on the target's row count; updating a table that
    // already holds rows is the ordinary case, not an edge one. So every trial -
    // control and collision alike - asserts success. A failing collision trial is
    // the PRODUCT's defect, and relaxing this to match what the code currently
    // does would delete the finding rather than record it.
    mplive::ExpectTrue(t.note, r.ran);
    mplive::ExpectEq("phaseD the one differing row was updated", r.applied, 1);
    mplive::ExpectTrue("phaseD the target connection survives the trial",
                       r.targetStillUsable);
}

} // namespace

void PhaseD_TargetReadWriteCollision(Ctx& c)
{
    std::printf("\n== PHASE D: does the execute pass write through the same "
                "connection it is reading the target on? ==\n");

    // THE GOVERNING VARIABLE, established by discarding two wrong guesses.
    //
    // The first draft of this phase supposed the threshold was the target's row
    // count against DataSyncOptions::batchRows - the RowCursor queue cap - on the
    // theory that a target small enough to fit in the queue would be fully
    // drained before the first write. Two observations killed that:
    //
    //   * a 5000-row target with the cap RAISED to 20000, so the whole table
    //     fits, still died; and
    //   * a 200-row target, comfortably inside the default 1000-row cap, passed
    //     on some runs and segfaulted on others.
    //
    // The cap bounds how far the reader may run AHEAD; it never guarantees the
    // reader has FINISHED. Reader and merge run concurrently, so when the first
    // differing row is at the head of the key order the write goes out while the
    // target's result set is still open no matter how large the queue is. The
    // real variable is simply WHETHER THE TARGET STREAM HAS REACHED END-OF-DATA
    // WHEN THE FIRST WRITE IS ISSUED - and for anything but a diff confined to
    // the tail, it has not. Small targets merely win the race more often, which
    // makes this nondeterministic rather than bounded, and a flaky corruption is
    // worse than a reliable one.
    //
    // The controls are therefore the configuration that is safe BY CONSTRUCTION
    // rather than by winning a race: the diff confined to the LAST row, so the
    // cursor has returned end-of-stream, which requires StreamRows to have
    // returned. Both directions, because the mechanism is a property of every
    // client library here and not of one driver. The 200-row case is
    // deliberately NOT asserted on anywhere - it is a coin flip, and pinning
    // either outcome would encode a race into the suite.
    //
    // ORDER MATTERS. The collision does not merely fail: it has been observed to
    // take the whole PROCESS down with a segfault, which is itself the finding
    // (concurrent use of one client-library connection from two threads is
    // undefined behavior, not a reportable error). Controls run first and print
    // their verdicts; the collision trials run last, so a crash leaves the
    // controls on the record - the same reason mysql_pg_live_exec4.cpp runs its
    // backend-killing item last. The process typically dies on the FIRST
    // collision trial, so SWIFTSQL_SCALE_COLLISION=pg|mysql selects just one when
    // both directions need to be characterized across two invocations.
    const Trial controls[] = {
        { 5000, 1000, 5000, false, "CONTROL (PostgreSQL target): diff at the LAST "
                                   "row, so the target stream has reached "
                                   "end-of-data before the write is sent" },
        { 5000, 1000, 5000, true,  "CONTROL (MySQL target): diff at the LAST row, "
                                   "the same thing in the other direction" },
    };
    const Trial collisions[] = {
        { 5000, 1000, 1, true,  "COLLISION (MySQL target): identical table, only "
                                "the diff moved to the FIRST row, so the write is "
                                "issued with ~4999 target rows still unread" },
        { 5000, 1000, 1, false, "COLLISION (PostgreSQL target): identical table, "
                                "only the diff moved to the FIRST row" },
    };

    const wxString which = mplive::Env("SWIFTSQL_SCALE_COLLISION");

    for (const Trial& t : controls) RunAndReport(c, t);

    std::printf("\n-- the collision trials follow; the process may not survive "
                "them --\n");
    for (const Trial& t : collisions) {
        if (which == L"pg"    && t.mysqlTarget)  continue;
        if (which == L"mysql" && !t.mysqlTarget) continue;
        RunAndReport(c, t);
    }
}

} // namespace mpscale
