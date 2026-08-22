// sync_plan_test.cpp — offline unit tests for the ORCHESTRATION layer:
// db::sync::SyncEngine::BuildPlan. Its subject is the SyncPlan itself — which
// units reach it, in what order, what preamble/postamble surrounds them, and
// what verdict each unit carries — not the diff results the units are built
// from. DiffSchema's own behaviour is sync_schema_test.cpp's subject and
// DiffRowChanges' is sync_data_test.cpp's; a test that never constructs a
// SyncEngine does not belong here.
//
// Covers: FK topological ordering, the MySQL FOREIGN_KEY_CHECKS preamble/
// postamble pair, the cross-engine structure gate (column-level Add allowed,
// whole-table create still refused), the structured DataVerdict for a
// no-primary-key table that must still REACH the plan to be displayable, and
// the structure-only Execute overload refusing a data-bearing plan.
//
// Driven through the stub IConnection in sync_stub.h: canned ListTables /
// GetTableSchema / StreamRows, and a RenderSchemaChange that emits one
// recognizable statement per change set, because none of these assertions
// inspect DDL text (that is ddl_test's subject).
#include "sync_stub.h"

#include "db/SyncEngine.h"

#include <atomic>

using namespace db;
using namespace db::sync;
using namespace synctest;

// ===========================================================================
//  A3. SyncEngine::BuildPlan — orchestration.
// ===========================================================================
static void TestBuildPlan()
{
    std::printf("[SyncEngine::BuildPlan — orchestration]\n");

    // ---- FK topological order: referenced table (customers) before orders ----
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::Postgres;
        // ListTables source order deliberately puts the *referencing* table first.
        src.tableList_ = { { L"orders", L"" }, { L"customers", L"" } };
        // orders → FK → customers
        TableSchema orders; orders.name = L"orders";
        orders.columns.push_back(Col(L"cid", ColKind::Integer, L"int"));
        NormForeignKey fk; fk.columns = { L"cid" }; fk.refTable = L"customers";
        fk.refColumns = { L"id" };
        orders.foreignKeys.push_back(fk);
        TableSchema customers; customers.name = L"customers";
        customers.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        src.schemas_[L"orders"]    = orders;
        src.schemas_[L"customers"] = customers;
        // target has neither table → both are createTable.

        SyncEngine eng(src, tgt, L"srcdb", L"tgtdb");
        SyncScope scope; scope.structure = true; scope.data = false;
        SyncPlan plan; wxString err; std::atomic<bool> stop{false};
        bool ok = eng.BuildPlan(scope, plan, err, stop, nullptr);
        ExpectTrue("topo: BuildPlan ok", ok);
        ExpectEq("topo: two units", (long long)plan.units.size(), 2);
        if (plan.units.size() == 2) {
            ExpectStr("topo: referenced table first", plan.units[0].table.Key(), L"customers");
            ExpectStr("topo: referencing table second", plan.units[1].table.Key(), L"orders");
        }
        ExpectTrue("topo: postgres → no FK-check preamble", plan.preamble.empty());
    }

    // ---- MySQL target → FOREIGN_KEY_CHECKS preamble/postamble ----
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::MySQL;
        src.tableList_ = { { L"t", L"" } };
        TableSchema t; t.name = L"t";
        t.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        src.schemas_[L"t"] = t;    // target missing → createTable → a DDL unit

        SyncEngine eng(src, tgt, L"s", L"d");
        SyncScope scope; scope.structure = true; scope.data = false;
        SyncPlan plan; wxString err; std::atomic<bool> stop{false};
        bool ok = eng.BuildPlan(scope, plan, err, stop, nullptr);
        ExpectTrue("mysql: BuildPlan ok", ok);
        ExpectTrue("mysql: preamble FK-check off",
                   !plan.preamble.empty() && plan.preamble[0].Contains(L"FOREIGN_KEY_CHECKS=0"));
        ExpectTrue("mysql: postamble FK-check on",
                   !plan.postamble.empty() && plan.postamble[0].Contains(L"FOREIGN_KEY_CHECKS=1"));
        ExpectTrue("mysql: DDL unit present",
                   plan.units.size() == 1 && !plan.units[0].ddl.empty());
    }

    // ---- cross-engine structure: column-level Add now goes through
    // (A4) — a real capability change from the pre-A4 blanket skip. The
    // missing "more" column maps cleanly (MySQL varchar(20) -> PG character
    // varying(20), Equivalent) so it IS emitted as DDL now; only a whole
    // missing TABLE stays un-auto-created cross-engine (see the next block).
    {
        StubConn src, tgt;
        src.dialect_ = Dialect::MySQL;      // different dialects
        tgt.dialect_ = Dialect::Postgres;
        src.tableList_ = { { L"t", L"" } };
        tgt.tableList_ = { { L"t", L"" } };
        TableSchema t; t.name = L"t";
        t.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        t.columns.push_back(Col(L"more", ColKind::Varchar, L"varchar(20)", 20));
        src.schemas_[L"t"] = t;
        TableSchema tt; tt.name = L"t";     // target has only id
        tt.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        tgt.schemas_[L"t"] = tt;

        SyncEngine eng(src, tgt, L"s", L"d");
        SyncScope scope; scope.structure = true; scope.data = false;
        SyncPlan plan; wxString err; std::atomic<bool> stop{false};
        bool ok = eng.BuildPlan(scope, plan, err, stop, nullptr);
        ExpectTrue("xengine: BuildPlan ok", ok);
        ExpectTrue("xengine: DDL emitted for the mappable column Add",
                   plan.units.size() == 1 && !plan.units[0].ddl.empty());
        ExpectTrue("xengine: TableUnit.changes carries a structured Add",
                   plan.units.size() == 1 &&
                   CountCol(plan.units[0].changes, ColumnChange::Op::Add) == 1);
    }

    // ---- cross-engine structure: a whole missing table is now AUTO-CREATED
    // (A5) when every column maps to the target dialect. DiffSchema translates
    // each column's type through the same Interpret(src)->Render(tgt) path the
    // column-level Add/Modify uses; the target driver's RenderCreateTable then
    // emits the CREATE. All-mappable columns (int, varchar) → a DDL unit. ----
    {
        StubConn src, tgt;
        src.dialect_ = Dialect::MySQL;
        tgt.dialect_ = Dialect::Postgres;
        src.tableList_ = { { L"brandnew", L"" } };
        TableSchema t; t.name = L"brandnew";
        t.columns.push_back(Col(L"id",   ColKind::Integer, L"int"));
        t.columns.push_back(Col(L"name", ColKind::Varchar, L"varchar(40)", 40));
        src.schemas_[L"brandnew"] = t;   // target has no such table at all

        SyncEngine eng(src, tgt, L"s", L"d");
        SyncScope scope; scope.structure = true; scope.data = false;
        SyncPlan plan; wxString err; std::atomic<bool> stop{false};
        bool ok = eng.BuildPlan(scope, plan, err, stop, nullptr);
        ExpectTrue("xengine(newtable): BuildPlan ok", ok);
        ExpectTrue("xengine(newtable): DDL emitted (whole-table create translated)",
                   plan.units.size() == 1 && !plan.units[0].ddl.empty());
        ExpectTrue("xengine(newtable): changes carry createTable",
                   plan.units.size() == 1 && plan.units[0].changes.createTable);
    }

    // ---- cross-engine structure: a whole missing table whose column is
    // Unmappable (MySQL `int unsigned` has no safe PG type) is REFUSED as a
    // whole — no createTable, no DDL unit — with a Finding naming the column
    // (ruling 1: a partial CREATE is worse than an honest refusal). ----
    {
        StubConn src, tgt;
        src.dialect_ = Dialect::MySQL;
        tgt.dialect_ = Dialect::Postgres;
        src.tableList_ = { { L"badnew", L"" } };
        TableSchema t; t.name = L"badnew";
        t.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        t.columns.push_back(Col(L"n",  ColKind::Integer, L"int unsigned"));
        src.schemas_[L"badnew"] = t;

        SyncEngine eng(src, tgt, L"s", L"d");
        SyncScope scope; scope.structure = true; scope.data = false;
        SyncPlan plan; wxString err; std::atomic<bool> stop{false};
        bool ok = eng.BuildPlan(scope, plan, err, stop, nullptr);
        ExpectTrue("xengine(badnew): BuildPlan ok", ok);
        ExpectTrue("xengine(badnew): no DDL (whole table refused)", plan.units.empty());
        ExpectTrue("xengine(badnew): Finding names the offending column + refusal",
                   HasWarning(plan.warnings, L"无法自动建表") &&
                   HasWarning(plan.warnings, L"列 n（源类型 int unsigned）"));
    }
}

// ===========================================================================
//  A6. T11 — the plan carries the PK verdict and a real RowChangeSet.
// ===========================================================================
static void TestPlanVerdicts()
{
    std::printf("[SyncPlan — structured data verdicts]\n");
    std::atomic<bool> stop{false};

    // ---- 无主键（已排除）: a VERDICT on the unit, not free text in warnings --
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::Postgres;
        src.tableList_ = { { L"nopk", L"" } };
        tgt.tableList_ = { { L"nopk", L"" } };
        TableSchema s; s.name = L"nopk";
        s.columns.push_back(Col(L"a", ColKind::Integer, L"int"));   // no primaryKey
        src.schemas_[L"nopk"] = s;
        tgt.schemas_[L"nopk"] = s;

        SyncEngine eng(src, tgt, L"s", L"d");
        SyncScope scope; scope.structure = false; scope.data = true;
        SyncPlan plan; wxString err;
        const bool ok = eng.BuildPlan(scope, plan, err, stop, nullptr);
        ExpectTrue("nopk: BuildPlan ok", ok);
        // The unit must EXIST — a table dropped from the plan cannot be shown
        // in the compare grid at all, which is why the verdict was previously
        // unreachable by the UI.
        ExpectEq("nopk: the table still reaches the plan", (long long)plan.units.size(), 1);
        ExpectTrue("nopk: verdict is structured, not text",
                   plan.units.size() == 1 &&
                   plan.units[0].dataVerdict == DataVerdict::NoPrimaryKey);
        ExpectTrue("nopk: verdict carries display text",
                   plan.units.size() == 1 && !plan.units[0].dataVerdictReason.IsEmpty());
    }

    // ---- a clean data diff reports Ok and a populated RowChangeSet ---------
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::Postgres;
        src.tableList_ = { { L"t", L"" } };
        tgt.tableList_ = { { L"t", L"" } };
        src.schemas_[L"t"] = TwoColSchema();
        tgt.schemas_[L"t"] = TwoColSchema();
        src.rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"b") } };
        tgt.rows_ = { { Num(L"1"), Txt(L"a") } };

        SyncEngine eng(src, tgt, L"s", L"d");
        SyncScope scope; scope.structure = false; scope.data = true;
        SyncPlan plan; wxString err;
        ExpectTrue("data: BuildPlan ok", eng.BuildPlan(scope, plan, err, stop, nullptr));
        ExpectTrue("data: verdict Ok",
                   plan.units.size() == 1 &&
                   plan.units[0].dataVerdict == DataVerdict::Ok);
        ExpectEq("data: RowChangeSet counted the insert",
                 plan.units.empty() ? -1 : plan.units[0].rows.Stat().inserts, 1);
        ExpectEq("data: legacy stat mirrored",
                 plan.units.empty() ? -1 : plan.units[0].stat.inserts, 1);
        ExpectEq("data: preview rendered from the sample",
                 plan.units.empty() ? -1 : (long long)plan.units[0].dml.size(), 1);
        // The structure-only Execute overload must refuse a data-bearing plan
        // rather than silently applying the capped preview.
        wxString e2;
        ExpectTrue("data: legacy Execute refuses a data-bearing plan",
                   !eng.Execute(plan, false, e2, stop, nullptr) && !e2.IsEmpty());
    }
}

// ===========================================================================
//  P0 — cross-engine existence detection must be identity-consistent.
// ===========================================================================
// THE BUG (real user report): a user synced structure into an EMPTY PostgreSQL
// target and the script failed with `relation "OpenIddictApplications" already
// exists`. Creating tables into an empty DB must never say "already exists".
//
// ROOT CAUSE. MySQL ListTables yields BARE names ("OpenIddictApplications");
// PostgreSQL ListTables yields SCHEMA-QUALIFIED names ("public.OpenIddict...").
// BuildPlan's table universe keyed on QualifiedName identity, so on a re-diff a
// table the tool had JUST auto-created on PG was reported as still MISSING (the
// bare source key never matched the public-qualified target key) and its CREATE
// was re-emitted — which on a live server is `relation ... already exists`. The
// fresh cross-engine auto-create SURFACED this; before it a whole-table
// cross-engine CREATE was refused, so the collision could never be emitted.
//
// The camelCase OpenIddict names are the trigger only because they are a
// realistic .NET schema; the defect is the bare-vs-qualified identity
// asymmetry, not case. PG->PG does NOT reproduce it (both sides qualified,
// keys match) — which is exactly why this pins the CROSS-engine direction.
static void TestCrossEngineExistenceIdentity()
{
    std::printf("[P0 — cross-engine existence detection identity]\n");
    std::atomic<bool> stop{false};

    // OpenIddict-shaped: camelCase names, a diamond of FKs among four tables.
    auto tbl = [](const wxString& name) {
        TableSchema t; t.name = name;
        t.columns.push_back(Col(L"Id",   ColKind::Integer, L"int"));
        t.columns.push_back(Col(L"Name", ColKind::Varchar, L"varchar(80)", 80));
        t.primaryKey = { L"Id" };
        return t;
    };
    TableSchema apps = tbl(L"OpenIddictApplications");

    TableSchema auths = tbl(L"OpenIddictAuthorizations");
    auths.columns.push_back(Col(L"AppId", ColKind::Integer, L"int"));
    { NormForeignKey fk; fk.columns = { L"AppId" };
      fk.refTable = L"OpenIddictApplications"; fk.refColumns = { L"Id" };
      auths.foreignKeys.push_back(fk); }

    TableSchema toks = tbl(L"OpenIddictTokens");
    toks.columns.push_back(Col(L"AppId",  ColKind::Integer, L"int"));
    toks.columns.push_back(Col(L"AuthId", ColKind::Integer, L"int"));
    { NormForeignKey f1; f1.columns = { L"AppId" };
      f1.refTable = L"OpenIddictApplications"; f1.refColumns = { L"Id" };
      NormForeignKey f2; f2.columns = { L"AuthId" };
      f2.refTable = L"OpenIddictAuthorizations"; f2.refColumns = { L"Id" };
      toks.foreignKeys.push_back(f1); toks.foreignKeys.push_back(f2); }

    TableSchema scopes = tbl(L"OpenIddictScopes");

    // The MySQL SOURCE: BARE names, exactly as MySqlDriver::ListTables yields.
    // ListTables order deliberately puts a referencing table before its parent
    // so the topo assertion below is not vacuous.
    StubConn src;
    src.dialect_ = Dialect::MySQL;
    src.tableList_ = { { L"OpenIddictTokens", L"" },
                       { L"OpenIddictAuthorizations", L"" },
                       { L"OpenIddictApplications", L"" },
                       { L"OpenIddictScopes", L"" } };
    src.schemas_[L"OpenIddictApplications"]   = apps;
    src.schemas_[L"OpenIddictAuthorizations"] = auths;
    src.schemas_[L"OpenIddictTokens"]         = toks;
    src.schemas_[L"OpenIddictScopes"]         = scopes;

    SyncScope scope; scope.structure = true; scope.data = false;

    // ---- run 1: EMPTY PG target -> exactly one createTable per table ----
    {
        StubConn tgt;
        tgt.dialect_ = Dialect::Postgres;      // empty tableList_ = nothing there

        SyncEngine eng(src, tgt, L"mydb", L"pgdb");
        SyncPlan plan; wxString err;
        ExpectTrue("run1: BuildPlan ok", eng.BuildPlan(scope, plan, err, stop, nullptr));
        ExpectEq("run1: four units, one per table (no duplicate CREATE)",
                 (long long)plan.units.size(), 4);
        int creates = 0;
        for (const auto& u : plan.units) if (u.changes.createTable) ++creates;
        ExpectEq("run1: four tables auto-created into the empty target", creates, 4);
        // FK topo: every referenced parent precedes the table that references it.
        std::vector<wxString> seenOrder;
        for (const auto& u : plan.units) seenOrder.push_back(u.table.Key());
        auto before = [&](const wxString& a, const wxString& b) {
            long ia = -1, ib = -1;
            for (long i = 0; i < (long)seenOrder.size(); ++i) {
                if (seenOrder[i] == a) ia = i;
                if (seenOrder[i] == b) ib = i;
            }
            return ia >= 0 && ib >= 0 && ia < ib;
        };
        ExpectTrue("run1: Applications created before Authorizations",
                   before(L"OpenIddictApplications", L"OpenIddictAuthorizations"));
        ExpectTrue("run1: Authorizations created before Tokens",
                   before(L"OpenIddictAuthorizations", L"OpenIddictTokens"));
    }

    // ---- run 2: the tables now EXIST on PG, reported PUBLIC-QUALIFIED the way
    // PgConnection::ListTables really returns them (schema='public'). The tool
    // must SEE them and re-emit ZERO CREATEs. ----
    {
        StubConn tgt;
        tgt.dialect_ = Dialect::Postgres;
        // {name, approxRows, schema} — the third field is the PG namespace, so
        // these are the SCHEMA-QUALIFIED identities PgConnection::ListTables
        // returns (`public.OpenIddictApplications`), the exact shape the bare
        // MySQL source names must still be recognized against.
        tgt.tableList_ = { { L"OpenIddictApplications",   L"", L"public" },
                           { L"OpenIddictAuthorizations", L"", L"public" },
                           { L"OpenIddictTokens",         L"", L"public" },
                           { L"OpenIddictScopes",         L"", L"public" } };
        // GetTableSchema keys by bare Name() in the stub; a fully-synced target.
        tgt.schemas_[L"OpenIddictApplications"]   = apps;
        tgt.schemas_[L"OpenIddictAuthorizations"] = auths;
        tgt.schemas_[L"OpenIddictTokens"]         = toks;
        tgt.schemas_[L"OpenIddictScopes"]         = scopes;

        SyncEngine eng(src, tgt, L"mydb", L"pgdb");
        SyncPlan plan; wxString err;
        ExpectTrue("run2: BuildPlan ok", eng.BuildPlan(scope, plan, err, stop, nullptr));

        // THE CONTRADICTION THIS PINS: the target catalog lists all four tables,
        // so the diff must NOT re-emit a single CREATE. Before the fix every one
        // came back as createTable (bare src key != public-qualified tgt key),
        // which on a live server is `relation "..." already exists`.
        int creates = 0;
        for (const auto& u : plan.units) if (u.changes.createTable) ++creates;
        ExpectEq("run2: ZERO re-CREATEs — existing tables are SEEN (schema-correct)",
                 creates, 0);
        // And no DROP unit invented for the "public.X"-only ghost identities the
        // old union admitted alongside the bare source names.
        int drops = 0;
        for (const auto& u : plan.units)
            for (const wxString& s : u.ddl)
                if (s.StartsWith(L"DROP TABLE")) ++drops;
        ExpectEq("run2: no ghost DROP from a duplicated table identity", drops, 0);
    }
}

// ===========================================================================
//  P0 — Execute() must BIND the session to the target database first.
//
//  THE FIELD FAILURE. A MySQL cloudpacs -> PostgreSQL structure sync into an
//  EMPTY target database died on its first statement with
//    ERROR: relation "OpenIddictApplications" already exists
//  The compare had just read that database and found it empty; the CREATE was
//  correct. They were aimed at two DIFFERENT databases.
//
//  WHY THE STATEMENT ALONE CANNOT SAY WHICH DATABASE. PostgreSQL has no way to
//  name a database in an identifier, so QualifiedTableSql() (db/DbDriver.h)
//  renders `"schema"."tbl"` and drops the db — the SESSION's binding is the
//  addressing. MySQL's RenderCreateTable emits a bare `CREATE TABLE `tbl``
//  for a different reason with the same consequence.
//
//  WHY IT WAS UNREACHABLE UNTIL IT WASN'T. While one connection served both
//  phases, BuildPlan's ListTables(tgtDb)/GetTableSchema(tgtDb) left it bound as
//  a SIDE EFFECT. The UI then moved each phase onto its own db::CloneConnection
//  (IConnection is not thread-safe) — and a clone is dialed from
//  EffectiveProfile(), whose `.database` is the CONNECT-TIME database, never the
//  one the wizard targets. So the execute connection opened somewhere else.
//
//  THE TEST THEREFORE USES A SECOND, NEVER-COMPARED-ON CONNECTION PAIR, because
//  that is the only shape in which the defect exists. Reusing the BuildPlan
//  stubs would pass with the fix REVERTED — BindSessions() runs in BuildPlan
//  too, and the assertion would be reading that call's leftovers.
// ===========================================================================
namespace {

// Records the binding AND the binding in force at each statement, so the test
// can assert ordering rather than mere occurrence: a UseDatabase that lands
// AFTER the CREATE is exactly as broken as one that never happens.
class BindingConn : public RecordingConn {
public:
    std::vector<wxString> binds;
    wxString              bound;
    wxString              firstStmt;
    wxString              dbAtFirstStmt;
    bool                  sawStmt = false;

    bool UseDatabase(const wxString& db, wxString&) override
    {
        binds.push_back(db);
        bound = db;
        return true;
    }

    bool Execute(const wxString& sql, db::QueryResult& r, wxString& e) override
    {
        if (!sawStmt) { sawStmt = true; firstStmt = sql; dbAtFirstStmt = bound; }
        return RecordingConn::Execute(sql, r, e);
    }
};

} // namespace

static void TestExecuteBindsTargetDatabase()
{
    std::printf("[SyncEngine::Execute — binds the session before the first DDL]\n");
    std::atomic<bool> stop{false};

    TableSchema apps;
    apps.name = L"OpenIddictApplications";
    apps.columns.push_back(Col(L"Id", ColKind::Varchar, L"varchar(255)", 255));
    apps.primaryKey = { L"Id" };

    // ---- phase 1: compare, on the connections the wizard's compare worker owns
    StubConn cmpSrc, cmpTgt;
    cmpSrc.dialect_ = Dialect::MySQL;
    cmpTgt.dialect_ = Dialect::Postgres;          // empty tableList_ = empty database
    cmpSrc.tableList_ = { { L"OpenIddictApplications", L"" } };
    cmpSrc.schemas_[L"OpenIddictApplications"] = apps;

    SyncScope scope; scope.structure = true; scope.data = false;
    SyncPlan plan; wxString err;
    SyncEngine cmp(cmpSrc, cmpTgt, L"cloudpacs", L"pgdb");
    ExpectTrue("bind: BuildPlan ok", cmp.BuildPlan(scope, plan, err, stop, nullptr));
    ExpectEq("bind: the empty target plans one CREATE", (long long)plan.units.size(), 1);

    // ---- phase 2: execute, on the FRESH pair the runner dialog clones ------
    BindingConn runSrc, runTgt;
    runSrc.dialect_ = Dialect::MySQL;
    runTgt.dialect_ = Dialect::Postgres;

    SyncEngine run(runSrc, runTgt, L"cloudpacs", L"pgdb");
    wxString runErr;
    const bool ok = run.Execute(plan, /*useTransaction=*/false, runErr, stop, nullptr);
    ExpectTrue("bind: Execute ok", ok);

    // THE ASSERTION THE FIELD FAILURE NEEDED. Not "was UseDatabase called" —
    // "was the target database in force when the CREATE went out".
    ExpectTrue("bind: a statement actually reached the target", runTgt.sawStmt);
    ExpectStr("bind: the first target statement is the CREATE",
              runTgt.firstStmt, L"CREATE TABLE OpenIddictApplications");
    ExpectStr("bind: and it ran with the TARGET database bound (not the profile's)",
              runTgt.dbAtFirstStmt, L"pgdb");
    ExpectStr("bind: the source session is bound to the source database",
              runSrc.bound, L"cloudpacs");

    // NEGATIVE CONTROL for the assertion itself: a connection that never gets
    // told which database it is on reports an EMPTY binding, which is precisely
    // the pre-fix state the check above must be able to distinguish.
    BindingConn unbound;
    db::QueryResult qr; wxString qe;
    unbound.Execute(L"CREATE TABLE OpenIddictApplications", qr, qe);
    ExpectStr("bind(neg): an unbound connection reports no database — the check "
              "can tell the two apart", unbound.dbAtFirstStmt, wxString());
}

int main()
{
    std::printf("== sync_plan_test ==\n");
    TestBuildPlan();
    TestPlanVerdicts();
    TestCrossEngineExistenceIdentity();
    TestExecuteBindsTargetDatabase();
    return Report("sync_plan_test");
}
