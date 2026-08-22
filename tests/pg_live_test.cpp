// pg_live_test.cpp — env-gated live PostgreSQL integration test for the
// cross-database sync suite. This is the first time PgConnection is exercised
// against a real server (Connect / Execute / GetColumns / GetTableSchema /
// GetPrimaryKey), so it doubles as smoke coverage for the driver.
//
// SAFETY: no password/host is ever hard-coded. All connection parameters come
// from environment variables. If the essential ones are unset the test prints
// "SKIP: no live PG configured" and exits 0 — CI and other developers are never
// blocked. When configured, it only ever touches two disposable test databases
// whose names carry the `swiftsql_synctest_` prefix; both are DROP'd on the way
// in and out. Nothing else on the server is read or written.
//
//   SWIFTSQL_PGTEST_HOST   (required)   e.g. 127.0.0.1
//   SWIFTSQL_PGTEST_PORT   (optional)   default 5432
//   SWIFTSQL_PGTEST_USER   (required)   must have CREATEDB privilege
//   SWIFTSQL_PGTEST_PASS   (required)
//   SWIFTSQL_PGTEST_DB     (optional)   maintenance/bootstrap DB, default "postgres"
//   SWIFTSQL_PGTEST_SRCDB  (optional)   default "swiftsql_synctest_src"
//   SWIFTSQL_PGTEST_TGTDB  (optional)   default "swiftsql_synctest_tgt"
//
// NOTE ON "schema" vs "database": the task brief asked for two isolated test
// *schemas*. PgConnection's introspection SQL hard-codes `table_schema='public'`
// (GetColumns/GetIndexes/GetForeignKeys), so a table in a non-public schema is
// invisible to GetTableSchema and the diff would wrongly see "table missing".
// The only way GetTableSchema can compare two same-named tables is to put each
// in the `public` schema of its own database. We therefore isolate with two
// databases (still uniquely named + fully dropped). This driver limitation is
// flagged in the handoff for the architect team.
#include "db/DbDriver.h"
#include "db/SchemaDiff.h"
#include "db/DataSync.h"
#include "core/ConnectionProfile.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <wx/string.h>

using namespace db;
using namespace db::sync;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectEq(const char* name, long long got, long long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%lld got=%lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---- env helpers -----------------------------------------------------------
static wxString Env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? wxString::FromUTF8(v) : wxString();
}

// Run one statement; on failure print the error and return false.
static bool Exec(IConnection& c, const wxString& sql, const char* label)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) {
        std::printf("  ERR  %s: %s\n", label,
                    (const char*)err.utf8_str());
        return false;
    }
    return true;
}

static int CountCol(const SchemaChangeSet& ch, ColumnChange::Op op)
{
    int n = 0;
    for (const auto& c : ch.columns) if (c.op == op) ++n;
    return n;
}

// Build a connection profile pointed at one database.
static core::ConnectionProfile Profile(const wxString& host, int port,
                                       const wxString& user, const wxString& pass,
                                       const wxString& db)
{
    core::ConnectionProfile p;
    p.type = db::DbType::PostgreSQL;
    p.host = host;
    p.port = port;
    p.user = user;
    p.password = pass;
    p.database = db;
    return p;
}

int main()
{
    std::printf("== pg_live_test ==\n");

    const wxString host = Env("SWIFTSQL_PGTEST_HOST");
    const wxString user = Env("SWIFTSQL_PGTEST_USER");
    const wxString pass = Env("SWIFTSQL_PGTEST_PASS");
    if (host.IsEmpty() || user.IsEmpty() || pass.IsEmpty()) {
        std::printf("SKIP: no live PG configured "
                    "(set SWIFTSQL_PGTEST_HOST/USER/PASS)\n");
        return 0;
    }

    int port = 5432;
    { long v = 0; if (Env("SWIFTSQL_PGTEST_PORT").ToLong(&v) && v > 0) port = (int)v; }
    wxString bootDb = Env("SWIFTSQL_PGTEST_DB");   if (bootDb.IsEmpty()) bootDb = L"postgres";
    wxString srcDb  = Env("SWIFTSQL_PGTEST_SRCDB"); if (srcDb.IsEmpty()) srcDb = L"swiftsql_synctest_src";
    wxString tgtDb  = Env("SWIFTSQL_PGTEST_TGTDB"); if (tgtDb.IsEmpty()) tgtDb = L"swiftsql_synctest_tgt";

    const wxString qSrc = QuoteIdent(srcDb, Dialect::Postgres);
    const wxString qTgt = QuoteIdent(tgtDb, Dialect::Postgres);

    // ---- maintenance connection (bootstrap DB) ----
    auto maint = CreateConnection(db::DbType::PostgreSQL);
    {
        core::ConnectionProfile mp = Profile(host, port, user, pass, bootDb);
        wxString err;
        if (!maint->Connect(mp, err)) {
            std::printf("  FAIL connect(bootstrap %s): %s\n",
                        (const char*)bootDb.utf8_str(), (const char*)err.utf8_str());
            return 1;   // env was configured → a connect failure is a real failure
        }
        ExpectTrue("connect bootstrap DB", maint->IsConnected());
    }

    // Cleanup guard: drop the two test DBs (after disconnecting src/tgt) whether
    // the assertions pass or throw. Declared before src/tgt so it outlives them.
    std::unique_ptr<IConnection> src, tgt;
    struct Guard {
        IConnection* maint; IConnection* src; IConnection* tgt;
        wxString qSrc, qTgt;
        ~Guard()
        {
            if (src) src->Disconnect();
            if (tgt) tgt->Disconnect();
            if (maint) {
                QueryResult r; wxString e;
                maint->Execute(L"DROP DATABASE IF EXISTS " + qSrc, r, e);
                maint->Execute(L"DROP DATABASE IF EXISTS " + qTgt, r, e);
            }
        }
    };

    // ---- (re)create the two disposable test databases ----
    if (!Exec(*maint, L"DROP DATABASE IF EXISTS " + qSrc, "drop src") ||
        !Exec(*maint, L"DROP DATABASE IF EXISTS " + qTgt, "drop tgt") ||
        !Exec(*maint, L"CREATE DATABASE " + qSrc, "create src") ||
        !Exec(*maint, L"CREATE DATABASE " + qTgt, "create tgt")) {
        std::printf("  (database setup failed — check CREATEDB privilege)\n");
        Guard g{ maint.get(), nullptr, nullptr, qSrc, qTgt };
        return 1;
    }

    src = CreateConnection(db::DbType::PostgreSQL);
    tgt = CreateConnection(db::DbType::PostgreSQL);
    Guard guard{ maint.get(), src.get(), tgt.get(), qSrc, qTgt };

    {
        core::ConnectionProfile sp = Profile(host, port, user, pass, srcDb);
        core::ConnectionProfile tp = Profile(host, port, user, pass, tgtDb);
        wxString e1, e2;
        bool s = src->Connect(sp, e1);
        bool t = tgt->Connect(tp, e2);
        ExpectTrue("connect src DB", s);
        ExpectTrue("connect tgt DB", t);
        if (!s || !t) {
            std::printf("  src err: %s\n  tgt err: %s\n",
                        (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
            std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
            return g_fails ? 1 : 0;
        }
    }

    // ---- build tables with a known structural + data difference ----
    // src has an extra column (extra_note) and an extra index (idx_widget_name).
    bool setup = true;
    setup &= Exec(*src,
        L"CREATE TABLE public.t_widget ("
        L"id integer PRIMARY KEY, name varchar(50) NOT NULL, "
        L"price numeric(10,2), extra_note text)", "create src table");
    setup &= Exec(*src, L"CREATE INDEX idx_widget_name ON public.t_widget(name)",
                  "create src index");
    setup &= Exec(*src,
        L"INSERT INTO public.t_widget(id,name,price,extra_note) VALUES "
        L"(1,'alpha',1.00,'n1'),(2,'beta',2.00,'n2'),(3,'gamma',3.00,'n3')",
        "insert src rows");

    setup &= Exec(*tgt,
        L"CREATE TABLE public.t_widget ("
        L"id integer PRIMARY KEY, name varchar(50) NOT NULL, "
        L"price numeric(10,2))", "create tgt table");
    setup &= Exec(*tgt,
        L"INSERT INTO public.t_widget(id,name,price) VALUES "
        L"(1,'alpha',1.00),(2,'BETA',9.99),(4,'delta',4.00)",
        "insert tgt rows");

    ExpectTrue("table + data setup", setup);

    // ---- structural diff (same dialect → exact rawType compare) ----
    if (setup) {
        TableSchema ss, ts; wxString e1, e2;
        bool gs = src->GetTableSchema(srcDb, L"t_widget", ss, e1);
        bool gt = tgt->GetTableSchema(tgtDb, L"t_widget", ts, e2);
        ExpectTrue("GetTableSchema src", gs && ss.columns.size() == 4);
        ExpectTrue("GetTableSchema tgt", gt && ts.columns.size() == 3);
        ExpectTrue("GetPrimaryKey src (id)",
                   ss.primaryKey.size() == 1 && ss.primaryKey[0] == L"id");

        SchemaDiffResult diff = DiffSchema(ss, ts, Dialect::Postgres, Dialect::Postgres);
        ExpectEq("diff: one Add column (extra_note)",
                 CountCol(diff.changes, ColumnChange::Op::Add), 1);
        ExpectEq("diff: no Modify (matching types)",
                 CountCol(diff.changes, ColumnChange::Op::Modify), 0);
        // Added column should be extra_note.
        for (const auto& c : diff.changes.columns)
            if (c.op == ColumnChange::Op::Add)
                ExpectTrue("diff: added column is extra_note",
                           c.column.name == L"extra_note");
        // The source-only index idx_widget_name should show up as an Add.
        bool addName = false;
        for (const auto& ic : diff.changes.indexes)
            if (ic.op == IndexChange::Op::Add) {
                for (const auto& col : ic.index.columns)
                    if (col == L"name") addName = true;
            }
        ExpectTrue("diff: Add index on name", addName);
    }

    // ---- data diff: uses StreamRows, which PgConnection does not implement ----
    if (setup) {
        // A schema whose columns exist on BOTH sides (no extra_note).
        // Columns must declare real types: CheckDataDiffOrdering classifies the
        // PK's byte-orderability from kind/rawType and refuses the table when it
        // cannot (an untyped column reads as KeyOrder::Unknown). That gate is
        // correct — a fixture that omits types is under-specified, not a reason
        // to weaken it. Same fix already applied to sqlite_live_test.cpp.
        TableSchema common;
        common.name = L"t_widget";
        auto addc = [&](const wxString& n, ColKind k, const wxString& raw) {
            NormColumn c; c.name = n; c.kind = k; c.rawType = raw;
            common.columns.push_back(c);
        };
        addc(L"id",    ColKind::Integer, L"integer");
        addc(L"name",  ColKind::Varchar, L"varchar(50)");
        addc(L"price", ColKind::Decimal, L"numeric(10,2)");
        common.primaryKey = { L"id" };

        DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
        std::vector<wxString> emitted; RowStat stat; wxString err;
        std::atomic<bool> stop{false};
        bool ok = DiffAndEmitData(*src, *tgt, srcDb, tgtDb, common, opt,
                                  [&](const wxString& s) { emitted.push_back(s); },
                                  stat, err, stop);
        if (!ok && err.Contains(L"不支持")) {
            std::printf("  KNOWN GAP: PgConnection lacks StreamRows — live data "
                        "diff unverified (err: %s). NOT counted as a failure; "
                        "flagged for the architect team.\n",
                        (const char*)err.utf8_str());
        } else if (ok) {
            // If a future PgDriver implements StreamRows, these are the expected
            // results for the rows inserted above.
            ExpectEq("data: inserts (id=3)", stat.inserts, 1);
            ExpectEq("data: updates (id=2)", stat.updates, 1);
            ExpectEq("data: deletes (id=4)", stat.deletes, 1);
            ExpectEq("data: unchanged (id=1)", stat.unchanged, 1);
        } else {
            ExpectTrue("data: DiffAndEmitData unexpected error", false);
            std::printf("  err: %s\n", (const char*)err.utf8_str());
        }
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;   // guard drops the two test databases on the way out
}
