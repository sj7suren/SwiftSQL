// mysql_pg_live_mysqltx.cpp — TRANSACTION INTEGRITY AGAINST A **MySQL** TARGET.
//
// ---------------------------------------------------------------------------
// WHY THIS SUITE EXISTS
// ---------------------------------------------------------------------------
// mysql_pg_live_exec4_test proved cancellation and connection loss against a
// **PostgreSQL** target: a cancelled table rolled back to zero rows while
// already-committed tables stayed committed, and pg_terminate_backend mid-batch
// produced a detected failure with the target left empty. Every one of those
// observations rests on a property PostgreSQL has and MySQL DOES NOT:
//
//     PostgreSQL rolls back DDL. MySQL commits it implicitly, mid-transaction.
//
// So MySQL is the engine where "the transaction was rolled back" and "the
// target is back the way it was" are two DIFFERENT claims, and the whole
// per-table transaction story has to be re-observed rather than assumed to
// carry over. Six items; the first four live here, M5/M6 in the second TU.
//
//   M1  Cancellation mid-write. Same shape as exec4 item 2 but on InnoDB, read
//       back through a SEPARATE connection so the answer cannot come from the
//       worker's own uncommitted snapshot.
//   M2  Connection loss. MySQL's equivalent of pg_terminate_backend is
//       `KILL <id>` from a second connection. The id is read as
//       `SELECT CONNECTION_ID()` ON THE VICTIM ITSELF — never by scanning
//       information_schema.PROCESSLIST for something that looks right, which on
//       a SHARED server is how a test kills a colleague's session.
//   M3  THE MySQL-SPECIFIC QUESTION: does the implicit DDL commit defeat the
//       per-table rollback? A table whose CREATE committed but whose INSERTs
//       rolled back is a HALF-STATE that cannot exist on PostgreSQL. It is
//       observed here directly, and the assertion is on whether the product
//       REPORTS it honestly, not on whether it avoids it — it cannot avoid it,
//       the server decides.
//   M4  SyncEngine::Execute's phase-1 COMMIT-failure path, which returned
//       WITHOUT issuing a ROLLBACK. Staged deterministically by killing the
//       target connection from the progress callback after the LAST DDL
//       statement, so the very next thing the engine does is the COMMIT.
//   M5  What the engines ACTUALLY do on ROLLBACK after a FAILED COMMIT
//       (mysql_pg_live_mysqltx2.cpp) — the prerequisite for M4/M6's error
//       handling, since a rollback that errors and one that is a harmless no-op
//       call for different handling.
//   M6  Whether the COMMIT-failure path leaves an open transaction behind
//       (mysql_pg_live_mysqltx2.cpp). PG-targeted, because a dead connection —
//       M4's route — cannot answer that question at all.
//
// ---------------------------------------------------------------------------
// THE RULE, INHERITED UNCHANGED
// ---------------------------------------------------------------------------
// An assertion READS THE TARGET BACK, and for anything involving a transaction
// it reads it back FROM A DIFFERENT CONNECTION. A backend asked about its own
// open transaction answers from inside it and will happily report uncommitted
// rows as present; that is not evidence of a commit, it is evidence of nothing.
//
// SAFETY: no credential is hard-coded; everything comes from SWIFTSQL_MYTEST_*
// (and SWIFTSQL_PGTEST_* for M5b/M6, which SKIP independently), and the suite
// exits 0 when unset. Its throwaway databases carry their own `swiftsql_xmtx_`
// prefix so they can never collide with another live suite's, and are dropped on
// the way IN as well as OUT.
#include "mysql_pg_live_mysqltx.h"

#include <algorithm>
#include <cstdio>

namespace mtx {

using mpexec::Dump;
using mpexec::ExpectStr;
using mpexec::ReportOf;
using mpexec::ShowResult;

CancelOutcome RunCancellable(IConnection& src, IConnection& tgt,
                             const std::vector<TableExecJob>& jobs,
                             const StopWhen& hook)
{
    CancelOutcome     o;
    std::atomic<bool> stop{false};
    DataSyncOptions   base;
    o.ok = ExecuteDataSync(src, tgt, jobs, base, o.result, o.err, stop,
        [&](const wxString& table, const wxString& phase, long long applied) {
            ++o.progressCalls;
            if (!o.tripped && hook && hook(table, phase, applied)) {
                o.tripped   = true;
                o.trippedAt = table + L"/" + phase + wxString::Format(L"/%lld", applied);
                stop.store(true);
            }
        });
    return o;
}

namespace {

constexpr long long kBulkRows    = 3000;    // six 500-row batches
constexpr long long kTinyRows    = 5;
constexpr long long kCancelAfter = 1000;    // two full batches in
constexpr long long kKillAfter   = 1000;

// Does table `t` exist in database `db`, asked from an arbitrary connection?
// M3 needs this from a WITNESS: whether a DDL-created table survived a rollback
// is precisely what the worker's own session cannot be trusted to answer.
bool TableExists(IConnection& c, const wxString& db, const wxString& t)
{
    return Scalar(c, L"SELECT COUNT(*) FROM information_schema.TABLES "
                     L"WHERE TABLE_SCHEMA='" + db + L"' AND TABLE_NAME='" + t + L"'")
           == L"1";
}

bool Setup(IConnection& src, IConnection& tgt)
{
    // Raised on BOTH sides: the source needs it to seed, and the target needs it
    // for M3's recovery fixture.
    for (IConnection* c : { &src, &tgt })
        Run(*c, L"SET SESSION cte_max_recursion_depth = 100000", "raise cte depth");

    for (const wxChar* t : { L"tiny", L"bulk" }) {
        if (!Run(src, wxString(L"DROP TABLE IF EXISTS `") + t + L"`", "src drop") ||
            !Run(tgt, wxString(L"DROP TABLE IF EXISTS `") + t + L"`", "tgt drop") ||
            !Run(src, wxString(L"CREATE TABLE `") + t + L"`" + kDdl, "src create") ||
            !Run(tgt, wxString(L"CREATE TABLE `") + t + L"`" + kDdl, "tgt create"))
            return false;
    }
    return SeedSeries(src, L"tiny", kTinyRows, L"tiny", 0, "src seed tiny") &&
           SeedSeries(src, L"bulk", kBulkRows, L"bulk", 0, "src seed bulk");
}

} // namespace

// ===========================================================================
// M1 — cancellation mid-write, MySQL target
// ===========================================================================
void M1_CancelMidWrite(Ctx& c)
{
    std::printf("\n== M1: cancel mid-write against a MySQL target ==\n");

    if (!Run(*c.tgt, L"TRUNCATE `tiny`", "reset tiny") ||
        !Run(*c.tgt, L"TRUNCATE `bulk`", "reset bulk")) { ++g_fails; return; }

    mplive::ExpectEq("M1 source bulk really has 3000 rows",
                     ScalarInt(*c.src, L"SELECT COUNT(*) FROM `bulk`"), kBulkRows);
    mplive::ExpectEq("M1 target starts empty",
                     ScalarInt(*c.tgt, L"SELECT (SELECT COUNT(*) FROM `tiny`) "
                                       L"+ (SELECT COUNT(*) FROM `bulk`)"), 0);

    bool ok = false;
    std::vector<TableExecJob> jobs =
        InsertJobs(*c.src, *c.tgt, c.srcDb, c.tgtDb, { L"tiny", L"bulk" }, ok);
    if (!ok) { ++g_fails; return; }

    // `liveMax` records the highest count the executor reported for `bulk` WHILE
    // IT RAN. Promptness cannot be read off the final report: a rolled-back
    // sweep correctly rewinds its counters to zero, so asserting on
    // TableExecReport::inserts would be asserting 0 < 3000 — true even for a
    // cancel that never worked.
    long long liveMax = 0;
    CancelOutcome o = RunCancellable(*c.src, *c.tgt, jobs,
        [&](const wxString& table, const wxString&, long long applied) {
            if (table == L"bulk") liveMax = std::max(liveMax, applied);
            return table == L"bulk" && applied >= kCancelAfter;
        });
    ShowResult(o.result, "M1 cancelled run");
    mplive::Observe("M1 cancel tripped at", o.trippedAt);
    mplive::Observe("M1 returned err", o.err);

    mplive::ExpectTrue("M1 the cancel hook actually fired", o.tripped);
    mplive::ExpectTrue("M1 a cancelled run returns false", !o.ok);
    mplive::ExpectTrue("M1 the failure names cancellation, not a server error",
                       o.err.Contains(L"取消"));

    const TableExecReport* bulk = ReportOf(o.result, L"bulk");
    const TableExecReport* tiny = ReportOf(o.result, L"tiny");
    mplive::ExpectTrue("M1 both tables appear in the report", bulk && tiny);
    if (!bulk || !tiny) return;

    mplive::Observe("M1 bulk rows sent before the cancel took effect (live)",
                    wxString::Format(L"%lld", liveMax));
    mplive::ExpectTrue("M1 the cancel stopped the table well short of 3000",
                       liveMax > 0 && liveMax < kBulkRows);
    mplive::ExpectTrue("M1 the cancel was prompt (<= one batch past the trip)",
                       liveMax <= kCancelAfter + 500);

    // THE COUNTER-HONESTY ASSERTION, re-run on InnoDB. TableApplier increments
    // as each batch is ACCEPTED, which inside an open transaction is not the
    // same as applied; RunTableSweep rewinds to the sweep's starting values on
    // rollback. Proven on PostgreSQL; InnoDB's undo log is a different mechanism
    // reaching the same place, so it is proven here too rather than assumed.
    mplive::ExpectEq("M1 the rolled-back sweep reports zero inserts", bulk->inserts, 0);
    mplive::ExpectTrue("M1 tiny reports committed", tiny->committed);
    mplive::ExpectTrue("M1 bulk reports NOT committed", !bulk->committed);
    mplive::ExpectTrue("M1 AllCommitted() is false", !o.result.AllCommitted());
    mplive::ExpectEq("M1 exactly one table is reported committed",
                     (long long)o.result.Committed().size(), 1);

    // THE DATABASE'S OWN ANSWER, THROUGH A SEPARATE CONNECTION.
    //
    // This is not belt-and-braces. InnoDB's default isolation is REPEATABLE
    // READ, and the worker connection can see its own uncommitted rows; asking
    // IT how many rows `bulk` holds would return whatever that session wrote,
    // committed or not. Only a witness that never joined the transaction can
    // distinguish "rolled back" from "still open and holding 1000 rows".
    wxString we;
    std::unique_ptr<IConnection> witness = c.Dial(c.tgtDb, we);
    if (!witness) {
        std::printf("  ERR  M1 witness connection failed: %s\n",
                    (const char*)we.utf8_str());
        ++g_fails;
        return;
    }
    const long long tinyRows = ScalarInt(*witness, L"SELECT COUNT(*) FROM `tiny`");
    const long long bulkRows = ScalarInt(*witness, L"SELECT COUNT(*) FROM `bulk`");
    mplive::Observe("M1 target row counts after cancel (SEPARATE connection)",
                    wxString::Format(L"tiny=%lld bulk=%lld", tinyRows, bulkRows));
    mplive::ExpectEq("M1 the cancelled table rolled back to zero rows", bulkRows, 0);
    mplive::ExpectEq("M1 the already-committed table stayed committed",
                     tinyRows, kTinyRows);

    // Was the transaction really CLOSED, or merely un-committed? An open
    // transaction on the worker holds InnoDB locks and an undo view for the life
    // of the connection — on the automation path, the life of the app. Asked
    // from the witness, because a session interrogating itself is by definition
    // executing the query it asks with and can never observe its own idleness.
    {
        const long long id = OwnConnectionId(*c.tgt);
        const wxString  st = Scalar(*witness,
            L"SELECT COUNT(*) FROM information_schema.INNODB_TRX "
            L"WHERE trx_mysql_thread_id = " + wxString::Format(L"%lld", id));
        mplive::Observe("M1 open InnoDB transactions on the worker, seen from outside", st);
        ExpectStr("M1 the cancelled table's transaction was really closed", st, L"0");
    }

    // CONVERGENCE. Same jobs, same connections, no cleanup in between — exactly
    // the state a user is in when they press 同步 again.
    std::printf("  -- M1 retry --\n");
    mpexec::ExecOutcome r = mpexec::RunExec(*c.src, *c.tgt, jobs);
    ShowResult(r.result, "M1 retry");
    mplive::ExpectTrue("M1 the retry succeeds", r.ok);
    if (!r.ok) std::printf("  ERR  M1 retry: %s\n", (const char*)r.err.utf8_str());

    const long long tiny2 = ScalarInt(*witness, L"SELECT COUNT(*) FROM `tiny`");
    const long long bulk2 = ScalarInt(*witness, L"SELECT COUNT(*) FROM `bulk`");
    mplive::Observe("M1 target row counts after retry",
                    wxString::Format(L"tiny=%lld bulk=%lld", tiny2, bulk2));
    mplive::ExpectEq("M1 retry converged bulk to 3000", bulk2, kBulkRows);
    // NOT 10: the retry re-diffs, so the rows `tiny` already holds must be seen
    // as unchanged rather than re-inserted. A duplicate here is the "wrote twice"
    // failure the suite's standing rule exists to catch.
    mplive::ExpectEq("M1 retry did not duplicate the committed table", tiny2, kTinyRows);

    ExpectStr("M1 retry wrote the right values at the batch boundaries",
              Dump(*witness, L"SELECT `id`,`v`,`note` FROM `bulk` "
                             L"WHERE `id` IN (1,500,501,1000,1001,3000) ORDER BY `id`"),
              L"1|10|bulk-1;500|5000|bulk-500;501|5010|bulk-501;"
              L"1000|10000|bulk-1000;1001|10010|bulk-1001;3000|30000|bulk-3000");

    witness->Disconnect();
}

// ===========================================================================
// M2 — losing the target connection mid-batch (KILL)
// ===========================================================================
//
// STAGED DETERMINISTICALLY, with no sleep anywhere:
//   * the KILL fires from the executor's OWN progress callback, so it lands
//     after a known number of applied rows rather than at a wall-clock moment
//     racing the batch loop;
//   * `KILL <id>` against a specific connection is total — it is not a dropped
//     packet that might be retried, it is the session ending, and the next
//     protocol exchange on that socket cannot succeed;
//   * the id came from the victim itself, so nothing else on this shared server
//     can be hit.
// ===========================================================================
void M2_ConnectionLoss(Ctx& c)
{
    std::printf("\n== M2: the MySQL target connection is KILLed mid-batch ==\n");

    if (!Run(*c.tgt, L"TRUNCATE `bulk`", "M2 reset bulk")) { ++g_fails; return; }

    const long long id = OwnConnectionId(*c.tgt);
    mplive::Observe("M2 target CONNECTION_ID()", wxString::Format(L"%lld", id));
    if (id <= 0) {
        std::printf("  SKIP M2: could not read the target connection id\n");
        return;
    }

    wxString e1, e2;
    std::unique_ptr<IConnection> killer  = c.Dial(c.tgtDb, e1);
    std::unique_ptr<IConnection> witness = c.Dial(c.tgtDb, e2);
    if (!killer || !witness) {
        std::printf("  SKIP M2: killer/witness connect failed: %s / %s\n",
                    (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
        return;
    }

    bool ok = false;
    std::vector<TableExecJob> jobs =
        InsertJobs(*c.src, *c.tgt, c.srcDb, c.tgtDb, { L"bulk" }, ok);
    if (!ok) { ++g_fails; return; }

    bool     fired = false;
    wxString killErr;
    CancelOutcome o = RunCancellable(*c.src, *c.tgt, jobs,
        [&](const wxString& table, const wxString&, long long applied) {
            if (!fired && table == L"bulk" && applied >= kKillAfter) {
                fired = true;
                QueryResult r;
                if (!killer->Execute(wxString::Format(L"KILL %lld", id), r, killErr))
                    killErr = L"KILL failed: " + killErr;
            }
            return false;   // NOT a cancel — the stop token is never tripped
        });

    mplive::ExpectTrue("M2 the KILL actually fired", fired);
    mplive::Observe("M2 KILL reply", killErr.IsEmpty() ? wxString(L"<ok>") : killErr);
    ShowResult(o.result, "M2");
    mplive::Observe("M2 returned err", o.err);

    // THE QUESTION THAT MATTERS MOST. A write that lost its connection halfway
    // and returned true would tell a user their data is synchronized when
    // two-thirds of it is missing.
    mplive::ExpectTrue("M2 a killed connection does NOT report success", !o.ok);
    mplive::ExpectTrue("M2 the failure carries an error message", !o.err.IsEmpty());
    mplive::ExpectTrue("M2 the failure is NOT misreported as a cancellation",
                       !o.err.Contains(L"取消"));
    mplive::ExpectTrue("M2 AllCommitted() is false", !o.result.AllCommitted());

    const TableExecReport* rep = ReportOf(o.result, L"bulk");
    mplive::ExpectTrue("M2 the table appears in the report", rep != nullptr);
    if (rep) {
        mplive::ExpectTrue("M2 the table reports NOT committed", !rep->committed);
        mplive::ExpectTrue("M2 the table reports an error", !rep->error.IsEmpty());
        mplive::Observe("M2 per-table error", rep->error);
        mplive::Observe("M2 inserts counter at death",
                        wxString::Format(L"%lld", rep->inserts));
        // The ROLLBACK statement itself cannot succeed on a dead connection, so
        // a rewind that only covered the clean rollback path would be wrong
        // here. InnoDB rolls a killed session's transaction back regardless, so
        // the rows are gone and the counter must say so.
        mplive::ExpectEq("M2 inserts does not count rows the dead transaction "
                         "discarded", rep->inserts, 0);
    }

    // THE TARGET'S STATE, through the WITNESS — the victim cannot be asked, it
    // is the thing that died.
    const long long left = ScalarInt(*witness, L"SELECT COUNT(*) FROM `bulk`");
    mplive::Observe("M2 rows left in the target", wxString::Format(L"%lld", left));
    mplive::ExpectEq("M2 the interrupted table rolled back to zero rows", left, 0);

    // The driver must have NOTICED. MySqlDriver::DropIfLost closes the handle on
    // CR_SERVER_GONE_ERROR/CR_SERVER_LOST so IsConnected() reports false;
    // a driver that kept answering from a dead handle is how a later query
    // crashes the client library.
    mplive::ExpectTrue("M2 the driver knows its connection is gone",
                       !c.tgt->IsConnected());

    killer->Disconnect();
    witness->Disconnect();
}

// ===========================================================================
// M3 — does MySQL's implicit DDL commit defeat the per-table rollback?
// ===========================================================================
//
// THE HALF-STATE THAT CANNOT EXIST ON PostgreSQL. Phase 1 of
// SyncEngine::ExecuteRun wraps the preamble and every unit's DDL in
// BEGIN…COMMIT, and MySQL commits each DDL statement implicitly the moment it
// runs. So when a LATER statement in that same phase fails, the ROLLBACK the
// engine issues cannot undo the CREATE TABLE that already happened — the table
// stays. The DATA phase (separate, per-table transactions) then rolls back
// cleanly on its own.
//
// Result: a table that EXISTS with ZERO rows, after a run that reported
// failure. That is not a defect fixable at this layer — the server decides — so
// the assertions are on HONESTY (the error must say the DDL is committed, or
// the user re-runs against a target they believe is untouched) and on
// RECOVERABILITY (a retry must be able to fill it).
// ===========================================================================
void M3_ImplicitDdlCommit(Ctx& c)
{
    std::printf("\n== M3: implicit DDL commit vs. the phase-1 rollback ==\n");

    Run(*c.tgt, L"DROP TABLE IF EXISTS `ddlhalf`", "M3 pre-drop");

    wxString we;
    std::unique_ptr<IConnection> witness = c.Dial(c.tgtDb, we);
    if (!witness) {
        std::printf("  SKIP M3: witness connect failed: %s\n",
                    (const char*)we.utf8_str());
        return;
    }
    mplive::ExpectTrue("M3 the DDL table does not exist beforehand",
                       !TableExists(*witness, c.tgtDb, L"ddlhalf"));

    // A hand-built plan: the MySQL preamble/postamble pair BuildPlan would emit,
    // one unit whose DDL creates a table and then runs a statement the server
    // must reject. Hand-built rather than compared-for because the point is the
    // ORDER of those two statements inside one transaction, which no fixture
    // could reliably make BuildPlan produce.
    SyncPlan plan;
    plan.preamble.push_back(L"SET FOREIGN_KEY_CHECKS=0");
    plan.postamble.push_back(L"SET FOREIGN_KEY_CHECKS=1");
    SyncPlan::TableUnit unit;
    unit.table = QualifiedName(L"ddlhalf");
    unit.ddl.push_back(wxString(L"CREATE TABLE `ddlhalf`") + kDdl);
    unit.ddl.push_back(L"ALTER TABLE `swiftsql_xmtx_no_such_table` ADD COLUMN `x` INT");
    plan.units.push_back(std::move(unit));

    SyncEngine     eng(*c.src, *c.tgt, c.srcDb, c.tgtDb);
    DataExecResult res;
    wxString       err;
    const bool ok = eng.Execute(plan, DataExecPlan{}, /*useTransaction=*/true, res,
                                err, g_never, {});

    mplive::ExpectTrue("M3 the run reports failure", !ok);
    mplive::Observe("M3 reported error", err);

    // THE OBSERVATION. Asked from the witness so the answer is about the
    // SERVER's catalog, not about the worker session's view of it.
    const bool exists = TableExists(*witness, c.tgtDb, L"ddlhalf");
    mplive::Observe("M3 does the DDL-created table survive the rollback?",
                    exists ? L"YES — implicit commit defeated the rollback"
                           : L"no — the rollback removed it");
    // Not an aspiration: this is what InnoDB does, and pinning it means the day
    // a MySQL release makes DDL transactional this test reports the change
    // rather than silently passing on a different mechanism.
    mplive::ExpectTrue("M3 MySQL's implicit DDL commit DOES defeat the phase-1 "
                       "rollback (the table survives)", exists);

    // THE HONESTY ASSERTION — the only part of this that is the product's job.
    mplive::ExpectTrue("M3 the error warns that committed MySQL DDL cannot be "
                       "rolled back", err.Contains(L"无法回滚"));

    if (exists) {
        // THE HALF-STATE, WITH THE DATA PHASE INCLUDED. The table is now real
        // and empty. A second run — structure already done, data in scope — must
        // be able to fill it; that is what makes the half-state RECOVERABLE
        // rather than a dead end the user has to clean up by hand.
        const long long rows = ScalarInt(*witness, L"SELECT COUNT(*) FROM `ddlhalf`");
        mplive::Observe("M3 rows in the surviving table",
                        wxString::Format(L"%lld", rows));
        mplive::ExpectEq("M3 the surviving table is EMPTY — structure committed, "
                         "no data (the documented half-state)", rows, 0);

        if (Run(*c.src, L"DROP TABLE IF EXISTS `ddlhalf`", "M3 src drop") &&
            Run(*c.src, wxString(L"CREATE TABLE `ddlhalf`") + kDdl, "M3 src create") &&
            SeedSeries(*c.src, L"ddlhalf", 20, L"half", 0, "M3 src seed")) {
            bool jok = false;
            std::vector<TableExecJob> jobs =
                InsertJobs(*c.src, *c.tgt, c.srcDb, c.tgtDb, { L"ddlhalf" }, jok);
            if (jok) {
                mpexec::ExecOutcome r = mpexec::RunExec(*c.src, *c.tgt, jobs);
                ShowResult(r.result, "M3 recovery");
                mplive::ExpectTrue("M3 a retry can fill the half-created table", r.ok);
                if (!r.ok)
                    std::printf("  ERR  M3 recovery: %s\n", (const char*)r.err.utf8_str());
                mplive::ExpectEq("M3 the half-state converges on retry",
                                 ScalarInt(*witness, L"SELECT COUNT(*) FROM `ddlhalf`"), 20);
            } else ++g_fails;
        } else ++g_fails;
    }

    witness->Disconnect();
}

// ===========================================================================
// M4 — SyncEngine::Execute's phase-1 COMMIT-failure path, on MySQL
// ===========================================================================
//
// THE DEFECT. ExecuteRun's phase 1 has three failure exits. Two of them —
// cancellation and a rejected statement — issue a ROLLBACK before returning.
// The third, `if (!tgt_.Execute(L"COMMIT", …)) { err = e; return false; }`, did
// not, so it returned with the transaction still OPEN on the connection.
//
// STAGING IT ON MySQL. A COMMIT that fails while the connection is healthy is
// not something InnoDB can be talked into on demand. The realistic MySQL
// failure — and the one that matters for a long-lived connection — is the
// connection dying, so the KILL fires from the progress callback after the LAST
// DDL statement, which means the very next thing the engine does is the COMMIT.
// That reaches the exact path under test, deterministically.
//
// WHAT THIS ITEM CAN AND CANNOT PROVE. It proves the path is REACHED and that
// the reporting is honest on it. It CANNOT prove the transaction was closed —
// the connection is dead, so there is nothing left to hold one. M6 (PG,
// deferred constraint) is the item that answers that.
// ===========================================================================
void M4_CommitFailurePath(Ctx& c)
{
    std::printf("\n== M4: the phase-1 COMMIT-failure path (MySQL) ==\n");

    // A dedicated victim connection: this item kills it, and later items still
    // need a live c.tgt.
    wxString ve, ke;
    std::unique_ptr<IConnection> victim = c.Dial(c.tgtDb, ve);
    std::unique_ptr<IConnection> killer = c.Dial(c.tgtDb, ke);
    if (!victim || !killer) {
        std::printf("  SKIP M4: connect failed: %s / %s\n",
                    (const char*)ve.utf8_str(), (const char*)ke.utf8_str());
        return;
    }
    const long long id = OwnConnectionId(*victim);
    mplive::Observe("M4 victim CONNECTION_ID()", wxString::Format(L"%lld", id));
    if (id <= 0) { std::printf("  SKIP M4: no connection id\n"); return; }

    // Exactly ONE statement in phase 1 beyond the preamble, so the callback that
    // fires after it is immediately followed by the COMMIT. Harmless statements
    // on purpose: the subject is the COMMIT, not what preceded it.
    SyncPlan plan;
    plan.preamble.push_back(L"SET FOREIGN_KEY_CHECKS=0");
    plan.postamble.push_back(L"SET FOREIGN_KEY_CHECKS=1");
    SyncPlan::TableUnit unit;
    unit.table = QualifiedName(L"tiny");
    unit.ddl.push_back(L"SET @swiftsql_xmtx_probe = 1");
    plan.units.push_back(std::move(unit));

    const int total = 2;   // preamble + the one probe statement
    wxString  killErr;
    bool      fired = false;
    auto progress = [&](int done, int, const wxString&) {
        if (!fired && done == total) {
            fired = true;
            QueryResult r;
            if (!killer->Execute(wxString::Format(L"KILL %lld", id), r, killErr))
                killErr = L"KILL failed: " + killErr;
        }
    };

    SyncEngine     eng(*c.src, *victim, c.srcDb, c.tgtDb);
    DataExecResult res;
    wxString       err;
    const bool ok = eng.Execute(plan, DataExecPlan{}, /*useTransaction=*/true, res,
                                err, g_never, progress);

    mplive::ExpectTrue("M4 the KILL fired right before the COMMIT", fired);
    mplive::Observe("M4 KILL reply", killErr.IsEmpty() ? wxString(L"<ok>") : killErr);
    mplive::ExpectTrue("M4 a failed COMMIT reports failure", !ok);
    mplive::Observe("M4 reported error", err);
    mplive::ExpectTrue("M4 the failure carries an error message", !err.IsEmpty());

    // THE ORIGINAL ERROR STAYS FIRST AND INTACT. Everything else this path
    // appends — the rollback attempt's failure, the postamble's — is secondary;
    // the COMMIT's own error is what the operator has to act on, and a
    // secondary failure that REPLACED it would bury the actionable half.
    mplive::ExpectTrue("M4 the COMMIT's own error leads the report",
                       err.StartsWith(L"Lost connection"));

    // THE ROLLBACK NOTE MUST BE ABSENT HERE, and that is a deliberate outcome
    // rather than an oversight. The path now attempts a ROLLBACK, and on this
    // dead connection it cannot be sent (MySqlDriver::DropIfLost already closed
    // the handle) — but the note that failure would carry claims the
    // transaction's disposition is UNKNOWN, and on a killed session it is
    // perfectly well known: the server rolled it back (M5a reads the row count
    // back through a live connection and finds zero). Emitting it here would be
    // both verbose and FALSE, so SyncEngine gates it on IsConnected().
    mplive::ExpectTrue("M4 no bogus 'disposition unknown' note when the "
                       "connection is provably gone (the server disposed of the "
                       "transaction by ending the session)",
                       !err.Contains(L"事务处置状态未知"));

    // MySQL honesty note, same as the statement-failure path carries: every DDL
    // statement in phase 1 committed implicitly as it ran, so a failed COMMIT
    // does not undo them.
    mplive::ExpectTrue("M4 the MySQL implicit-DDL-commit warning is carried on "
                       "the COMMIT-failure path too", err.Contains(L"无法回滚"));

    // THE POSTAMBLE GUARD, on this path. It lives in Execute()'s scope and
    // ExecuteRun's COMMIT-failure `return false` is inside it, so the restore
    // MUST be attempted here as on every other exit. On a dead connection it
    // cannot succeed, and its failure is likewise appended.
    mplive::ExpectTrue("M4 the postamble guard covers the COMMIT-failure path",
                       err.Contains(L"会话状态未能恢复"));

    victim->Disconnect();
    killer->Disconnect();
}

} // namespace mtx

// ===========================================================================
int main()
{
    using namespace mtx;

    std::printf("== mysql_pg_live_mysqltx (MySQL target transaction integrity) ==\n");

    const wxString host = mplive::Env("SWIFTSQL_MYTEST_HOST");
    const wxString user = mplive::Env("SWIFTSQL_MYTEST_USER");
    const wxString pass = mplive::Env("SWIFTSQL_MYTEST_PASS");
    const bool     myOk = !host.IsEmpty() && !user.IsEmpty() && !pass.IsEmpty();
    if (!myOk)
        std::printf("SKIP (MySQL half): no live MySQL configured "
                    "(set SWIFTSQL_MYTEST_HOST/USER/PASS)\n");

    const int      port  = mplive::EnvInt("SWIFTSQL_MYTEST_PORT", 3306);
    const wxString srcDb = L"swiftsql_xmtx_src";
    const wxString tgtDb = L"swiftsql_xmtx_tgt";

    std::unique_ptr<IConnection> maint;
    if (myOk) {
        maint = CreateConnection(DbType::MySQL);
        wxString err;
        if (!maint->Connect(mplive::Profile(DbType::MySQL, host, port, user, pass,
                                            wxString()), err)) {
            std::printf("SKIP (MySQL half): connect failed: %s\n",
                        (const char*)err.utf8_str());
            maint.reset();
        } else {
            mplive::Observe("mysql server version", maint->ServerVersion());
        }
    }

    if (maint) {
        // Dropped on the way IN as well as OUT: a previous run that died mid-way
        // must not leave this one inheriting its tables on a shared server.
        mplive::Exec(*maint, L"DROP DATABASE IF EXISTS `" + srcDb + L"`", "drop src (pre)");
        mplive::Exec(*maint, L"DROP DATABASE IF EXISTS `" + tgtDb + L"`", "drop tgt (pre)");
        const bool provisioned =
            mplive::Exec(*maint, L"CREATE DATABASE `" + srcDb + L"`", "create src") &&
            mplive::Exec(*maint, L"CREATE DATABASE `" + tgtDb + L"`", "create tgt");

        auto src = CreateConnection(DbType::MySQL);
        auto tgt = CreateConnection(DbType::MySQL);

        struct Guard {
            IConnection *maint, *src, *tgt;
            wxString     srcDb, tgtDb;
            ~Guard()
            {
                if (src) src->Disconnect();
                if (tgt) tgt->Disconnect();
                QueryResult r; wxString e;
                if (maint) {
                    maint->Execute(L"DROP DATABASE IF EXISTS `" + srcDb + L"`", r, e);
                    maint->Execute(L"DROP DATABASE IF EXISTS `" + tgtDb + L"`", r, e);
                }
            }
        };

        // Inner scope so the guard's cleanup has already run by the time the
        // leftover check below executes: this server is SHARED, and a suite that
        // quietly leaves `swiftsql_xmtx_*` behind is a problem for whoever uses
        // it next, so cleanup is asserted rather than assumed.
        {
            Guard guard{ maint.get(), src.get(), tgt.get(), srcDb, tgtDb };

            mplive::ExpectTrue("provision two throwaway databases", provisioned);
            if (provisioned) {
                wxString e1, e2;
                const bool a = src->Connect(mplive::Profile(DbType::MySQL, host, port,
                                                            user, pass, srcDb), e1);
                const bool b = tgt->Connect(mplive::Profile(DbType::MySQL, host, port,
                                                            user, pass, tgtDb), e2);
                mplive::ExpectTrue("connect src", a);
                mplive::ExpectTrue("connect tgt", b);
                if (a && b) {
                    Ctx ctx;
                    ctx.src = src.get(); ctx.tgt = tgt.get();
                    ctx.srcDb = srcDb;   ctx.tgtDb = tgtDb;
                    ctx.host = host;     ctx.user = user;  ctx.pass = pass;
                    ctx.port = port;

                    if (Setup(*src, *tgt)) {
                        M1_CancelMidWrite(ctx);
                        M3_ImplicitDdlCommit(ctx);
                        M4_CommitFailurePath(ctx);
                        M5_RollbackAfterFailedCommit_MySQL(ctx);

                        // M2 runs LAST of the MySQL items because it kills the
                        // SHARED target connection out from under the driver, so
                        // nothing after it can rely on `tgt` being alive. The
                        // running total is printed first so a crash still leaves
                        // a usable record instead of discarding four items.
                        std::printf("\n-- running total before M2: %d checks, %d failed --\n",
                                    g_checks, g_fails);
                        M2_ConnectionLoss(ctx);
                    } else {
                        mplive::ExpectTrue("fixture setup", false);
                    }
                } else {
                    std::printf("  ERR  %s %s\n", (const char*)e1.utf8_str(),
                                (const char*)e2.utf8_str());
                }
            }
        }   // ~Guard: disconnects both sessions and drops both databases

        const wxString left = mplive::ScalarOf(*maint,
            L"SELECT COUNT(*) FROM information_schema.SCHEMATA "
            L"WHERE SCHEMA_NAME LIKE 'swiftsql\\_xmtx\\_%'");
        mplive::Observe("swiftsql_xmtx_* databases left on the server", left);
        mplive::ExpectEq("this suite cleaned up after itself", left == L"0" ? 0 : 1, 0);
        maint->Disconnect();
    }

    // The PostgreSQL half gates INDEPENDENTLY: it needs no MySQL fixture, and
    // M6 is the only item that can answer whether the COMMIT-failure path
    // leaves a transaction open, because a dead connection cannot hold one.
    M5_RollbackAfterFailedCommit_Pg();
    M6_CommitFailureLeavesNoOpenTxn();

    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
