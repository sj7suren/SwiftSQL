// mysql_pg_live_exec3.cpp — third TU of the LIVE EXECUTE/WRITE-PATH suite.
// See mysql_pg_live_exec.cpp's header for the suite's purpose and its standing
// rule (every assertion reads the TARGET back). Split off purely to keep every
// TU under the charter's 1000-line ceiling.
//
// The two items here close the last gaps where a whole block of write-path code
// had still never executed after items A-H:
//
//   B3 — the MySQL driver's PARAMETER BINDING. Every write in items A-H that
//        exercised coercion went into PostgreSQL, i.e. through PgStream's
//        PQexecParams. MySqlStream::ExecChunk's mysql_stmt_bind_param path —
//        the is_null flag array, MYSQL_TYPE_BLOB binding, HexToBytes — had only
//        ever run for plain INT/VARCHAR columns (item A's same-engine leg),
//        where the NULL and binary branches are simply never taken. This drives
//        PostgreSQL → MySQL so those branches finally execute.
//
//   C3 — the delete-sweep window. ExecuteDataSync runs DELETEs for ALL tables in
//        one sweep and INSERT/UPDATEs in a second, each table in its own
//        transaction. So a table's deletes can COMMIT and its inserts/updates
//        then FAIL, leaving rows removed with no compensating write. The design
//        acknowledges this (DataSyncExec.h: "committed=false WITH a non-zero
//        deletes ... the honest description of a partial apply"), but it had
//        never been observed, and it is the single most destructive reachable
//        outcome of this feature. It is pinned here so it can never regress into
//        something worse — or be quietly forgotten.
#include "mysql_pg_live_exec.h"

using namespace mpexec;

namespace mpexec {

// ---------------------------------------------------------------------------
// B3 — PostgreSQL → MySQL: the MySQL driver's bind path
// ---------------------------------------------------------------------------
void ItemB3_MySqlTargetBinding(IConnection& my, IConnection& pg,
                               const wxString& myDb, const wxString& pgDb)
{
    constexpr int kRows = 700;   // crosses the 500-row batch boundary once
    std::printf("\n== item B3: PG -> MySQL, exercising MySqlStream's bind path ==\n");

    const wxString q = QuoteIdent(myDb, Dialect::MySQL);
    bool ok =
        mplive::Exec(pg, L"DROP TABLE IF EXISTS t_bind", "drop pg bind") &&
        mplive::Exec(pg, L"CREATE TABLE t_bind ("
                     L"  id integer NOT NULL PRIMARY KEY,"
                     L"  flag boolean NOT NULL,"
                     L"  note varchar(50) NULL,"
                     L"  payload bytea NULL)", "create pg bind") &&
        mplive::Exec(my, L"DROP TABLE IF EXISTS " + q + L".t_bind", "drop my bind") &&
        mplive::Exec(my, L"CREATE TABLE " + q + L".t_bind ("
                     L"  id INT NOT NULL PRIMARY KEY,"
                     L"  flag TINYINT(1) NOT NULL,"
                     L"  note VARCHAR(50) NULL,"
                     L"  payload VARBINARY(16) NULL) ENGINE=InnoDB", "create my bind");
    if (!ok) { mplive::ExpectTrue("itemB3 fixture setup", false); return; }

    // Same value scheme as item B, mirrored: NULL / empty-string / text on the
    // `note` column and a 4-byte payload containing a 0x00, so the two branches
    // that MySqlStream has never taken (is_null, MYSQL_TYPE_BLOB) both fire on
    // most batches rather than on one lucky row.
    for (int base = 1; base <= kRows && ok; base += 100) {
        wxString values;
        for (int id = base; id < base + 100 && id <= kRows; ++id) {
            if (!values.IsEmpty()) values += L",";
            const wxString note = (id % 3 == 0) ? wxString(L"NULL")
                                : (id % 3 == 1) ? wxString(L"''")
                                                : wxString::Format(L"'n%d'", id);
            // Every 7th row's payload is NULL, so a bytea NULL also crosses.
            const wxString pay = (id % 7 == 0)
                ? wxString(L"NULL")
                : wxString::Format(L"decode('AA00BB%02X','hex')", id & 0xFF);
            values += wxString::Format(L"(%d,%s,%s,%s)", id,
                                       (id % 2 == 0) ? L"true" : L"false", note, pay);
        }
        ok = mplive::Exec(pg, L"INSERT INTO t_bind (id,flag,note,payload) VALUES " + values,
                          "seed pg bind");
    }
    if (!ok) { mplive::ExpectTrue("itemB3 seed", false); return; }

    DataDiffSpec spec;
    spec.srcDb = pgDb;
    spec.tgtDb = myDb;
    wxString e1, e2;
    if (!pg.GetTableSchema(pgDb, L"t_bind", spec.srcSchema, e1) ||
        !my.GetTableSchema(myDb, L"t_bind", spec.tgtSchema, e2)) {
        std::printf("  ERR  B3 schemas: %s / %s\n",
                    (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
        mplive::ExpectTrue("itemB3 schemas read", false);
        return;
    }

    const CompareOutcome cmp = Compare1(pg, my, spec);
    std::printf("  OBS  B3 compare: ins=%lld upd=%lld del=%lld blocked=%lld executable=%d\n",
                cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes,
                cmp.stat.blocked, (int)cmp.executable);
    if (!cmp.executable) {
        // If the reverse direction's value gate refuses this table, that is an
        // ANSWER, not a crash — but it must be an explicit one, and the rest of
        // the item cannot run. Recorded rather than asserted away.
        mplive::ExpectTrue("itemB3 PG->MySQL boolean/bytea table is executable",
                           cmp.executable);
        return;
    }
    mplive::ExpectEq("itemB3 compare counts every row as an insert",
                     cmp.stat.inserts, kRows);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_bind", Allow{true, true, false}, false)});
    const ExecOutcome x = RunExec(pg, my, jobs);
    ShowResult(x.result, "B3");
    mplive::ExpectTrue("itemB3 execute ok", x.ok);
    if (!x.ok) std::printf("  ERR  B3: %s\n", (const char*)x.err.utf8_str());

    const wxString t = q + L".t_bind";
    mplive::ExpectEq("itemB3 target row count",
                     ScalarInt(my, L"SELECT COUNT(*) FROM " + t), kRows);
    mplive::ExpectEq("itemB3 every id exactly once",
                     ScalarInt(my, L"SELECT COUNT(DISTINCT id) FROM " + t), kRows);
    mplive::ExpectEq("itemB3 id checksum",
                     ScalarInt(my, L"SELECT COALESCE(SUM(id),0) FROM " + t),
                     (long long)kRows * (kRows + 1) / 2);

    // ---- boolean → TINYINT(1), the reverse coercion -------------------------
    const long long ones = ScalarInt(my, L"SELECT COUNT(*) FROM " + t + L" WHERE flag=1");
    const long long zeros = ScalarInt(my, L"SELECT COUNT(*) FROM " + t + L" WHERE flag=0");
    std::printf("  OBS  B3 flag: 1=%lld 0=%lld\n", ones, zeros);
    mplive::ExpectEq("itemB3 PG true wrote MySQL 1",  ones,  kRows / 2);
    mplive::ExpectEq("itemB3 PG false wrote MySQL 0", zeros, kRows - kRows / 2);
    mplive::ExpectEq("itemB3 no flag landed as anything other than 0/1",
                     ScalarInt(my, L"SELECT COUNT(*) FROM " + t +
                                   L" WHERE flag NOT IN (0,1)"), 0);

    // ---- the is_null branch -------------------------------------------------
    mplive::ExpectEq("itemB3 NULL notes stayed NULL",
                     ScalarInt(my, L"SELECT COUNT(*) FROM " + t + L" WHERE note IS NULL"),
                     kRows / 3);
    mplive::ExpectEq("itemB3 empty-string notes stayed empty, not NULL",
                     ScalarInt(my, L"SELECT COUNT(*) FROM " + t + L" WHERE note=''"),
                     (kRows + 2) / 3);
    mplive::ExpectEq("itemB3 NULL payloads stayed NULL",
                     ScalarInt(my, L"SELECT COUNT(*) FROM " + t + L" WHERE payload IS NULL"),
                     kRows / 7);
    ExpectStr("itemB3 NULL vs empty string distinguishable at a MySQL target",
              Dump(my, L"SELECT id, CASE WHEN note IS NULL THEN '#NULL#' "
                       L"WHEN note='' THEN '#EMPTY#' ELSE note END FROM " + t +
                       L" WHERE id IN (1,2,3) ORDER BY id"),
              L"1|#EMPTY#;2|n2;3|#NULL#");

    // ---- the MYSQL_TYPE_BLOB branch, byte for byte --------------------------
    ExpectStr("itemB3 bytea round-tripped into VARBINARY byte for byte",
              Dump(my, L"SELECT id, HEX(payload) FROM " + t +
                       L" WHERE id IN (1,2,255,256) ORDER BY id"),
              L"1|AA00BB01;2|AA00BB02;255|AA00BBFF;256|AA00BB00");
    mplive::ExpectEq("itemB3 no payload was truncated at the 0x00 byte",
                     ScalarInt(my, L"SELECT COUNT(*) FROM " + t +
                                   L" WHERE payload IS NOT NULL AND LENGTH(payload)<>4"), 0);
    // Rows either side of the batch boundary.
    ExpectStr("itemB3 rows across the 500-row batch boundary",
              Dump(my, L"SELECT id FROM " + t +
                       L" WHERE id IN (499,500,501,502) ORDER BY id"),
              L"499;500;501;502");

    const CompareOutcome again = Compare1(pg, my, spec);
    std::printf("  OBS  B3 re-compare: ins=%lld upd=%lld del=%lld\n",
                again.stat.inserts, again.stat.updates, again.stat.deletes);
    mplive::ExpectEq("itemB3 re-compare finds no differences",
                     again.stat.inserts + again.stat.updates + again.stat.deletes, 0);
}

// ---------------------------------------------------------------------------
// C3 — the delete sweep commits, then the insert/update sweep fails
// ---------------------------------------------------------------------------
//
// This is not a hypothetical. ExecuteDataSync deliberately runs DELETEs for
// every table first (reverse FK order) and INSERT/UPDATEs second (forward FK
// order), each table-sweep in its OWN transaction. Both halves of that design
// are defensible on their own — FK correctness demands the two directions, and
// per-table transactions exist so a 10M-row sync does not exhaust WAL/undo —
// but together they mean a table's deletes can be durably committed while the
// writes that were supposed to replace them roll back.
//
// The result is rows GONE from the customer's target with nothing put back, and
// a report that says committed=false. That report is honest, and the counters
// do describe it precisely, but the outcome is real data loss on a run the user
// will read as "it failed". It is pinned here — with the target read back — so
// that (a) it is on the record as observed rather than argued, and (b) any
// future change to the sweep/transaction structure has to confront it.
void ItemC3_DeleteSweepWindow(IConnection& my, IConnection& pg,
                              const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item C3: deletes commit, then the write sweep fails ==\n");
    const wxString q = QuoteIdent(myDb, Dialect::MySQL);

    const bool setup =
        mplive::Exec(my, L"DROP TABLE IF EXISTS " + q + L".t_loss", "drop loss") &&
        mplive::Exec(my, L"CREATE TABLE " + q + L".t_loss "
                     L"(id INT NOT NULL PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB",
                     "create loss") &&
        // id=2 will be UPDATEd to 500, which the target refuses.
        mplive::Exec(my, L"INSERT INTO " + q + L".t_loss (id,v) VALUES (1,1),(2,500)",
                     "seed loss") &&
        mplive::Exec(pg, L"DROP TABLE IF EXISTS t_loss", "drop pg loss") &&
        mplive::Exec(pg, L"CREATE TABLE t_loss (id integer PRIMARY KEY, "
                         L"v integer NOT NULL CHECK (v < 100))", "create pg loss") &&
        // 90 and 91 are extras the delete sweep will remove and commit.
        mplive::Exec(pg, L"INSERT INTO t_loss (id,v) VALUES (1,1),(2,2),(90,5),(91,5)",
                     "seed pg loss");
    if (!setup) { mplive::ExpectTrue("itemC3 fixture setup", false); return; }

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_loss", spec)) {
        mplive::ExpectTrue("itemC3 spec built", false);
        return;
    }
    ExpectStr("itemC3 target before the run",
              Dump(pg, L"SELECT id, v FROM t_loss ORDER BY id"),
              L"1|1;2|2;90|5;91|5");

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_loss", Allow{true, true, true}, true)});
    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "C3");
    std::printf("  OBS  C3 error: [%s]\n", (const char*)x.err.utf8_str());
    mplive::ExpectTrue("itemC3 run fails", !x.ok);

    const TableExecReport* rep = ReportOf(x.result, L"t_loss");
    mplive::ExpectTrue("itemC3 table reported", rep != nullptr);
    if (rep) {
        // The honest partial-apply signature the header describes: NOT
        // committed, yet a non-zero delete count.
        mplive::ExpectTrue("itemC3 table reports committed=false", !rep->committed);
        mplive::ExpectEq("itemC3 report shows the deletes that DID commit",
                         rep->deletes, 2);
        mplive::ExpectEq("itemC3 report shows no update was applied", rep->updates, 0);
        mplive::ExpectTrue("itemC3 report carries the failure reason",
                           !rep->error.IsEmpty());
    }

    // THE OBSERVED OUTCOME. The extras are gone — durably, in their own
    // committed transaction — while the update that failed rolled back. The
    // target is in neither its original state nor the intended one.
    const wxString got = Dump(pg, L"SELECT id, v FROM t_loss ORDER BY id");
    std::printf("  OBS  C3 target after the failed run: [%s]\n",
                (const char*)got.utf8_str());
    ExpectStr("itemC3 TARGET: deletes are durable even though the run failed",
              got, L"1|1;2|2");
    mplive::ExpectEq("itemC3 the deleted rows are really gone",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_loss WHERE id IN (90,91)"), 0);
    mplive::ExpectEq("itemC3 the refused update did NOT partially land",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_loss WHERE id=2 AND v=2"), 1);

    // And the state is REACHABLE again: re-running after the user fixes the
    // source must converge rather than compound the damage.
    mplive::Exec(my, L"UPDATE " + q + L".t_loss SET v=50 WHERE id=2", "fix source");
    const ExecOutcome x2 = RunExec(my, pg, jobs);
    ShowResult(x2.result, "C3-retry");
    mplive::ExpectTrue("itemC3 retry after fixing the source succeeds", x2.ok);
    ExpectStr("itemC3 TARGET converges on retry",
              Dump(pg, L"SELECT id, v FROM t_loss ORDER BY id"), L"1|1;2|50");
}

// ---------------------------------------------------------------------------
// D3 — the ORDINARY case: a target that already holds rows
// ---------------------------------------------------------------------------
//
// WHY THIS WAS MISSING FOR THREE ROUNDS. Every earlier fixture in this suite
// either started with an EMPTY target (so the diff is pure INSERT and the merge
// never opens a read on the target at all) or with a tiny one (3 rows, which the
// reader drains before the merge has anything to write). Both dodge the single
// most common thing a user does: sync a table the target already has rows in.
//
// WHAT IT PINS. The execute pass reads the target through DataSync.cpp's
// RowCursor, which drains the result set on a BACKGROUND thread while this
// thread issues the writes. While both used one IConnection, the first UPDATE or
// DELETE emitted before the target stream reached end-of-data was sent on a
// connection with a query still in flight, from the wrong thread:
//   MySQL      -> "Commands out of sync; you can't run this command now"
//   PostgreSQL -> PQexec returns NULL: an EMPTY error string and a target
//                 connection left permanently DEAD; on other runs, a segfault.
// The fix gives the target read its own connection (db::CloneConnection).
//
// This is deliberately NOT behind the scale gate. It is a CORRECTNESS test, not
// a performance one, and the defect it covers needs no scale — 2000 rows is far
// more than enough, and the failure was also observed at 200. Putting it here
// means an ordinary `ctest` run catches a regression instead of a scale run
// nobody launches by default.
//
// THE DIFF IS MIXED AND STARTS AT THE FIRST KEY, on purpose. Every one of the
// 2000 shared rows differs, so the very first row the merge classifies is a
// write and there is no chance of winning the race by accident. Inserts and
// deletes are included as well, so the DELETE sweep — which reads the target and
// deletes from it, and had the identical exposure — is covered too.
namespace {

constexpr const wchar_t* kOrdTable = L"t_ord";
constexpr long long kOrdShared  = 2000;   // rows BOTH sides have (all differing)
constexpr long long kOrdInserts = 100;    // ids 2001..2100, source only
constexpr long long kOrdDeletes = 50;     // ids 3001..3050, target only

// Fill ids [from,to] with v = id + vOffset. Batched 500 to the statement so a
// 2000-row fixture costs four round trips, not 2000.
bool SeedOrd(IConnection& c, const wxString& qualified, long long from,
             long long to, long long vOffset)
{
    for (long long base = from; base <= to; base += 500) {
        wxString values;
        for (long long id = base; id < base + 500 && id <= to; ++id) {
            if (!values.IsEmpty()) values += L",";
            values += wxString::Format(L"(%lld,'n%lld',%lld)", id, id, id + vOffset);
        }
        if (!mplive::Exec(c, L"INSERT INTO " + qualified +
                             L" (id,name,v) VALUES " + values, "seed ord"))
            return false;
    }
    return true;
}

// One direction of the test. `mysqlTarget` false = MySQL -> PostgreSQL.
void OrdinaryDirection(IConnection& my, IConnection& pg, const wxString& myDb,
                       const wxString& pgDb, bool mysqlTarget)
{
    const char* label = mysqlTarget ? "D3/mysql-target" : "D3/pg-target";
    std::printf("\n-- %s: %lld pre-existing target rows, ALL of them differing, "
                "plus %lld inserts and %lld deletes --\n",
                label, kOrdShared, kOrdInserts, kOrdDeletes);

    const wxString myT = QuoteIdent(myDb, Dialect::MySQL) + L"." + kOrdTable;
    const wxString pgT = kOrdTable;

    const bool built =
        mplive::Exec(my, L"DROP TABLE IF EXISTS " + myT, "drop my ord") &&
        mplive::Exec(my, L"CREATE TABLE " + myT + L" ("
                     L"  id INT NOT NULL PRIMARY KEY,"
                     L"  name VARCHAR(40) NOT NULL,"
                     L"  v BIGINT NOT NULL) ENGINE=InnoDB", "create my ord") &&
        mplive::Exec(pg, wxString(L"DROP TABLE IF EXISTS ") + pgT, "drop pg ord") &&
        mplive::Exec(pg, wxString(L"CREATE TABLE ") + pgT + L" ("
                     L"  id integer NOT NULL PRIMARY KEY,"
                     L"  name varchar(40) NOT NULL,"
                     L"  v bigint NOT NULL)", "create pg ord");
    if (!built) { mplive::ExpectTrue("itemD3 fixture created", false); return; }

    IConnection&   src   = mysqlTarget ? pg    : my;
    IConnection&   tgt   = mysqlTarget ? my    : pg;
    const wxString srcDb = mysqlTarget ? pgDb  : myDb;
    const wxString tgtDb = mysqlTarget ? myDb  : pgDb;
    const wxString srcT  = mysqlTarget ? pgT   : myT;
    const wxString tgtT  = mysqlTarget ? myT   : pgT;

    // SOURCE: the desired state — ids 1..2100, v = id.
    // TARGET: ids 1..2000 with v = id + 7 (so EVERY shared row is an UPDATE, the
    // first at id=1), plus 50 extras the delete sweep must remove.
    const bool seeded =
        SeedOrd(src, srcT, 1, kOrdShared + kOrdInserts, 0) &&
        SeedOrd(tgt, tgtT, 1, kOrdShared, 7) &&
        SeedOrd(tgt, tgtT, 3001, 3000 + kOrdDeletes, 0);
    if (!seeded) { mplive::ExpectTrue("itemD3 fixture seeded", false); return; }

    DataDiffSpec spec;
    if (!MakeSpec(src, tgt, srcDb, tgtDb, kOrdTable, spec)) {
        mplive::ExpectTrue("itemD3 spec built", false);
        return;
    }

    // What the user would have been shown before pressing 执行.
    const CompareOutcome cmp = Compare1(src, tgt, spec);
    std::printf("  OBS  %s compare: ins=%lld upd=%lld del=%lld executable=%d\n",
                label, cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes,
                (int)cmp.executable);
    mplive::ExpectEq("itemD3 compare counts the updates", cmp.stat.updates, kOrdShared);
    mplive::ExpectEq("itemD3 compare counts the inserts", cmp.stat.inserts, kOrdInserts);
    mplive::ExpectEq("itemD3 compare counts the deletes", cmp.stat.deletes, kOrdDeletes);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec,
        MakeAllow(kOrdTable, Allow{true, true, true}, true)});

    const ExecOutcome x = RunExec(src, tgt, jobs);
    ShowResult(x.result, label);
    if (!x.ok) std::printf("  ERR  %s: [%s]\n", label, (const char*)x.err.utf8_str());

    // THE ASSERTION THE DEFECT FAILED. Before the fix this run died with
    // "Commands out of sync" (MySQL target) or an empty error and a dead
    // connection (PostgreSQL target).
    mplive::ExpectTrue("itemD3 syncing into a populated target SUCCEEDS", x.ok);

    // BY THE SPEC'S OWN KEY, never the bare table name — a PostgreSQL source is
    // schema-qualified ("public.t_ord") and a MySQL source is not. Looking this
    // up as kOrdTable silently missed every PG-source run and read 0 for a sweep
    // that had applied its rows (the false negative phase D documents).
    const TableExecReport* rep = ReportOf(x.result, spec.SourceTable().Key());
    mplive::ExpectTrue("itemD3 table reported", rep != nullptr);
    if (rep) {
        mplive::ExpectEq("itemD3 every differing row was updated",
                         rep->updates, kOrdShared);
        mplive::ExpectEq("itemD3 every new row was inserted", rep->inserts, kOrdInserts);
        mplive::ExpectEq("itemD3 every extra row was deleted", rep->deletes, kOrdDeletes);
        mplive::ExpectTrue("itemD3 the table committed", rep->committed);
    }

    // ---- READ THE TARGET BACK. The suite's standing rule ---------------------
    mplive::ExpectEq("itemD3 TARGET row count",
                     ScalarInt(tgt, L"SELECT COUNT(*) FROM " + tgtT),
                     kOrdShared + kOrdInserts);
    mplive::ExpectEq("itemD3 TARGET no row kept its stale value",
                     ScalarInt(tgt, L"SELECT COUNT(*) FROM " + tgtT + L" WHERE v <> id"), 0);
    mplive::ExpectEq("itemD3 TARGET the extras are gone",
                     ScalarInt(tgt, L"SELECT COUNT(*) FROM " + tgtT +
                                    L" WHERE id > 3000"), 0);
    mplive::ExpectEq("itemD3 TARGET the new rows landed",
                     ScalarInt(tgt, L"SELECT COUNT(*) FROM " + tgtT +
                                    L" WHERE id > 2000"), kOrdInserts);
    mplive::ExpectEq("itemD3 TARGET id checksum",
                     ScalarInt(tgt, L"SELECT COALESCE(SUM(id),0) FROM " + tgtT),
                     (kOrdShared + kOrdInserts) * (kOrdShared + kOrdInserts + 1) / 2);
    // CONCAT is spelled the same on both engines here (PostgreSQL has had the
    // variadic form since 9.1), so one query serves both directions.
    mplive::ExpectEq("itemD3 TARGET no name was mangled",
                     ScalarInt(tgt, L"SELECT COUNT(*) FROM " + tgtT +
                                    L" WHERE name <> CONCAT('n', id)"), 0);

    // THE TRANSACTION-VISIBILITY CHECK. The target read runs on its own
    // connection and therefore sees COMMITTED state only — never this sweep's
    // own in-flight writes. If that changed the diff in any way, a re-compare
    // would still find work to do. It must find none.
    const CompareOutcome again = Compare1(src, tgt, spec);
    std::printf("  OBS  %s re-compare: ran=%d ins=%lld upd=%lld del=%lld\n", label,
                (int)again.ran, again.stat.inserts, again.stat.updates,
                again.stat.deletes);
    // `ran` FIRST, and not as decoration. Verified against the unfixed build:
    // when the collision left the target connection dead, this compare failed
    // outright and reported 0/0/0 — which is indistinguishable from "converged"
    // if only the counts are asserted, and the check passed vacuously in exactly
    // the run it was supposed to catch. A zero that came from a broken query is
    // not evidence of convergence.
    mplive::ExpectTrue("itemD3 re-compare actually ran", again.ran);
    mplive::ExpectEq("itemD3 re-compare finds the two sides converged",
                     again.stat.inserts + again.stat.updates + again.stat.deletes, 0);

    // The target connection must still be usable. When this failed it did not
    // just fail one sync: in the application this is the ConnectionTree's own
    // long-lived entry, so poisoning it takes the user's session down with it.
    QueryResult qr; wxString e;
    const bool alive = tgt.Execute(L"SELECT 1", qr, e);
    if (!alive)
        std::printf("  OBS  %s target connection is DEAD; error text=[%s]\n",
                    label, (const char*)e.utf8_str());
    mplive::ExpectTrue("itemD3 the target connection survives the sync", alive);
}

} // namespace

void ItemD3_PopulatedTarget(IConnection& my, IConnection& pg,
                            const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item D3: the ORDINARY case — a target that already holds "
                "rows, both directions ==\n");
    // Both directions, because the collision was a property of every client
    // library here (each keeps a connection busy until its result set is
    // consumed) and not of one driver.
    OrdinaryDirection(my, pg, myDb, pgDb, false);   // MySQL -> PostgreSQL
    OrdinaryDirection(my, pg, myDb, pgDb, true);    // PostgreSQL -> MySQL
}

} // namespace mpexec
