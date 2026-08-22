// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_fkrestore.cpp — LIVE proof that a MySQL target's session is left
// CLEAN after a sync run, on every exit path.
//
// THE DEFECT THIS PINS. db::sync::SyncEngine::BuildPlan emits, for a MySQL
// target, exactly one preamble/postamble pair:
//
//     preamble : SET FOREIGN_KEY_CHECKS=0
//     postamble: SET FOREIGN_KEY_CHECKS=1
//
// Execute() built its statement list from `preamble` plus each unit's `ddl` and
// NEVER COLLECTED `postamble` — nothing in src/ executed it at all. So every
// sync against a MySQL target disabled foreign-key enforcement on the target
// connection and never re-enabled it. On the automation path
// (ui::RunJobHeadless) that connection is the ConnectionTree's own long-lived
// entry, which the user then keeps using for ad-hoc SQL.
//
// WHY THIS SUITE EXISTS RATHER THAN A UNIT TEST. `SET FOREIGN_KEY_CHECKS` is
// SESSION state on a real server: it is not transactional (a ROLLBACK does not
// restore it), it is per-connection, and its effect is on the server's
// constraint checking rather than on anything the client can introspect from
// its own memory. A mock cannot exhibit any of that. Every assertion here is
// therefore read back FROM THE SERVER — `SELECT @@SESSION.foreign_key_checks`
// on the very connection the engine ran on — and the decisive one is behavioural
// rather than a variable read: after the run, an INSERT that violates the FK
// must actually be REFUSED by the server.
//
// The three exit paths are covered separately because they are separate code
// paths in Execute(), and a trailing statement (which is how the postamble was
// modelled before) would have skipped two of them:
//
//   1. SUCCESS     — the run completes normally.
//   2. FAILURE     — a statement fails; Execute returns false via an early
//                    return, after a ROLLBACK.
//   3. CANCELLATION— the stop token trips mid-run; Execute returns false via a
//                    different early return, also after a ROLLBACK.
//
// Case 3 additionally OBSERVES the relaxation while it is in force (reading the
// variable from inside the progress callback, between statements), so this suite
// cannot pass by having "fixed" the restore by never relaxing in the first
// place. Case 1 proves the same thing functionally: it inserts child rows whose
// parent does not exist, which the target's FK would refuse if checks were on.
//
// SAFETY: no credential is hard-coded; everything comes from SWIFTSQL_MYTEST_*
// and the suite SKIPs (exit 0) when unset. Its throwaway databases carry their
// own `swiftsql_xfkr_` prefix so they can never collide with another live
// suite's, and are dropped on the way in as well as out.
#include "mysql_pg_live.h"

#include "db/SyncEngine.h"

#include <atomic>
#include <cstdio>
#include <memory>

using namespace db;
using namespace db::sync;

namespace {

const std::atomic<bool> g_never{false};

// `SELECT @@SESSION.foreign_key_checks` as an integer, read on the connection
// passed in. -1 means the query itself failed, which is reported as a distinct
// outcome rather than silently folded into "not 1".
long long FkChecks(IConnection& c)
{
    const wxString s = mplive::ScalarOf(c, L"SELECT @@SESSION.foreign_key_checks");
    long v = -1;
    if (s.IsEmpty() || !s.ToLong(&v)) return -1;
    return v;
}

// THE BEHAVIOURAL CHECK. A variable reading 1 is a claim about a session
// setting; this is a claim about the SERVER'S ENFORCEMENT, which is what the
// user actually loses. Inserts a child row pointing at a parent id that does not
// exist and reports whether the server refused it. Any row that does land is
// removed again so the check can be repeated.
bool FkEnforced(IConnection& c)
{
    QueryResult r; wxString err;
    const bool inserted =
        c.Execute(L"INSERT INTO `child` (`id`,`pid`,`tag`) VALUES (99999,7777,'probe')",
                  r, err);
    if (inserted) {
        QueryResult r2; wxString e2;
        c.Execute(L"DELETE FROM `child` WHERE `id`=99999", r2, e2);
        return false;              // the violating row was accepted → NOT enforced
    }
    return true;                   // refused → enforcement is live
}

long long CountOf(IConnection& c, const wxString& table)
{
    const wxString s = mplive::ScalarOf(c, L"SELECT COUNT(*) FROM " + table);
    long long v = -1;
    if (s.IsEmpty() || !s.ToLongLong(&v)) return -1;
    return v;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
// The asymmetry is the point: the SOURCE has no foreign key, so it can legally
// hold child rows whose pid has no parent; the TARGET has the foreign key. Those
// rows can therefore only be written to the target while FOREIGN_KEY_CHECKS is
// off, which makes "the relaxation still works" observable as data rather than
// as a setting. InnoDB explicitly, because MyISAM ignores foreign keys entirely
// and would make the whole fixture vacuous.
bool Setup(IConnection& src, IConnection& tgt)
{
    const bool s =
        mplive::Exec(src, L"DROP TABLE IF EXISTS `child`",  "src drop child") &&
        mplive::Exec(src, L"DROP TABLE IF EXISTS `parent`", "src drop parent") &&
        mplive::Exec(src, L"CREATE TABLE `parent` (`id` INT PRIMARY KEY, "
                          L"`name` VARCHAR(50)) ENGINE=InnoDB", "src create parent") &&
        mplive::Exec(src, L"CREATE TABLE `child` (`id` INT PRIMARY KEY, "
                          L"`pid` INT NOT NULL, `tag` VARCHAR(50)) ENGINE=InnoDB",
                     "src create child") &&
        mplive::Exec(src, L"INSERT INTO `child` (`id`,`pid`,`tag`) VALUES "
                          L"(1,9001,'a'),(2,9002,'b'),(3,9003,'c')", "src seed child");

    const bool t =
        mplive::Exec(tgt, L"DROP TABLE IF EXISTS `child`",  "tgt drop child") &&
        mplive::Exec(tgt, L"DROP TABLE IF EXISTS `parent`", "tgt drop parent") &&
        mplive::Exec(tgt, L"CREATE TABLE `parent` (`id` INT PRIMARY KEY, "
                          L"`name` VARCHAR(50)) ENGINE=InnoDB", "tgt create parent") &&
        // Same column shape as the source (so the data pass has nothing
        // structural to reconcile) PLUS the constraint the source lacks.
        mplive::Exec(tgt, L"CREATE TABLE `child` (`id` INT PRIMARY KEY, "
                          L"`pid` INT NOT NULL, `tag` VARCHAR(50), "
                          L"CONSTRAINT `fk_child_parent` FOREIGN KEY (`pid`) "
                          L"REFERENCES `parent`(`id`)) ENGINE=InnoDB",
                     "tgt create child");
    return s && t;
}

// Data-only scope over `child` alone. Structure is deliberately OUT of scope:
// the tables already match column-for-column, and leaving structure on would let
// the engine try to reconcile the FK itself — a different subject. `parent` is
// excluded so it stays empty and the child rows stay genuinely orphaned.
SyncScope ChildOnly()
{
    SyncScope sc;
    sc.structure = false;
    sc.data      = true;
    sc.tables.push_back(QualifiedName(L"child"));
    return sc;
}

DataExecPlan AllowInsertUpdate(const SyncPlan& plan)
{
    DataExecPlan d;
    for (const auto& u : plan.units) {
        TableDataSpec t(u.table.Key());
        t.EnableInserts();
        t.EnableUpdates();
        d.tables.push_back(std::move(t));
    }
    return d;
}

// ===========================================================================
// CASE 1 — SUCCESS
// ===========================================================================
void CaseSuccess(IConnection& src, IConnection& tgt,
                 const wxString& srcDb, const wxString& tgtDb)
{
    std::printf("\n-- case 1: successful run --\n");
    mplive::Exec(tgt, L"DELETE FROM `child`", "tgt clear child");

    SyncEngine eng(src, tgt, srcDb, tgtDb);
    SyncPlan   plan;
    wxString   err;
    if (!eng.BuildPlan(ChildOnly(), plan, err, g_never, {})) {
        mplive::ExpectTrue("case1 BuildPlan", false);
        std::printf("  ERR  %s\n", (const char*)err.utf8_str());
        return;
    }
    // The pair must actually be in the plan, or the rest of this case proves
    // nothing about a restore that was never needed.
    mplive::ExpectEq("case1 plan has a MySQL preamble",
                     (long long)plan.preamble.size(), 1);
    mplive::ExpectEq("case1 plan has a MySQL postamble",
                     (long long)plan.postamble.size(), 1);

    DataExecResult res;
    const bool ok = eng.Execute(plan, AllowInsertUpdate(plan), true, res, err,
                                g_never, {});
    if (!ok) std::printf("  ERR  execute: %s\n", (const char*)err.utf8_str());
    mplive::ExpectTrue("case1 execute succeeds", ok);

    // THE RELAXATION STILL WORKS. These three rows reference parent ids that do
    // not exist; the target's FK would have refused every one of them with
    // checks on, so their presence is proof the preamble did its job.
    const long long n = CountOf(tgt, L"`child`");
    mplive::Observe("case1 target child rows", wxString::Format(L"%lld", n));
    mplive::ExpectEq("case1 orphaned child rows landed (FK relaxation worked)", n, 3);

    const long long fk = FkChecks(tgt);
    mplive::Observe("case1 @@SESSION.foreign_key_checks after success",
                    wxString::Format(L"%lld", fk));
    mplive::ExpectEq("case1 session restored after SUCCESS", fk, 1);

    mplive::ExpectTrue("case1 server actually enforces the FK again",
                       FkEnforced(tgt));
}

// ===========================================================================
// CASE 2 — FAILURE
// ===========================================================================
// A statement that the server will reject, injected AFTER the preamble in the
// same statement list. Execute() takes its failure early-return (with a
// ROLLBACK on the way) — the path a trailing postamble statement could never
// have reached.
void CaseFailure(IConnection& src, IConnection& tgt,
                 const wxString& srcDb, const wxString& tgtDb)
{
    std::printf("\n-- case 2: failed run --\n");
    mplive::Exec(tgt, L"DELETE FROM `child`", "tgt clear child");

    SyncEngine eng(src, tgt, srcDb, tgtDb);
    SyncPlan   plan;
    wxString   err;
    if (!eng.BuildPlan(ChildOnly(), plan, err, g_never, {})) {
        mplive::ExpectTrue("case2 BuildPlan", false);
        return;
    }
    if (plan.units.empty()) {
        mplive::ExpectTrue("case2 plan has a unit to attach the failure to", false);
        return;
    }
    plan.units[0].ddl.push_back(
        L"ALTER TABLE `swiftsql_fkr_no_such_table` ADD COLUMN `x` INT");

    DataExecResult res;
    const bool ok = eng.Execute(plan, AllowInsertUpdate(plan), true, res, err,
                                g_never, {});
    mplive::ExpectTrue("case2 execute reports failure", !ok);
    mplive::Observe("case2 reported error", err);

    const long long fk = FkChecks(tgt);
    mplive::Observe("case2 @@SESSION.foreign_key_checks after failure",
                    wxString::Format(L"%lld", fk));
    mplive::ExpectEq("case2 session restored after FAILURE", fk, 1);

    mplive::ExpectTrue("case2 server actually enforces the FK again",
                       FkEnforced(tgt));
}

// ===========================================================================
// CASE 3 — CANCELLATION (and the mid-run observation)
// ===========================================================================
// Two harmless statements are appended so the run has somewhere to be cancelled
// BETWEEN, after the preamble has already taken effect. The progress callback
// fires once the preamble has executed; that is where the relaxation is read
// live, and where the stop token is tripped.
void CaseCancel(IConnection& src, IConnection& tgt,
                const wxString& srcDb, const wxString& tgtDb)
{
    std::printf("\n-- case 3: cancelled run --\n");
    mplive::Exec(tgt, L"DELETE FROM `child`", "tgt clear child");

    SyncEngine eng(src, tgt, srcDb, tgtDb);
    SyncPlan   plan;
    wxString   err;
    if (!eng.BuildPlan(ChildOnly(), plan, err, g_never, {})) {
        mplive::ExpectTrue("case3 BuildPlan", false);
        return;
    }
    if (plan.units.empty()) {
        mplive::ExpectTrue("case3 plan has a unit to extend", false);
        return;
    }
    plan.units[0].ddl.push_back(L"SET @swiftsql_fkr_probe = 1");
    plan.units[0].ddl.push_back(L"SET @swiftsql_fkr_probe = 2");

    std::atomic<bool> stop{false};
    long long duringRun = -2;      // -2 = the callback never ran

    // Read the session variable BETWEEN statements: tgt has just returned from
    // Execute() and is idle, so this is an ordinary query on an idle connection,
    // not re-entry into a busy one.
    auto progress = [&](int done, int /*total*/, const wxString&) {
        if (done == 1 && duringRun == -2) {
            duringRun = FkChecks(tgt);
            stop.store(true);      // cancel from here on
        }
    };

    DataExecResult res;
    const bool ok = eng.Execute(plan, AllowInsertUpdate(plan), true, res, err,
                                stop, progress);

    mplive::Observe("case3 @@SESSION.foreign_key_checks DURING the run",
                    wxString::Format(L"%lld", duringRun));
    // The relaxation is in force mid-run. Without this the whole suite could be
    // satisfied by a build that simply never relaxes anything.
    mplive::ExpectEq("case3 FK checks are OFF during the run", duringRun, 0);

    mplive::ExpectTrue("case3 execute reports cancellation", !ok);
    mplive::Observe("case3 reported error", err);

    const long long fk = FkChecks(tgt);
    mplive::Observe("case3 @@SESSION.foreign_key_checks after cancellation",
                    wxString::Format(L"%lld", fk));
    mplive::ExpectEq("case3 session restored after CANCELLATION", fk, 1);

    mplive::ExpectTrue("case3 server actually enforces the FK again",
                       FkEnforced(tgt));
}

} // namespace

int main()
{
    std::printf("== mysql_pg_live_fkrestore (MySQL target session restore, live) ==\n");

    const wxString host = mplive::Env("SWIFTSQL_MYTEST_HOST");
    const wxString user = mplive::Env("SWIFTSQL_MYTEST_USER");
    const wxString pass = mplive::Env("SWIFTSQL_MYTEST_PASS");
    if (host.IsEmpty() || user.IsEmpty() || pass.IsEmpty()) {
        std::printf("SKIP: no live MySQL configured "
                    "(set SWIFTSQL_MYTEST_HOST/USER/PASS)\n");
        return 0;
    }
    const int port = mplive::EnvInt("SWIFTSQL_MYTEST_PORT", 3306);

    const wxString srcDb = L"swiftsql_xfkr_src";
    const wxString tgtDb = L"swiftsql_xfkr_tgt";

    auto maint = CreateConnection(DbType::MySQL);
    wxString err;
    if (!maint->Connect(mplive::Profile(DbType::MySQL, host, port, user, pass,
                                        wxString()), err)) {
        std::printf("SKIP: MySQL connect failed: %s\n", (const char*)err.utf8_str());
        return 0;
    }
    mplive::Observe("mysql server version", maint->ServerVersion());

    // Dropped on the way IN as well as OUT: a previous run that died mid-way
    // must not leave this one inheriting its tables on a shared server.
    mplive::Exec(*maint, L"DROP DATABASE IF EXISTS `" + srcDb + L"`", "drop src (pre)");
    mplive::Exec(*maint, L"DROP DATABASE IF EXISTS `" + tgtDb + L"`", "drop tgt (pre)");
    const bool provisioned =
        mplive::Exec(*maint, L"CREATE DATABASE `" + srcDb + L"`", "create src") &&
        mplive::Exec(*maint, L"CREATE DATABASE `" + tgtDb + L"`", "create tgt");

    std::unique_ptr<IConnection> src, tgt;
    struct Guard {
        IConnection*                  maint;
        std::unique_ptr<IConnection>* src;
        std::unique_ptr<IConnection>* tgt;
        wxString                      srcDb, tgtDb;
        ~Guard()
        {
            for (auto* p : { src, tgt }) if (p && *p) (*p)->Disconnect();
            QueryResult r; wxString e;
            if (maint) {
                maint->Execute(L"DROP DATABASE IF EXISTS `" + srcDb + L"`", r, e);
                maint->Execute(L"DROP DATABASE IF EXISTS `" + tgtDb + L"`", r, e);
            }
        }
    };

    src = CreateConnection(DbType::MySQL);
    tgt = CreateConnection(DbType::MySQL);

    // The guard lives in an INNER scope so its cleanup has already run by the
    // time the leftover check below executes. This server is shared, and a
    // suite that quietly leaves `swiftsql_xfkr_*` behind on every run is a
    // problem for whoever uses it next — so cleanup is asserted, not assumed.
    {
    Guard guard{ maint.get(), &src, &tgt, srcDb, tgtDb };

    mplive::ExpectTrue("provision two throwaway databases", provisioned);
    if (!provisioned) {
        std::printf("\n== %d checks, %d failed ==\n", mplive::g_checks, mplive::g_fails);
        return 1;
    }

    {
        wxString e1, e2;
        const bool a = src->Connect(mplive::Profile(DbType::MySQL, host, port, user,
                                                    pass, srcDb), e1);
        const bool b = tgt->Connect(mplive::Profile(DbType::MySQL, host, port, user,
                                                    pass, tgtDb), e2);
        mplive::ExpectTrue("connect src", a);
        mplive::ExpectTrue("connect tgt", b);
        if (!a || !b) {
            std::printf("  ERR  %s %s\n", (const char*)e1.utf8_str(),
                        (const char*)e2.utf8_str());
            std::printf("\n== %d checks, %d failed ==\n", mplive::g_checks, mplive::g_fails);
            return 1;
        }
    }

    if (!Setup(*src, *tgt)) {
        mplive::ExpectTrue("fixture setup", false);
        std::printf("\n== %d checks, %d failed ==\n", mplive::g_checks, mplive::g_fails);
        return 1;
    }

    // The baseline. If the server hands out sessions with checks already off,
    // every later assertion would be meaningless, so it is established rather
    // than assumed.
    const long long base = FkChecks(*tgt);
    mplive::Observe("baseline @@SESSION.foreign_key_checks", wxString::Format(L"%lld", base));
    mplive::ExpectEq("baseline session has FK checks ON", base, 1);
    mplive::ExpectTrue("baseline server enforces the FK", FkEnforced(*tgt));

    CaseSuccess(*src, *tgt, srcDb, tgtDb);
    CaseFailure(*src, *tgt, srcDb, tgtDb);
    CaseCancel (*src, *tgt, srcDb, tgtDb);
    }   // ~Guard: disconnects both sessions and drops both databases

    const wxString left = mplive::ScalarOf(*maint,
        L"SELECT COUNT(*) FROM information_schema.SCHEMATA "
        L"WHERE SCHEMA_NAME LIKE 'swiftsql\\_xfkr\\_%'");
    mplive::Observe("swiftsql_xfkr_* databases left on the server", left);
    mplive::ExpectEq("this suite cleaned up after itself", left == L"0" ? 0 : 1, 0);

    std::printf("\n== %d checks, %d failed ==\n", mplive::g_checks, mplive::g_fails);
    return mplive::g_fails == 0 ? 0 : 1;
}
