// mysql_pg_live_exec5.cpp — items 3 and 4 of the suite described in
// mysql_pg_live_exec4.h. Separate TU purely for the charter's 1000-line ceiling.

#include "mysql_pg_live_exec4.h"

#include <cstdio>

namespace mpx4 {
namespace {

bool Run(IConnection& c, const wxString& sql, const char* label)
{
    return mplive::Exec(c, sql, label);
}

// Same rationale as mysql_pg_live_exec4.cpp's copy: mpexec::MakeSpec resolves
// through search_path and therefore always finds `public`, which item 4's
// cross-schema half cannot use. Duplicated rather than shared because the two
// TUs are the only consumers and a fifth header for one function would cost
// more than it saves.
bool MakeSpecQ(IConnection& src, IConnection& tgt, const wxString& srcDb,
               const wxString& tgtDb, const QualifiedName& table,
               DataDiffSpec& out)
{
    wxString e1, e2;
    if (!src.GetTableSchema(srcDb, table, out.srcSchema, e1) ||
        !tgt.GetTableSchema(tgtDb, table, out.tgtSchema, e2)) {
        std::printf("  ERR  GetTableSchema(%s): %s %s\n",
                    (const char*)table.Key().utf8_str(),
                    (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
        return false;
    }
    if (out.srcSchema.columns.empty() || out.tgtSchema.columns.empty()) {
        std::printf("  ERR  spec %s: empty schema (src=%d tgt=%d)\n",
                    (const char*)table.Key().utf8_str(),
                    (int)out.srcSchema.columns.size(),
                    (int)out.tgtSchema.columns.size());
        return false;
    }
    out.srcDb = srcDb;
    out.tgtDb = tgtDb;
    return true;
}

// ===========================================================================
// ITEM 3 — losing the connection mid-batch
// ===========================================================================
//
// STAGING IT HONESTLY. The brief allows skipping this if it cannot be staged
// reliably, on the grounds that a flaky test is worse than a documented gap. It
// CAN be staged deterministically here, and the reason is worth stating because
// it is what separates this from the usual flaky-by-construction network test:
//
//   * The kill is fired from the executor's OWN progress callback, so it
//     happens at a known point in the stream (after N applied rows) rather than
//     at a wall-clock moment that races the batch loop. There is no sleep
//     anywhere in this item.
//   * pg_terminate_backend() against a specific pid is synchronous and total —
//     it is not a dropped packet that might be retried, it is the backend
//     process ending. The next protocol exchange on that socket cannot succeed.
//   * The pid is the TARGET connection's own (SELECT pg_backend_pid() on it),
//     read before the run, so nothing else on this shared server can be hit.
//
// The one thing this cannot control is whether the driver survives at all. If
// PgDriver mishandles a terminated backend the process dies here, which is why
// main() runs this item last and prints a running total before it.
// ---------------------------------------------------------------------------
constexpr long long kLossRows      = 3000;
constexpr long long kKillAfterRows = 1000;

bool SetupItem3(IConnection& src, IConnection& tgt)
{
    const wxChar* ddl = L"(id int PRIMARY KEY, v int NOT NULL, note text)";
    return Run(src, L"DROP TABLE IF EXISTS public.bulk2", "src drop bulk2") &&
           Run(tgt, L"DROP TABLE IF EXISTS public.bulk2", "tgt drop bulk2") &&
           Run(src, wxString(L"CREATE TABLE public.bulk2") + ddl, "src create bulk2") &&
           Run(tgt, wxString(L"CREATE TABLE public.bulk2") + ddl, "tgt create bulk2") &&
           Run(src, wxString::Format(
                    L"INSERT INTO public.bulk2 "
                    L"SELECT g, g*10, 'loss-'||g FROM generate_series(1,%lld) g",
                    kLossRows), "src seed bulk2");
}

} // namespace

void Item3_ConnectionLossMidBatch(Ctx& c)
{
    std::printf("\n== ITEM 3: the target connection is killed mid-batch ==\n");

    if (!SetupItem3(*c.src, *c.tgt)) {
        std::printf("  ERR  item 3 fixture setup failed\n");
        ++g_fails;
        return;
    }

    // The victim's pid, read off the victim itself. Never a pid found by
    // searching pg_stat_activity for something that looks right — on a SHARED
    // server that is how a test kills a colleague's session.
    const wxString pidStr = Scalar(*c.tgt, L"SELECT pg_backend_pid()");
    long long pid = 0;
    if (!pidStr.ToLongLong(&pid) || pid <= 0) {
        std::printf("  SKIP item 3: could not read the target backend pid [%s]\n",
                    (const char*)pidStr.utf8_str());
        return;
    }
    mplive::Observe("item3 target backend pid", pidStr);

    // The executioner, and separately the witness. Two extra connections
    // because they answer two different questions at two different times, and
    // the witness must outlive the victim to be able to answer at all.
    wxString e1, e2;
    std::unique_ptr<IConnection> killer  = c.Dial(c.tgtDb, e1);
    std::unique_ptr<IConnection> witness = c.Dial(c.tgtDb, e2);
    if (!killer || !witness) {
        std::printf("  SKIP item 3: could not open killer/witness connections: "
                    "%s / %s\n", (const char*)e1.utf8_str(),
                    (const char*)e2.utf8_str());
        return;
    }

    DataDiffSpec spec;
    if (!MakeSpecQ(*c.src, *c.tgt, c.srcDb, c.tgtDb,
                   QualifiedName(L"public", L"bulk2"), spec)) {
        ++g_fails;
        return;
    }
    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec,
        MakeAllow(spec.SourceTable().Key(), Allow{true, false, false}, false)});

    bool     fired = false;
    wxString killReply;
    CancelExecOutcome o = RunExecCancellable(*c.src, *c.tgt, jobs,
        [&](const wxString& table, const wxString&, long long applied) {
            if (!fired && table == L"public.bulk2" && applied >= kKillAfterRows) {
                fired = true;
                killReply = Scalar(*killer, wxString::Format(
                    L"SELECT pg_terminate_backend(%lld)", pid));
            }
            return false;   // never trip the stop token — this is NOT a cancel
        });

    mplive::ExpectTrue("item3 the kill actually fired", fired);
    mplive::Observe("item3 pg_terminate_backend replied", killReply);
    ShowResult(o.result, "item3");
    mplive::Observe("item3 returned err", o.err);

    // THE QUESTION THAT MATTERS MOST: is there any path where this reports
    // success? A write that lost its connection halfway and returned true would
    // tell a user their data is synchronized when a third of it is missing.
    mplive::ExpectTrue("item3 a killed connection does NOT report success", !o.ok);
    mplive::ExpectTrue("item3 the failure carries an error message", !o.err.IsEmpty());
    mplive::ExpectTrue("item3 AllCommitted() is false", !o.result.AllCommitted());

    const TableExecReport* rep = ReportOf(o.result, L"public.bulk2");
    mplive::ExpectTrue("item3 the table appears in the report", rep != nullptr);
    if (rep) {
        mplive::ExpectTrue("item3 the table reports NOT committed", !rep->committed);
        mplive::ExpectTrue("item3 the table reports an error", !rep->error.IsEmpty());
        mplive::Observe("item3 per-table error", rep->error);
        mplive::Observe("item3 inserts counter at death",
                        wxString::Format(L"%lld", rep->inserts));
        // The same counter-honesty defect item 2b found, reached by a completely
        // different failure mode: the server accepted two 500-row batches inside
        // the transaction, then the backend was killed and PostgreSQL discarded
        // all of it. Before the DataSyncExec.cpp fix this reported inserts=1000
        // for a table the witness below shows holding zero rows. Asserted here
        // as well as in item 2b because a rewind that only covered the clean
        // ROLLBACK path would still be wrong here — the ROLLBACK statement
        // itself cannot succeed on a dead connection.
        mplive::ExpectEq("item3 inserts does not count rows the dead "
                         "transaction discarded", rep->inserts, 0);
    }

    // THE TARGET'S STATE, read through the WITNESS. The victim connection
    // cannot be asked — it is the thing that died, and a driver that silently
    // reconnected would answer from a session that never saw the transaction.
    //
    // DataSyncExec.h documents one transaction per (table, sweep). A backend
    // terminated mid-transaction is rolled back by PostgreSQL itself, so the
    // documented state and the engine-enforced state agree here: zero rows.
    const long long left =
        ScalarInt(*witness, L"SELECT count(*) FROM public.bulk2");
    mplive::Observe("item3 rows left in the target",
                    wxString::Format(L"%lld", left));
    mplive::ExpectEq("item3 the interrupted table rolled back to zero rows", left, 0);

    killer->Disconnect();
    witness->Disconnect();
}

// ===========================================================================
// ITEM 4 — FK ordering at execute time
// ===========================================================================
//
// WHY THIS NEEDED A CONTROL RUN.
//
// The two sweeps' ordering (INSERT/UPDATE forward, DELETE reverse) has never
// been exercised against a real foreign key — every existing execute-path
// fixture is FK-free. A green "the sync succeeded" against a fixture that would
// ALSO have succeeded in the wrong order proves nothing at all, and that is the
// easy mistake to make here: with FKs absent, or deferred, or on a target that
// disables them, both orders pass.
//
// So each half runs TWICE. First a CONTROL with the jobs handed to
// ExecuteDataSync in deliberately WRONG order, which must FAIL at the server
// with a foreign-key violation — that is what proves the fixture has teeth.
// Then the PRODUCTION path (SyncEngine::BuildPlan, which owns the topological
// sort, feeding SyncEngine::Execute), which must succeed. Only the pair of
// results is evidence.
//
// NAMING. The parent is `zparent`/`zparx` and the child `achild`/`achix` so
// that ALPHABETICAL order is the WRONG order. If the parent sorted first by
// accident of name, a topological sort that did nothing whatsoever would pass.
// ---------------------------------------------------------------------------
namespace {

struct FkPair {
    QualifiedName parent, child;
    const char*   tag;
};

// Both databases get identical structure, including the FK, because the target
// must ENFORCE the constraint for the ordering to be observable at all.
bool CreateFkPair(IConnection& c, const FkPair& p, const wxString& side)
{
    const wxString qp = QuoteIdent(p.parent.Schema(), Dialect::Postgres) + L"." +
                        QuoteIdent(p.parent.Name(), Dialect::Postgres);
    const wxString qc = QuoteIdent(p.child.Schema(), Dialect::Postgres) + L"." +
                        QuoteIdent(p.child.Name(), Dialect::Postgres);
    const std::string tag = std::string(p.tag) + " " + std::string(side.utf8_str());
    return Run(c, L"CREATE SCHEMA IF NOT EXISTS " +
                  QuoteIdent(p.child.Schema(), Dialect::Postgres),
               (tag + " schema").c_str()) &&
           Run(c, L"DROP TABLE IF EXISTS " + qc, (tag + " drop child").c_str()) &&
           Run(c, L"DROP TABLE IF EXISTS " + qp, (tag + " drop parent").c_str()) &&
           Run(c, L"CREATE TABLE " + qp + L"(id int PRIMARY KEY, label text)",
               (tag + " create parent").c_str()) &&
           // NOT NULL and no ON DELETE action on purpose: the constraint must
           // be violated, not silently satisfied by a cascade or a NULL-out.
           Run(c, L"CREATE TABLE " + qc + L"(id int PRIMARY KEY, "
                  L"pid int NOT NULL REFERENCES " + qp + L"(id))",
               (tag + " create child").c_str());
}

wxString Q(const QualifiedName& n)
{
    return QuoteIdent(n.Schema(), Dialect::Postgres) + L"." +
           QuoteIdent(n.Name(), Dialect::Postgres);
}

// The control: jobs in the WRONG order, straight into ExecuteDataSync (which,
// by its own contract, does not sort — SyncEngine owns that). `parentFirst`
// selects the order; for the INSERT sweep the wrong order is child-first, for
// the DELETE sweep (which walks the list backwards) it is parent-first.
bool ControlRun(IConnection& src, IConnection& tgt, const wxString& srcDb,
                const wxString& tgtDb, const FkPair& p, bool parentFirst,
                const Allow& allow, bool deleteSwitch, const char* tag,
                wxString& errOut)
{
    std::vector<QualifiedName> order;
    if (parentFirst) { order.push_back(p.parent); order.push_back(p.child); }
    else             { order.push_back(p.child);  order.push_back(p.parent); }

    std::vector<TableExecJob> jobs;
    for (const QualifiedName& t : order) {
        DataDiffSpec spec;
        if (!MakeSpecQ(src, tgt, srcDb, tgtDb, t, spec)) { errOut = L"<spec failed>"; return false; }
        jobs.push_back(TableExecJob{spec,
            MakeAllow(spec.SourceTable().Key(), allow, deleteSwitch)});
    }
    mpexec::ExecOutcome o = mpexec::RunExec(src, tgt, jobs);
    ShowResult(o.result, tag);
    errOut = o.err;
    return o.ok;
}

// The production path. BuildPlan owns the topological sort; Execute drives the
// two sweeps. Returns the plan so the caller can assert on the ORDER as well as
// on the outcome — a run that succeeded for some other reason (say, because the
// diff turned out empty) must not be mistaken for correct ordering.
bool ProductionRun(IConnection& src, IConnection& tgt, const wxString& srcDb,
                   const wxString& tgtDb, const FkPair& p, const Allow& allow,
                   bool deleteSwitch, const char* tag, SyncPlan& plan,
                   DataExecResult& result, wxString& errOut)
{
    SyncEngine eng(src, tgt, srcDb, tgtDb);
    SyncScope  scope;
    scope.structure = false;      // the tables already exist, identically
    scope.data      = true;
    scope.tables    = { p.parent, p.child };

    const std::atomic<bool> never{false};
    if (!eng.BuildPlan(scope, plan, errOut, never, nullptr)) {
        std::printf("  ERR  %s BuildPlan: %s\n", tag, (const char*)errOut.utf8_str());
        return false;
    }
    for (const wxString& w : plan.warnings)
        std::printf("  WARN %s plan: %s\n", tag, (const char*)w.utf8_str());

    DataExecPlan data;
    for (const SyncPlan::TableUnit& u : plan.units)
        data.tables.push_back(MakeAllow(u.table.Key(), allow, deleteSwitch));

    const bool ok = eng.Execute(plan, data, /*useTransaction=*/false, result,
                                errOut, never, nullptr);
    ShowResult(result, tag);
    return ok;
}

wxString DumpPair(IConnection& c, const FkPair& p)
{
    return Dump(c, L"SELECT 'P', id FROM " + Q(p.parent) +
                   L" UNION ALL SELECT 'C', id FROM " + Q(p.child) +
                   L" ORDER BY 1, 2");
}

// ---------------------------------------------------------------------------
// One FK pair, both directions. Runs on a fixture created fresh by the caller.
// ---------------------------------------------------------------------------
void RunFkHalf(Ctx& c, const FkPair& p)
{
    std::printf("\n-- item 4: %s (%s -> %s) --\n", p.tag,
                (const char*)p.parent.Key().utf8_str(),
                (const char*)p.child.Key().utf8_str());

    // ---- seed: source has parents AND children, target has neither --------
    if (!Run(*c.src, L"INSERT INTO " + Q(p.parent) + L" VALUES (1,'p1'),(2,'p2')",
             "seed src parent") ||
        !Run(*c.src, L"INSERT INTO " + Q(p.child) + L" VALUES (10,1),(11,2)",
             "seed src child"))
        { ++g_fails; return; }

    ExpectStr("item4 target starts empty (insert phase)", DumpPair(*c.tgt, p), L"");

    // ---- INSERT: the control (child first) MUST fail ----------------------
    // If this passes, every ordering assertion below is vacuous and the right
    // response is to fix the fixture, not to celebrate.
    wxString ctlErr;
    const bool ctlOk = ControlRun(*c.src, *c.tgt, c.srcDb, c.tgtDb, p,
                                  /*parentFirst=*/false, Allow{true, false, false},
                                  false, "item4 insert CONTROL (child first)", ctlErr);
    mplive::Observe("item4 insert control error", ctlErr);
    mplive::ExpectTrue("item4 insert control FAILS in the wrong order "
                       "(proves the FK is enforced and the fixture has teeth)",
                       !ctlOk);
    ExpectStr("item4 the failed control left the target empty",
              DumpPair(*c.tgt, p), L"");

    // ---- INSERT: the production path MUST succeed -------------------------
    SyncPlan       plan;
    DataExecResult res;
    wxString       err;
    const bool ok = ProductionRun(*c.src, *c.tgt, c.srcDb, c.tgtDb, p,
                                  Allow{true, false, false}, false,
                                  "item4 insert PRODUCTION", plan, res, err);
    for (const SyncPlan::TableUnit& u : plan.units)
        mplive::Observe("item4 plan unit", u.table.Key());

    mplive::ExpectEq("item4 the plan holds both tables",
                     (long long)plan.units.size(), 2);
    if (plan.units.size() == 2) {
        // THE TOPOLOGICAL ASSERTION, at the plan. The parent must sort first
        // even though its name sorts last.
        ExpectStr("item4 the referenced (parent) table sorts FIRST",
                  plan.units[0].table.Key(), p.parent.Key());
        ExpectStr("item4 the referencing (child) table sorts SECOND",
                  plan.units[1].table.Key(), p.child.Key());
    }
    mplive::ExpectTrue("item4 the production insert run succeeds", ok);
    if (!ok) std::printf("  ERR  item4 insert: %s\n", (const char*)err.utf8_str());
    ExpectStr("item4 both tables landed, parents and children",
              DumpPair(*c.tgt, p), L"C|10;C|11;P|1;P|2");

    // ---- DELETE: source emptied, target holds both ------------------------
    // Same two tables, opposite direction. The delete sweep walks the plan
    // BACKWARDS, so the child must go first or the parent's delete is refused.
    if (!Run(*c.src, L"DELETE FROM " + Q(p.child),  "empty src child") ||
        !Run(*c.src, L"DELETE FROM " + Q(p.parent), "empty src parent"))
        { ++g_fails; return; }

    wxString ctlErr2;
    // For the DELETE sweep the list is walked in reverse, so handing it
    // parent-LAST is what makes the parent be deleted FIRST — the wrong order.
    const bool ctlOk2 = ControlRun(*c.src, *c.tgt, c.srcDb, c.tgtDb, p,
                                   /*parentFirst=*/false, Allow{false, false, true},
                                   true, "item4 delete CONTROL (parent first)", ctlErr2);
    mplive::Observe("item4 delete control error", ctlErr2);
    mplive::ExpectTrue("item4 delete control FAILS in the wrong order", !ctlOk2);
    ExpectStr("item4 the failed delete control left the target intact",
              DumpPair(*c.tgt, p), L"C|10;C|11;P|1;P|2");

    SyncPlan       plan2;
    DataExecResult res2;
    wxString       err2;
    const bool ok2 = ProductionRun(*c.src, *c.tgt, c.srcDb, c.tgtDb, p,
                                   Allow{false, false, true}, true,
                                   "item4 delete PRODUCTION", plan2, res2, err2);
    mplive::ExpectTrue("item4 the production delete run succeeds", ok2);
    if (!ok2) std::printf("  ERR  item4 delete: %s\n", (const char*)err2.utf8_str());
    ExpectStr("item4 both tables emptied, children before parents",
              DumpPair(*c.tgt, p), L"");
}

} // namespace

void Item4_FkOrdering(Ctx& c)
{
    std::printf("\n== ITEM 4: FK topological ordering at execute time ==\n");

    // ---- half A: the FK stays inside one schema ---------------------------
    // Never validated before at execute time, cross-schema or not: every
    // existing execute-path fixture is FK-free.
    const FkPair same{ QualifiedName(L"public", L"zparent"),
                       QualifiedName(L"public", L"achild"),
                       "same-schema FK" };
    if (CreateFkPair(*c.src, same, L"src") && CreateFkPair(*c.tgt, same, L"tgt"))
        RunFkHalf(c, same);
    else { std::printf("  ERR  item 4 same-schema fixture failed\n"); ++g_fails; }

    // ---- half B: the FK crosses a schema boundary -------------------------
    // NormForeignKey::refTable now carries a QUALIFIED key for PostgreSQL, and
    // SyncEngine's topological sort keys on QualifiedName::Key(). This is the
    // half that checks the two spellings actually meet: if the FK recorded a
    // bare `zparx` while the unit was keyed `public.zparx`, the edge would
    // simply not match, the sort would silently do nothing, and the run would
    // fail in exactly the way the control run above fails.
    const FkPair cross{ QualifiedName(L"public",  L"zparx"),
                        QualifiedName(L"archive", L"achix"),
                        "cross-schema FK" };
    if (CreateFkPair(*c.src, cross, L"src") && CreateFkPair(*c.tgt, cross, L"tgt"))
        RunFkHalf(c, cross);
    else { std::printf("  ERR  item 4 cross-schema fixture failed\n"); ++g_fails; }

    // ---- the adjacent finding: the plan's postamble is never executed -----
    // Not one of the four items, and found while reading the FK path for them:
    // SyncEngine::BuildPlan appends `SET FOREIGN_KEY_CHECKS=1` to
    // plan.postamble for a MySQL target, and SyncEngine::Execute builds its
    // statement list from plan.preamble + each unit's ddl ONLY. Nothing in
    // src/ executes plan.postamble at all (the UI copies it into a preview and
    // a danger scan, neither of which runs it).
    //
    // Asserted here rather than merely written down because the consequence is
    // a data-integrity one and belongs in a test that fails if it regresses:
    // for a MySQL target the run turns FK enforcement OFF on the shared target
    // connection and never turns it back on, so every later write on that
    // connection — including the user's own ad-hoc SQL in the same session —
    // is unconstrained. This suite's target is PostgreSQL, which produces no
    // preamble/postamble at all, so the assertion is on the PAIR's shape.
    {
        SyncEngine eng(*c.src, *c.tgt, c.srcDb, c.tgtDb);
        SyncScope  scope;
        scope.structure = false;
        scope.data      = true;
        scope.tables    = { QualifiedName(L"public", L"zparent") };
        SyncPlan plan; wxString e;
        const std::atomic<bool> never{false};
        if (eng.BuildPlan(scope, plan, e, never, nullptr)) {
            mplive::Observe("item4 pg plan preamble size",
                            wxString::Format(L"%d", (int)plan.preamble.size()));
            mplive::Observe("item4 pg plan postamble size",
                            wxString::Format(L"%d", (int)plan.postamble.size()));
            // PostgreSQL emits neither, so a PG run cannot be harmed by the
            // gap. This pins that fact, so that the day someone gives the PG
            // path a postamble, this test fails and points at the unexecuted
            // half rather than letting it ship silently.
            mplive::ExpectEq("item4 a PostgreSQL target plan has no postamble "
                             "(SyncEngine::Execute never runs one — see comment)",
                             (long long)plan.postamble.size(), 0);
        }
    }
}

} // namespace mpx4
