// mysql_pg_live_exec4.cpp — see mysql_pg_live_exec4.h for why this suite exists.
//
// This TU carries the harness (main, provisioning, the cancellable run) plus
// items 1 and 2. Items 3 and 4 are in mysql_pg_live_exec5.cpp.

#include "mysql_pg_live_exec4.h"

#include <algorithm>
#include <cstdio>

namespace mpx4 {

// ---------------------------------------------------------------------------
// The cancellable run. See the header for why the cancel is tripped from the
// progress callback rather than from a timer.
// ---------------------------------------------------------------------------
CancelExecOutcome RunExecCancellable(IConnection& src, IConnection& tgt,
                                     const std::vector<TableExecJob>& jobs,
                                     const StopWhen& shouldStop)
{
    CancelExecOutcome o;
    std::atomic<bool>  stop{false};
    DataSyncOptions    base;

    o.ok = ExecuteDataSync(src, tgt, jobs, base, o.result, o.err, stop,
        [&](const wxString& table, const wxString& phase, long long applied) {
            ++o.progressCalls;
            if (!o.tripped && shouldStop && shouldStop(table, phase, applied)) {
                o.tripped   = true;
                o.trippedAt = table + L"/" + phase +
                              wxString::Format(L"/%lld", applied);
                stop.store(true);
            }
        });
    return o;
}

namespace {

// ---------------------------------------------------------------------------
// A spec builder that takes QUALIFIED names.
//
// mpexec::MakeSpec takes a bare wxString, which converts to an UNQUALIFIED
// QualifiedName and therefore resolves through PostgreSQL's search_path — i.e.
// it always finds `public`. That is fine for a suite whose tables are all in
// public and disqualifying for this one: item 1's entire subject is the
// difference between public.orders and archive.orders, so it must be able to
// name the one it means.
// ---------------------------------------------------------------------------
bool MakeSpecQ(IConnection& src, IConnection& tgt, const wxString& srcDb,
               const wxString& tgtDb, const QualifiedName& srcTable,
               const QualifiedName& tgtTable, DataDiffSpec& out)
{
    wxString e1, e2;
    if (!src.GetTableSchema(srcDb, srcTable, out.srcSchema, e1)) {
        std::printf("  ERR  GetTableSchema(src,%s): %s\n",
                    (const char*)srcTable.Key().utf8_str(),
                    (const char*)e1.utf8_str());
        return false;
    }
    if (!tgt.GetTableSchema(tgtDb, tgtTable, out.tgtSchema, e2)) {
        std::printf("  ERR  GetTableSchema(tgt,%s): %s\n",
                    (const char*)tgtTable.Key().utf8_str(),
                    (const char*)e2.utf8_str());
        return false;
    }
    // An empty column list is DiffSchema's "table does not exist" convention.
    // Catching it HERE matters: a spec built over a table the driver could not
    // see would make every downstream assertion fail for a setup reason and
    // look exactly like the product defect this suite hunts.
    if (out.srcSchema.columns.empty() || out.tgtSchema.columns.empty()) {
        std::printf("  ERR  spec %s->%s: empty schema (src cols=%d tgt cols=%d)\n",
                    (const char*)srcTable.Key().utf8_str(),
                    (const char*)tgtTable.Key().utf8_str(),
                    (int)out.srcSchema.columns.size(),
                    (int)out.tgtSchema.columns.size());
        return false;
    }
    out.srcDb = srcDb;
    out.tgtDb = tgtDb;
    return true;
}

bool Run(IConnection& c, const wxString& sql, const char* label)
{
    return mplive::Exec(c, sql, label);
}

// ===========================================================================
// ITEM 1 — writing into a table that is not in `public`
// ===========================================================================
//
// FIXTURE DESIGN, AND WHY THE TWO TABLES ARE STRUCTURALLY IDENTICAL.
//
// mysql_pg_live_schema.cpp deliberately gave archive.orders an EXTRA column, so
// that a reader which silently substituted one schema for the other returned a
// column list that did not match the table it claimed to describe. That is the
// right shape for a READ test and the WRONG shape for this one: if the two
// tables differ structurally, a write aimed at the wrong schema fails with a
// column error, and the test would pass for a reason that has nothing to do
// with schema targeting. It would prove PostgreSQL rejects bad SQL.
//
// So here the two tables are byte-identical in structure and differ ONLY in
// their contents (public: amount 100/200/300 tagged 'pub'; archive: 900/901/902
// tagged 'arc'). A write that lands in the wrong schema therefore SUCCEEDS at
// the server, and the only thing that can catch it is reading both schemas back
// — which is exactly the failure mode being hunted, and exactly why every
// assertion below reads BOTH.
// ---------------------------------------------------------------------------
bool SetupItem1Src(IConnection& src)
{
    return Run(src, L"DROP SCHEMA IF EXISTS archive CASCADE", "src drop archive") &&
           Run(src, L"CREATE SCHEMA archive", "src create archive") &&
           Run(src, L"DROP TABLE IF EXISTS public.orders", "src drop public.orders") &&
           Run(src, L"CREATE TABLE public.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
               "src create public.orders") &&
           Run(src, L"CREATE TABLE archive.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
               "src create archive.orders") &&
           Run(src, L"INSERT INTO public.orders VALUES "
                    L"(1,100,'pub'),(2,200,'pub'),(3,300,'pub')",
               "src seed public.orders") &&
           Run(src, L"INSERT INTO archive.orders VALUES "
                    L"(1,900,'arc'),(2,901,'arc'),(3,902,'arc')",
               "src seed archive.orders");
}

// The target starts SHORT in both schemas — one row each — so both tables have
// a real, non-empty insert diff. A target where only the intended table had
// work to do would let a mis-aimed write be absorbed as a no-op.
bool SetupItem1Tgt(IConnection& tgt)
{
    return Run(tgt, L"DROP SCHEMA IF EXISTS archive CASCADE", "tgt drop archive") &&
           Run(tgt, L"CREATE SCHEMA archive", "tgt create archive") &&
           Run(tgt, L"DROP TABLE IF EXISTS public.orders", "tgt drop public.orders") &&
           Run(tgt, L"CREATE TABLE public.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
               "tgt create public.orders") &&
           Run(tgt, L"CREATE TABLE archive.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
               "tgt create archive.orders") &&
           Run(tgt, L"INSERT INTO public.orders VALUES (1,100,'pub')",
               "tgt seed public.orders") &&
           Run(tgt, L"INSERT INTO archive.orders VALUES (1,900,'arc')",
               "tgt seed archive.orders");
}

// Read one schema's table back, fully and in a fixed order. Schema-qualified in
// the SQL on purpose: a read that relied on search_path would be subject to the
// same ambiguity the product is being tested for, and could not be trusted to
// say which table it had looked at.
wxString DumpOrders(IConnection& c, const wxChar* schema)
{
    return Dump(c, wxString::Format(
        L"SELECT id, amount, tag FROM %s.orders ORDER BY id", schema));
}

void Item1Phase(IConnection& src, IConnection& tgt, const wxString& srcDb,
                const wxString& tgtDb, const Allow& allow, bool deleteSwitch,
                const char* tag, const wxString& wantArchive,
                const wxString& wantPublic)
{
    DataDiffSpec spec;
    const QualifiedName archive(L"archive", L"orders");
    if (!MakeSpecQ(src, tgt, srcDb, tgtDb, archive, archive, spec)) {
        ++g_fails;
        return;
    }

    // The spec must name archive on BOTH sides. If either side resolved to
    // public, everything after this is measuring the wrong table pair and the
    // read-backs below would be meaningless rather than merely failing.
    ExpectStr("item1 spec source is archive.orders", spec.SourceTable().Key(),
              L"archive.orders");
    ExpectStr("item1 spec target is archive.orders", spec.TargetTable().Key(),
              L"archive.orders");

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(spec.SourceTable().Key(), allow,
                                                deleteSwitch)});

    mpexec::ExecOutcome o = mpexec::RunExec(src, tgt, jobs);
    ShowResult(o.result, tag);
    mplive::ExpectTrue(tag, o.ok);
    if (!o.ok)
        std::printf("  ERR  %s: %s\n", tag, (const char*)o.err.utf8_str());

    // THE TWO CLAIMS, ASSERTED SEPARATELY.
    const wxString gotArchive = DumpOrders(tgt, L"archive");
    const wxString gotPublic  = DumpOrders(tgt, L"public");
    ExpectStr((wxString(tag) + L" — archive.orders got the rows").utf8_str(),
              gotArchive, wantArchive);
    ExpectStr((wxString(tag) + L" — public.orders untouched").utf8_str(),
              gotPublic, wantPublic);
}

} // namespace

// ---------------------------------------------------------------------------
void Item1_WriteIntoNonPublicSchema(Ctx& c)
{
    std::printf("\n== ITEM 1: ExecuteDataSync writes into archive.orders, "
                "and ONLY archive.orders ==\n");

    if (!SetupItem1Src(*c.src) || !SetupItem1Tgt(*c.tgt)) {
        std::printf("  ERR  item 1 fixture setup failed\n");
        ++g_fails;
        return;
    }

    // Baseline, so the phases below are measured against an observed starting
    // point rather than an assumed one.
    ExpectStr("item1 baseline archive.orders", DumpOrders(*c.tgt, L"archive"),
              L"1|900|arc");
    ExpectStr("item1 baseline public.orders", DumpOrders(*c.tgt, L"public"),
              L"1|100|pub");

    // ---- phase A: INSERT (the ExecuteBatch path) --------------------------
    // Both target tables are short by two rows. Only archive is authorized.
    Item1Phase(*c.src, *c.tgt, c.srcDb, c.tgtDb, Allow{true, false, false}, false,
               "item1A insert into archive.orders",
               L"1|900|arc;2|901|arc;3|902|arc",   // archive converged
               L"1|100|pub");                      // public still one row

    // ---- phase B: UPDATE (the RenderRowDml path) --------------------------
    // A DIFFERENT code path from phase A: inserts go through
    // IConnection::ExecuteBatch, updates through a rendered single statement.
    // Both build their table name from TableApplier::qualified_, but they are
    // separately capable of losing it, so both are exercised.
    //
    // Perturb the SAME row id in BOTH schemas to the SAME wrong value. If the
    // update landed in public, public's 111 would be corrected to 100 and
    // archive's would stay 111 — the exact inverse of the expectation, which is
    // a far stronger signal than "archive is right".
    Run(*c.tgt, L"UPDATE archive.orders SET amount=111 WHERE id=1", "perturb archive");
    Run(*c.tgt, L"UPDATE public.orders  SET amount=111 WHERE id=1", "perturb public");
    ExpectStr("item1B perturbation is in place (archive)",
              DumpOrders(*c.tgt, L"archive"), L"1|111|arc;2|901|arc;3|902|arc");
    ExpectStr("item1B perturbation is in place (public)",
              DumpOrders(*c.tgt, L"public"), L"1|111|pub");

    Item1Phase(*c.src, *c.tgt, c.srcDb, c.tgtDb, Allow{false, true, false}, false,
               "item1B update in archive.orders",
               L"1|900|arc;2|901|arc;3|902|arc",   // archive's 111 corrected
               L"1|111|pub");                      // public's 111 left alone

    // ---- phase C: DELETE (the reverse sweep, rendered per row) ------------
    // An extra row the source does not have, in BOTH schemas. Only archive's
    // may go. This is the most destructive verb in the system and the one where
    // a wrong-schema write is least recoverable, so it gets its own phase
    // rather than riding along with the others.
    Run(*c.tgt, L"INSERT INTO archive.orders VALUES (99,999,'extra')", "extra archive");
    Run(*c.tgt, L"INSERT INTO public.orders  VALUES (99,999,'extra')", "extra public");
    ExpectStr("item1C extra row is in place (archive)",
              DumpOrders(*c.tgt, L"archive"),
              L"1|900|arc;2|901|arc;3|902|arc;99|999|extra");
    ExpectStr("item1C extra row is in place (public)",
              DumpOrders(*c.tgt, L"public"), L"1|111|pub;99|999|extra");

    Item1Phase(*c.src, *c.tgt, c.srcDb, c.tgtDb, Allow{false, false, true}, true,
               "item1C delete from archive.orders",
               L"1|900|arc;2|901|arc;3|902|arc",   // archive's extra row gone
               L"1|111|pub;99|999|extra");         // public's extra row kept

    Item1D_SequenceRepair(c);
}

// ---------------------------------------------------------------------------
// ITEM 1d — the SEQUENCE-REPAIR write, which is a fourth schema-sensitive path
//
// Phases A-C cover the three DML verbs. There is a fourth thing this executor
// writes, and it is the one most likely to get a schema wrong because it does
// not name the table in SQL syntax at all: TableApplier::ApplySequenceFixes
// advances the target's identity generator past the values just written, via
//
//     setval(pg_get_serial_sequence('<qualified table>', '<column>'), …)
//
// where the qualified table is passed as a STRING LITERAL for PostgreSQL to
// parse, not as an identifier the parser sees directly. That is a genuinely
// different failure surface from an INSERT: a bare `seqt` in that literal would
// resolve through search_path to public's sequence and silently advance the
// WRONG schema's generator, leaving archive's untouched. Nothing about the row
// data would look wrong, and the damage would surface later as a duplicate-key
// failure on the next ordinary insert — into a table nobody had synced.
//
// Both schemas get their own `serial` column so the two sequences exist and are
// independently observable, and the assertion reads BOTH, as everywhere else in
// item 1.
// ---------------------------------------------------------------------------
namespace {

bool SetupItem1dSrc(IConnection& src)
{
    return Run(src, L"CREATE TABLE public.seqt("
                    L"id serial PRIMARY KEY, v int NOT NULL)", "src create public.seqt") &&
           Run(src, L"CREATE TABLE archive.seqt("
                    L"id serial PRIMARY KEY, v int NOT NULL)", "src create archive.seqt") &&
           Run(src, L"INSERT INTO public.seqt(v)  VALUES (1),(2),(3)", "src seed public.seqt") &&
           Run(src, L"INSERT INTO archive.seqt(v) VALUES (91),(92),(93)", "src seed archive.seqt");
}

bool SetupItem1dTgt(IConnection& tgt)
{
    return Run(tgt, L"CREATE TABLE public.seqt("
                    L"id serial PRIMARY KEY, v int NOT NULL)", "tgt create public.seqt") &&
           Run(tgt, L"CREATE TABLE archive.seqt("
                    L"id serial PRIMARY KEY, v int NOT NULL)", "tgt create archive.seqt");
}

} // namespace

void Item1D_SequenceRepair(Ctx& c)
{
    std::printf("\n-- item 1d: the sequence repair must advance archive's "
                "generator, not public's --\n");

    if (!SetupItem1dSrc(*c.src) || !SetupItem1dTgt(*c.tgt)) {
        std::printf("  ERR  item 1d fixture setup failed\n");
        ++g_fails;
        return;
    }

    DataDiffSpec spec;
    const QualifiedName seqt(L"archive", L"seqt");
    if (!MakeSpecQ(*c.src, *c.tgt, c.srcDb, c.tgtDb, seqt, seqt, spec)) {
        ++g_fails;
        return;
    }
    // If the target column were not recognized as auto-increment, no fix would
    // be built and the nextval assertions below would pass for the wrong reason
    // (an untouched sequence still sitting at 1 in BOTH schemas). Pin it.
    const NormColumn* idCol = spec.tgtSchema.FindColumn(L"id");
    mplive::ExpectTrue("item1d the target's serial column is seen as "
                       "auto-increment (so a fix is actually built)",
                       idCol && idCol->autoIncrement);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec,
        MakeAllow(spec.SourceTable().Key(), Allow{true, false, false}, false)});
    mpexec::ExecOutcome o = mpexec::RunExec(*c.src, *c.tgt, jobs);
    ShowResult(o.result, "item1d");   // prints any sequence-repair warning
    mplive::ExpectTrue("item1d the run succeeds", o.ok);
    if (!o.ok) std::printf("  ERR  item1d: %s\n", (const char*)o.err.utf8_str());

    ExpectStr("item1d archive.seqt got the rows",
              Dump(*c.tgt, L"SELECT id, v FROM archive.seqt ORDER BY id"),
              L"1|91;2|92;3|93");
    ExpectStr("item1d public.seqt is still empty",
              Dump(*c.tgt, L"SELECT id, v FROM public.seqt ORDER BY id"), L"");

    // THE SEQUENCES THEMSELVES. nextval is the honest probe: it is what the
    // target's next ordinary INSERT would actually receive, which is the thing
    // the repair exists to make safe. archive's must clear the 3 literal ids
    // just written; public's must be untouched at its initial value.
    const wxString aNext = Scalar(*c.tgt,
        L"SELECT nextval(pg_get_serial_sequence('archive.seqt','id'))");
    const wxString pNext = Scalar(*c.tgt,
        L"SELECT nextval(pg_get_serial_sequence('public.seqt','id'))");
    mplive::Observe("item1d archive.seqt next id", aNext);
    mplive::Observe("item1d public.seqt next id", pNext);
    ExpectStr("item1d archive's sequence was advanced past the written ids",
              aNext, L"4");
    ExpectStr("item1d public's sequence was NOT touched", pNext, L"1");
}

// ===========================================================================
// ITEM 2 — cancellation mid-write
// ===========================================================================
//
// WHAT IS ACTUALLY BEING ASKED. Four separate questions, and they have four
// separate answers, so they get four separate assertions:
//
//   (a) Does the cancel stop the run promptly, or does it finish the table?
//   (b) What is in the TARGET afterwards? DataSyncExec.h documents a
//       transaction per (table, sweep), so a cancelled table must roll back to
//       nothing. That claim has never been checked at a database.
//   (c) Is the partial state reported HONESTLY — can a caller reading
//       DataExecResult tell which tables committed?
//   (d) Does a retry converge, or does the first attempt leave debris that
//       makes the second one fail?
//
// FIXTURE. `tiny` (5 rows) is listed FIRST and is not cancelled, so it commits
// before the cancel is tripped; `bulk` (3000 rows = 6 full 500-row batches) is
// listed second and is cancelled at 1000 applied rows, i.e. after two batches
// have already been sent to the server inside its open transaction. Cancelling
// at a batch boundary in the middle of a table is the case that distinguishes
// "the transaction rolled back" from "the rows before the cancel survived" —
// a cancel on the first or last batch could be explained away by either.
// ---------------------------------------------------------------------------
namespace {

constexpr long long kBulkRows   = 3000;
constexpr long long kCancelAfter = 1000;   // two full 500-row batches

bool SetupItem2(IConnection& src, IConnection& tgt)
{
    if (!Run(src, L"DROP TABLE IF EXISTS public.tiny", "src drop tiny") ||
        !Run(src, L"DROP TABLE IF EXISTS public.bulk", "src drop bulk") ||
        !Run(tgt, L"DROP TABLE IF EXISTS public.tiny", "tgt drop tiny") ||
        !Run(tgt, L"DROP TABLE IF EXISTS public.bulk", "tgt drop bulk"))
        return false;

    const wxChar* ddl = L"(id int PRIMARY KEY, v int NOT NULL, note text)";
    if (!Run(src, wxString(L"CREATE TABLE public.tiny") + ddl, "src create tiny") ||
        !Run(src, wxString(L"CREATE TABLE public.bulk") + ddl, "src create bulk") ||
        !Run(tgt, wxString(L"CREATE TABLE public.tiny") + ddl, "tgt create tiny") ||
        !Run(tgt, wxString(L"CREATE TABLE public.bulk") + ddl, "tgt create bulk"))
        return false;

    return Run(src, L"INSERT INTO public.tiny "
                    L"SELECT g, g*10, 'tiny-'||g FROM generate_series(1,5) g",
               "src seed tiny") &&
           Run(src, wxString::Format(
                    L"INSERT INTO public.bulk "
                    L"SELECT g, g*10, 'bulk-'||g FROM generate_series(1,%lld) g",
                    kBulkRows), "src seed bulk");
}

// Both targets start EMPTY, so every source row is an insert and the row counts
// below are unambiguous.
bool ResetItem2Target(IConnection& tgt)
{
    return Run(tgt, L"TRUNCATE public.tiny", "reset tiny") &&
           Run(tgt, L"TRUNCATE public.bulk", "reset bulk");
}

std::vector<TableExecJob> Item2Jobs(IConnection& src, IConnection& tgt,
                                    const wxString& srcDb, const wxString& tgtDb,
                                    bool& ok)
{
    std::vector<TableExecJob> jobs;
    ok = true;
    for (const wxChar* t : { L"tiny", L"bulk" }) {
        DataDiffSpec spec;
        const QualifiedName q(L"public", t);
        if (!MakeSpecQ(src, tgt, srcDb, tgtDb, q, q, spec)) { ok = false; return jobs; }
        jobs.push_back(TableExecJob{spec,
            MakeAllow(spec.SourceTable().Key(), Allow{true, false, false}, false)});
    }
    return jobs;
}

} // namespace

// ---------------------------------------------------------------------------
void Item2_CancelMidWrite(Ctx& c)
{
    std::printf("\n== ITEM 2: cancel mid-write — what does the target hold, "
                "and does a retry converge? ==\n");

    if (!SetupItem2(*c.src, *c.tgt) || !ResetItem2Target(*c.tgt)) {
        std::printf("  ERR  item 2 fixture setup failed\n");
        ++g_fails;
        return;
    }
    mplive::ExpectEq("item2 source bulk really has 3000 rows",
                     ScalarInt(*c.src, L"SELECT count(*) FROM public.bulk"),
                     kBulkRows);
    mplive::ExpectEq("item2 target starts empty",
                     ScalarInt(*c.tgt,
                        L"SELECT (SELECT count(*) FROM public.tiny)"
                        L"      + (SELECT count(*) FROM public.bulk)"), 0);

    bool specsOk = false;
    std::vector<TableExecJob> jobs =
        Item2Jobs(*c.src, *c.tgt, c.srcDb, c.tgtDb, specsOk);
    if (!specsOk) { ++g_fails; return; }

    // ---- the cancelled run ------------------------------------------------
    // `liveMax` records the highest row count the executor reported for `bulk`
    // WHILE IT RAN. It is needed because promptness can no longer be read off
    // the final report: a rolled-back sweep now correctly rewinds its counters
    // to zero, so TableExecReport::inserts is 0 whether the cancel landed after
    // one batch or after all six. Asserting on that number would be asserting
    // 0 < 3000, which is true for a cancel that never worked at all.
    long long liveMax = 0;
    CancelExecOutcome o = RunExecCancellable(*c.src, *c.tgt, jobs,
        [&](const wxString& table, const wxString&, long long applied) {
            if (table == L"public.bulk") liveMax = std::max(liveMax, applied);
            return table == L"public.bulk" && applied >= kCancelAfter;
        });
    ShowResult(o.result, "item2 cancelled run");
    mplive::Observe("item2 cancel tripped at", o.trippedAt);
    mplive::Observe("item2 returned err", o.err);
    mplive::Observe("item2 progress callbacks",
                    wxString::Format(L"%lld", o.progressCalls));

    mplive::ExpectTrue("item2 the cancel hook actually fired", o.tripped);
    mplive::ExpectTrue("item2 a cancelled run returns false", !o.ok);
    mplive::ExpectTrue("item2 the failure names cancellation, not a server error",
                       o.err.Contains(L"取消"));

    // (a) PROMPTNESS, measured rather than felt. The cancel is tripped at 1000
    // rows; if the token were only consulted BETWEEN TABLES — and not between
    // rows, as DataSync.h claims — the table would run to completion and the
    // live figure would reach 3000. Anything at or below one further batch means
    // the stream really did notice mid-table.
    const TableExecReport* bulk = ReportOf(o.result, L"public.bulk");
    const TableExecReport* tiny = ReportOf(o.result, L"public.tiny");
    mplive::ExpectTrue("item2 both tables appear in the report", bulk && tiny);
    if (!bulk || !tiny) return;
    mplive::Observe("item2 bulk rows sent before the cancel took effect (live)",
                    wxString::Format(L"%lld", liveMax));
    mplive::ExpectTrue("item2 the cancel stopped the table well short of 3000",
                       liveMax > 0 && liveMax < kBulkRows);
    mplive::ExpectTrue("item2 the cancel was prompt (<= one batch past the trip)",
                       liveMax <= kCancelAfter + 500);
    // …and the rewound report agrees with the database rather than with the
    // wire: 1000 rows were accepted by the server inside the transaction, none
    // survived it, and the counter says none.
    mplive::ExpectEq("item2 the rolled-back sweep reports zero inserts",
                     bulk->inserts, 0);

    // (c) HONEST REPORTING, at the level the caller can act on.
    mplive::ExpectTrue("item2 tiny reports committed", tiny->committed);
    mplive::ExpectTrue("item2 bulk reports NOT committed", !bulk->committed);
    mplive::ExpectTrue("item2 bulk was attempted", bulk->attempted);
    mplive::ExpectTrue("item2 AllCommitted() is false", !o.result.AllCommitted());
    mplive::ExpectEq("item2 exactly one table is reported committed",
                     (long long)o.result.Committed().size(), 1);

    // (b) THE DATABASE'S OWN ANSWER — the only one that settles it.
    //
    // The claim under test is DataSyncExec.h's "one transaction per (table,
    // sweep)": the cancelled table must hold ZERO rows, because its transaction
    // was rolled back, even though 1000+ rows had already been sent inside it.
    // The committed table must hold all 5, because its transaction closed before
    // the cancel and per-table boundaries mean a later cancellation cannot reach
    // back into it.
    const long long tinyRows = ScalarInt(*c.tgt, L"SELECT count(*) FROM public.tiny");
    const long long bulkRows = ScalarInt(*c.tgt, L"SELECT count(*) FROM public.bulk");
    mplive::Observe("item2 target row counts after cancel",
                    wxString::Format(L"tiny=%lld bulk=%lld", tinyRows, bulkRows));
    mplive::ExpectEq("item2 the cancelled table rolled back to zero rows", bulkRows, 0);
    mplive::ExpectEq("item2 the already-committed table stayed committed", tinyRows, 5);

    // A rollback that left the transaction OPEN would hold the target's locks
    // and snapshot open indefinitely, and the row counts above cannot tell the
    // difference — they are read on that same connection, which would happily
    // show its own uncommitted state.
    //
    // Asked from a SEPARATE connection, because a backend interrogating itself
    // is by definition running the query used to ask and therefore always
    // reports 'active'; a self-query can never observe 'idle in transaction'
    // and is worthless as a probe. 'idle' is the only acceptable answer here.
    {
        const wxString pid = Scalar(*c.tgt, L"SELECT pg_backend_pid()");
        wxString de;
        if (auto w = c.Dial(c.tgtDb, de)) {
            const wxString st = Scalar(*w, L"SELECT state FROM pg_stat_activity "
                                           L"WHERE pid = " + pid);
            mplive::Observe("item2 target backend state, seen from outside", st);
            ExpectStr("item2 the cancelled table's transaction was really closed",
                      st, L"idle");
            w->Disconnect();
        } else {
            std::printf("  note item2 txn-state probe skipped: %s\n",
                        (const char*)de.utf8_str());
        }
    }

    // (d) CONVERGENCE. Same jobs, same connections, no cleanup in between —
    // whatever the cancelled attempt left behind is the starting state, which
    // is precisely the situation a user hits when they press 同步 again.
    std::printf("  -- item2 retry --\n");
    mpexec::ExecOutcome r = mpexec::RunExec(*c.src, *c.tgt, jobs);
    ShowResult(r.result, "item2 retry");
    mplive::ExpectTrue("item2 the retry succeeds", r.ok);
    if (!r.ok)
        std::printf("  ERR  item2 retry: %s\n", (const char*)r.err.utf8_str());

    const long long tiny2 = ScalarInt(*c.tgt, L"SELECT count(*) FROM public.tiny");
    const long long bulk2 = ScalarInt(*c.tgt, L"SELECT count(*) FROM public.bulk");
    mplive::Observe("item2 target row counts after retry",
                    wxString::Format(L"tiny=%lld bulk=%lld", tiny2, bulk2));
    mplive::ExpectEq("item2 retry converged bulk to 3000", bulk2, kBulkRows);
    // NOT 10. The retry re-diffs, so the 5 rows tiny already holds must be
    // detected as unchanged rather than re-inserted — a duplicate here would be
    // the "wrote twice" failure mode the suite's standing rule exists to catch.
    mplive::ExpectEq("item2 retry did not duplicate the committed table", tiny2, 5);

    // Contents, not just counts: a table with the right number of wrong rows
    // passes every count assertion above.
    ExpectStr("item2 retry wrote the right values at the batch boundaries",
              Dump(*c.tgt, L"SELECT id, v, note FROM public.bulk "
                           L"WHERE id IN (1,500,501,1000,1001,3000) ORDER BY id"),
              L"1|10|bulk-1;500|5000|bulk-500;501|5010|bulk-501;"
              L"1000|10000|bulk-1000;1001|10010|bulk-1001;3000|30000|bulk-3000");

    Item2b_MixedSweepHonesty(c);
}

// ===========================================================================
// ITEM 2b — the sharp form of "is the partial state reported honestly?"
// ===========================================================================
//
// The run above cancels a table that had only inserts, so its report is
// unambiguous once the counters are right: committed=false, everything zero.
// The case that actually tests the REPORT is a table where one sweep genuinely
// committed and the next was cancelled, because then committed=false is the
// correct state for BOTH a durable change and a discarded one, and the counters
// are the only thing that can tell a caller which is which.
//
// The fixture makes both halves large enough to be unmistakable and puts them
// on ONE table: 5 target-only rows the DELETE sweep removes and commits, then
// 3000 source rows the INSERT sweep is cancelled partway through. The honest
// report is deletes=5 (durable), inserts=0 (rolled back), committed=false.
//
// THIS IS WHERE THE DEFECT WAS FOUND. Before the fix in DataSyncExec.cpp this
// reported inserts=1000 — one per row of the two 500-row batches the server had
// accepted inside the doomed transaction — while the database held none of
// them. A caller reading that report would tell the user 1000 rows had been
// inserted, alongside 5 deletions that really happened, with no way to tell the
// true number from the phantom one. Note the prior partial-failure coverage
// (mysql_pg_live_exec3.cpp item C3) could not have caught this: its write sweep
// failed on its FIRST statement, where the counter was still legitimately 0.
// ---------------------------------------------------------------------------
namespace {

constexpr long long kMixedInserts = 3000;
constexpr long long kMixedDeletes = 5;

bool SetupItem2b(IConnection& src, IConnection& tgt)
{
    const wxChar* ddl = L"(id int PRIMARY KEY, v int NOT NULL, note text)";
    return Run(src, L"DROP TABLE IF EXISTS public.mixed", "src drop mixed") &&
           Run(tgt, L"DROP TABLE IF EXISTS public.mixed", "tgt drop mixed") &&
           Run(src, wxString(L"CREATE TABLE public.mixed") + ddl, "src create mixed") &&
           Run(tgt, wxString(L"CREATE TABLE public.mixed") + ddl, "tgt create mixed") &&
           Run(src, wxString::Format(
                    L"INSERT INTO public.mixed "
                    L"SELECT g, g*10, 'mix-'||g FROM generate_series(1,%lld) g",
                    kMixedInserts), "src seed mixed") &&
           // Target-only rows, far out of the source's id range so the two
           // populations can never be confused in a count.
           Run(tgt, wxString::Format(
                    L"INSERT INTO public.mixed "
                    L"SELECT 900000+g, g, 'gone-'||g FROM generate_series(1,%lld) g",
                    kMixedDeletes), "tgt seed mixed extras");
}

} // namespace

void Item2b_MixedSweepHonesty(Ctx& c)
{
    std::printf("\n== ITEM 2b: one sweep commits, the next is cancelled — "
                "what does the report say happened? ==\n");

    if (!SetupItem2b(*c.src, *c.tgt)) {
        std::printf("  ERR  item 2b fixture setup failed\n");
        ++g_fails;
        return;
    }
    mplive::ExpectEq("item2b target starts with only the 5 extra rows",
                     ScalarInt(*c.tgt, L"SELECT count(*) FROM public.mixed"),
                     kMixedDeletes);

    DataDiffSpec spec;
    const QualifiedName mixed(L"public", L"mixed");
    if (!MakeSpecQ(*c.src, *c.tgt, c.srcDb, c.tgtDb, mixed, mixed, spec)) {
        ++g_fails;
        return;
    }
    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec,
        MakeAllow(spec.SourceTable().Key(), Allow{true, false, true},
                  /*deleteMasterSwitch=*/true)});

    // Cancel the INSERT sweep only. The DELETE sweep runs first and moves 5
    // rows, so gating on the phase (not merely on the count) states the intent
    // rather than relying on 5 being smaller than the trip point.
    CancelExecOutcome o = RunExecCancellable(*c.src, *c.tgt, jobs,
        [](const wxString& table, const wxString& phase, long long applied) {
            return table == L"public.mixed" && phase == L"插入" &&
                   applied >= kCancelAfter;
        });
    ShowResult(o.result, "item2b");
    mplive::Observe("item2b cancel tripped at", o.trippedAt);
    mplive::ExpectTrue("item2b the cancel hook fired during the insert sweep",
                       o.tripped);
    mplive::ExpectTrue("item2b the run returns false", !o.ok);

    const TableExecReport* rep = ReportOf(o.result, L"public.mixed");
    mplive::ExpectTrue("item2b the table appears in the report", rep != nullptr);
    if (!rep) return;
    mplive::ExpectTrue("item2b the table reports NOT committed", !rep->committed);

    // ---- what the database really holds -----------------------------------
    // Read FIRST, so the assertions on the report below are stated against an
    // observed truth rather than an assumed one.
    const long long survivingExtras = ScalarInt(*c.tgt,
        L"SELECT count(*) FROM public.mixed WHERE id > 900000");
    const long long landedInserts = ScalarInt(*c.tgt,
        L"SELECT count(*) FROM public.mixed WHERE id <= 900000");
    mplive::Observe("item2b database truth",
                    wxString::Format(L"target-only rows left=%lld, "
                                     L"source rows landed=%lld",
                                     survivingExtras, landedInserts));

    mplive::ExpectEq("item2b the DELETE sweep's work is durable "
                     "(its transaction committed before the cancel)",
                     survivingExtras, 0);
    mplive::ExpectEq("item2b the cancelled INSERT sweep landed nothing",
                     landedInserts, 0);

    // ---- and whether the report says so ------------------------------------
    mplive::ExpectEq("item2b report: deletes counts the rows that really went",
                     rep->deletes, kMixedDeletes);
    // THE ASSERTION THE DEFECT WAS FOUND BY. Was inserts=1000 against a table
    // holding zero source rows; TableExecReport documents these counters as
    // rows actually applied, so this is the value that makes the report true.
    mplive::ExpectEq("item2b report: inserts does NOT count rolled-back rows",
                     rep->inserts, 0);

    // The retry must still converge from this genuinely half-applied state —
    // deletes durable, inserts absent — which is what a user sees when they
    // press 同步 again after a cancel.
    mpexec::ExecOutcome r = mpexec::RunExec(*c.src, *c.tgt, jobs);
    ShowResult(r.result, "item2b retry");
    mplive::ExpectTrue("item2b the retry succeeds", r.ok);
    mplive::ExpectEq("item2b retry converged the table to the source's 3000 rows",
                     ScalarInt(*c.tgt, L"SELECT count(*) FROM public.mixed"),
                     kMixedInserts);
}

} // namespace mpx4

// ===========================================================================
// main
// ===========================================================================
int main()
{
    using namespace mpx4;

    std::printf("== mysql_pg_live_exec4 (schema-targeted write / cancel / "
                "connection loss / FK ordering, live) ==\n");

    const wxString host = mplive::Env("SWIFTSQL_PGTEST_HOST");
    const wxString user = mplive::Env("SWIFTSQL_PGTEST_USER");
    const wxString pass = mplive::Env("SWIFTSQL_PGTEST_PASS");
    if (host.IsEmpty() || user.IsEmpty() || pass.IsEmpty()) {
        std::printf("SKIP: no live PostgreSQL configured "
                    "(set SWIFTSQL_PGTEST_HOST/USER/PASS)\n");
        return 0;
    }
    const int      port = mplive::EnvInt("SWIFTSQL_PGTEST_PORT", 5432);
    wxString       boot = mplive::Env("SWIFTSQL_PGTEST_DB");
    if (boot.IsEmpty()) boot = L"postgres";

    // Own prefix, distinct from every other live suite's, so these can never
    // collide even if two suites are run concurrently on the shared server.
    const wxString srcDb = L"swiftsql_xexec4_src";
    const wxString tgtDb = L"swiftsql_xexec4_tgt";
    const wxString qSrc  = QuoteIdent(srcDb, Dialect::Postgres);
    const wxString qTgt  = QuoteIdent(tgtDb, Dialect::Postgres);

    auto maint = CreateConnection(DbType::PostgreSQL);
    wxString err;
    if (!maint->Connect(mplive::Profile(DbType::PostgreSQL, host, port, user,
                                        pass, boot), err)) {
        std::printf("SKIP: PostgreSQL connect failed: %s\n",
                    (const char*)err.utf8_str());
        return 0;
    }
    mplive::Observe("pg server version", maint->ServerVersion());

    // Dropped on the way IN as well as out. Item 3 deliberately kills a backend
    // and can in principle take this process with it; if it ever does, the
    // guard below never runs and the next run must be able to clean up after
    // it rather than inherit its half-written tables.
    mplive::Exec(*maint, L"DROP DATABASE IF EXISTS " + qSrc, "drop src (pre)");
    mplive::Exec(*maint, L"DROP DATABASE IF EXISTS " + qTgt, "drop tgt (pre)");
    const bool provisioned =
        mplive::Exec(*maint, L"CREATE DATABASE " + qSrc, "create src") &&
        mplive::Exec(*maint, L"CREATE DATABASE " + qTgt, "create tgt");

    std::unique_ptr<IConnection> src, tgt;
    struct Guard {
        IConnection*                  maint;
        std::unique_ptr<IConnection>* src;
        std::unique_ptr<IConnection>* tgt;
        wxString                      qSrc, qTgt;
        ~Guard()
        {
            // Disconnect first: PostgreSQL refuses to DROP a database that
            // still has a session attached, so a live handle here would leave
            // test databases behind on a shared server.
            for (auto* p : { src, tgt }) if (p && *p) (*p)->Disconnect();
            QueryResult r; wxString e;
            if (maint) {
                maint->Execute(L"DROP DATABASE IF EXISTS " + qSrc, r, e);
                maint->Execute(L"DROP DATABASE IF EXISTS " + qTgt, r, e);
            }
        }
    };

    src = CreateConnection(DbType::PostgreSQL);
    tgt = CreateConnection(DbType::PostgreSQL);
    Guard guard{ maint.get(), &src, &tgt, qSrc, qTgt };

    mplive::ExpectTrue("provision two throwaway databases", provisioned);
    if (!provisioned) {
        std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
        return 1;
    }

    {
        wxString e1, e2;
        const bool a = src->Connect(mplive::Profile(DbType::PostgreSQL, host, port,
                                                    user, pass, srcDb), e1);
        const bool b = tgt->Connect(mplive::Profile(DbType::PostgreSQL, host, port,
                                                    user, pass, tgtDb), e2);
        mplive::ExpectTrue("connect source and target", a && b);
        if (!(a && b)) {
            std::printf("  errs: %s / %s\n", (const char*)e1.utf8_str(),
                        (const char*)e2.utf8_str());
            std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
            return 1;
        }
    }

    Ctx ctx;
    ctx.src = src.get();
    ctx.tgt = tgt.get();
    ctx.srcDb = srcDb;
    ctx.tgtDb = tgtDb;
    ctx.host = host; ctx.user = user; ctx.pass = pass; ctx.port = port;

    Item1_WriteIntoNonPublicSchema(ctx);
    Item2_CancelMidWrite(ctx);
    Item4_FkOrdering(ctx);

    // Item 3 runs LAST, on purpose. It terminates a live backend out from under
    // a driver that has never been asked to survive that, so it is the one item
    // with a real chance of taking the process down. Everything above has
    // already printed its verdict by the time it starts, and the running total
    // is printed here so a crash still leaves a usable record rather than
    // discarding three items' worth of observations.
    std::printf("\n-- running total before item 3: %d checks, %d failed --\n",
                g_checks, g_fails);
    Item3_ConnectionLossMidBatch(ctx);

    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;   // guard drops both databases on the way out
}
