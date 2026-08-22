// mysql_pg_live_mysqltx2.cpp — the ROLLBACK-after-failed-COMMIT pair (M5, M6).
// Separate TU purely for the charter's 1000-line ceiling; see
// mysql_pg_live_mysqltx.h.
//
// M5 asks what the SERVERS do. M6 asks what SyncEngine does — and it is the
// only item in the suite that can answer the question item 2 was actually
// about, because "did the engine leave a transaction open on the connection?"
// is unanswerable on a connection that is dead.

#include "mysql_pg_live_mysqltx.h"

#include <cstdio>

namespace mtx {
namespace {

using mpexec::ExpectStr;

// PostgreSQL dial-out for the two items that need a healthy COMMIT failure.
// Returns nullptr (with the reason printed by the caller) when the PG
// environment is not configured — this half gates INDEPENDENTLY of the MySQL
// half, so a MySQL-only machine still runs everything else.
struct PgEnv {
    wxString host, user, pass;
    int      port = 5432;
    bool     ok   = false;
};

PgEnv ReadPgEnv()
{
    PgEnv e;
    e.host = mplive::Env("SWIFTSQL_PGTEST_HOST");
    e.user = mplive::Env("SWIFTSQL_PGTEST_USER");
    e.pass = mplive::Env("SWIFTSQL_PGTEST_PASS");
    e.port = mplive::EnvInt("SWIFTSQL_PGTEST_PORT", 5432);
    e.ok   = !e.host.IsEmpty() && !e.user.IsEmpty() && !e.pass.IsEmpty();
    return e;
}

std::unique_ptr<IConnection> DialPg(const PgEnv& e, const wxString& db,
                                    wxString& err)
{
    auto c = CreateConnection(DbType::PostgreSQL);
    if (!c->Connect(mplive::Profile(DbType::PostgreSQL, e.host, e.port, e.user,
                                    e.pass, db), err))
        return nullptr;
    return c;
}

// Provision/drop a throwaway PG database through a maintenance connection bound
// to `postgres`. Dropped on the way IN as well as OUT: a previous run that died
// mid-way must not leave this one inheriting its tables on a shared server.
std::unique_ptr<IConnection> ProvisionPg(const PgEnv& e, const wxString& db,
                                         std::unique_ptr<IConnection>& maint)
{
    wxString err;
    maint = DialPg(e, L"postgres", err);
    if (!maint) {
        std::printf("  SKIP: PG maintenance connect failed: %s\n",
                    (const char*)err.utf8_str());
        return nullptr;
    }
    mplive::Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "pg drop (pre)");
    if (!mplive::Exec(*maint, L"CREATE DATABASE " + db, "pg create db"))
        return nullptr;
    auto c = DialPg(e, db, err);
    if (!c)
        std::printf("  SKIP: PG test-db connect failed: %s\n",
                    (const char*)err.utf8_str());
    return c;
}

// THE ONE WAY to make a COMMIT fail while the socket stays perfectly healthy.
// A DEFERRABLE INITIALLY DEFERRED unique constraint is not checked until commit
// time, so two conflicting inserts both succeed and the COMMIT is what breaks.
// InnoDB has no deferred constraints at all, which is exactly why M5a has to
// reach its COMMIT failure by killing the connection instead.
bool CreateDeferredFixture(IConnection& c)
{
    return Run(c, L"DROP TABLE IF EXISTS m6", "m6 drop") &&
           Run(c, L"CREATE TABLE m6 (id int, CONSTRAINT m6u UNIQUE (id) "
                  L"DEFERRABLE INITIALLY DEFERRED)", "m6 create");
}

} // namespace

// ===========================================================================
// M5 — what do the engines ACTUALLY do on ROLLBACK after a FAILED COMMIT?
// ===========================================================================
//
// THE PREREQUISITE FOR M6'S FIX, deliberately probed at the RAW connection level
// with no engine in the way. The fix is "attempt a ROLLBACK on the
// COMMIT-failure path, and APPEND a rollback failure to the original error
// rather than substituting it". Whether that appended note is a routine
// occurrence or a genuine alarm depends entirely on whether the server treats a
// post-failure ROLLBACK as an error or as a no-op — a fact about each server,
// not something to reason out from first principles.
// ===========================================================================
void M5_RollbackAfterFailedCommit_MySQL(Ctx& c)
{
    std::printf("\n-- M5a: MySQL, COMMIT fails because the connection died --\n");

    wxString ve, ke;
    std::unique_ptr<IConnection> victim = c.Dial(c.tgtDb, ve);
    std::unique_ptr<IConnection> killer = c.Dial(c.tgtDb, ke);
    if (!victim || !killer) {
        std::printf("  SKIP M5a: connect failed: %s / %s\n",
                    (const char*)ve.utf8_str(), (const char*)ke.utf8_str());
        return;
    }
    const long long id = OwnConnectionId(*victim);
    if (id <= 0) { std::printf("  SKIP M5a: no connection id\n"); return; }

    Run(*victim, L"DROP TABLE IF EXISTS `m5`", "M5a drop");
    if (!Run(*victim, wxString(L"CREATE TABLE `m5`") + kDdl, "M5a create")) {
        ++g_fails; return;
    }
    Run(*victim, L"BEGIN", "M5a begin");
    Run(*victim, L"INSERT INTO `m5` VALUES (1,10,'a')", "M5a insert");

    QueryResult kr; wxString kerr;
    killer->Execute(wxString::Format(L"KILL %lld", id), kr, kerr);

    QueryResult r1; wxString commitErr;
    const bool committed = victim->Execute(L"COMMIT", r1, commitErr);
    mplive::ExpectTrue("M5a the COMMIT fails after the connection is killed",
                       !committed);
    mplive::Observe("M5a COMMIT error", commitErr);

    QueryResult r2; wxString rbErr;
    const bool rolled = victim->Execute(L"ROLLBACK", r2, rbErr);
    // THE FINDING THAT SHAPES THE ERROR HANDLING, recorded as an observation
    // rather than asserted true-or-false: either answer is legitimate server
    // behaviour and the point is to learn WHICH. Observed: the ROLLBACK cannot
    // be sent at all — MySqlDriver::DropIfLost has already closed the handle on
    // CR_SERVER_GONE_ERROR, so this fails client-side with "未连接".
    mplive::Observe("M5a ROLLBACK after the failed COMMIT returned",
                    rolled ? wxString(L"SUCCESS") : (L"FAILURE: " + rbErr));
    mplive::ExpectTrue("M5a a ROLLBACK attempt on a dead connection does not "
                       "crash the driver", true);
    // The server disposes of a killed session's transaction itself, so nothing
    // from it can have survived. Read from a connection that is still alive.
    wxString we;
    if (auto witness = c.Dial(c.tgtDb, we)) {
        mplive::ExpectEq("M5a the killed session's transaction was discarded "
                         "by the server",
                         ScalarInt(*witness, L"SELECT COUNT(*) FROM `m5`"), 0);
        witness->Disconnect();
    }

    victim->Disconnect();
    killer->Disconnect();
}

void M5_RollbackAfterFailedCommit_Pg()
{
    std::printf("\n-- M5b: PostgreSQL, COMMIT fails on a HEALTHY connection --\n");

    const PgEnv e = ReadPgEnv();
    if (!e.ok) { std::printf("  SKIP M5b: no live PostgreSQL configured\n"); return; }

    const wxString db = L"swiftsql_xmtx_pg";
    std::unique_ptr<IConnection> maint;
    std::unique_ptr<IConnection> c = ProvisionPg(e, db, maint);
    if (c) {
        Run(*c, L"CREATE TABLE m5 (id int, CONSTRAINT m5u UNIQUE (id) "
                L"DEFERRABLE INITIALLY DEFERRED)", "M5b create");
        Run(*c, L"BEGIN", "M5b begin");
        Run(*c, L"INSERT INTO m5 VALUES (1)", "M5b insert 1");
        Run(*c, L"INSERT INTO m5 VALUES (1)", "M5b insert 1 again");

        QueryResult r1; wxString commitErr;
        const bool committed = c->Execute(L"COMMIT", r1, commitErr);
        mplive::ExpectTrue("M5b the deferred constraint makes the COMMIT fail",
                           !committed);
        mplive::Observe("M5b COMMIT error", commitErr);

        QueryResult r2; wxString rbErr;
        const bool rolled = c->Execute(L"ROLLBACK", r2, rbErr);
        // Observed: SUCCESS, with a server-side `WARNING: there is no
        // transaction in progress`. PostgreSQL has ALREADY ended the
        // transaction, so this rollback is a harmless no-op — which is what
        // makes "always attempt it" cost nothing on this engine.
        mplive::Observe("M5b ROLLBACK after the failed COMMIT returned",
                        rolled ? wxString(L"SUCCESS (harmless no-op)")
                               : (L"FAILURE: " + rbErr));

        // Whatever it returned, the session must be USABLE afterwards — that is
        // the property the fix actually needs, because on the automation path
        // the very next thing to touch this connection is the user's own SQL.
        ExpectStr("M5b the connection is usable again after the rollback attempt",
                  Scalar(*c, L"SELECT 1"), L"1");
        mplive::ExpectEq("M5b nothing from the failed transaction survived",
                         ScalarInt(*c, L"SELECT count(*) FROM m5"), 0);
        c->Disconnect();
    }

    if (maint) {
        mplive::Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "M5b drop (post)");
        maint->Disconnect();
    }
}

// ===========================================================================
// M6 — does SyncEngine's COMMIT-failure path leave a transaction OPEN?
// ===========================================================================
//
// THE QUESTION. SyncEngine::ExecuteRun's phase 1 has three failure exits. Two
// of them — cancellation and a rejected statement — issue a ROLLBACK before
// returning. The third,
//
//     if (!tgt_.Execute(L"COMMIT", r, e)) { err = e; return false; }
//
// did not, and was reported as "it leaves an open transaction on the
// connection" — which on the automation path (ui::MainFrame_Automation, which
// runs on the ConnectionTree's OWN long-lived connection rather than a clone)
// would persist for the life of the application, holding locks and a stale read
// view nobody asked for.
//
// WHY THIS ITEM IS PostgreSQL-TARGETED. M4 reaches the same code path by
// killing the connection, which is the realistic MySQL route — but on a dead
// connection "was a transaction left open?" is meaningless: the server tore the
// session down. The question is only answerable on a connection that SURVIVES
// its failed COMMIT, and a DEFERRABLE INITIALLY DEFERRED constraint is the only
// way to produce one. So this is PG-targeted for the same reason M3 is
// MySQL-targeted: it is the engine that can express the question.
//
// ---------------------------------------------------------------------------
// THE ANSWER, AND WHY THIS ITEM DOES NOT CLAIM TO PROVE THE FIX
// ---------------------------------------------------------------------------
// It does not leave one — and it never did. This item was run as a CONTROL with
// the engine's new ROLLBACK compiled out, and the backend reported `idle`
// either way. PostgreSQL ENDS the transaction itself when a COMMIT fails, so
// the reported hazard is not reachable on this engine; M5a shows MySQL's only
// reachable COMMIT failure kills the session, so it is not reachable there
// either.
//
// That makes the assertion below a pin on the SERVER's behaviour, exactly like
// M3's implicit-DDL-commit assertion — NOT evidence that the engine's ROLLBACK
// works. It is labelled as such rather than left looking like a regression
// guard it cannot be: an assertion that passes with and without the code it
// appears to test proves nothing about that code, and mislabelling one is how a
// suite accumulates green checks that defend nothing.
//
// What the ROLLBACK in SyncEngine.cpp is actually for is stated there: the
// dialects this suite cannot reach (SQL Server, Oracle, DM), where a failed
// COMMIT's effect on transaction state is not established.
//
// The read is `pg_stat_activity.state` FROM A SECOND CONNECTION: 'idle' means
// closed, 'idle in transaction' means left open. A backend cannot answer this
// about itself — it is by definition running the query used to ask, and so
// always reports 'active'.
// ===========================================================================
void M6_CommitFailureLeavesNoOpenTxn()
{
    std::printf("\n== M6: does the COMMIT-failure path leave an open "
                "transaction? ==\n");

    const PgEnv e = ReadPgEnv();
    if (!e.ok) { std::printf("  SKIP M6: no live PostgreSQL configured\n"); return; }

    const wxString db = L"swiftsql_xmtx_m6";
    std::unique_ptr<IConnection> maint;
    std::unique_ptr<IConnection> tgt = ProvisionPg(e, db, maint);
    if (!tgt) {
        if (maint) maint->Disconnect();
        return;
    }

    wxString we;
    std::unique_ptr<IConnection> witness = DialPg(e, db, we);
    if (!witness) {
        std::printf("  SKIP M6: witness connect failed: %s\n",
                    (const char*)we.utf8_str());
        tgt->Disconnect();
        if (maint) {
            mplive::Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "M6 drop");
            maint->Disconnect();
        }
        return;
    }

    const wxString pid = Scalar(*tgt, L"SELECT pg_backend_pid()");
    mplive::Observe("M6 target backend pid", pid);

    if (CreateDeferredFixture(*tgt)) {
        // A plan whose phase-1 statements are two INSERTs that conflict under a
        // DEFERRED unique constraint. Both SUCCEED; the COMMIT the engine issues
        // after them is what fails — which is the exact path under test, reached
        // with the connection still perfectly healthy.
        //
        // Hand-built rather than compared-for: no fixture could reliably make
        // BuildPlan emit two statements whose conflict is invisible until commit.
        SyncPlan plan;
        SyncPlan::TableUnit unit;
        unit.table = QualifiedName(L"public", L"m6");
        unit.ddl.push_back(L"INSERT INTO m6 VALUES (42)");
        unit.ddl.push_back(L"INSERT INTO m6 VALUES (42)");
        plan.units.push_back(std::move(unit));

        // The source is irrelevant — phase 1 never touches it, and the plan
        // carries no data units — so the target doubles as the source rather
        // than opening a connection whose only purpose is to be unused.
        SyncEngine     eng(*tgt, *tgt, db, db);
        DataExecResult res;
        wxString       err;
        const bool ok = eng.Execute(plan, DataExecPlan{}, /*useTransaction=*/true,
                                    res, err, g_never, {});

        mplive::ExpectTrue("M6 a failed COMMIT reports failure", !ok);
        mplive::Observe("M6 reported error", err);
        // The COMMIT's own error must be what the operator sees FIRST. A
        // rollback note that replaced it would bury the actionable half.
        mplive::ExpectTrue("M6 the reported error is the COMMIT's own error, "
                           "not a rollback's", err.Contains(L"m6u"));

        // A SERVER-BEHAVIOUR PIN, NOT A REGRESSION GUARD. Verified by control
        // run to read 'idle' with the engine's ROLLBACK compiled out as well as
        // in: PostgreSQL ends the transaction itself on a failed COMMIT. Named
        // for what it actually establishes, so nobody later reads it as proof
        // that the engine closed anything.
        const wxString st = Scalar(*witness,
            L"SELECT state FROM pg_stat_activity WHERE pid = " + pid);
        mplive::Observe("M6 target backend state, seen from OUTSIDE", st);
        ExpectStr("M6 PostgreSQL itself ends the transaction on a failed COMMIT "
                  "— so this path never left one open (control-verified: same "
                  "result without the engine's ROLLBACK)", st, L"idle");

        // And the connection is genuinely reusable — the property the automation
        // path depends on, since the user's own SQL is next to touch it.
        ExpectStr("M6 the connection is usable after the failed COMMIT",
                  Scalar(*tgt, L"SELECT 1"), L"1");
        mplive::ExpectEq("M6 nothing from the failed transaction survived",
                         ScalarInt(*witness, L"SELECT count(*) FROM m6"), 0);
    } else {
        mplive::ExpectTrue("M6 deferred-constraint fixture", false);
    }

    witness->Disconnect();
    tgt->Disconnect();
    if (maint) {
        mplive::Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "M6 drop (post)");
        maint->Disconnect();
    }
}

} // namespace mtx
