// mysql_pg_live_mysqltx.h — shared harness for the MySQL-target TRANSACTION
// INTEGRITY suite. Split across two TUs purely for the charter's 1000-line
// ceiling, same reason mysql_pg_live_exec2/3/5.cpp exist; there is no second
// consumer.
//
// See mysql_pg_live_mysqltx.cpp's header comment for why the suite exists and
// what each item asks. SAFETY: no credential appears here or in either TU —
// everything comes from SWIFTSQL_MYTEST_* / SWIFTSQL_PGTEST_* and both halves
// skip cleanly when unset.
#pragma once

#include "mysql_pg_live_exec.h"     // mplive + mpexec helpers (Dump/Scalar/MakeSpec/…)

#include "db/SyncEngine.h"

#include <atomic>
#include <functional>
#include <memory>

namespace mtx {

using namespace db;
using namespace db::sync;
using mpexec::Allow;
using mpexec::MakeAllow;
using mpexec::MakeSpec;
using mpexec::Scalar;
using mpexec::ScalarInt;
using mplive::g_checks;
using mplive::g_fails;

inline const std::atomic<bool> g_never{false};

// The table shape every fixture in this suite uses. InnoDB explicitly: MyISAM
// is non-transactional and would make every assertion here vacuous.
inline const wxChar* kDdl = L" (`id` INT PRIMARY KEY, `v` INT NOT NULL, "
                            L"`note` VARCHAR(64)) ENGINE=InnoDB";

// ---------------------------------------------------------------------------
// Dial-out parameters, so an item can open its OWN extra connections (M1 needs
// a witness, M2 needs a killer AND a witness, M4/M5a need a victim they are
// allowed to destroy) without re-reading the environment — and without any item
// being able to reach a server the harness did not already choose.
// ---------------------------------------------------------------------------
struct Ctx {
    IConnection* src = nullptr;      // MySQL, bound to srcDb
    IConnection* tgt = nullptr;      // MySQL, bound to tgtDb
    wxString     srcDb, tgtDb;
    wxString     host, user, pass;
    int          port = 3306;

    std::unique_ptr<IConnection> Dial(const wxString& db, wxString& err) const
    {
        auto c = CreateConnection(DbType::MySQL);
        if (!c->Connect(mplive::Profile(DbType::MySQL, host, port, user, pass, db),
                        err))
            return nullptr;
        return c;
    }
};

inline bool Run(IConnection& c, const wxString& sql, const char* label)
{
    return mplive::Exec(c, sql, label);
}

// The victim's own connection id, read ON THE VICTIM. See the TU header: the
// alternative — scanning information_schema.PROCESSLIST for something that looks
// right — is how a shared-server test kills a colleague's session, so there is
// deliberately no code path in this suite that can produce an id the target
// connection did not report about itself.
inline long long OwnConnectionId(IConnection& c)
{
    long long v = -1;
    if (!Scalar(c, L"SELECT CONNECTION_ID()").ToLongLong(&v)) return -1;
    return v;
}

// MySQL has no generate_series; 8.0's recursive CTE is the closest equivalent
// and needs cte_max_recursion_depth raised past its 1000 default, which the
// fixture setup does per connection.
inline bool SeedSeries(IConnection& c, const wxString& table, long long n,
                       const wxString& tag, long long idBase, const char* label)
{
    return Run(c, wxString::Format(
        L"INSERT INTO `%s` (`id`,`v`,`note`) "
        L"WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM s WHERE n < %lld) "
        L"SELECT %lld+n, n*10, CONCAT('%s-',n) FROM s",
        table.wc_str(), n, idBase, tag.wc_str()), label);
}

inline std::vector<TableExecJob> InsertJobs(IConnection& src, IConnection& tgt,
                                            const wxString& srcDb,
                                            const wxString& tgtDb,
                                            const std::vector<wxString>& tables,
                                            bool& ok)
{
    std::vector<TableExecJob> jobs;
    ok = true;
    for (const wxString& t : tables) {
        DataDiffSpec spec;
        if (!MakeSpec(src, tgt, srcDb, tgtDb, t, spec)) { ok = false; return jobs; }
        jobs.push_back(TableExecJob{spec,
            MakeAllow(spec.SourceTable().Key(), Allow{true, false, false}, false)});
    }
    return jobs;
}

// ---------------------------------------------------------------------------
// A CANCELLABLE execute run. mpexec::RunExec hard-codes the never-cancelled
// token; this variant owns a real one and lets the progress callback trip it, so
// the cancel lands at a KNOWN point in the stream (after N applied rows) rather
// than at whatever moment a sleeping thread happened to wake. A wall-clock
// cancel would make "what is in the target afterwards?" depend on machine speed,
// i.e. flaky exactly where the answer matters most.
//
// `hook` is also how M2 fires its KILL — same mechanism, but there it returns
// false so the token is never tripped, because a connection loss is NOT a
// cancellation and the two must not be conflated in the result.
// ---------------------------------------------------------------------------
struct CancelOutcome {
    bool           ok = false;
    wxString       err;
    DataExecResult result;
    long long      progressCalls = 0;
    bool           tripped = false;
    wxString       trippedAt;
};

using StopWhen = std::function<bool(const wxString& table, const wxString& phase,
                                    long long applied)>;

CancelOutcome RunCancellable(IConnection& src, IConnection& tgt,
                             const std::vector<TableExecJob>& jobs,
                             const StopWhen& hook);          // mysql_pg_live_mysqltx.cpp

// ---------------------------------------------------------------------------
// The items. M1-M4 + main live in mysql_pg_live_mysqltx.cpp; M5/M6 — the
// ROLLBACK-after-failed-COMMIT pair — in mysql_pg_live_mysqltx2.cpp.
// ---------------------------------------------------------------------------
void M1_CancelMidWrite(Ctx& c);
void M2_ConnectionLoss(Ctx& c);
void M3_ImplicitDdlCommit(Ctx& c);
void M4_CommitFailurePath(Ctx& c);
void M5_RollbackAfterFailedCommit_MySQL(Ctx& c);
void M5_RollbackAfterFailedCommit_Pg();
// M6 is the one that proves the FIX rather than the engines' behaviour: it
// drives SyncEngine::Execute into a phase-1 COMMIT failure on a connection that
// is still HEALTHY, which is the only situation where "did the engine leave the
// transaction open?" is answerable at all.
void M6_CommitFailureLeavesNoOpenTxn();

} // namespace mtx
