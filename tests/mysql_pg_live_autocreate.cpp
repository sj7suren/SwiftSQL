// mysql_pg_live_autocreate.cpp — the LIVE proof for cross-engine auto-CREATE-
// TABLE (A5): a table that exists on the SOURCE and is entirely MISSING on the
// target is now created on the target in the target's own dialect, then
// populated by a subsequent data sync.
//
// WHY THIS FILE EXISTS. DiffSchema used to REFUSE a cross-engine whole-table
// create (a Finding, never a CREATE). It now translates every column's type
// through the same Interpret(src)->Render(tgt) path the column-level Add/Modify
// uses and hands a target-native TableSchema to the target driver's existing
// RenderCreateTable. That write path earns confidence only from OBSERVED
// behaviour on real servers — the standing rule of this suite. So every
// assertion here reads the TARGET CATALOG back (information_schema / pg_indexes)
// and checks the columns, their types, the index, the identity attribute and
// the row count that actually landed — never the generated SQL string.
//
// Three proofs:
//   A  MySQL -> PostgreSQL: a realistic column mix (int PK+auto_increment,
//      varchar, text, tinyint(1) boolean-ish, datetime, decimal, a secondary
//      index) missing on PG -> created + populated, verified from pg_catalog.
//   B  PostgreSQL -> MySQL: the reverse direction, verified from MySQL's
//      information_schema.
//   C  the honest-refusal case: a MySQL `bigint unsigned` column (Unmappable to
//      PG) refuses the WHOLE table with a Finding naming that column, and the
//      target table is verifiably ABSENT from the catalog. A green result that
//      couldn't have failed proves nothing — so the refusal is confirmed by the
//      table NOT being there, not by trusting the plan.
//
// SAFETY: identical contract to mysql_pg_live.h — nothing hard-coded, every
// parameter from SWIFTSQL_{MY,PG}TEST_*, SKIP + exit 0 when unset, and only ever
// touches databases carrying the `swiftsql_xcreate_` prefix (dropped on the way
// IN and OUT). Uses the SAME env variables as mysql_pg_live_test.cpp.
#include "mysql_pg_live.h"

#include "db/SyncEngine.h"
#include "db/DataSync.h"
#include "db/DataSyncExec.h"

#include <atomic>
#include <memory>

using namespace db;
using namespace db::sync;
using mplive::Env;
using mplive::EnvInt;
using mplive::Exec;
using mplive::Observe;
using mplive::ScalarOf;
using mplive::ExpectEq;
using mplive::ExpectTrue;

namespace {

const std::atomic<bool> g_never{false};

// A count read back from the catalog as text -> integer (-1 on any parse
// failure, which then fails the ExpectEq honestly rather than reading as 0).
long long CountVal(const wxString& s)
{
    long v = -1;
    return s.ToLong(&v) ? (long long)v : -1;
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

// Run the STRUCTURE-only sync pass end to end against the real servers — the
// auto-CREATE under test — and return whether Execute() succeeded. `planOut`
// carries the plan back so the caller can inspect its warnings.
bool RunStructure(IConnection& src, IConnection& tgt, const wxString& srcDb,
                  const wxString& tgtDb, const wxString& table,
                  SyncPlan& planOut, const char* tag)
{
    SyncEngine eng(src, tgt, srcDb, tgtDb);
    SyncScope scope;
    scope.structure = true;
    scope.data      = false;
    scope.tables.push_back(QualifiedName(table));

    wxString err;
    if (!eng.BuildPlan(scope, planOut, err, g_never, {})) {
        std::printf("  ERR  %s BuildPlan: %s\n", tag, (const char*)err.utf8_str());
        return false;
    }
    // Structure-only: the overload that refuses data plans (there are none here).
    const bool ok = eng.Execute(planOut, true, err, g_never, {});
    if (!ok) std::printf("  ERR  %s structure Execute: %s\n", tag,
                         (const char*)err.utf8_str());
    return ok;
}

// Populate the freshly-created target — the "subsequent data sync" half. Built
// the way production's execute pass is (DataDiffSpec from a REAL introspection
// of BOTH sides + ExecuteDataSync), which is the proven cross-engine data path:
// reading the target's REAL schema is what makes the value bridges convert
// (MySQL tinyint 1/0 -> PG boolean t/f, and back), and it side-steps the
// separate cross-engine table-identity asymmetry in BuildPlan's whole-database
// table universe (a MySQL bare `widgets` vs a PG `public.widgets` are two keys
// there) — not this feature's concern.
bool Populate(IConnection& src, IConnection& tgt, const wxString& srcDb,
              const wxString& tgtDb, const wxString& table, const char* tag)
{
    DataDiffSpec spec;
    wxString e1, e2;
    if (!src.GetTableSchema(srcDb, QualifiedName(table), spec.srcSchema, e1) ||
        !tgt.GetTableSchema(tgtDb, QualifiedName(table), spec.tgtSchema, e2)) {
        std::printf("  ERR  %s populate introspection: %s / %s\n", tag,
                    (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
        return false;
    }
    spec.srcDb = srcDb;
    spec.tgtDb = tgtDb;

    TableDataSpec allow(spec.SourceTable().Key());
    allow.EnableInserts();
    std::vector<TableExecJob> jobs{ TableExecJob{ spec, std::move(allow) } };

    DataSyncOptions base;
    DataExecResult  res;
    wxString        err;
    const bool ok = ExecuteDataSync(src, tgt, jobs, base, res, err, g_never, {});
    for (const auto& r : res.tables)
        std::printf("  OBS  %s populate [%s] committed=%d ins=%lld err=[%s]\n",
                    tag, (const char*)r.table.utf8_str(), (int)r.committed,
                    r.inserts, (const char*)r.error.utf8_str());
    if (!ok) std::printf("  ERR  %s ExecuteDataSync: %s\n", tag,
                         (const char*)err.utf8_str());
    return ok;
}

// ---------------------------------------------------------------------------
// PROOF A — MySQL -> PostgreSQL: create a missing table, then populate it.
// ---------------------------------------------------------------------------
void ProofMyToPg(IConnection& my, IConnection& pg, const wxString& myDb,
                 const wxString& pgDb)
{
    std::printf("\n== PROOF A: MySQL -> PostgreSQL auto-create + populate ==\n");
    const wxString qMy = QuoteIdent(myDb, Dialect::MySQL);

    // Source: a realistic mix. `active` is the boolean-ish tinyint(1); the KEY on
    // `name` is the secondary index that must survive translation.
    const bool setup =
        Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".widgets", "drop my widgets") &&
        Exec(my, L"CREATE TABLE " + qMy + L".widgets ("
                 L"  id INT NOT NULL AUTO_INCREMENT,"
                 L"  name VARCHAR(80) NOT NULL,"
                 L"  descr TEXT,"
                 L"  active TINYINT(1) NOT NULL DEFAULT 1,"
                 L"  created_at DATETIME NOT NULL,"
                 L"  price DECIMAL(10,2) NOT NULL,"
                 L"  PRIMARY KEY (id),"
                 L"  KEY ix_name (name)"
                 L") ENGINE=InnoDB", "create my widgets") &&
        Exec(my, L"INSERT INTO " + qMy + L".widgets (name,descr,active,created_at,price) VALUES "
                 L"('alpha','first',1,'2021-01-01 10:00:00',9.99),"
                 L"('beta',NULL,0,'2021-02-02 11:30:00',19.50),"
                 L"('gamma','third',1,'2021-03-03 12:45:00',5.00)",
             "seed my widgets");
    if (!setup) { ExpectTrue("PROOF A fixture built", false); return; }

    // PG target: the table does NOT exist. Confirm that BEFORE the sync so the
    // "created" assertion cannot be satisfied by a stale table.
    ExpectEq("A: target has NO widgets before sync",
             CountVal(ScalarOf(pg, L"SELECT count(*) FROM information_schema.tables "
                                 L"WHERE table_name='widgets'")), 0);

    // Pass 1: structure only — this is the auto-CREATE under test.
    SyncPlan sp;
    if (!RunStructure(my, pg, myDb, pgDb, L"widgets", sp, "A/struct")) {
        ExpectTrue("A: structure sync succeeded", false); return;
    }
    ExpectTrue("A: structure sync succeeded", true);

    // Ruling 6: the `active`/`price` defaults are source-dialect literals that do
    // not carry across engines, so they were omitted with a warning naming the
    // table (a MySQL `DEFAULT 1` on what became a PG boolean would break CREATE).
    bool defWarn = false;
    for (const wxString& w : sp.warnings)
        if (w.Contains(L"默认值未随") && w.Contains(L"widgets")) defWarn = true;
    ExpectTrue("A: default-omission warned (ruling 6)", defWarn);

    // ---- read the PG catalog back: the table, its columns and their types ----
    ExpectEq("A: widgets now EXISTS on PG",
             CountVal(ScalarOf(pg, L"SELECT count(*) FROM information_schema.tables "
                                 L"WHERE table_name='widgets'")), 1);

    auto pgType = [&](const wxString& col) {
        return ScalarOf(pg, L"SELECT data_type FROM information_schema.columns "
                            L"WHERE table_name='widgets' AND column_name='" + col + L"'");
    };
    Observe("A pg id type",         pgType(L"id"));
    Observe("A pg name type",       pgType(L"name"));
    Observe("A pg descr type",      pgType(L"descr"));
    Observe("A pg active type",     pgType(L"active"));
    Observe("A pg created_at type", pgType(L"created_at"));
    Observe("A pg price type",      pgType(L"price"));

    ExpectStr("A: int -> bigint",                     pgType(L"id"),         L"bigint");
    ExpectStr("A: varchar(80) -> character varying",  pgType(L"name"),       L"character varying");
    ExpectStr("A: text -> text",                      pgType(L"descr"),      L"text");
    ExpectStr("A: tinyint(1) -> boolean",             pgType(L"active"),     L"boolean");
    ExpectStr("A: datetime -> timestamp",             pgType(L"created_at"), L"timestamp without time zone");
    ExpectStr("A: decimal(10,2) -> numeric",          pgType(L"price"),      L"numeric");

    // The varchar length survived (not collapsed to an unbounded text).
    ExpectStr("A: varchar length carried",
              ScalarOf(pg, L"SELECT character_maximum_length FROM information_schema.columns "
                           L"WHERE table_name='widgets' AND column_name='name'"), L"80");
    // AUTO_INCREMENT -> a real identity column (ruling 4, best-effort by the
    // driver — never a refusal).
    ExpectStr("A: auto_increment -> identity",
              ScalarOf(pg, L"SELECT is_identity FROM information_schema.columns "
                           L"WHERE table_name='widgets' AND column_name='id'"), L"YES");
    // PK carried (ruling 5) — essential for the data sync that follows.
    ExpectEq("A: primary key carried",
             CountVal(ScalarOf(pg,
                 L"SELECT count(*) FROM information_schema.table_constraints "
                 L"WHERE table_name='widgets' AND constraint_type='PRIMARY KEY'")), 1);
    // Secondary index translated + included (ruling 3).
    ExpectEq("A: secondary index ix_name created",
             CountVal(ScalarOf(pg, L"SELECT count(*) FROM pg_indexes "
                                 L"WHERE tablename='widgets' AND indexname='ix_name'")), 1);

    // Pass 2: the subsequent data sync populates the freshly-created table.
    if (!Populate(my, pg, myDb, pgDb, L"widgets", "A/data")) {
        ExpectTrue("A: data sync succeeded", false); return;
    }
    ExpectTrue("A: data sync succeeded", true);

    ExpectEq("A: 3 rows populated on PG",
             CountVal(ScalarOf(pg, L"SELECT count(*) FROM widgets")), 3);
    // A value read back through the translated types: the boolean-ish tinyint
    // and the decimal, for the row that is unambiguous.
    ExpectStr("A: row 'alpha' active+price landed correctly",
              ScalarOf(pg, L"SELECT active::text || '|' || price::text "
                           L"FROM widgets WHERE name='alpha'"), L"true|9.99");
    ExpectStr("A: row 'beta' NULL descr + false active landed",
              ScalarOf(pg, L"SELECT coalesce(descr,'<null>') || '|' || active::text "
                           L"FROM widgets WHERE name='beta'"), L"<null>|false");
}

// ---------------------------------------------------------------------------
// PROOF B — PostgreSQL -> MySQL: the reverse direction.
// ---------------------------------------------------------------------------
void ProofPgToMy(IConnection& pg, IConnection& my, const wxString& pgDb,
                 const wxString& myDb)
{
    std::printf("\n== PROOF B: PostgreSQL -> MySQL auto-create + populate ==\n");
    const wxString qMy = QuoteIdent(myDb, Dialect::MySQL);

    const bool setup =
        Exec(pg, L"DROP TABLE IF EXISTS gadgets", "drop pg gadgets") &&
        Exec(pg, L"CREATE TABLE gadgets ("
                 L"  id integer GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,"
                 L"  label varchar(60) NOT NULL,"
                 L"  notes text,"
                 L"  flag boolean NOT NULL DEFAULT false,"
                 L"  made_on date NOT NULL,"
                 L"  weight numeric(8,3) NOT NULL)", "create pg gadgets") &&
        Exec(pg, L"CREATE INDEX ix_label ON gadgets(label)", "create pg ix_label") &&
        Exec(pg, L"INSERT INTO gadgets (label,notes,flag,made_on,weight) VALUES "
                 L"('one','n1',true,'2022-05-05',1.250),"
                 L"('two',NULL,false,'2022-06-06',2.500),"
                 L"('three','n3',true,'2022-07-07',3.750)", "seed pg gadgets");
    if (!setup) { ExpectTrue("PROOF B fixture built", false); return; }

    ExpectEq("B: target has NO gadgets before sync",
             CountVal(ScalarOf(my, L"SELECT count(*) FROM information_schema.tables "
                                 L"WHERE table_schema='" + myDb + L"' AND table_name='gadgets'")), 0);

    SyncPlan sp;
    if (!RunStructure(pg, my, pgDb, myDb, L"gadgets", sp, "B/struct")) {
        ExpectTrue("B: structure sync succeeded", false); return;
    }
    ExpectTrue("B: structure sync succeeded", true);

    ExpectEq("B: gadgets now EXISTS on MySQL",
             CountVal(ScalarOf(my, L"SELECT count(*) FROM information_schema.tables "
                                 L"WHERE table_schema='" + myDb + L"' AND table_name='gadgets'")), 1);

    auto myType = [&](const wxString& col) {
        return ScalarOf(my, L"SELECT data_type FROM information_schema.columns "
                            L"WHERE table_schema='" + myDb + L"' AND table_name='gadgets' "
                            L"AND column_name='" + col + L"'");
    };
    Observe("B my id type",     myType(L"id"));
    Observe("B my label type",  myType(L"label"));
    Observe("B my notes type",  myType(L"notes"));
    Observe("B my flag type",   myType(L"flag"));
    Observe("B my made_on type",myType(L"made_on"));
    Observe("B my weight type", myType(L"weight"));

    ExpectStr("B: integer -> bigint",           myType(L"id"),      L"bigint");
    ExpectStr("B: varchar(60) -> varchar",      myType(L"label"),   L"varchar");
    ExpectStr("B: text -> text",                myType(L"notes"),   L"text");
    ExpectStr("B: boolean -> tinyint",          myType(L"flag"),    L"tinyint");
    ExpectStr("B: date -> date",                myType(L"made_on"), L"date");
    ExpectStr("B: numeric(8,3) -> decimal",     myType(L"weight"),  L"decimal");

    // identity -> AUTO_INCREMENT (ruling 4).
    ExpectStr("B: identity -> auto_increment",
              ScalarOf(my, L"SELECT extra FROM information_schema.columns "
                           L"WHERE table_schema='" + myDb + L"' AND table_name='gadgets' "
                           L"AND column_name='id'"), L"auto_increment");
    // PK (ruling 5) + secondary index (ruling 3).
    ExpectEq("B: primary key carried",
             CountVal(ScalarOf(my, L"SELECT count(*) FROM information_schema.statistics "
                                 L"WHERE table_schema='" + myDb + L"' AND table_name='gadgets' "
                                 L"AND index_name='PRIMARY'")), 1);
    ExpectEq("B: secondary index ix_label created",
             CountVal(ScalarOf(my, L"SELECT count(*) FROM information_schema.statistics "
                                 L"WHERE table_schema='" + myDb + L"' AND table_name='gadgets' "
                                 L"AND index_name='ix_label'")), 1);

    if (!Populate(pg, my, pgDb, myDb, L"gadgets", "B/data")) {
        ExpectTrue("B: data sync succeeded", false); return;
    }
    ExpectTrue("B: data sync succeeded", true);

    ExpectEq("B: 3 rows populated on MySQL",
             CountVal(ScalarOf(my, L"SELECT count(*) FROM " + qMy + L".gadgets")), 3);
    ExpectStr("B: row 'one' flag+weight landed correctly",
              ScalarOf(my, L"SELECT concat(flag,'|',weight) FROM " + qMy +
                           L".gadgets WHERE label='one'"), L"1|1.250");
}

// ---------------------------------------------------------------------------
// PROOF C — the honest refusal: an Unmappable column refuses the WHOLE table.
// ---------------------------------------------------------------------------
void ProofRefusal(IConnection& my, IConnection& pg, const wxString& myDb,
                  const wxString& pgDb)
{
    std::printf("\n== PROOF C: Unmappable column -> whole-table refusal ==\n");
    const wxString qMy = QuoteIdent(myDb, Dialect::MySQL);

    const bool setup =
        Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".badunsigned", "drop my badunsigned") &&
        Exec(my, L"CREATE TABLE " + qMy + L".badunsigned ("
                 L"  id INT NOT NULL PRIMARY KEY,"
                 L"  big BIGINT UNSIGNED NOT NULL"   // no safe PG type
                 L") ENGINE=InnoDB", "create my badunsigned");
    if (!setup) { ExpectTrue("PROOF C fixture built", false); return; }

    SyncEngine eng(my, pg, myDb, pgDb);
    SyncScope scope; scope.structure = true; scope.data = false;
    scope.tables.push_back(QualifiedName(L"badunsigned"));
    SyncPlan plan; wxString err;
    const bool built = eng.BuildPlan(scope, plan, err, g_never, {});
    ExpectTrue("C: BuildPlan ok", built);

    // No DDL unit — the whole table was refused, not partially created.
    bool anyCreate = false;
    for (const auto& u : plan.units)
        if (u.changes.createTable || !u.ddl.empty()) anyCreate = true;
    ExpectTrue("C: no createTable / DDL unit emitted", !anyCreate);

    // The refusal names the offending column and its source type.
    bool named = false;
    for (const wxString& w : plan.warnings) {
        std::printf("  RSN  C plan warning: %s\n", (const char*)w.utf8_str());
        if (w.Contains(L"无法自动建表") && w.Contains(L"big")) named = true;
    }
    ExpectTrue("C: a Finding names column `big` and refuses the table", named);

    // Execute the (empty-of-DDL) plan and then PROVE the table is ABSENT from
    // the PG catalog — a green result that couldn't have failed proves nothing,
    // so the refusal is confirmed by the table NOT being there.
    const bool ran = eng.Execute(plan, true, err, g_never, {});
    ExpectTrue("C: Execute of the refused plan is a no-op success", ran);
    ExpectEq("C: badunsigned is ABSENT from the PG catalog",
             CountVal(ScalarOf(pg, L"SELECT count(*) FROM information_schema.tables "
                                 L"WHERE table_name='badunsigned'")), 0);
}

// ---------------------------------------------------------------------------
// PROOF D — THE P0: re-syncing structure must NOT re-CREATE existing tables.
// ---------------------------------------------------------------------------
// The user report: structure sync into an EMPTY PostgreSQL target failed with
// `relation "OpenIddictApplications" already exists`. Root cause: MySQL
// ListTables yields BARE names, PostgreSQL yields SCHEMA-QUALIFIED ones, so on
// the SECOND BuildPlan a table the first run had just auto-created was reported
// as still missing (bare src key != `public.`-qualified tgt key) and its CREATE
// was re-emitted — which the server rejects. This proof drives the FULL
// BuildPlan/Execute path (the user's path) TWICE over an OpenIddict-shaped set
// of camelCase tables with a diamond of FKs, and pins that the second run
// re-creates NOTHING and executes clean. Before the identity fix the second
// Execute died with exactly the reported error.
void ProofReSyncIdempotent(IConnection& my, IConnection& pg, const wxString& myDb,
                           const wxString& pgDb)
{
    std::printf("\n== PROOF D: cross-engine re-sync is idempotent (the P0) ==\n");
    const wxString qMy = QuoteIdent(myDb, Dialect::MySQL);

    // BIGINT keys (not INT) and no AUTO_INCREMENT on purpose: this proof isolates
    // the TABLE-IDENTITY P0 (the `already exists` re-CREATE) from an orthogonal,
    // pre-existing cross-engine COLUMN-diff churn — MySQL `int` auto-widens to PG
    // `bigint` on create, but ColumnDiffersCrossDialect then re-flags int-vs-
    // bigint on the re-diff and PG's Modify path drags a `DROP DEFAULT` onto an
    // identity column. That is a separate defect (see the report), so keeping the
    // columns round-tripping lets run 2 be the genuinely clean no-op the P0
    // requires. varchar and bigint round-trip; the FK diamond and camelCase names
    // — the actual P0 trigger — are preserved.
    const bool setup =
        Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictTokens`",         "drop toks") &&
        Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictAuthorizations`", "drop auths") &&
        Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictScopes`",         "drop scopes") &&
        Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictApplications`",   "drop apps") &&
        Exec(my, L"CREATE TABLE " + qMy + L".`OpenIddictApplications` ("
                 L"  `Id` BIGINT NOT NULL PRIMARY KEY,"
                 L"  `ClientId` VARCHAR(100) NOT NULL) ENGINE=InnoDB", "create apps") &&
        Exec(my, L"CREATE TABLE " + qMy + L".`OpenIddictAuthorizations` ("
                 L"  `Id` BIGINT NOT NULL PRIMARY KEY,"
                 L"  `AppId` BIGINT NOT NULL,"
                 L"  CONSTRAINT `FK_Auth_App` FOREIGN KEY (`AppId`) "
                 L"    REFERENCES `OpenIddictApplications`(`Id`)) ENGINE=InnoDB", "create auths") &&
        Exec(my, L"CREATE TABLE " + qMy + L".`OpenIddictScopes` ("
                 L"  `Id` BIGINT NOT NULL PRIMARY KEY,"
                 L"  `Name` VARCHAR(100) NOT NULL) ENGINE=InnoDB", "create scopes") &&
        Exec(my, L"CREATE TABLE " + qMy + L".`OpenIddictTokens` ("
                 L"  `Id` BIGINT NOT NULL PRIMARY KEY,"
                 L"  `AppId` BIGINT NOT NULL, `AuthId` BIGINT NOT NULL,"
                 L"  CONSTRAINT `FK_Tok_App` FOREIGN KEY (`AppId`) "
                 L"    REFERENCES `OpenIddictApplications`(`Id`),"
                 L"  CONSTRAINT `FK_Tok_Auth` FOREIGN KEY (`AuthId`) "
                 L"    REFERENCES `OpenIddictAuthorizations`(`Id`)) ENGINE=InnoDB", "create toks");
    if (!setup) { ExpectTrue("PROOF D fixture built", false); return; }

    const wchar_t* tbls[] = { L"OpenIddictApplications", L"OpenIddictAuthorizations",
                              L"OpenIddictScopes", L"OpenIddictTokens" };

    // The target must be genuinely empty of these four so "created" is real.
    for (const wchar_t* t : tbls)
        Exec(pg, L"DROP TABLE IF EXISTS \"" + wxString(t) + L"\" CASCADE", "pg drop pre");

    auto countTbls = [&]() {
        return CountVal(ScalarOf(pg,
            L"SELECT count(*) FROM information_schema.tables "
            L"WHERE table_schema='public' AND table_name IN "
            L"('OpenIddictApplications','OpenIddictAuthorizations',"
            L"'OpenIddictScopes','OpenIddictTokens')"));
    };
    ExpectEq("D: none of the four exist on PG before sync", countTbls(), 0);

    auto runOnce = [&](const char* tag, int& createCount, bool& executed) {
        SyncEngine eng(my, pg, myDb, pgDb);
        SyncScope scope; scope.structure = true; scope.data = false;
        for (const wchar_t* t : tbls) scope.tables.push_back(QualifiedName(t));
        SyncPlan plan; wxString err;
        if (!eng.BuildPlan(scope, plan, err, g_never, {})) {
            std::printf("  ERR  %s BuildPlan: %s\n", tag, (const char*)err.utf8_str());
            createCount = -1; executed = false; return;
        }
        createCount = 0;
        for (const auto& u : plan.units) if (u.changes.createTable) ++createCount;
        executed = eng.Execute(plan, true, err, g_never, {});
        if (!executed) std::printf("  ERR  %s Execute: %s\n", tag,
                                   (const char*)err.utf8_str());
    };

    // ---- run 1: the auto-create into the empty target ----
    int c1 = -2; bool e1 = false;
    runOnce("D/run1", c1, e1);
    ExpectEq("D: run1 plan creates all four tables", c1, 4);
    ExpectTrue("D: run1 structure Execute succeeded", e1);
    ExpectEq("D: all four tables now exist on PG", countTbls(), 4);
    // camelCase survived verbatim — the tool quoted the identity, so PG did not
    // fold it to lower case (which would itself have hidden the table on re-diff).
    ExpectEq("D: camelCase identity preserved exactly",
             CountVal(ScalarOf(pg, L"SELECT count(*) FROM information_schema.tables "
                                 L"WHERE table_name='OpenIddictApplications'")), 1);

    // ---- run 2: THE P0 — re-sync must SEE the tables and re-create NONE ----
    int c2 = -2; bool e2 = false;
    runOnce("D/run2", c2, e2);
    ExpectEq("D: run2 re-diff re-creates NOTHING (existing tables are seen)", c2, 0);
    ExpectTrue("D: run2 Execute is a clean no-op — no 'already exists'", e2);
    ExpectEq("D: still exactly four tables after the re-sync", countTbls(), 4);

    // Clean up this proof's tables so a re-run starts from the same empty slate.
    for (const wchar_t* t : tbls)
        Exec(pg, L"DROP TABLE IF EXISTS \"" + wxString(t) + L"\" CASCADE", "pg drop post");
    Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictTokens`",         "my drop post toks");
    Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictAuthorizations`", "my drop post auths");
    Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictScopes`",         "my drop post scopes");
    Exec(my, L"DROP TABLE IF EXISTS " + qMy + L".`OpenIddictApplications`",   "my drop post apps");
}

} // namespace

int main()
{
    const wxString myHost = Env("SWIFTSQL_MYTEST_HOST");
    const wxString myUser = Env("SWIFTSQL_MYTEST_USER");
    const wxString myPass = Env("SWIFTSQL_MYTEST_PASS");
    const wxString pgHost = Env("SWIFTSQL_PGTEST_HOST");
    const wxString pgUser = Env("SWIFTSQL_PGTEST_USER");
    const wxString pgPass = Env("SWIFTSQL_PGTEST_PASS");
    if (myHost.IsEmpty() || myUser.IsEmpty() || myPass.IsEmpty() ||
        pgHost.IsEmpty() || pgUser.IsEmpty() || pgPass.IsEmpty()) {
        std::printf("SKIP mysql_pg_live_autocreate_test "
                    "(set SWIFTSQL_MYTEST_* and SWIFTSQL_PGTEST_*)\n");
        return 0;
    }
    const int      myPort = EnvInt("SWIFTSQL_MYTEST_PORT", 3306);
    const int      pgPort = EnvInt("SWIFTSQL_PGTEST_PORT", 5432);
    const wxString myBoot = Env("SWIFTSQL_MYTEST_DB").IsEmpty()
                                ? wxString(L"mysql") : Env("SWIFTSQL_MYTEST_DB");
    const wxString pgBoot = Env("SWIFTSQL_PGTEST_DB").IsEmpty()
                                ? wxString(L"postgres") : Env("SWIFTSQL_PGTEST_DB");
    const wxString myDb = L"swiftsql_xcreate_my";
    const wxString pgDb = L"swiftsql_xcreate_pg";

    auto myMaint = CreateConnection(DbType::MySQL);
    auto pgMaint = CreateConnection(DbType::PostgreSQL);
    wxString err;
    if (!myMaint->Connect(mplive::Profile(DbType::MySQL, myHost, myPort,
                                          myUser, myPass, myBoot), err)) {
        std::printf("SKIP mysql_pg_live_autocreate_test (MySQL connect: %s)\n",
                    (const char*)err.utf8_str());
        return 0;
    }
    if (!pgMaint->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort,
                                          pgUser, pgPass, pgBoot), err)) {
        std::printf("SKIP mysql_pg_live_autocreate_test (PG connect: %s)\n",
                    (const char*)err.utf8_str());
        return 0;
    }

    // Dropped on the way IN as well as OUT.
    Exec(*myMaint, L"DROP DATABASE IF EXISTS " + QuoteIdent(myDb, Dialect::MySQL), "drop my db (pre)");
    Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + pgDb, "drop pg db (pre)");
    if (!Exec(*myMaint, L"CREATE DATABASE " + QuoteIdent(myDb, Dialect::MySQL), "create my db") ||
        !Exec(*pgMaint, L"CREATE DATABASE " + pgDb, "create pg db")) {
        std::printf("SKIP mysql_pg_live_autocreate_test (CREATE DATABASE failed)\n");
        Exec(*myMaint, L"DROP DATABASE IF EXISTS " + QuoteIdent(myDb, Dialect::MySQL), "drop my db (post)");
        Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + pgDb, "drop pg db (post)");
        return 0;
    }

    {
        auto my = CreateConnection(DbType::MySQL);
        auto pg = CreateConnection(DbType::PostgreSQL);
        wxString e1, e2;
        const bool okMy = my->Connect(mplive::Profile(DbType::MySQL, myHost, myPort,
                                                      myUser, myPass, myDb), e1);
        const bool okPg = pg->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort,
                                                      pgUser, pgPass, pgDb), e2);
        if (okMy && okPg) {
            std::printf("== cross-engine auto-CREATE-TABLE, live MySQL + PostgreSQL ==\n");
            Observe("MySQL version", ScalarOf(*my, L"SELECT version()"));
            Observe("PG version",    ScalarOf(*pg, L"SHOW server_version"));

            ProofMyToPg(*my, *pg, myDb, pgDb);
            ProofPgToMy(*pg, *my, pgDb, myDb);
            ProofRefusal(*my, *pg, myDb, pgDb);
            ProofReSyncIdempotent(*my, *pg, myDb, pgDb);
        } else {
            std::printf("  ERR  connect to test DBs: %s / %s\n",
                        (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
            ++mplive::g_fails;
        }
        my->Disconnect();
        pg->Disconnect();
    }

    Exec(*myMaint, L"DROP DATABASE IF EXISTS " + QuoteIdent(myDb, Dialect::MySQL), "drop my db (post)");
    Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + pgDb, "drop pg db (post)");
    myMaint->Disconnect();
    pgMaint->Disconnect();

    std::printf("\n== %d checks, %d failures ==\n", mplive::g_checks, mplive::g_fails);
    return mplive::g_fails == 0 ? 0 : 1;
}
