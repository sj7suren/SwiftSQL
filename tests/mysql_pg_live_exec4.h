// mysql_pg_live_exec4.h — shared harness for the SCHEMA-AWARE WRITE, CANCEL,
// CONNECTION-LOSS and FK-ORDERING live suite.
//
// ---------------------------------------------------------------------------
// WHY A FOURTH LIVE TARGET AND NOT MORE ITEMS IN mysql_pg_live_exec_test
// ---------------------------------------------------------------------------
// The existing execute suite (mysql_pg_live_exec*.cpp, 187 checks) proves the
// write path against a MySQL source and a PostgreSQL target whose tables all
// live in `public` and none of which has a foreign key. Four questions it
// structurally cannot ask were named by the engineers who ran it as the highest
// value things still unobserved, and each needs a fixture the existing one does
// not have:
//
//   1. WRITING into a table that is NOT in `public`. The qualified-name work
//      proved compare/plan/stream tell public.orders from archive.orders — the
//      READ half. No test has ever WRITTEN into archive.orders, and a write
//      landing in the wrong schema is precisely the failure that change exists
//      to prevent. It needs two same-named tables in two schemas, which the
//      exec suite's one-schema fixture cannot express.
//
//   2. CANCELLING mid-write. The stop token has never been tripped during an
//      actual write; every prior run passed the never-cancelled g_never. It
//      needs a table big enough to span several 500-row batches AND a second,
//      already-committed table to prove the per-table transaction boundary.
//
//   3. LOSING THE CONNECTION mid-batch. It needs a third connection standing by
//      to terminate the worker's own backend at a controlled moment.
//
//   4. FK ORDERING at execute time. Every existing execute-path fixture is
//      FK-FREE, so the two sweeps' ordering (INSERT/UPDATE forward, DELETE
//      reverse) has never been validated against a real foreign key at all —
//      not even same-schema. It needs real FK constraints on a target that
//      actually enforces them.
//
// ---------------------------------------------------------------------------
// WHY THE TARGET IS PostgreSQL FOR ALL FOUR
// ---------------------------------------------------------------------------
// Items 1 and 4's cross-schema half require a schema layer, which MySQL does
// not have (see QualifiedName.h's two-data-models note), so PostgreSQL is
// forced there. Item 4's SAME-schema half could in principle run against a
// MySQL target, and deliberately does not: SyncEngine::BuildPlan appends
// `SET FOREIGN_KEY_CHECKS=0` to the plan preamble for every MySQL target, so a
// MySQL run cannot distinguish correct FK ordering from no ordering at all —
// both pass. Asserting ordering there would be asserting nothing. PostgreSQL
// enforces every FK immediately and has no such escape hatch, so a
// wrong-ordered INSERT fails at the server, which is what makes the assertion
// mean something.
//
// ---------------------------------------------------------------------------
// THE RULE, INHERITED UNCHANGED FROM mysql_pg_live_exec.h
// ---------------------------------------------------------------------------
// Every assertion READS THE TARGET BACK. `Execute() returned true` is not
// evidence. For this suite there is a second, sharper form of the same rule:
// item 1 asserts on BOTH schemas every time, because "archive.orders has the
// right rows" and "public.orders was not touched" are two different claims and
// a write to the wrong schema can satisfy the first while breaking the second.
//
// SAFETY: identical contract to the other live suites — nothing hard-coded,
// everything from SWIFTSQL_PGTEST_*, skip cleanly when unset, and only ever
// touch databases carrying the `swiftsql_xexec4_` prefix (dropped on the way in
// AND out, so a previous crashed run cannot colour this one).
#pragma once

#include "mysql_pg_live_exec.h"   // ExpectStr/Dump/Scalar/MakeSpec/MakeAllow/…

#include "db/SyncEngine.h"

#include <memory>

namespace mpx4 {

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

// ---------------------------------------------------------------------------
// Everything an item needs to run. Passed by reference so an item can open its
// OWN extra connections (item 3 needs a third one to shoot the second with, and
// a fourth to read the wreckage back through, because the connection it is
// asking about is the one that just died).
// ---------------------------------------------------------------------------
struct Ctx {
    IConnection* src = nullptr;    // PostgreSQL, bound to srcDb
    IConnection* tgt = nullptr;    // PostgreSQL, bound to tgtDb
    wxString     srcDb, tgtDb;
    // Dial-out parameters, so an item can create additional connections of its
    // own without re-reading the environment (and without any item being able to
    // reach a server the harness did not already choose).
    wxString     host, user, pass;
    int          port = 5432;

    std::unique_ptr<IConnection> Dial(const wxString& db, wxString& err) const
    {
        auto c = CreateConnection(DbType::PostgreSQL);
        if (!c->Connect(mplive::Profile(DbType::PostgreSQL, host, port, user,
                                        pass, db), err))
            return nullptr;
        return c;
    }
};

// ---------------------------------------------------------------------------
// A CANCELLABLE execute run.
//
// mpexec::RunExec hard-codes the never-cancelled token, which is correct for
// every item that came before and useless for item 2. This variant owns a real
// std::atomic<bool> and hands the progress callback a hook that can trip it, so
// the cancel lands at a KNOWN point in the stream (after N applied rows) rather
// than at whatever moment a sleeping test thread happened to wake up. A
// wall-clock cancel would make the observation "what is in the target after a
// cancel?" depend on machine speed, i.e. would be flaky exactly where the answer
// matters most.
// ---------------------------------------------------------------------------
struct CancelExecOutcome {
    bool           ok = false;
    wxString       err;
    DataExecResult result;
    long long      progressCalls = 0;
    bool           tripped = false;      // did the hook actually fire?
    wxString       trippedAt;            // "table/phase/applied" when it did
};

// `shouldStop` is consulted on every progress tick; returning true trips the
// token from that moment on. It receives the same (table, phase, applied) the
// production UI's gauge receives, so a test can say "cancel this table after
// 1000 rows" in the vocabulary the executor actually speaks.
using StopWhen = std::function<bool(const wxString& table, const wxString& phase,
                                    long long applied)>;

CancelExecOutcome RunExecCancellable(IConnection& src, IConnection& tgt,
                                     const std::vector<TableExecJob>& jobs,
                                     const StopWhen& shouldStop);

// ---------------------------------------------------------------------------
// The four items. Split across two TUs purely for the charter's 1000-line
// ceiling — same reason mysql_pg_live_exec2/3.cpp exist.
// ---------------------------------------------------------------------------
void Item1_WriteIntoNonPublicSchema(Ctx& c);   // exec4.cpp
// Called by Item1: the sequence-repair write, which names its table inside a
// STRING LITERAL rather than as an identifier and so is a separate chance to
// lose the schema.
void Item1D_SequenceRepair(Ctx& c);            // exec4.cpp
void Item2_CancelMidWrite(Ctx& c);             // exec4.cpp
// Called by Item2; declared here rather than made static because it is the half
// that found a real defect and deserves to be nameable from outside the TU.
void Item2b_MixedSweepHonesty(Ctx& c);         // exec4.cpp
void Item3_ConnectionLossMidBatch(Ctx& c);     // exec5.cpp
void Item4_FkOrdering(Ctx& c);                 // exec5.cpp

} // namespace mpx4
