// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_scale.h — shared harness for the SCALE / MEMORY live suite.
//
// ---------------------------------------------------------------------------
// WHY A SEPARATE TARGET, AND WHY IT HAS ITS OWN GATE
// ---------------------------------------------------------------------------
// The two-pass streaming design (DataSync.h's DiffRowChanges + DataSyncExec.h's
// ExecuteDataSync) exists for exactly one reason: to make a diff whose size
// exceeds memory possible at all. Its three load-bearing claims are
//
//   1. COMPARE materializes a bounded sample — at most RowChangeSet::kSampleCap
//      (500) RowChange objects — plus EXACT counts, with Truncated() set beyond
//      the cap. The bug it replaced held one wxString per differing row.
//   2. EXECUTE re-runs the diff and streams into 500-row ExecuteBatch calls, so
//      peak memory is one batch and not the whole diff.
//   3. Row READS are memory-flat on both sides, via the two-thread bounded-queue
//      sorted merge (DataSync.cpp's RowCursor, cap = DataSyncOptions::batchRows).
//
// Every one of those had only ever been observed on tables of 2-3000 rows
// (mysql_pg_live_exec2.cpp's item H is the largest, at 3000). That is the
// problem this suite exists to fix: a design that is memory-flat in ARGUMENT and
// quadratic in PRACTICE looks identical at 3000 rows. Flatness is not a property
// you can read off a source file — RowCursor's queue is visibly bounded, and
// that tells you nothing about what libmysqlclient, libpq, the wxString
// allocator or the merge's own temporaries do behind it.
//
// So this suite MEASURES, with GetProcessMemoryInfo, sampled on a polling thread
// so each phase gets its OWN peak rather than the process-lifetime high-water
// mark that PeakWorkingSetSize would give.
//
// THE MEASUREMENT THAT ACTUALLY ESTABLISHES FLATNESS is not one number, it is
// two: a SMALL diff and a LARGE diff over the SAME table, in that order. Table
// size is held constant so the only variable is how many rows DIFFER, which is
// precisely the quantity claims 1 and 2 say memory is independent of. Small runs
// first because a process allocator only ever grows — measuring large first and
// small second would let the large run's arena satisfy the small run and report
// "flat" for a design that was not. One point is an anecdote; the pair is the
// evidence.
//
// ---------------------------------------------------------------------------
// GATING: OPT-IN, NOT MERELY ENV-GATED
// ---------------------------------------------------------------------------
// Its siblings skip when SWIFTSQL_MYTEST_* / SWIFTSQL_PGTEST_* are unset, which
// is enough when a suite takes seconds. This one seeds and scans hundreds of
// thousands of rows on a SHARED server and takes minutes, so credentials alone
// must not be enough to trigger it: an engineer with live credentials set — the
// normal state for anyone working on sync — would otherwise have every ctest run
// dominated by it. It therefore needs BOTH the credentials AND an explicit
// SWIFTSQL_SCALETEST=1. Absent that it prints SKIP and exits 0.
//
// SWIFTSQL_SCALE_ROWS overrides the row count (default kDefaultRows) so the same
// binary can be smoke-run at 20k while developing and at 500k for the real
// measurement, without a rebuild.
//
// SAFETY: identical contract to every other live suite — nothing hard-coded,
// everything from the environment, and only databases carrying the
// `swiftsql_xscale_` prefix are touched, dropped on the way IN as well as out.
#pragma once

#include "mysql_pg_live_exec.h"   // ExpectStr/Dump/Scalar/MakeSpec/MakeAllow/…

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace mpscale {

using namespace db;
using namespace db::sync;
using mplive::g_checks;
using mplive::g_fails;
using mpexec::Allow;
using mpexec::Dump;
using mpexec::ExpectStr;
using mpexec::MakeAllow;
using mpexec::MakeSpec;
using mpexec::ReportOf;
using mpexec::Scalar;
using mpexec::ScalarInt;
using mpexec::ShowResult;

// The target size. 500k is the number at which the mechanisms are unambiguously
// engaged: 1000x the sample cap, 1000x the batch size, and ~122 progress ticks
// from the merge's every-4096-rows cadence.
constexpr long long kDefaultRows = 500000;

// How many rows differ in the SMALL-diff phase. Deliberately ABOVE the 500-row
// sample cap, so the small phase also has Truncated() set and the two phases
// differ ONLY in magnitude — if the small phase sat under the cap it would be
// exercising a different code path (nothing dropped) and the comparison would
// prove nothing about the drop path's cost.
constexpr long long kSmallDiff = 1000;

// ---------------------------------------------------------------------------
// Memory measurement
// ---------------------------------------------------------------------------

// One reading. Both numbers are reported because they answer different
// questions and one of them can lie:
//   * workingSetKb is PHYSICAL pages currently resident. The OS may trim it
//     under pressure, so a fall in working set is not evidence that anything
//     was freed.
//   * privateKb is PROCESS_MEMORY_COUNTERS_EX::PrivateUsage, i.e. private
//     commit charge — what this process has actually asked the OS to back. That
//     is the honest one for "did the diff materialize", because a materialized
//     500k-row diff must show up as commit whether or not it is resident.
struct MemSample {
    long long workingSetKb = 0;
    long long privateKb    = 0;
};

MemSample ReadMem();

// Per-phase peak, sampled on a polling thread.
//
// PeakWorkingSetSize cannot do this job: it is monotonic over the process
// lifetime, so once the large phase has run, every later phase reports the large
// phase's peak. Polling at 20 ms gives each phase an independent high-water mark.
// The sampler is a plain thread reading its OWN process's counters — it does not
// touch a database connection, so it cannot perturb what it measures.
class MemWatch {
public:
    explicit MemWatch(const char* label);
    ~MemWatch();

    MemWatch(const MemWatch&)            = delete;
    MemWatch& operator=(const MemWatch&) = delete;

    // Stop sampling and print the phase's account. Idempotent; the destructor
    // calls it so an early return still reports.
    void Finish();

    long long StartWorkingSetKb() const { return start_.workingSetKb; }
    long long StartPrivateKb()    const { return start_.privateKb; }
    long long PeakWorkingSetKb()  const { return peakWs_.load(); }
    long long PeakPrivateKb()     const { return peakPriv_.load(); }
    // The number the flatness claim is ABOUT: how much this phase's peak rose
    // above the memory already committed when it started. Using the raw peak
    // instead would fold in every earlier phase's retained arena and make a
    // genuinely flat run look like it grew.
    long long PeakPrivateRiseKb() const { return peakPriv_.load() - start_.privateKb; }
    long long ElapsedMs() const;

private:
    const char*            label_;
    MemSample              start_;
    std::atomic<long long> peakWs_{0}, peakPriv_{0};
    std::atomic<bool>      stop_{false};
    std::atomic<long long> elapsedMs_{0};
    std::chrono::steady_clock::time_point t0_;
    std::thread            th_;
    bool                   finished_ = false;
};

// ---------------------------------------------------------------------------
// What one measured phase produced
// ---------------------------------------------------------------------------

struct PhaseResult {
    bool          ran        = false;
    wxString      err;
    RowChangeStat stat;                 // compare phases only
    bool          truncated  = false;
    bool          executable = false;
    long long     progressTicks = 0;    // how often the gauge moved
    long long     lastScanned   = 0;    // merge's final rowsScanned (compare)
    long long     ms            = 0;
    long long     peakPrivateRiseKb  = 0;
    long long     peakWorkingSetKb   = 0;
    long long     startPrivateKb     = 0;

    double RowsPerSec(long long rows) const
    {
        return ms > 0 ? (double)rows * 1000.0 / (double)ms : 0.0;
    }
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

struct Ctx {
    IConnection* my = nullptr;      // MySQL, bound to myDb — the SOURCE
    IConnection* pg = nullptr;      // PostgreSQL, bound to pgDb — the TARGET
    wxString     myDb, pgDb;
    wxString     myHost, myUser, myPass;
    wxString     pgHost, pgUser, pgPass;
    int          myPort = 3306, pgPort = 5432;
    long long    rows   = kDefaultRows;

    // A second connection to the same PostgreSQL database. The engine-pressure
    // phase needs one to WATCH the writer's transaction from outside — asking
    // the writer about its own transaction from inside it would answer a
    // different question, and would serialize behind whatever it is doing.
    std::unique_ptr<IConnection> DialPg(wxString& err) const;
    std::unique_ptr<IConnection> DialMy(wxString& err) const;
};

// The table both sides carry, unqualified. Deliberately narrow (3 columns, ~40
// bytes/row): the subject is HOW MANY rows, not how wide, and a shared server
// should not be asked to carry a fat one to prove a point about counts.
inline const wchar_t* kTable = L"t_bulk";

// ---------------------------------------------------------------------------
// Seeding
// ---------------------------------------------------------------------------

// Fill `rows` rows into an already-created, EMPTY table, efficiently enough to
// be polite on a shared box: 1000 rows go over the wire, then the table doubles
// itself server-side (INSERT INTO t SELECT … FROM t) until it reaches the
// target. That is ~log2(n/1000) statements — 9 for 500k — instead of the 500k
// single-row inserts or 100 giant multi-row statements the obvious approaches
// would send. Returns false with the reason printed.
bool SeedTable(IConnection& c, Dialect d, const wxString& qualified,
               long long rows);

// ---------------------------------------------------------------------------
// The measured passes
// ---------------------------------------------------------------------------

// A compare pass, fully instrumented. Unlike mpexec::Compare1 this one wires
// DataSyncOptions::progress (so the merge's intra-table cadence is observable at
// all) and wraps the call in a MemWatch.
PhaseResult MeasuredCompare(Ctx& c, const DataDiffSpec& spec, const char* label);

// An execute pass, fully instrumented. `allow` is what the table may do.
PhaseResult MeasuredExecute(Ctx& c, const std::vector<TableExecJob>& jobs,
                            const char* label);

// Print one phase's numbers in a fixed, greppable shape so the small and large
// runs can be read off against each other without arithmetic.
void ReportPhase(const char* label, const PhaseResult& p, long long rowsTouched);

// ---------------------------------------------------------------------------
// The phases. Split across two TUs purely for the charter's 1000-line ceiling.
// ---------------------------------------------------------------------------

// Phase 1+2: the flatness measurement itself — small diff then large diff over
// the same table, plus exact counts and Truncated() at scale.
void PhaseA_MemoryCurve(Ctx& c);          // mysql_pg_live_scale.cpp

// Phase 3: progress cadence and MEASURED cancel latency on a large write.
void PhaseB_ProgressAndCancel(Ctx& c);    // mysql_pg_live_scale2.cpp

// Phase 4: what the per-table transaction does to the target engine — PostgreSQL
// WAL bytes and transaction age, watched from a second connection.
void PhaseC_EnginePressure(Ctx& c);       // mysql_pg_live_scale2.cpp

// Phase D: the TARGET-SIDE READ/WRITE COLLISION, isolated to a single variable.
//
// Runs FIRST, and on its own throwaway connections, because the failure it
// characterizes leaves the target connection unusable — every later statement on
// it fails — so it cannot share the connections the measuring phases need.
//
// What it establishes: the execute pass streams the TARGET side of the diff over
// the SAME IConnection it writes through (DataSyncExec.cpp's RunTableSweep hands
// one `tgt` to both StreamRowChanges and TableApplier). That is safe only while
// the target's entire result set drains into RowCursor's bounded queue before the
// first write is issued — i.e. only while the target table is SMALLER than
// DataSyncOptions::batchRows. Past that the write is issued against a connection
// with a query still in flight.
//
// The experiment varies exactly one thing at a time: target size against a fixed
// queue cap, then the queue cap against a fixed target size. If the failure
// follows the CAP rather than the SIZE, the mechanism is established rather than
// merely consistent with the evidence.
void PhaseD_TargetReadWriteCollision(Ctx& c);   // mysql_pg_live_scale3.cpp

} // namespace mpscale
