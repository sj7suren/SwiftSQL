// mysql_pg_live_scale.cpp — the harness, the memory instrumentation, and
// phase A (the flatness curve). See mysql_pg_live_scale.h for why this suite
// exists and why it has its own opt-in gate.

#include "mysql_pg_live_scale.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>

namespace mpscale {

// ---------------------------------------------------------------------------
// Memory measurement
// ---------------------------------------------------------------------------

MemSample ReadMem()
{
    MemSample m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    // The EX form is required for PrivateUsage; GetProcessMemoryInfo takes the
    // base struct and the size tells it which it really got.
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        m.workingSetKb = static_cast<long long>(pmc.WorkingSetSize / 1024);
        m.privateKb    = static_cast<long long>(pmc.PrivateUsage / 1024);
    }
    return m;
}

MemWatch::MemWatch(const char* label)
    : label_(label), start_(ReadMem()), t0_(std::chrono::steady_clock::now())
{
    peakWs_.store(start_.workingSetKb);
    peakPriv_.store(start_.privateKb);
    th_ = std::thread([this] {
        while (!stop_.load()) {
            const MemSample s = ReadMem();
            if (s.workingSetKb > peakWs_.load())  peakWs_.store(s.workingSetKb);
            if (s.privateKb    > peakPriv_.load()) peakPriv_.store(s.privateKb);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
}

MemWatch::~MemWatch() { Finish(); }

long long MemWatch::ElapsedMs() const { return elapsedMs_.load(); }

void MemWatch::Finish()
{
    if (finished_) return;
    finished_ = true;
    stop_.store(true);
    if (th_.joinable()) th_.join();
    // One last reading AFTER the thread is joined: a phase short enough to
    // finish between two 20 ms samples would otherwise report its starting
    // value as its peak.
    const MemSample s = ReadMem();
    if (s.workingSetKb > peakWs_.load())  peakWs_.store(s.workingSetKb);
    if (s.privateKb    > peakPriv_.load()) peakPriv_.store(s.privateKb);
    elapsedMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0_).count());
    std::printf("  MEM  %-22s start_priv=%lldKB peak_priv=%lldKB "
                "rise=%lldKB peak_ws=%lldKB  (%lld ms)\n",
                label_, start_.privateKb, peakPriv_.load(),
                peakPriv_.load() - start_.privateKb, peakWs_.load(),
                elapsedMs_.load());
}

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

std::unique_ptr<IConnection> Ctx::DialPg(wxString& err) const
{
    auto c = CreateConnection(DbType::PostgreSQL);
    if (!c->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort, pgUser,
                                    pgPass, pgDb), err))
        return nullptr;
    return c;
}

std::unique_ptr<IConnection> Ctx::DialMy(wxString& err) const
{
    auto c = CreateConnection(DbType::MySQL);
    if (!c->Connect(mplive::Profile(DbType::MySQL, myHost, myPort, myUser,
                                    myPass, myDb), err))
        return nullptr;
    return c;
}

// ---------------------------------------------------------------------------
// Seeding
// ---------------------------------------------------------------------------

namespace {

constexpr long long kSeedBase = 1000;   // rows sent over the wire before doubling

// The row text BOTH engines must produce identically. If MySQL's CONCAT and
// PostgreSQL's || ever disagreed here the compare would report N updates for
// two tables that are semantically identical, and the whole suite would be
// measuring a bug rather than a scale property — so the client-side seed uses
// this one expression for both and the server-side doubling mirrors it.
wxString RowValues(long long id)
{
    return wxString::Format(L"(%lld,'row-%lld',%lld)", id, id, id * 2);
}

} // namespace

bool SeedTable(IConnection& c, Dialect d, const wxString& qualified,
               long long rows)
{
    const long long base = std::min<long long>(kSeedBase, rows);

    // ---- the base, client-side, in one multi-row INSERT per 500 rows --------
    for (long long start = 1; start <= base; start += 500) {
        const long long end = std::min<long long>(start + 499, base);
        wxString values;
        for (long long id = start; id <= end; ++id) {
            if (!values.IsEmpty()) values += L",";
            values += RowValues(id);
        }
        if (!mplive::Exec(c, L"INSERT INTO " + qualified +
                             L" (id,name,v) VALUES " + values, "seed base"))
            return false;
    }

    // ---- doubling, server-side ---------------------------------------------
    // Each statement copies the rows already present, offset by the current
    // count. ~log2(rows/1000) round trips instead of thousands.
    const wxString nameExpr = (d == Dialect::MySQL)
        ? wxString(L"CONCAT('row-', id + %lld)")
        : wxString(L"'row-' || (id + %lld)::text");

    long long cur = base;
    while (cur < rows) {
        const long long take = std::min<long long>(cur, rows - cur);
        const wxString sql =
            L"INSERT INTO " + qualified + L" (id,name,v) SELECT id + " +
            wxString::Format(L"%lld, ", cur) +
            wxString::Format(nameExpr, cur) +
            wxString::Format(L", (id + %lld) * 2 FROM ", cur) + qualified +
            wxString::Format(L" WHERE id <= %lld", take);
        if (!mplive::Exec(c, sql, "seed double")) return false;
        cur += take;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The measured passes
// ---------------------------------------------------------------------------

PhaseResult MeasuredCompare(Ctx& c, const DataDiffSpec& spec, const char* label)
{
    PhaseResult p;

    DataSyncOptions opt;
    // The compare pass must see EVERY category regardless of what the user
    // intends to apply — otherwise the counts it reports are not the table's
    // counts. DataSync.h says so explicitly; this mirrors mpexec::Compare1.
    opt.insert = opt.update = opt.deleteMissing = true;
    opt.rowsEstimated = c.rows;
    opt.progress = [&p](const wxString&, long long scanned, long long) {
        ++p.progressTicks;
        p.lastScanned = scanned;
    };

    RowLayout   layout;
    RowChangeSet set;
    {
        MemWatch w(label);
        p.ran = DiffRowChanges(*c.my, *c.pg, spec, opt, layout, set, p.err,
                               mpexec::g_never);
        w.Finish();
        p.ms                = w.ElapsedMs();
        p.peakPrivateRiseKb = w.PeakPrivateRiseKb();
        p.peakWorkingSetKb  = w.PeakWorkingSetKb();
        p.startPrivateKb    = w.StartPrivateKb();
    }
    p.stat       = set.Stat();
    p.truncated  = set.Truncated();
    p.executable = set.Executable();
    return p;
}

PhaseResult MeasuredExecute(Ctx& c, const std::vector<TableExecJob>& jobs,
                            const char* label)
{
    PhaseResult p;
    DataSyncOptions base;
    DataExecResult  res;
    {
        MemWatch w(label);
        p.ran = ExecuteDataSync(*c.my, *c.pg, jobs, base, res, p.err,
                                mpexec::g_never,
            [&p](const wxString&, const wxString&, long long applied) {
                ++p.progressTicks;
                p.lastScanned = applied;
            });
        w.Finish();
        p.ms                = w.ElapsedMs();
        p.peakPrivateRiseKb = w.PeakPrivateRiseKb();
        p.peakWorkingSetKb  = w.PeakWorkingSetKb();
        p.startPrivateKb    = w.StartPrivateKb();
    }
    ShowResult(res, label);
    if (!p.ran) std::printf("  ERR  %s: %s\n", label, (const char*)p.err.utf8_str());
    // Keyed by the job's own SourceTable().Key() rather than kTable: it happens
    // to be the same string for this suite's MySQL source, but a bare-name
    // lookup is exactly what silently reported zero applied rows for the
    // PostgreSQL-source phases (see scale2/scale3), so no lookup here relies on
    // the two coinciding.
    const TableExecReport* rep = jobs.empty()
        ? nullptr : ReportOf(res, jobs.front().spec.SourceTable().Key());
    if (rep) {
        p.stat.inserts = rep->inserts;
        p.stat.updates = rep->updates;
        p.stat.deletes = rep->deletes;
        p.executable   = rep->committed;
    }
    return p;
}

void ReportPhase(const char* label, const PhaseResult& p, long long rowsTouched)
{
    std::printf("  OBS  %-22s ins=%lld upd=%lld del=%lld trunc=%d "
                "ticks=%lld %lld ms  %.0f rows/s  peak_rise=%lldKB\n",
                label, p.stat.inserts, p.stat.updates, p.stat.deletes,
                (int)p.truncated, p.progressTicks, p.ms,
                p.RowsPerSec(rowsTouched), p.peakPrivateRiseKb);
}

// ===========================================================================
// PHASE A — the flatness curve
// ===========================================================================
namespace {

// The ceiling a MATERIALIZING implementation could not meet. 500k RowChange
// objects — each carrying three converted Cells plus a rowKey wxString — cost
// on the order of 150-250 MB; the pre-fix design that held one rendered
// wxString per differing row costs comparably. 64 MB is therefore far above
// anything the streaming design should need (a bounded queue of 1000 rows plus
// one 500-row batch is well under 1 MB of row data) and far below anything the
// bug this design replaced could achieve. It is an absolute bound and not a
// ratio, so it stays meaningful even if the small phase's rise is ~0.
constexpr long long kFlatCeilingKb = 64 * 1024;

// The paired form of the same claim: growing the diff 500x must not grow peak
// memory by more than a fixed slack. Slack rather than a multiplier because the
// small phase's rise can legitimately be near zero, and 500x of near-zero is a
// meaningless bound.
constexpr long long kFlatSlackKb = 32 * 1024;

bool CreateTables(Ctx& c, const wxString& qMy)
{
    return mplive::Exec(*c.my, L"DROP TABLE IF EXISTS " + qMy + L"." + kTable,
                        "drop my bulk") &&
           mplive::Exec(*c.my, L"CREATE TABLE " + qMy + L"." + kTable + L" ("
                        L"  id INT NOT NULL PRIMARY KEY,"
                        L"  name VARCHAR(40) NOT NULL,"
                        L"  v BIGINT NOT NULL) ENGINE=InnoDB", "create my bulk") &&
           mplive::Exec(*c.pg, wxString(L"DROP TABLE IF EXISTS ") + kTable,
                        "drop pg bulk") &&
           mplive::Exec(*c.pg, wxString(L"CREATE TABLE ") + kTable + L" ("
                        L"  id integer NOT NULL PRIMARY KEY,"
                        L"  name varchar(40) NOT NULL,"
                        L"  v bigint NOT NULL)", "create pg bulk");
}

} // namespace

void PhaseA_MemoryCurve(Ctx& c)
{
    std::printf("\n== PHASE A: memory curve, small diff then large diff, "
                "%lld-row table ==\n", c.rows);

    const wxString qMy = QuoteIdent(c.myDb, Dialect::MySQL);
    if (!CreateTables(c, qMy)) {
        mplive::ExpectTrue("phaseA fixture: tables created", false);
        return;
    }

    // ---- seed both sides identically ---------------------------------------
    const auto seed0 = std::chrono::steady_clock::now();
    const bool seeded =
        SeedTable(*c.my, Dialect::MySQL, qMy + L"." + kTable, c.rows) &&
        SeedTable(*c.pg, Dialect::Postgres, kTable, c.rows);
    const long long seedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - seed0).count();
    if (!seeded) {
        mplive::ExpectTrue("phaseA fixture: both sides seeded", false);
        return;
    }
    std::printf("  OBS  seeded %lld rows on BOTH sides in %lld ms\n", c.rows, seedMs);
    mplive::ExpectEq("phaseA source really holds the requested row count",
                     ScalarInt(*c.my, L"SELECT COUNT(*) FROM " + qMy + L"." + kTable),
                     c.rows);
    mplive::ExpectEq("phaseA target really holds the requested row count",
                     ScalarInt(*c.pg, wxString(L"SELECT COUNT(*) FROM ") + kTable),
                     c.rows);

    // PostgreSQL will not choose an index scan for an unanalyzed table, and the
    // merge's ORDER BY id over 500k rows is the difference between an index scan
    // and a full sort-to-disk. Analyzing is what the production path's target
    // would also have had; not doing it would measure the planner's ignorance
    // rather than the sync engine.
    mplive::Exec(*c.pg, wxString(L"ANALYZE ") + kTable, "analyze pg bulk");

    DataDiffSpec spec;
    if (!MakeSpec(*c.my, *c.pg, c.myDb, c.pgDb, kTable, spec)) {
        mplive::ExpectTrue("phaseA spec built", false);
        return;
    }
    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec,
        MakeAllow(kTable, Allow{true, true, true}, true)});

    // =======================================================================
    // SMALL DIFF — same table, kSmallDiff rows differ
    // =======================================================================
    // Runs FIRST, deliberately: a process allocator only grows, so measuring
    // the large diff first would let its retained arena absorb the small run and
    // report flatness for a design that did not have it.
    std::printf("\n-- small diff: %lld of %lld rows differ --\n", kSmallDiff, c.rows);
    // BOTH PHASES USE AN INSERT-ONLY DIFF, AND THAT IS FORCED, NOT PREFERRED.
    //
    // Phase D establishes that a write emitted while the target's result stream
    // is still open collides with it on the shared connection — non-
    // deterministically, sometimes as a failed statement and a dead connection,
    // sometimes as a segfault. An UPDATE-based small diff trips that, wherever
    // the differing rows are placed: putting them at the HEAD collides
    // immediately, and putting them at the TAIL only *probably* avoids it,
    // because whether the reader has run ahead far enough to have finished is a
    // race against a 1000-row queue. A measurement standing on a race is not a
    // measurement.
    //
    // Deleting the LAST kSmallDiff target rows instead makes the diff pure
    // INSERT, and that is deterministically safe: the target cursor reaches
    // genuine end-of-stream at id N-kSmallDiff, which by construction requires
    // StreamRows to have returned, and only THEN does the merge start emitting
    // rows the target lacks. The connection is provably idle at the first write.
    //
    // It also makes the two phases a cleaner controlled comparison than an
    // update/insert pair would have been: small and large now differ ONLY in how
    // many rows the diff contains, which is exactly the variable the flatness
    // claim is about.
    if (!mplive::Exec(*c.pg, wxString::Format(
            L"DELETE FROM %s WHERE id > %lld", kTable, c.rows - kSmallDiff),
            "perturb pg for small diff")) {
        mplive::ExpectTrue("phaseA small-diff perturbation applied", false);
        return;
    }

    const PhaseResult smallCmp = MeasuredCompare(c, spec, "compare/small");
    ReportPhase("compare/small", smallCmp, c.rows);
    mplive::ExpectTrue("phaseA small compare ran", smallCmp.ran);
    mplive::ExpectEq("phaseA small compare counts inserts exactly",
                     smallCmp.stat.inserts, kSmallDiff);
    mplive::ExpectEq("phaseA small compare finds no updates",
                     smallCmp.stat.updates, 0);
    mplive::ExpectEq("phaseA small compare finds no deletes",
                     smallCmp.stat.deletes, 0);
    // kSmallDiff (1000) is above the 500-row sample cap, so the SMALL phase is
    // truncated too. Both phases therefore exercise the drop path and differ
    // only in magnitude — if the small one sat under the cap it would be running
    // different code and the comparison would prove nothing.
    mplive::ExpectTrue("phaseA small compare is also truncated past the cap",
                       smallCmp.truncated);
    // The unchanged counter is the one number that tells a clean compare apart
    // from a compare that read nothing at all (RowChange.h documents exactly
    // this). At scale it is also the proof the full scan really happened.
    mplive::ExpectEq("phaseA small compare counted every unchanged row",
                     smallCmp.stat.unchanged, c.rows - kSmallDiff);

    const PhaseResult smallExec = MeasuredExecute(c, jobs, "execute/small");
    ReportPhase("execute/small", smallExec, kSmallDiff);
    mplive::ExpectTrue("phaseA small execute ran", smallExec.ran);
    mplive::ExpectEq("phaseA small execute applied exactly the differing rows",
                     smallExec.stat.inserts, kSmallDiff);
    mplive::ExpectEq("phaseA small execute converged the target",
                     ScalarInt(*c.pg, wxString(L"SELECT COUNT(*) FROM ") + kTable),
                     c.rows);

    // =======================================================================
    // LARGE DIFF — same table, EVERY row differs
    // =======================================================================
    std::printf("\n-- large diff: all %lld rows differ (target emptied) --\n", c.rows);
    if (!mplive::Exec(*c.pg, wxString(L"TRUNCATE ") + kTable, "empty pg for large diff")) {
        mplive::ExpectTrue("phaseA large-diff perturbation applied", false);
        return;
    }
    mplive::Exec(*c.pg, wxString(L"ANALYZE ") + kTable, "analyze pg bulk (empty)");

    const PhaseResult bigCmp = MeasuredCompare(c, spec, "compare/large");
    ReportPhase("compare/large", bigCmp, c.rows);
    mplive::ExpectTrue("phaseA large compare ran", bigCmp.ran);
    // THE COUNT CLAIM AT SCALE. The counts come from a full scan while only 500
    // rows are retained, so this asserts the scan and the sample are genuinely
    // independent — a design that counted its sample would report 500 here.
    mplive::ExpectEq("phaseA large compare reports the EXACT insert count, "
                     "not the sample cap", bigCmp.stat.inserts, c.rows);
    mplive::ExpectEq("phaseA large compare finds no updates", bigCmp.stat.updates, 0);
    mplive::ExpectEq("phaseA large compare finds no deletes", bigCmp.stat.deletes, 0);
    mplive::ExpectEq("phaseA large compare counted no unchanged rows",
                     bigCmp.stat.unchanged, 0);
    mplive::ExpectTrue("phaseA large compare sets Truncated() past the 500-row cap",
                       bigCmp.truncated);
    mplive::ExpectTrue("phaseA large compare stayed executable", bigCmp.executable);

    // Intra-table progress during the COMPARE (the merge's every-4096-rows
    // cadence). A 500k-row scan must move the gauge ~122 times; firing once per
    // table would leave a frozen dialog for the whole scan.
    // Tied to the DOCUMENTED cadence rather than a magic number, so the
    // assertion means the same thing at 20k as at 500k. DataSync.h promises one
    // tick per ~4096 scanned rows; half that is generous slack for the exact
    // definition of "scanned" (it counts rows pulled from BOTH cursors) while
    // still excluding the failure being tested, which is "fires once per table".
    const long long expectTicks = c.rows / 4096;
    std::printf("  OBS  compare/large progress ticks=%lld lastScanned=%lld "
                "(expect ~%lld at one per 4096 scanned rows)\n",
                bigCmp.progressTicks, bigCmp.lastScanned, expectTicks);
    mplive::ExpectTrue("phaseA compare progress fires throughout the scan, "
                       "not once per table",
                       bigCmp.progressTicks >= std::max<long long>(2, expectTicks / 2));

    const PhaseResult bigExec = MeasuredExecute(c, jobs, "execute/large");
    ReportPhase("execute/large", bigExec, c.rows);
    mplive::ExpectTrue("phaseA large execute ran", bigExec.ran);
    mplive::ExpectEq("phaseA large execute applied every row",
                     bigExec.stat.inserts, c.rows);
    mplive::ExpectTrue("phaseA large execute committed", bigExec.executable);
    // The write side reports once per applied batch (500 rows), so the expected
    // count is rows/500; same half-slack rationale as the compare side.
    mplive::ExpectTrue("phaseA execute progress fires throughout the write",
                       bigExec.progressTicks >=
                           std::max<long long>(2, (c.rows / 500) / 2));

    // ---- the target, exactly ------------------------------------------------
    mplive::ExpectEq("phaseA target row count exact",
                     ScalarInt(*c.pg, wxString(L"SELECT COUNT(*) FROM ") + kTable),
                     c.rows);
    mplive::ExpectEq("phaseA every id present exactly once",
                     ScalarInt(*c.pg, wxString(L"SELECT COUNT(DISTINCT id) FROM ") + kTable),
                     c.rows);
    mplive::ExpectEq("phaseA id checksum",
                     ScalarInt(*c.pg, wxString(L"SELECT COALESCE(SUM(id),0) FROM ") + kTable),
                     c.rows * (c.rows + 1) / 2);
    mplive::ExpectEq("phaseA no row has a malformed name or value",
                     ScalarInt(*c.pg, wxString::Format(
                         L"SELECT COUNT(*) FROM %s WHERE name <> ('row-' || id::text) "
                         L"OR v <> id * 2", kTable)), 0);

    const PhaseResult again = MeasuredCompare(c, spec, "compare/converged");
    ReportPhase("compare/converged", again, c.rows);
    mplive::ExpectEq("phaseA re-compare finds nothing left to do",
                     again.stat.Total(), 0);
    mplive::ExpectTrue("phaseA re-compare is not truncated (nothing differs)",
                       !again.truncated);

    // =======================================================================
    // THE VERDICT: is peak memory flat with respect to DIFF SIZE?
    // =======================================================================
    const long long ratio = kSmallDiff > 0 ? c.rows / kSmallDiff : 0;
    std::printf("\n-- flatness: diff grew %lldx (%lld -> %lld rows) --\n",
                ratio, kSmallDiff, c.rows);
    std::printf("  OBS  compare peak rise: small=%lldKB large=%lldKB (delta %lldKB)\n",
                smallCmp.peakPrivateRiseKb, bigCmp.peakPrivateRiseKb,
                bigCmp.peakPrivateRiseKb - smallCmp.peakPrivateRiseKb);
    std::printf("  OBS  execute peak rise: small=%lldKB large=%lldKB (delta %lldKB)\n",
                smallExec.peakPrivateRiseKb, bigExec.peakPrivateRiseKb,
                bigExec.peakPrivateRiseKb - smallExec.peakPrivateRiseKb);

    mplive::ExpectTrue("phaseA COMPARE peak memory stays under the ceiling a "
                       "materializing diff could not meet",
                       bigCmp.peakPrivateRiseKb < kFlatCeilingKb);
    mplive::ExpectTrue("phaseA EXECUTE peak memory stays under the same ceiling",
                       bigExec.peakPrivateRiseKb < kFlatCeilingKb);
    mplive::ExpectTrue("phaseA COMPARE peak memory does not scale with diff size",
                       bigCmp.peakPrivateRiseKb <=
                           smallCmp.peakPrivateRiseKb + kFlatSlackKb);
    mplive::ExpectTrue("phaseA EXECUTE peak memory does not scale with diff size",
                       bigExec.peakPrivateRiseKb <=
                           smallExec.peakPrivateRiseKb + kFlatSlackKb);

    // Throughput, recorded rather than asserted: it is a property of this
    // server on this day, and pinning a number would make the suite fail for
    // reasons that have nothing to do with the code.
    std::printf("  OBS  throughput: compare/large %.0f rows/s, "
                "execute/large %.0f rows/s\n",
                bigCmp.RowsPerSec(c.rows), bigExec.RowsPerSec(c.rows));
}

} // namespace mpscale

// ===========================================================================
// main
// ===========================================================================
int main()
{
    using namespace mpscale;

    // UNBUFFERED, deliberately. This suite runs for minutes and puts the engine
    // under a kind of pressure nothing else here does, so it is the suite most
    // likely to die partway through. With the default block buffering a crash
    // discards every observation made before it and leaves an empty log — which
    // is exactly the run whose output is most worth having.
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("== mysql_pg_live_scale (two-pass streaming sync at scale, live) ==\n");

    // ---- the opt-in gate ---------------------------------------------------
    // Credentials alone are NOT enough; see the header. This suite seeds and
    // scans hundreds of thousands of rows on a shared server.
    if (mplive::Env("SWIFTSQL_SCALETEST") != L"1") {
        std::printf("SKIP: scale suite is opt-in (set SWIFTSQL_SCALETEST=1). "
                    "It seeds ~500k rows on a shared server and takes minutes.\n");
        return 0;
    }

    const wxString myHost = mplive::Env("SWIFTSQL_MYTEST_HOST");
    const wxString myUser = mplive::Env("SWIFTSQL_MYTEST_USER");
    const wxString myPass = mplive::Env("SWIFTSQL_MYTEST_PASS");
    const wxString pgHost = mplive::Env("SWIFTSQL_PGTEST_HOST");
    const wxString pgUser = mplive::Env("SWIFTSQL_PGTEST_USER");
    const wxString pgPass = mplive::Env("SWIFTSQL_PGTEST_PASS");
    if (myHost.IsEmpty() || myUser.IsEmpty() || pgHost.IsEmpty() || pgUser.IsEmpty()) {
        std::printf("SKIP: no live MySQL+PostgreSQL configured "
                    "(set SWIFTSQL_MYTEST_* and SWIFTSQL_PGTEST_*)\n");
        return 0;
    }
    const int myPort = mplive::EnvInt("SWIFTSQL_MYTEST_PORT", 3306);
    const int pgPort = mplive::EnvInt("SWIFTSQL_PGTEST_PORT", 5432);

    long long rows = kDefaultRows;
    {
        const wxString r = mplive::Env("SWIFTSQL_SCALE_ROWS");
        long long v = 0;
        if (!r.IsEmpty() && r.ToLongLong(&v) && v >= kSmallDiff * 2) rows = v;
    }

    // Own prefix, distinct from every other live suite's.
    const wxString myDb = L"swiftsql_xscale_src";
    const wxString pgDb = L"swiftsql_xscale_tgt";
    const wxString qMy  = QuoteIdent(myDb, Dialect::MySQL);
    const wxString qPg  = QuoteIdent(pgDb, Dialect::Postgres);

    wxString boot = mplive::Env("SWIFTSQL_PGTEST_DB");
    if (boot.IsEmpty()) boot = L"postgres";

    auto myMaint = CreateConnection(DbType::MySQL);
    auto pgMaint = CreateConnection(DbType::PostgreSQL);
    wxString e1, e2;
    if (!myMaint->Connect(mplive::Profile(DbType::MySQL, myHost, myPort, myUser,
                                          myPass, wxString()), e1)) {
        std::printf("SKIP: MySQL connect failed: %s\n", (const char*)e1.utf8_str());
        return 0;
    }
    if (!pgMaint->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort,
                                          pgUser, pgPass, boot), e2)) {
        std::printf("SKIP: PostgreSQL connect failed: %s\n", (const char*)e2.utf8_str());
        return 0;
    }
    mplive::Observe("mysql server version", myMaint->ServerVersion());
    mplive::Observe("pg server version", pgMaint->ServerVersion());
    std::printf("  OBS  row count for this run: %lld\n", rows);

    // Dropped on the way IN as well as out, so a previous crashed run cannot
    // colour this one.
    mplive::Exec(*myMaint, L"DROP DATABASE IF EXISTS " + qMy, "drop my (pre)");
    mplive::Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + qPg, "drop pg (pre)");
    const bool provisioned =
        mplive::Exec(*myMaint, L"CREATE DATABASE " + qMy, "create my") &&
        mplive::Exec(*pgMaint, L"CREATE DATABASE " + qPg, "create pg");

    std::unique_ptr<IConnection> my, pg;
    struct Guard {
        IConnection*                  myMaint;
        IConnection*                  pgMaint;
        std::unique_ptr<IConnection>* my;
        std::unique_ptr<IConnection>* pg;
        wxString                      qMy, qPg;
        ~Guard()
        {
            // Disconnect first: PostgreSQL refuses to DROP a database that
            // still has a session attached, so a live handle here would leave a
            // 500k-row table behind on a shared server.
            for (auto* p : { my, pg }) if (p && *p) (*p)->Disconnect();
            QueryResult r; wxString e;
            if (myMaint) myMaint->Execute(L"DROP DATABASE IF EXISTS " + qMy, r, e);
            if (pgMaint) pgMaint->Execute(L"DROP DATABASE IF EXISTS " + qPg, r, e);
        }
    };

    my = CreateConnection(DbType::MySQL);
    pg = CreateConnection(DbType::PostgreSQL);
    Guard guard{ myMaint.get(), pgMaint.get(), &my, &pg, qMy, qPg };

    mplive::ExpectTrue("provision two throwaway databases", provisioned);
    if (!provisioned) {
        std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
        return 1;
    }

    {
        wxString a, b;
        const bool okMy = my->Connect(mplive::Profile(DbType::MySQL, myHost, myPort,
                                                      myUser, myPass, myDb), a);
        const bool okPg = pg->Connect(mplive::Profile(DbType::PostgreSQL, pgHost,
                                                      pgPort, pgUser, pgPass, pgDb), b);
        mplive::ExpectTrue("connect source and target", okMy && okPg);
        if (!(okMy && okPg)) {
            std::printf("  errs: %s / %s\n", (const char*)a.utf8_str(),
                        (const char*)b.utf8_str());
            std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
            return 1;
        }
    }

    Ctx ctx;
    ctx.my = my.get();
    ctx.pg = pg.get();
    ctx.myDb = myDb; ctx.pgDb = pgDb;
    ctx.myHost = myHost; ctx.myUser = myUser; ctx.myPass = myPass; ctx.myPort = myPort;
    ctx.pgHost = pgHost; ctx.pgUser = pgUser; ctx.pgPass = pgPass; ctx.pgPort = pgPort;
    ctx.rows = rows;

    const MemSample atStart = ReadMem();
    std::printf("  MEM  %-22s start_priv=%lldKB peak_ws=%lldKB\n",
                "baseline/connected", atStart.privateKb, atStart.workingSetKb);

    PhaseA_MemoryCurve(ctx);
    PhaseB_ProgressAndCancel(ctx);
    PhaseC_EnginePressure(ctx);

    // Phase D runs LAST, on its own throwaway connections, for the same reason
    // mysql_pg_live_exec4.cpp runs its backend-killing item last: it provokes a
    // failure that has been observed to segfault the process outright. Every
    // measurement above has already printed by the time it starts, and the
    // running total below means a crash still leaves a usable record rather than
    // discarding three phases of numbers.
    std::printf("\n-- running total before phase D: %d checks, %d failed --\n",
                g_checks, g_fails);
    PhaseD_TargetReadWriteCollision(ctx);

    const MemSample atEnd = ReadMem();
    std::printf("\n  MEM  %-22s priv=%lldKB ws=%lldKB (baseline was %lldKB priv)\n",
                "final/after all phases", atEnd.privateKb, atEnd.workingSetKb,
                atStart.privateKb);

    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;   // guard drops both databases on the way out
}
