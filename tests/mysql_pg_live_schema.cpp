// mysql_pg_live_schema.cpp — the SCHEMA-QUALIFIED IDENTITY suite.
//
// This target exists to answer one question that no offline test can:
// when a PostgreSQL database holds two tables with the SAME NAME in DIFFERENT
// SCHEMAS, does this program keep them apart?
//
// The defect it was written for: db::sync::SyncPlan::TableUnit::table was a bare
// table name, and so were ui::TableKey, SyncSelection's map key, and
// TableDataSpec::table_. QA flagged it twice as latent, on the argument that the
// wizard only ever pairs one database against one database so two same-named
// tables cannot both appear. That argument turned out to be wrong about the
// LAYER BELOW it, which is what this suite proves against a real server.
//
// SAFETY: identical contract to mysql_pg_live.h — nothing hard-coded, everything
// from SWIFTSQL_PGTEST_*, skip cleanly when unset, and only ever touch databases
// carrying the `swiftsql_xschema_` prefix (dropped on the way in AND out).

#include "mysql_pg_live.h"

#include "db/DataSync.h"
#include "db/SyncEngine.h"

#include <atomic>
#include <memory>

using namespace db;
using namespace db::sync;
using mplive::Env;
using mplive::EnvInt;
using mplive::Exec;
using mplive::Observe;
using mplive::ExpectEq;
using mplive::ExpectTrue;

namespace {

const std::atomic<bool> g_never{false};

wxString Scalar(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return L"<QUERY-FAILED: " + err + L">";
    if (r.rows.empty() || r.rows[0].empty()) return L"<NO-ROWS>";
    return r.rows[0][0];
}

void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++mplive::g_checks;
    if (got != want) {
        ++mplive::g_fails;
        std::printf("  FAIL %s\n       want=[%s]\n        got=[%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---------------------------------------------------------------------------
// Fixture: one database, two schemas, one table name, DIFFERENT contents.
//
// The contents differ deliberately and unmistakably: public.orders holds 2 rows
// with amount 100/200, archive.orders holds 3 rows with amount 900/901/902. Any
// confusion between the two is therefore visible as a wrong row COUNT and a
// wrong SUM, not as a subtle mismatch that could be argued away.
// ---------------------------------------------------------------------------
bool SetupTwoSchemas(IConnection& pg)
{
    return Exec(pg, L"DROP SCHEMA IF EXISTS archive CASCADE", "drop archive") &&
           Exec(pg, L"CREATE SCHEMA archive", "create archive") &&
           Exec(pg, L"DROP TABLE IF EXISTS public.orders", "drop public.orders") &&
           Exec(pg, L"CREATE TABLE public.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
                "create public.orders") &&
           Exec(pg, L"INSERT INTO public.orders VALUES (1,100,'pub'),(2,200,'pub')",
                "seed public.orders") &&
           // Deliberately a DIFFERENT column set as well: archive.orders has an
           // extra `archived_at` column public.orders does not. A reader that
           // silently substitutes one schema for the other therefore returns a
           // column list that does not match the table it claims to describe.
           Exec(pg, L"CREATE TABLE archive.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text, "
                    L"archived_at date)",
                "create archive.orders") &&
           Exec(pg, L"INSERT INTO archive.orders VALUES "
                    L"(1,900,'arc','2020-01-01'),(2,901,'arc','2020-01-02'),"
                    L"(3,902,'arc','2020-01-03')",
                "seed archive.orders");
}

// The TARGET database for probe 4. Same shape, deliberately short of rows:
// public.orders is missing 1 source row, archive.orders is missing 2. Both
// tables therefore have a non-empty data diff and both reach the plan.
bool SetupTargetDb(IConnection& pg)
{
    return Exec(pg, L"CREATE SCHEMA IF NOT EXISTS archive", "tgt create archive") &&
           Exec(pg, L"CREATE TABLE public.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
                "tgt create public.orders") &&
           Exec(pg, L"INSERT INTO public.orders VALUES (1,100,'pub')",
                "tgt seed public.orders") &&
           Exec(pg, L"CREATE TABLE archive.orders("
                    L"id int PRIMARY KEY, amount int NOT NULL, tag text, "
                    L"archived_at date)",
                "tgt create archive.orders") &&
           Exec(pg, L"INSERT INTO archive.orders VALUES "
                    L"(1,900,'arc','2020-01-01')",
                "tgt seed archive.orders");
}

// ---------------------------------------------------------------------------
// PROBE 1 — the catalog listing
//
// ListTables(db) is what fills the wizard's table picker. If it returns two rows
// both spelled "orders" with nothing to tell them apart, then every layer above
// it is choosing between two indistinguishable strings, and no amount of
// stable-key discipline in the UI can recover an identity the layer below never
// provided.
// ---------------------------------------------------------------------------
void ProbeListing(IConnection& pg, const wxString& db)
{
    std::printf("\n-- PROBE 1: ListTables sees how many `orders`? --\n");

    std::vector<TableInfo> tables; wxString err;
    if (!pg.ListTables(db, tables, err)) {
        std::printf("  ERR  ListTables: %s\n", (const char*)err.utf8_str());
        return;
    }

    int named = 0;
    for (const TableInfo& t : tables) {
        if (t.name == L"orders") ++named;
        Observe("ListTables row", t.name);
    }
    ExpectEq("ListTables reports both `orders` tables", named, 2);

    // The identity question, stated as an assertion rather than a remark: are
    // the two entries distinguishable from each other at all?
    bool distinguishable = false;
    for (size_t i = 0; i < tables.size() && !distinguishable; ++i)
        for (size_t j = i + 1; j < tables.size(); ++j)
            if (tables[i].name == tables[j].name &&
                tables[i].schema != tables[j].schema) { distinguishable = true; break; }
    ExpectTrue("the two `orders` entries carry different schemas", distinguishable);
}

// ---------------------------------------------------------------------------
// PROBE 2 — introspection
//
// GetTableSchema is what the structure diff compares and what BuildRowLayout
// derives its column bridges from. Asking for archive.orders must describe
// archive.orders — in particular it must have the `archived_at` column, which
// public.orders does not.
// ---------------------------------------------------------------------------
void ProbeIntrospection(IConnection& pg, const wxString& db)
{
    std::printf("\n-- PROBE 2: GetTableSchema describes WHICH orders? --\n");

    TableSchema pub, arc; wxString e1, e2;
    const bool okPub = pg.GetTableSchema(db, QualifiedName(L"public", L"orders"), pub, e1);
    const bool okArc = pg.GetTableSchema(db, QualifiedName(L"archive", L"orders"), arc, e2);

    ExpectTrue("GetTableSchema(public.orders) succeeds", okPub);
    ExpectTrue("GetTableSchema(archive.orders) succeeds", okArc);
    if (!okPub || !okArc) {
        std::printf("  ERR  pub=%s arc=%s\n", (const char*)e1.utf8_str(),
                    (const char*)e2.utf8_str());
        return;
    }

    Observe("public.orders columns", wxString::Format(L"%d", (int)pub.columns.size()));
    Observe("archive.orders columns", wxString::Format(L"%d", (int)arc.columns.size()));

    ExpectEq("public.orders has 3 columns", (long long)pub.columns.size(), 3);
    ExpectEq("archive.orders has 4 columns", (long long)arc.columns.size(), 4);

    // The decisive one: the column that exists in exactly one of the two.
    ExpectTrue("archive.orders has archived_at", arc.FindColumn(L"archived_at") != nullptr);
    ExpectTrue("public.orders does NOT have archived_at",
               pub.FindColumn(L"archived_at") == nullptr);

    // And the identity travels back out with the schema it was read from, rather
    // than being silently reduced to a bare name.
    ExpectStr("public.orders schema round-trips", pub.name.Schema(), L"public");
    ExpectStr("archive.orders schema round-trips", arc.name.Schema(), L"archive");
}

// ---------------------------------------------------------------------------
// PROBE 3 — row streaming
//
// StreamRows is the read half of the data diff. It must read the schema it was
// asked for, not whichever one search_path happens to resolve.
// ---------------------------------------------------------------------------
void ProbeStreaming(IConnection& pg, const wxString& db)
{
    std::printf("\n-- PROBE 3: StreamRows reads WHICH orders? --\n");

    auto sumOf = [&](const QualifiedName& tbl, long long& rows, long long& sum) {
        rows = 0; sum = 0;
        StreamOptions o;
        o.columns = {L"id", L"amount"};
        o.orderBy = {L"id"};
        wxString err;
        const bool ok = pg.StreamRows(db, tbl, o,
            [&](const std::vector<Cell>& r) {
                ++rows;
                long long v = 0;
                if (r.size() > 1) r[1].text.ToLongLong(&v);
                sum += v;
                return true;
            }, err);
        if (!ok) std::printf("  ERR  StreamRows(%s): %s\n",
                             (const char*)tbl.Display().utf8_str(),
                             (const char*)err.utf8_str());
        return ok;
    };

    long long pubRows = 0, pubSum = 0, arcRows = 0, arcSum = 0;
    ExpectTrue("StreamRows(public.orders) succeeds",
               sumOf(QualifiedName(L"public", L"orders"), pubRows, pubSum));
    ExpectTrue("StreamRows(archive.orders) succeeds",
               sumOf(QualifiedName(L"archive", L"orders"), arcRows, arcSum));

    Observe("public.orders rows/sum",
            wxString::Format(L"%lld / %lld", pubRows, pubSum));
    Observe("archive.orders rows/sum",
            wxString::Format(L"%lld / %lld", arcRows, arcSum));

    ExpectEq("public.orders streams 2 rows", pubRows, 2);
    ExpectEq("public.orders sums to 300", pubSum, 300);
    ExpectEq("archive.orders streams 3 rows", arcRows, 3);
    ExpectEq("archive.orders sums to 2703", arcSum, 2703);
}

// ---------------------------------------------------------------------------
// PROBE 4 -- the whole plan, end to end
//
// This is the defect in the shape a user would meet it, and the single most
// important assertion in this file.
//
// Setup: TWO databases, each holding public.orders AND archive.orders. Source
// and target differ in row counts, so BOTH tables have a real data diff and both
// therefore earn a place in the plan (SyncEngine drops units with nothing to do,
// which is why the source and target cannot be the same database here).
//
// THE ASSERTION: the plan must hold TWO units with DISTINCT identities.
//
// Before schema-qualified identity, SyncEngine built its table universe from
// `std::set<wxString>` over BARE NAMES. Both tables hashed to "orders", the
// `seen` set admitted only the first, and the plan carried ONE unit for TWO
// tables -- whichever schema won supplied the structure diff, the data diff and
// the execution target for both. That is the conflation, and `units.size() == 2`
// is what proves it is gone.
// ---------------------------------------------------------------------------
// `src` and `tgt` MUST be two independent connections: the data diff streams
// both sides concurrently from background threads and db::IConnection is not
// thread-safe. Sharing one handle corrupts the libpq protocol rather than
// producing a wrong answer, so it would mask -- not reveal -- the behaviour
// under test.
void ProbePlanIdentity(IConnection& src, IConnection& tgt,
                       const wxString& srcDb, const wxString& tgtDb)
{
    std::printf("\n-- PROBE 4: two same-named tables in ONE plan --\n");

    SyncEngine eng(src, tgt, srcDb, tgtDb);

    SyncScope scope;
    scope.structure = true;
    scope.data      = true;
    // Deliberately UNSCOPED (every table), because that is the path the wizard
    // takes and the path the bare-name `seen` set collapsed. Naming the two
    // tables explicitly would prove less: it would test the scope filter rather
    // than the table universe.

    SyncPlan plan; wxString err;
    const bool ok = eng.BuildPlan(scope, plan, err, g_never,
                                  [](const wxString&, int, int) {});
    ExpectTrue("BuildPlan over two same-named tables succeeds", ok);
    if (!ok) {
        std::printf("  ERR  BuildPlan: %s\n", (const char*)err.utf8_str());
        return;
    }

    for (const SyncPlan::TableUnit& u : plan.units)
        Observe("plan unit", u.table.Key());

    ExpectEq("plan holds TWO units, not one", (long long)plan.units.size(), 2);
    if (plan.units.size() != 2) return;

    // Distinct identity STRINGS -- precisely what a bare table name could not
    // deliver, and the property every layer above (ui::TableKey, SyncSelection's
    // map, DataExecPlan::Find) keys on.
    ExpectTrue("the two units have DISTINCT keys",
               plan.units[0].table.Key() != plan.units[1].table.Key());
    ExpectTrue("one unit is public.orders",
               plan.units[0].table.Key() == L"public.orders" ||
               plan.units[1].table.Key() == L"public.orders");
    ExpectTrue("the other is archive.orders",
               plan.units[0].table.Key() == L"archive.orders" ||
               plan.units[1].table.Key() == L"archive.orders");

    // Each unit describes its OWN table. The column counts differ between the
    // two schemas (3 vs 4), so a unit carrying the wrong one is visible here --
    // this is the assertion that would have caught a plan that kept two units
    // but filled both from the same introspection.
    for (const SyncPlan::TableUnit& u : plan.units) {
        const bool isArchive = u.table.Schema() == L"archive";
        ExpectEq(isArchive ? "archive unit carries the 4-column schema"
                           : "public unit carries the 3-column schema",
                 (long long)u.dataSpec.srcSchema.columns.size(),
                 isArchive ? 4 : 3);
        // ...and its own row counts: public has 1 extra source row, archive 2.
        ExpectEq(isArchive ? "archive unit detected 2 inserts"
                           : "public unit detected 1 insert",
                 u.rows.Stat().inserts, isArchive ? 2 : 1);
    }
}

// ---------------------------------------------------------------------------
// PROBE 5 — the read/write asymmetry
//
// The higher-severity half: DataSync's merge streamed BOTH sides using the
// source name while DataSyncExec's applier wrote to the target name. Same-named
// tables are the only configuration where those agree — so a differently-named
// pair read one table and wrote another, silently.
//
// This probe pairs DIFFERENTLY-named tables on purpose (public.orders ->
// archive.orders_v2) and asserts the diff describes that pair: 2 source rows
// against an EMPTY target is 2 inserts. If the merge read the target as
// `public.orders` (the source name) it would compare a table against itself and
// report ZERO differences — which is exactly the silent wrong-table read.
// ---------------------------------------------------------------------------
void ProbeReadWriteAsymmetry(IConnection& src, IConnection& tgt,
                             const wxString& db)
{
    std::printf("\n-- PROBE 5: differently-named source/target pair --\n");

    if (!Exec(src, L"DROP TABLE IF EXISTS archive.orders_v2", "drop orders_v2") ||
        !Exec(src, L"CREATE TABLE archive.orders_v2("
                  L"id int PRIMARY KEY, amount int NOT NULL, tag text)",
              "create orders_v2"))
        return;

    DataDiffSpec spec;
    wxString e1, e2;
    if (!src.GetTableSchema(db, QualifiedName(L"public", L"orders"), spec.srcSchema, e1) ||
        !tgt.GetTableSchema(db, QualifiedName(L"archive", L"orders_v2"), spec.tgtSchema, e2)) {
        std::printf("  ERR  introspection: %s %s\n", (const char*)e1.utf8_str(),
                    (const char*)e2.utf8_str());
        return;
    }
    spec.srcDb = db;
    spec.tgtDb = db;

    Observe("spec source", spec.srcSchema.name.Key());
    Observe("spec target", spec.tgtSchema.name.Key());

    DataSyncOptions opt;
    opt.insert = opt.update = opt.deleteMissing = true;
    RowLayout layout; RowChangeSet set; wxString err;
    const bool ok = DiffRowChanges(src, tgt, spec, opt, layout, set, err, g_never);
    ExpectTrue("DiffRowChanges over a differently-named pair succeeds", ok);
    if (!ok) {
        std::printf("  ERR  DiffRowChanges: %s\n", (const char*)err.utf8_str());
        return;
    }

    Observe("detected ins/upd/del",
            wxString::Format(L"%lld / %lld / %lld", set.Stat().inserts,
                             set.Stat().updates, set.Stat().deletes));

    // The empty target means every source row is an insert. Zero would mean the
    // merge compared public.orders against public.orders.
    ExpectEq("2 inserts detected (target really was read as orders_v2)",
             set.Stat().inserts, 2);
    ExpectEq("no phantom updates", set.Stat().updates, 0);
    ExpectEq("no phantom deletes", set.Stat().deletes, 0);
}

} // namespace

int main()
{
    const wxString pgHost = Env("SWIFTSQL_PGTEST_HOST");
    const wxString pgUser = Env("SWIFTSQL_PGTEST_USER");
    const wxString pgPass = Env("SWIFTSQL_PGTEST_PASS");
    if (pgHost.IsEmpty() || pgUser.IsEmpty() || pgPass.IsEmpty()) {
        std::printf("SKIP mysql_pg_live_schema_test "
                    "(set SWIFTSQL_PGTEST_HOST/USER/PASS)\n");
        return 0;
    }
    const int      pgPort = EnvInt("SWIFTSQL_PGTEST_PORT", 5432);
    const wxString pgBoot = Env("SWIFTSQL_PGTEST_DB").IsEmpty()
                                ? wxString(L"postgres") : Env("SWIFTSQL_PGTEST_DB");
    const wxString db     = L"swiftsql_xschema_pg";
    // Probe 4 needs a real TARGET database: SyncEngine drops units that have
    // nothing to do, so a database compared against itself yields an empty plan
    // and would prove nothing about how many units two same-named tables make.
    const wxString db2    = L"swiftsql_xschema_pg2";

    auto maint = CreateConnection(DbType::PostgreSQL);
    wxString err;
    if (!maint->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort,
                                        pgUser, pgPass, pgBoot), err)) {
        std::printf("SKIP mysql_pg_live_schema_test (PG connect failed: %s)\n",
                    (const char*)err.utf8_str());
        return 0;
    }

    // Dropped on the way IN as well as out: a previous crashed run must not be
    // able to make this one pass or fail for the wrong reason.
    Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "drop db (pre)");
    Exec(*maint, L"DROP DATABASE IF EXISTS " + db2, "drop db2 (pre)");
    if (!Exec(*maint, L"CREATE DATABASE " + db, "create db") ||
        !Exec(*maint, L"CREATE DATABASE " + db2, "create db2")) {
        std::printf("SKIP mysql_pg_live_schema_test (CREATE DATABASE failed)\n");
        Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "drop db (post)");
        Exec(*maint, L"DROP DATABASE IF EXISTS " + db2, "drop db2 (post)");
        return 0;
    }

    {
        auto pg = CreateConnection(DbType::PostgreSQL);
        if (!pg->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort,
                                         pgUser, pgPass, db), err)) {
            std::printf("  ERR  connect to %s: %s\n", (const char*)db.utf8_str(),
                        (const char*)err.utf8_str());
            Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "drop db (post)");
            Exec(*maint, L"DROP DATABASE IF EXISTS " + db2, "drop db2 (post)");
            return 1;
        }

        std::printf("== schema-qualified table identity, live PostgreSQL ==\n");
        Observe("server version", Scalar(*pg, L"SHOW server_version"));

        if (SetupTwoSchemas(*pg)) {
            ProbeListing(*pg, db);
            ProbeIntrospection(*pg, db);
            ProbeStreaming(*pg, db);
            // Independent connections for the concurrent-merge probes (see
            // ProbePlanIdentity's note): pgT is bound to the TARGET database,
            // pgS is a second handle on the source.
            auto pgT = CreateConnection(DbType::PostgreSQL);
            auto pgS = CreateConnection(DbType::PostgreSQL);
            wxString e2, e3;
            const bool okT = pgT->Connect(mplive::Profile(DbType::PostgreSQL, pgHost,
                                          pgPort, pgUser, pgPass, db2), e2);
            const bool okS = pgS->Connect(mplive::Profile(DbType::PostgreSQL, pgHost,
                                          pgPort, pgUser, pgPass, db), e3);
            if (okT && okS && SetupTargetDb(*pgT)) {
                ProbePlanIdentity(*pg, *pgT, db, db2);
                // The asymmetry probe stays within ONE database -- it is about
                // two differently-NAMED tables, not two databases.
                ProbeReadWriteAsymmetry(*pg, *pgS, db);
            } else {
                std::printf("  ERR  second/target connection: %s %s\n",
                            (const char*)e2.utf8_str(), (const char*)e3.utf8_str());
                ++mplive::g_fails;
            }
            pgT->Disconnect();
            pgS->Disconnect();
        } else {
            std::printf("  ERR  fixture setup failed\n");
            ++mplive::g_fails;
        }
        pg->Disconnect();
    }

    Exec(*maint, L"DROP DATABASE IF EXISTS " + db, "drop db (post)");
    Exec(*maint, L"DROP DATABASE IF EXISTS " + db2, "drop db2 (post)");
    maint->Disconnect();

    std::printf("\n== %d checks, %d failures ==\n", mplive::g_checks, mplive::g_fails);
    return mplive::g_fails == 0 ? 0 : 1;
}
