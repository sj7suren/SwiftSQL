// mysql_pg_live_scale2.cpp — phases B and C of the scale suite. Separate TU
// purely for the charter's 1000-line ceiling, exactly as mysql_pg_live_exec5.cpp
// splits exec4. See mysql_pg_live_scale.h.

#include "mysql_pg_live_scale.h"

#include <cstdio>

namespace mpscale {
namespace {

// ---------------------------------------------------------------------------
// A cancellable execute that MEASURES the acknowledgement latency
// ---------------------------------------------------------------------------
//
// mpexec::RunExec hard-codes the never-cancelled token and mpx4's
// RunExecCancellable records only WHETHER the hook fired. Neither can answer the
// question that matters at scale: once the user presses 取消 on a 500k-row write,
// how long until the call actually returns?
//
// The token is tripped from inside the executor's OWN progress callback, so the
// cancel lands at a KNOWN point in the stream (after N applied rows) rather than
// at whatever wall-clock moment a sleeping test thread happened to wake up —
// the same discipline mysql_pg_live_exec4.h explains. The clock starts on that
// exact line and stops when ExecuteDataSync returns, so the number is the
// engine's acknowledgement latency and contains no test-side scheduling.
struct CancelRun {
    bool           ok = false;
    wxString       err;
    DataExecResult result;
    bool           tripped = false;
    long long      trippedAtRows = 0;
    long long      latencyMs = -1;
    long long      progressTicks = 0;
};

CancelRun RunAndCancelAfter(IConnection& src, IConnection& tgt,
                            const std::vector<TableExecJob>& jobs,
                            long long cancelAfterRows)
{
    CancelRun r;
    std::atomic<bool> stop{false};
    std::chrono::steady_clock::time_point trippedAt;

    DataSyncOptions base;
    r.ok = ExecuteDataSync(src, tgt, jobs, base, r.result, r.err, stop,
        [&](const wxString&, const wxString&, long long applied) {
            ++r.progressTicks;
            if (!r.tripped && applied >= cancelAfterRows) {
                r.tripped       = true;
                r.trippedAtRows = applied;
                trippedAt       = std::chrono::steady_clock::now();
                stop.store(true);
            }
        });

    if (r.tripped)
        r.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - trippedAt).count();
    return r;
}

// A scalar read through a connection that may legitimately not support the
// query (a metrics table that is off, a permission the account lacks). Returns
// -1 rather than failing the suite: an unavailable server metric is a gap in the
// OBSERVATION, not a defect in the product, and conflating the two would make
// this suite fail on a differently-configured box.
long long TryScalarInt(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return -1;
    if (r.rows.empty() || r.rows[0].empty()) return -1;
    long long v = 0;
    return r.rows[0][0].ToLongLong(&v) ? v : -1;
}

} // namespace

// ===========================================================================
// PHASE B — progress cadence and MEASURED cancel latency
// ===========================================================================
void PhaseB_ProgressAndCancel(Ctx& c)
{
    std::printf("\n== PHASE B: progress cadence and cancel latency on a "
                "%lld-row write ==\n", c.rows);

    const wxString qMy = QuoteIdent(c.myDb, Dialect::MySQL);

    // PhaseA left the target converged. Empty it so the cancelled run has the
    // full diff in front of it — cancelling a run with nothing to do would
    // measure nothing.
    if (!mplive::Exec(*c.pg, wxString(L"TRUNCATE ") + kTable, "empty pg for cancel")) {
        mplive::ExpectTrue("phaseB fixture: target emptied", false);
        return;
    }

    DataDiffSpec spec;
    if (!MakeSpec(*c.my, *c.pg, c.myDb, c.pgDb, kTable, spec)) {
        mplive::ExpectTrue("phaseB spec built", false);
        return;
    }
    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(kTable, Allow{true, true, false}, false)});

    // Cancel a fifth of the way in: far enough that many 500-row batches have
    // already gone (so the run is genuinely under way and the merge's cursors
    // are both mid-stream), early enough that a broken cancel has plenty of
    // remaining work to hang on.
    const long long cancelAt = c.rows / 5;
    std::printf("  OBS  cancelling after ~%lld applied rows\n", cancelAt);

    const auto t0 = std::chrono::steady_clock::now();
    const CancelRun r = RunAndCancelAfter(*c.my, *c.pg, jobs, cancelAt);
    const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    ShowResult(r.result, "B/cancelled");
    std::printf("  OBS  tripped=%d at %lld rows; cancel latency %lld ms; "
                "whole run %lld ms; progress ticks %lld\n",
                (int)r.tripped, r.trippedAtRows, r.latencyMs, totalMs,
                r.progressTicks);

    mplive::ExpectTrue("phaseB the cancel hook actually fired", r.tripped);
    mplive::ExpectTrue("phaseB a cancelled run reports failure, not success", !r.ok);

    // THE NUMBER THIS PHASE EXISTS FOR. RowCursor polls its stop flag on a 50 ms
    // timed wait and calls IConnection::Cancel() to abort the in-flight query
    // server-side, so the acknowledgement is bounded by that poll plus one batch
    // — NOT by the remaining 400k rows. 30 s is a deliberately loose ceiling:
    // it is far above any plausible correct value and far below "it finished the
    // whole table first", which is the failure being excluded. A tight bound
    // would make this flaky on a loaded shared server without making it stricter
    // about the thing that matters.
    mplive::ExpectTrue("phaseB cancel is acknowledged promptly, not after the "
                       "remaining rows", r.latencyMs >= 0 && r.latencyMs < 30000);

    // Intra-table progress on the WRITE side, at scale. Derived from the batch
    // size the executor actually uses (500 rows per report) against the rows it
    // got through before the cancel, so it means the same thing at any run size
    // rather than encoding one.
    const long long expectTicks = cancelAt / 500;
    mplive::ExpectTrue("phaseB progress fired repeatedly before the cancel",
                       r.progressTicks >= std::max<long long>(2, expectTicks / 2));

    // ---- what a cancelled per-table transaction leaves behind ---------------
    // The per-table transaction must have rolled back, so the target holds
    // nothing — and the report must not claim rows that no longer exist (the
    // defect mysql_pg_live_exec4.cpp item 2b pinned, restated at scale).
    const long long landed =
        ScalarInt(*c.pg, wxString(L"SELECT COUNT(*) FROM ") + kTable);
    std::printf("  OBS  rows in the target after the cancel: %lld\n", landed);
    mplive::ExpectEq("phaseB the cancelled table's transaction rolled back "
                     "entirely", landed, 0);

    const TableExecReport* rep = ReportOf(r.result, kTable);
    if (rep) {
        std::printf("  OBS  report after cancel: ins=%lld committed=%d\n",
                    rep->inserts, (int)rep->committed);
        mplive::ExpectEq("phaseB report does not count rolled-back inserts",
                         rep->inserts, 0);
        mplive::ExpectTrue("phaseB report does not claim the table committed",
                           !rep->committed);
    } else {
        mplive::ExpectTrue("phaseB a report exists for the cancelled table", false);
    }

    // The retry must still converge from the cancelled state — what a user sees
    // when they press 同步 again.
    const PhaseResult retry = MeasuredExecute(c, jobs, "execute/retry");
    ReportPhase("execute/retry", retry, c.rows);
    mplive::ExpectTrue("phaseB the retry after a cancel succeeds", retry.ran);
    mplive::ExpectEq("phaseB the retry converged the target",
                     ScalarInt(*c.pg, wxString(L"SELECT COUNT(*) FROM ") + kTable),
                     c.rows);
}

// ===========================================================================
// PHASE C — what one per-table transaction does to each engine
// ===========================================================================
//
// DataSyncExec.h justifies per-table transactions with a specific claim: "a
// single transaction spanning a 10M-row sync exhausts PostgreSQL's WAL and
// MySQL's undo tablespace". That is an argument about a transaction this design
// never opens — but the transaction it DOES open still covers one whole table,
// so at 500k rows the question "does a per-TABLE transaction survive?" is live
// and has never been asked. This phase asks it from OUTSIDE the writer: a second
// connection watches the writing backend, because asking the writer about its
// own transaction would serialize behind whatever it is doing.
// ---------------------------------------------------------------------------
namespace {

// C1's watcher: poll the writer's transaction age and the WAL position while a
// 500k-row insert runs inside ONE transaction.
struct PgPressure {
    long long walBytes        = -1;
    long long maxXactAgeMs    = -1;
    long long maxBlocked      = 0;   // sessions blocked behind our locks
    long long samples         = 0;
};

} // namespace

void PhaseC_EnginePressure(Ctx& c)
{
    std::printf("\n== PHASE C: per-table transaction pressure at %lld rows ==\n",
                c.rows);

    const wxString qMy = QuoteIdent(c.myDb, Dialect::MySQL);

    // -----------------------------------------------------------------------
    // C1 — PostgreSQL as the target: WAL generated by one per-table transaction
    // -----------------------------------------------------------------------
    wxString werr;
    std::unique_ptr<IConnection> watch = c.DialPg(werr);
    if (!watch) {
        std::printf("  SKIP C1: could not open a watcher connection: %s\n",
                    (const char*)werr.utf8_str());
    } else {
        const long long writerPid = ScalarInt(*c.pg, L"SELECT pg_backend_pid()");
        mplive::Observe("C1 writer backend pid",
                        wxString::Format(L"%lld", writerPid));

        if (!mplive::Exec(*c.pg, wxString(L"TRUNCATE ") + kTable, "empty pg for C1")) {
            mplive::ExpectTrue("phaseC1 fixture: target emptied", false);
        } else {
            DataDiffSpec spec;
            std::vector<TableExecJob> jobs;
            if (!MakeSpec(*c.my, *c.pg, c.myDb, c.pgDb, kTable, spec)) {
                mplive::ExpectTrue("phaseC1 spec built", false);
            } else {
                jobs.push_back(TableExecJob{spec,
                    MakeAllow(kTable, Allow{true, true, false}, false)});

                // WAL is measured as an LSN DIFFERENCE across the run, read from
                // the watcher so the writer's own transaction state cannot
                // affect it.
                const wxString lsn0 = Scalar(*watch, L"SELECT pg_current_wal_lsn()");

                PgPressure       press;
                std::atomic<bool> watching{true};
                std::thread watcher([&] {
                    // Its OWN connection: db connections are not thread-safe and
                    // sharing the writer's would be a heap-corruption bug, not a
                    // measurement.
                    while (watching.load()) {
                        ++press.samples;
                        const long long ageMs = TryScalarInt(*watch, wxString::Format(
                            L"SELECT COALESCE(EXTRACT(EPOCH FROM (now() - xact_start)) "
                            L"* 1000, -1)::bigint FROM pg_stat_activity WHERE pid = %lld",
                            writerPid));
                        if (ageMs > press.maxXactAgeMs) press.maxXactAgeMs = ageMs;
                        const long long blocked = TryScalarInt(*watch, wxString::Format(
                            L"SELECT COUNT(*) FROM pg_stat_activity "
                            L"WHERE wait_event_type = 'Lock' AND pid <> %lld", writerPid));
                        if (blocked > press.maxBlocked) press.maxBlocked = blocked;
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                });

                const PhaseResult run = MeasuredExecute(c, jobs, "execute/C1-pg");
                watching.store(false);
                watcher.join();

                const wxString lsn1 = Scalar(*watch, L"SELECT pg_current_wal_lsn()");
                press.walBytes = TryScalarInt(*watch, wxString::Format(
                    L"SELECT pg_wal_lsn_diff('%s','%s')::bigint", lsn1, lsn0));

                ReportPhase("execute/C1-pg", run, c.rows);
                std::printf("  OBS  C1 WAL generated: %lld bytes (%.1f MB) "
                            "for %lld rows = %.0f bytes/row\n",
                            press.walBytes, press.walBytes / 1048576.0, c.rows,
                            press.walBytes > 0 ? (double)press.walBytes / (double)c.rows : 0.0);
                std::printf("  OBS  C1 longest observed transaction age: %lld ms "
                            "(%lld samples); peak sessions blocked on our locks: %lld\n",
                            press.maxXactAgeMs, press.samples, press.maxBlocked);

                mplive::ExpectTrue("phaseC1 PostgreSQL survives a per-table "
                                   "transaction over the whole table", run.ran);
                mplive::ExpectEq("phaseC1 the whole table landed",
                                 ScalarInt(*c.pg, wxString(L"SELECT COUNT(*) FROM ") + kTable),
                                 c.rows);
                // Not an assertion about a good number, an assertion that the
                // measurement HAPPENED — a silently -1 WAL reading would let a
                // real regression in transaction size go unnoticed.
                mplive::ExpectTrue("phaseC1 WAL generation was actually measured",
                                   press.walBytes > 0);
                mplive::ExpectTrue("phaseC1 no other session was left blocked "
                                   "behind the writer's locks", press.maxBlocked == 0);
            }
        }
    }

    // -----------------------------------------------------------------------
    // C2 — MySQL as the TARGET: undo footprint of one per-table transaction
    // -----------------------------------------------------------------------
    // Every phase above writes into PostgreSQL, so MySQL has only ever been read
    // here and its undo tablespace — the half of DataSyncExec.h's claim that is
    // about MySQL — has never been under any pressure at all. This runs the same
    // table the other way (PostgreSQL source, MySQL target) and watches the
    // writing transaction's row count grow from a second MySQL connection, which
    // is the direct observation of "one transaction, 500k rows of undo".
    std::printf("\n-- C2: the same table written INTO MySQL --\n");

    wxString merr;
    std::unique_ptr<IConnection> myWatch = c.DialMy(merr);
    if (!myWatch) {
        std::printf("  SKIP C2: could not open a MySQL watcher connection: %s\n",
                    (const char*)merr.utf8_str());
        return;
    }

    const wchar_t* kRev = L"t_bulk_r";
    const bool prepped =
        mplive::Exec(*c.pg, wxString(L"DROP TABLE IF EXISTS ") + kRev, "drop pg rev") &&
        mplive::Exec(*c.pg, wxString(L"CREATE TABLE ") + kRev +
                     L" (LIKE " + kTable + L" INCLUDING ALL)", "create pg rev") &&
        mplive::Exec(*c.pg, wxString(L"INSERT INTO ") + kRev +
                     L" SELECT * FROM " + kTable, "seed pg rev") &&
        mplive::Exec(*c.my, L"DROP TABLE IF EXISTS " + qMy + L"." + kRev, "drop my rev") &&
        mplive::Exec(*c.my, L"CREATE TABLE " + qMy + L"." + kRev + L" ("
                     L"  id INT NOT NULL PRIMARY KEY,"
                     L"  name VARCHAR(40) NOT NULL,"
                     L"  v BIGINT NOT NULL) ENGINE=InnoDB", "create my rev");
    if (!prepped) {
        mplive::ExpectTrue("phaseC2 fixture prepared", false);
        return;
    }

    // NOTE THE ARGUMENT ORDER: source is PostgreSQL, target is MySQL. This is
    // the only place in the suite the two swap, and getting it backwards would
    // silently measure the direction already covered.
    DataDiffSpec rspec;
    if (!MakeSpec(*c.pg, *c.my, c.pgDb, c.myDb, kRev, rspec)) {
        mplive::ExpectTrue("phaseC2 spec built", false);
        return;
    }
    std::vector<TableExecJob> rjobs;
    rjobs.push_back(TableExecJob{rspec, MakeAllow(kRev, Allow{true, true, false}, false)});

    const long long connId = ScalarInt(*c.my, L"SELECT CONNECTION_ID()");
    mplive::Observe("C2 MySQL writer connection id", wxString::Format(L"%lld", connId));
    const long long hist0 = TryScalarInt(*myWatch,
        L"SELECT count FROM information_schema.INNODB_METRICS "
        L"WHERE name = 'trx_rseg_history_len'");

    long long maxRowsModified = -1, maxHistory = hist0, samples = 0;
    std::atomic<bool> watching{true};
    std::thread watcher([&] {
        while (watching.load()) {
            ++samples;
            // trx_rows_modified IS the undo footprint of the open transaction:
            // it is how many rows this one transaction would have to roll back.
            // Watching it climb to the table's row count is the direct evidence
            // that one per-table transaction really does span the whole table.
            const long long rm = TryScalarInt(*myWatch, wxString::Format(
                L"SELECT trx_rows_modified FROM information_schema.innodb_trx "
                L"WHERE trx_mysql_thread_id = %lld", connId));
            if (rm > maxRowsModified) maxRowsModified = rm;
            const long long h = TryScalarInt(*myWatch,
                L"SELECT count FROM information_schema.INNODB_METRICS "
                L"WHERE name = 'trx_rseg_history_len'");
            if (h > maxHistory) maxHistory = h;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // Measured with the same instrumentation as every other phase, but note it
    // runs pg -> my, so MeasuredExecute (which is wired my -> pg) cannot be
    // used; this is the one hand-rolled run in the suite.
    PhaseResult rev;
    {
        MemWatch w("execute/C2-mysql");
        DataSyncOptions base;
        DataExecResult  res;
        rev.ran = ExecuteDataSync(*c.pg, *c.my, rjobs, base, res, rev.err,
                                  mpexec::g_never,
            [&rev](const wxString&, const wxString&, long long) { ++rev.progressTicks; });
        w.Finish();
        rev.ms                = w.ElapsedMs();
        rev.peakPrivateRiseKb = w.PeakPrivateRiseKb();
        ShowResult(res, "C2");
        if (!rev.ran)
            std::printf("  ERR  C2: %s\n", (const char*)rev.err.utf8_str());
        // By the spec's key, not the bare name: this is the one phase whose
        // SOURCE is PostgreSQL, so the report is filed under "public.t_bulk_r".
        // Looking it up as kRev found nothing and printed ins=0 for a sweep that
        // had just inserted every row.
        if (const TableExecReport* rr = ReportOf(res, rspec.SourceTable().Key())) {
            rev.stat.inserts = rr->inserts;
            rev.executable   = rr->committed;
        }
    }
    watching.store(false);
    watcher.join();

    ReportPhase("execute/C2-mysql", rev, c.rows);
    std::printf("  OBS  C2 peak trx_rows_modified for the writing transaction: "
                "%lld (of %lld rows; %lld samples)\n",
                maxRowsModified, c.rows, samples);
    std::printf("  OBS  C2 InnoDB history list length: %lld -> peak %lld\n",
                hist0, maxHistory);

    mplive::ExpectTrue("phaseC2 MySQL survives a per-table transaction over the "
                       "whole table", rev.ran);
    mplive::ExpectEq("phaseC2 every row landed in MySQL",
                     ScalarInt(*c.my, L"SELECT COUNT(*) FROM " + qMy + L"." + kRev),
                     c.rows);
    mplive::ExpectEq("phaseC2 no row is malformed in MySQL",
                     ScalarInt(*c.my, L"SELECT COUNT(*) FROM " + qMy + L"." + kRev +
                               L" WHERE name <> CONCAT('row-', id) OR v <> id * 2"), 0);
    mplive::ExpectTrue("phaseC2 memory stays flat writing INTO MySQL too",
                       rev.peakPrivateRiseKb < 64 * 1024);
}

} // namespace mpscale
