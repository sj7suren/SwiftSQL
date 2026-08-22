// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ddl_test.cpp — offline unit tests for the *real* DDL/literal rendering that
// sync_test.cpp deliberately stubs out.
//
// sync_test drives orchestration through a StubConn whose RenderSchemaChange
// returns a canned "CREATE TABLE t"/"ALTER TABLE t" — so the genuine dialect DDL
// produced by MySqlConnection::RenderSchemaChange / PgConnection::RenderSchemaChange
// (which fan out into the db::detail helpers in MySqlSql.cpp / PgSql.cpp) was
// never asserted. This file closes that gap.
//
// Key trick (self-verified below): RenderSchemaChange is pure rendering — it only
// touches QuoteIdent + the db::detail free functions, never the MYSQL*/PGconn*
// handle — so we drive it through db::CreateConnection(...) instances that are
// *never* Connect()ed. No wire protocol, no DB client library at runtime.
//
// Also covers db::RenderLiteral (SqlLiteral.cpp) across every Dialect × CellKind
// with the escaping/framing edge cases, and db::LimitOffsetClause.
//
// Same dependency-free harness as sync_test.cpp: an ExpectTrue/ExpectStr/ExpectSub
// assert loop, non-zero exit on any failure, no doctest/Catch2.
#include "db/DbDriver.h"
#include "db/SchemaModel.h"
#include "db/SyncTypes.h"
#include "core/DbTypes.h"

#include <cstdio>
#include <vector>
#include <wx/string.h>

using namespace db;

static int g_checks = 0;
static int g_fails  = 0;

static wxString Vis(const wxString& s)
{
    wxString o = s;
    o.Replace(L"\r", L"\\r");
    o.Replace(L"\n", L"\\n");
    return o;
}

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)Vis(want).utf8_str(),
                    (const char*)Vis(got).utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// Substring assertion (structure without over-fitting the exact byte layout).
static void ExpectSub(const char* name, const wxString& hay, const wxString& needle)
{
    ++g_checks;
    if (!hay.Contains(needle)) {
        ++g_fails;
        std::printf("  FAIL %s\n    expected to contain: [%s]\n    in                 : [%s]\n",
                    name, (const char*)Vis(needle).utf8_str(),
                    (const char*)Vis(hay).utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectNoSub(const char* name, const wxString& hay, const wxString& needle)
{
    ++g_checks;
    if (hay.Contains(needle)) {
        ++g_fails;
        std::printf("  FAIL %s\n    expected NOT to contain: [%s]\n    in                     : [%s]\n",
                    name, (const char*)Vis(needle).utf8_str(),
                    (const char*)Vis(hay).utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---- small builders for schema value objects ------------------------------
static NormColumn Col(const wxString& name, ColKind kind, const wxString& raw,
                      long long length = -1, int scale = -1)
{
    NormColumn c;
    c.name    = name;
    c.kind    = kind;
    c.rawType = raw;
    c.length  = length;
    c.scale   = scale;
    return c;
}

// A cross-engine ColumnChange must never hand a foreign dialect's native
// rawType text to a driver expecting its OWN native syntax — PgSql.cpp's
// PgTypeText (post the A2 ternary fix) trusts a present rawType verbatim, on
// the assumption it is either genuine same-dialect text or was already
// translated to this dialect's syntax upstream (SyncTypeMap/DialectProfile,
// A1/A3). These fixtures build ONE NormColumn/TableSchema and feed it through
// BOTH the MySQL and Postgres renderers to compare their output side by side;
// MySQL's ColumnDef always uses rawType verbatim by design, so the shared
// fixture's MySQL-native rawType ("int", "varchar(120)"...) is correct for the
// MySQL render but must be stripped before the Postgres render, so
// PgTypeText's kind->PgTypeFromKind fallback fires — exactly the "already
// cross-engine-translated" contract PG's real callers (A4) uphold.
static NormColumn StripRawType(NormColumn c) { c.rawType.Clear(); return c; }
static TableSchema StripRawType(TableSchema s)
{
    for (auto& c : s.columns) c = StripRawType(c);
    return s;
}
static SchemaChangeSet StripRawType(SchemaChangeSet ch)
{
    if (ch.createTable) ch.createSchema = StripRawType(ch.createSchema);
    for (auto& cc : ch.columns)
        if (cc.op != ColumnChange::Op::Drop) cc.column = StripRawType(cc.column);
    return ch;
}

// Index of the first statement in `stmts` that contains `needle` (-1 = none).
static int FindStmt(const std::vector<wxString>& stmts, const wxString& needle)
{
    for (size_t i = 0; i < stmts.size(); ++i)
        if (stmts[i].Contains(needle)) return (int)i;
    return -1;
}

// Join for a single-blob substring check when we don't care which statement.
static wxString JoinAll(const std::vector<wxString>& stmts)
{
    wxString s;
    for (const auto& x : stmts) { s += x; s += L"\n"; }
    return s;
}

// ===========================================================================
//  Self-verification: RenderSchemaChange on an *unconnected* driver instance
//  does not crash / does not depend on connection state.
// ===========================================================================
static void TestUnconnectedAssumption()
{
    std::printf("[self-check — unconnected RenderSchemaChange]\n");

    auto my = CreateConnection(DbType::MySQL);
    auto pg = CreateConnection(DbType::PostgreSQL);
    ExpectTrue("factory: MySQL instance",  my != nullptr);
    ExpectTrue("factory: Postgres instance", pg != nullptr);
    ExpectTrue("assumption: MySQL not connected",  my && !my->IsConnected());
    ExpectTrue("assumption: Postgres not connected", pg && !pg->IsConnected());
    ExpectTrue("dialect: MySQL",    my && my->GetDialect() == Dialect::MySQL);
    ExpectTrue("dialect: Postgres", pg && pg->GetDialect() == Dialect::Postgres);

    // A trivial dropTable set — proves the call path runs with no live handle.
    SchemaChangeSet ch; ch.table = L"probe"; ch.dropTable = true;
    std::vector<wxString> ms, ps; wxString me, pe;
    bool mok = my && my->RenderSchemaChange(ch, ms, me);
    bool pok = pg && pg->RenderSchemaChange(ch, ps, pe);
    ExpectTrue("unconnected MySQL render ok",  mok && ms.size() == 1);
    ExpectTrue("unconnected Postgres render ok", pok && ps.size() == 1);
}

// ===========================================================================
//  1a. createTable — MySQL (single inline statement) vs Postgres (multi-stmt).
// ===========================================================================
static TableSchema UsersSchema()
{
    // A realistic table: PK, a NOT NULL col with default, a secondary unique
    // index, and a self-explanatory FK.
    TableSchema s;
    s.name = L"users";
    NormColumn id = Col(L"id", ColKind::Integer, L"int", -1);
    id.notNull = true; id.autoIncrement = true;
    NormColumn email = Col(L"email", ColKind::Varchar, L"varchar(120)", 120);
    email.notNull = true;
    NormColumn name = Col(L"name", ColKind::Varchar, L"varchar(50)", 50);
    name.hasDefault = true; name.defaultExpr = L"anon";
    NormColumn oid = Col(L"org_id", ColKind::Integer, L"int");
    s.columns = { id, email, name, oid };
    s.primaryKey = { L"id" };

    NormIndex uq; uq.name = L"uq_email"; uq.columns = { L"email" }; uq.unique = true;
    s.indexes.push_back(uq);

    NormForeignKey fk; fk.name = L"fk_org"; fk.columns = { L"org_id" };
    fk.refTable = L"orgs"; fk.refColumns = { L"id" };
    fk.onDelete = L"CASCADE";
    s.foreignKeys.push_back(fk);

    s.engine = L"InnoDB"; s.charset = L"utf8mb4";
    return s;
}

static void TestCreateTable(IConnection& my, IConnection& pg)
{
    std::printf("[createTable — MySQL vs Postgres]\n");

    SchemaChangeSet ch;
    ch.table = L"users";
    ch.createTable = true;
    ch.createSchema = UsersSchema();

    std::vector<wxString> ms, ps; wxString me, pe;
    ExpectTrue("mysql: render ok", my.RenderSchemaChange(ch, ms, me));
    // PG render gets the same change set with rawType stripped — see
    // StripRawType's banner comment for why (its MySQL-native rawType is not
    // valid PG input; kind must drive PgTypeFromKind instead).
    ExpectTrue("pg: render ok",    pg.RenderSchemaChange(StripRawType(ch), ps, pe));

    // ---- MySQL: everything inline in ONE statement ----
    ExpectTrue("mysql: single statement", ms.size() == 1);
    const wxString m = ms.empty() ? wxString() : ms[0];
    ExpectSub("mysql: CREATE TABLE keyword", m, L"CREATE TABLE");
    ExpectSub("mysql: backtick table ident", m, L"`users`");
    ExpectSub("mysql: backtick column ident", m, L"`email` varchar(120) NOT NULL");
    ExpectSub("mysql: autoincrement col", m, L"`id` int NOT NULL AUTO_INCREMENT");
    ExpectSub("mysql: string default quoted", m, L"`name` varchar(50) DEFAULT 'anon'");
    ExpectSub("mysql: inline PRIMARY KEY", m, L"PRIMARY KEY (`id`)");
    ExpectSub("mysql: inline UNIQUE INDEX", m, L"UNIQUE INDEX `uq_email` (`email`)");
    ExpectSub("mysql: inline FK constraint", m, L"CONSTRAINT `fk_org` FOREIGN KEY (`org_id`) REFERENCES `orgs` (`id`)");
    ExpectSub("mysql: FK on delete", m, L"ON DELETE CASCADE");
    ExpectSub("mysql: engine option", m, L"ENGINE=InnoDB");
    ExpectSub("mysql: charset option", m, L"DEFAULT CHARSET=utf8mb4");
    ExpectNoSub("mysql: no double-quote idents", m, L"\"users\"");

    // ---- Postgres: CREATE TABLE + separate CREATE INDEX + separate ALTER ADD FK ----
    ExpectTrue("pg: multiple statements (>=3)", ps.size() >= 3);
    const wxString pcreate = ps.empty() ? wxString() : ps[0];
    ExpectSub("pg: CREATE TABLE keyword", pcreate, L"CREATE TABLE");
    ExpectSub("pg: double-quote table ident", pcreate, L"\"users\"");
    ExpectSub("pg: double-quote column ident", pcreate, L"\"email\" character varying(120) NOT NULL");
    ExpectSub("pg: identity for autoinc", pcreate, L"\"id\" integer GENERATED BY DEFAULT AS IDENTITY");
    ExpectSub("pg: inline PRIMARY KEY", pcreate, L"PRIMARY KEY (\"id\")");
    ExpectNoSub("pg: no backtick idents", pcreate, L"`");

    // The secondary index is its own CREATE INDEX statement (schema-level object).
    int idxStmt = FindStmt(ps, L"CREATE UNIQUE INDEX");
    ExpectTrue("pg: CREATE UNIQUE INDEX is own stmt", idxStmt >= 0);
    if (idxStmt >= 0) {
        ExpectSub("pg: index names table+col", ps[idxStmt],
                  L"\"uq_email\" ON \"users\" (\"email\")");
        ExpectTrue("pg: index after CREATE TABLE", idxStmt > 0);
    }
    // The FK is its own ALTER TABLE ADD statement.
    int fkStmt = FindStmt(ps, L"FOREIGN KEY");
    ExpectTrue("pg: FK is own stmt", fkStmt >= 0);
    if (fkStmt >= 0) {
        ExpectSub("pg: ALTER ADD FK", ps[fkStmt],
                  L"ALTER TABLE \"users\" ADD CONSTRAINT \"fk_org\" FOREIGN KEY (\"org_id\") REFERENCES \"orgs\" (\"id\")");
        ExpectSub("pg: FK on delete", ps[fkStmt], L"ON DELETE CASCADE");
    }

    // ---- dialect contrast: same changeSet, opposite quoting ----
    ExpectTrue("contrast: MySQL uses backticks, PG uses double-quotes",
               m.Contains(L"`users`") && pcreate.Contains(L"\"users\"") &&
               !m.Contains(L"\"users\"") && !pcreate.Contains(L"`"));
}

// ===========================================================================
//  1b. ADD / MODIFY / DROP columns — dialect differences.
// ===========================================================================
static void TestColumnChanges(IConnection& my, IConnection& pg)
{
    std::printf("[column ADD/MODIFY/DROP — MySQL vs Postgres]\n");

    // ---- ADD COLUMN (with positional hint on MySQL) ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        NormColumn add = Col(L"age", ColKind::Integer, L"int");
        add.notNull = true;
        ColumnChange cc; cc.op = ColumnChange::Op::Add; cc.column = add;
        cc.afterColumn = L"id";
        ch.columns.push_back(cc);

        std::vector<wxString> ms, ps; wxString me, pe;
        my.RenderSchemaChange(ch, ms, me);
        pg.RenderSchemaChange(StripRawType(ch), ps, pe);
        ExpectStr("add: mysql text", ms.empty() ? wxString() : ms[0],
                  L"ALTER TABLE `t` ADD COLUMN `age` int NOT NULL AFTER `id`");
        // PG has no positional AFTER; column appended.
        ExpectStr("add: pg text", ps.empty() ? wxString() : ps[0],
                  L"ALTER TABLE \"t\" ADD COLUMN \"age\" integer NOT NULL");
        ExpectNoSub("add: pg no AFTER clause", ps.empty() ? wxString() : ps[0], L"AFTER");
    }

    // ---- MODIFY COLUMN: MySQL single MODIFY vs PG multi ALTER COLUMN sub-clauses ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        NormColumn mod = Col(L"name", ColKind::Varchar, L"varchar(40)", 40);
        mod.notNull = true; mod.hasDefault = true; mod.defaultExpr = L"guest";
        ColumnChange cc; cc.op = ColumnChange::Op::Modify; cc.column = mod;
        ch.columns.push_back(cc);

        std::vector<wxString> ms, ps; wxString me, pe;
        my.RenderSchemaChange(ch, ms, me);
        pg.RenderSchemaChange(StripRawType(ch), ps, pe);

        // MySQL: exactly one MODIFY COLUMN statement using the engine-native
        // rawType. Note the shared defaultExpr is rendered by *dialect contract*:
        // MySQL's RenderDefault quotes+escapes a bare string ("guest" → 'guest'),
        // while PG's PgDefault emits it verbatim (see the SET DEFAULT assert below).
        ExpectTrue("modify: mysql single stmt", ms.size() == 1);
        ExpectStr("modify: mysql MODIFY COLUMN", ms.empty() ? wxString() : ms[0],
                  L"ALTER TABLE `t` MODIFY COLUMN `name` varchar(40) NOT NULL DEFAULT 'guest'");

        // PG: three independent ALTER COLUMN clauses (TYPE, nullability, default).
        ExpectTrue("modify: pg multi stmt (3)", ps.size() == 3);
        int tyStmt  = FindStmt(ps, L"TYPE");
        int nnStmt  = FindStmt(ps, L"SET NOT NULL");
        int defStmt = FindStmt(ps, L"SET DEFAULT");
        ExpectTrue("modify: pg has TYPE clause", tyStmt >= 0);
        ExpectTrue("modify: pg has SET NOT NULL clause", nnStmt >= 0);
        ExpectTrue("modify: pg has SET DEFAULT clause", defStmt >= 0);
        if (tyStmt >= 0)
            ExpectSub("modify: pg TYPE maps kind→pg type", ps[tyStmt],
                      L"ALTER COLUMN \"name\" TYPE character varying(40)");
        if (defStmt >= 0)
            ExpectSub("modify: pg default verbatim", ps[defStmt],
                      L"SET DEFAULT guest");
        // Ordering inside the modify: TYPE precedes nullability precedes default.
        ExpectTrue("modify: pg clause order TYPE<NN<DEF",
                   tyStmt >= 0 && nnStmt > tyStmt && defStmt > nnStmt);
    }

    // ---- MODIFY without NOT NULL / default → PG emits DROP NOT NULL + DROP DEFAULT ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        NormColumn mod = Col(L"note", ColKind::Text, L"text");   // nullable, no default
        ColumnChange cc; cc.op = ColumnChange::Op::Modify; cc.column = mod;
        ch.columns.push_back(cc);

        std::vector<wxString> ps; wxString pe;
        pg.RenderSchemaChange(ch, ps, pe);
        ExpectTrue("modify(null): pg DROP NOT NULL", FindStmt(ps, L"DROP NOT NULL") >= 0);
        ExpectTrue("modify(null): pg DROP DEFAULT", FindStmt(ps, L"DROP DEFAULT") >= 0);
    }

    // ---- DROP COLUMN ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        ColumnChange cc; cc.op = ColumnChange::Op::Drop;
        cc.column = Col(L"legacy", ColKind::Text, L"text");
        ch.columns.push_back(cc);

        std::vector<wxString> ms, ps; wxString me, pe;
        my.RenderSchemaChange(ch, ms, me);
        pg.RenderSchemaChange(ch, ps, pe);
        ExpectStr("drop: mysql text", ms.empty() ? wxString() : ms[0],
                  L"ALTER TABLE `t` DROP COLUMN `legacy`");
        ExpectStr("drop: pg text", ps.empty() ? wxString() : ps[0],
                  L"ALTER TABLE \"t\" DROP COLUMN \"legacy\"");
    }
}

// ===========================================================================
//  1c. Index & FK add/drop; dropTable; and the ADD-before-DROP ordering.
// ===========================================================================
static void TestIndexFkAndOrdering(IConnection& my, IConnection& pg)
{
    std::printf("[index/FK add+drop, dropTable, ordering]\n");

    // ---- index Add / Drop ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        NormIndex add; add.name = L"ix_new"; add.columns = { L"a", L"b" };
        IndexChange ia; ia.op = IndexChange::Op::Add; ia.index = add;
        NormIndex drop; drop.name = L"ix_old"; drop.columns = { L"c" };
        IndexChange id; id.op = IndexChange::Op::Drop; id.index = drop;
        ch.indexes = { ia, id };

        std::vector<wxString> ms, ps; wxString me, pe;
        my.RenderSchemaChange(ch, ms, me);
        pg.RenderSchemaChange(ch, ps, pe);

        // MySQL: ADD INDEX via ALTER TABLE; DROP INDEX via ALTER TABLE.
        ExpectTrue("idx: mysql ADD INDEX", FindStmt(ms, L"ADD INDEX `ix_new` (`a`, `b`)") >= 0);
        ExpectTrue("idx: mysql DROP INDEX", FindStmt(ms, L"DROP INDEX `ix_old`") >= 0);
        // PG: CREATE INDEX (schema-level) + DROP INDEX IF EXISTS.
        ExpectTrue("idx: pg CREATE INDEX", FindStmt(ps, L"CREATE INDEX \"ix_new\" ON \"t\" (\"a\", \"b\")") >= 0);
        ExpectTrue("idx: pg DROP INDEX IF EXISTS", FindStmt(ps, L"DROP INDEX IF EXISTS \"ix_old\"") >= 0);
        // Ordering: the Add precedes the Drop.
        ExpectTrue("idx: mysql add before drop",
                   FindStmt(ms, L"ADD INDEX") < FindStmt(ms, L"DROP INDEX"));
        ExpectTrue("idx: pg add before drop",
                   FindStmt(ps, L"CREATE INDEX") < FindStmt(ps, L"DROP INDEX"));
    }

    // ---- FK Add / Drop ----
    {
        SchemaChangeSet ch; ch.table = L"orders";
        NormForeignKey add; add.name = L"fk_c"; add.columns = { L"cid" };
        add.refTable = L"customers"; add.refColumns = { L"id" };
        FkChange fa; fa.op = FkChange::Op::Add; fa.fk = add;
        NormForeignKey drop; drop.name = L"fk_old"; drop.columns = { L"x" };
        drop.refTable = L"y"; drop.refColumns = { L"id" };
        FkChange fd; fd.op = FkChange::Op::Drop; fd.fk = drop;
        ch.foreignKeys = { fa, fd };

        std::vector<wxString> ms, ps; wxString me, pe;
        my.RenderSchemaChange(ch, ms, me);
        pg.RenderSchemaChange(ch, ps, pe);

        ExpectTrue("fk: mysql ADD FK",
                   FindStmt(ms, L"ADD CONSTRAINT `fk_c` FOREIGN KEY (`cid`) REFERENCES `customers` (`id`)") >= 0);
        // MySQL drops FK with DROP FOREIGN KEY; PG with DROP CONSTRAINT.
        ExpectTrue("fk: mysql DROP FOREIGN KEY", FindStmt(ms, L"DROP FOREIGN KEY `fk_old`") >= 0);
        ExpectTrue("fk: pg ADD CONSTRAINT",
                   FindStmt(ps, L"ADD CONSTRAINT \"fk_c\" FOREIGN KEY (\"cid\") REFERENCES \"customers\" (\"id\")") >= 0);
        ExpectTrue("fk: pg DROP CONSTRAINT", FindStmt(ps, L"DROP CONSTRAINT \"fk_old\"") >= 0);
        ExpectTrue("fk: mysql add before drop",
                   FindStmt(ms, L"ADD CONSTRAINT") < FindStmt(ms, L"DROP FOREIGN KEY"));
    }

    // ---- combined change set: DROP-class emitted in reverse (FK → index → col),
    //      and all ADD/MODIFY come before any DROP-class. ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        // one column Add + one column Drop
        ColumnChange addc; addc.op = ColumnChange::Op::Add;
        addc.column = Col(L"newcol", ColKind::Integer, L"int");
        ColumnChange dropc; dropc.op = ColumnChange::Op::Drop;
        dropc.column = Col(L"oldcol", ColKind::Text, L"text");
        ch.columns = { addc, dropc };
        // one index Drop
        NormIndex di; di.name = L"ix_gone"; di.columns = { L"oldcol" };
        IndexChange idc; idc.op = IndexChange::Op::Drop; idc.index = di;
        ch.indexes = { idc };
        // one FK Drop
        NormForeignKey df; df.name = L"fk_gone"; df.columns = { L"oldcol" };
        df.refTable = L"o"; df.refColumns = { L"id" };
        FkChange fdc; fdc.op = FkChange::Op::Drop; fdc.fk = df;
        ch.foreignKeys = { fdc };

        std::vector<wxString> ms; wxString me;
        my.RenderSchemaChange(ch, ms, me);
        int addCol   = FindStmt(ms, L"ADD COLUMN `newcol`");
        int dropFk   = FindStmt(ms, L"DROP FOREIGN KEY `fk_gone`");
        int dropIdx  = FindStmt(ms, L"DROP INDEX `ix_gone`");
        int dropCol  = FindStmt(ms, L"DROP COLUMN `oldcol`");
        ExpectTrue("order: ADD COLUMN present", addCol >= 0);
        ExpectTrue("order: DROP FK present", dropFk >= 0);
        ExpectTrue("order: DROP INDEX present", dropIdx >= 0);
        ExpectTrue("order: DROP COLUMN present", dropCol >= 0);
        // ADD precedes every DROP-class.
        ExpectTrue("order: ADD before all DROPs",
                   addCol < dropFk && addCol < dropIdx && addCol < dropCol);
        // DROP-class reverse dependency order: FK → index → column.
        ExpectTrue("order: DROP FK before DROP INDEX before DROP COLUMN",
                   dropFk < dropIdx && dropIdx < dropCol);
    }

    // ---- dropTable short-circuit ----
    {
        SchemaChangeSet ch; ch.table = L"junk"; ch.dropTable = true;
        // populate other members to prove dropTable short-circuits them.
        ColumnChange cc; cc.op = ColumnChange::Op::Add;
        cc.column = Col(L"x", ColKind::Integer, L"int");
        ch.columns.push_back(cc);

        std::vector<wxString> ms, ps; wxString me, pe;
        my.RenderSchemaChange(ch, ms, me);
        pg.RenderSchemaChange(ch, ps, pe);
        ExpectStr("droptable: mysql", ms.empty() ? wxString() : ms[0],
                  L"DROP TABLE IF EXISTS `junk`");
        ExpectStr("droptable: pg", ps.empty() ? wxString() : ps[0],
                  L"DROP TABLE IF EXISTS \"junk\"");
        ExpectTrue("droptable: mysql short-circuits (1 stmt)", ms.size() == 1);
        ExpectTrue("droptable: pg short-circuits (1 stmt)", ps.size() == 1);
    }

    // ---- MySQL PRIMARY KEY index add/drop special-casing ----
    {
        SchemaChangeSet ch; ch.table = L"t";
        NormIndex pk; pk.name = L"PRIMARY"; pk.columns = { L"id" };
        pk.primary = true; pk.unique = true;
        IndexChange add; add.op = IndexChange::Op::Add; add.index = pk;
        ch.indexes = { add };
        std::vector<wxString> ms; wxString me;
        my.RenderSchemaChange(ch, ms, me);
        ExpectTrue("pk: mysql ADD PRIMARY KEY", FindStmt(ms, L"ADD PRIMARY KEY (`id`)") >= 0);

        SchemaChangeSet ch2; ch2.table = L"t";
        IndexChange drop; drop.op = IndexChange::Op::Drop; drop.index = pk;
        ch2.indexes = { drop };
        std::vector<wxString> ms2; wxString me2;
        my.RenderSchemaChange(ch2, ms2, me2);
        ExpectTrue("pk: mysql DROP PRIMARY KEY", FindStmt(ms2, L"DROP PRIMARY KEY") >= 0);
    }
}

// ===========================================================================
//  2. RenderLiteral — Dialect × CellKind matrix.
// ===========================================================================
static Cell MkNull()                 { return Cell{ CellKind::Null,    L"" }; }
static Cell MkNum(const wxString& t)  { return Cell{ CellKind::Numeric, t }; }
static Cell MkTxt(const wxString& t)  { return Cell{ CellKind::Text,    t }; }
static Cell MkBin(const wxString& h)  { return Cell{ CellKind::Binary,  h }; }

static void TestRenderLiteral()
{
    std::printf("[RenderLiteral — dialect x cellkind matrix]\n");

    const Dialect all[] = { Dialect::MySQL, Dialect::Postgres, Dialect::Oracle,
                            Dialect::SqlServer, Dialect::Sqlite };
    const char* names[] = { "mysql", "postgres", "oracle", "sqlserver", "sqlite" };

    // ---- Null → NULL for every dialect ----
    for (int i = 0; i < 5; ++i) {
        wxString nm = wxString::Format(L"null(%s)", wxString::FromUTF8(names[i]).wc_str());
        ExpectStr((const char*)wxString::Format(L"null:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkNull(), all[i]), L"NULL");
    }

    // ---- Numeric → bare, unquoted; empty numeric → NULL (guard) ----
    for (int i = 0; i < 5; ++i) {
        ExpectStr((const char*)wxString::Format(L"num:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkNum(L"-12.50"), all[i]), L"-12.50");
        ExpectStr((const char*)wxString::Format(L"num-empty:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkNum(L""), all[i]), L"NULL");
    }

    // ---- Text: quoted; single quote doubled on ALL dialects ----
    for (int i = 0; i < 5; ++i) {
        ExpectStr((const char*)wxString::Format(L"txt:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkTxt(L"abc"), all[i]), L"'abc'");
        ExpectStr((const char*)wxString::Format(L"txt-empty:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkTxt(L""), all[i]), L"''");
        ExpectStr((const char*)wxString::Format(L"txt-quote:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkTxt(L"O'Brien"), all[i]), L"'O''Brien'");
    }

    // ---- Text backslash: MySQL doubles it; standard SQL leaves it alone ----
    ExpectStr("txt-backslash:mysql (doubled)",
              RenderLiteral(MkTxt(L"a\\b"), Dialect::MySQL), L"'a\\\\b'");
    ExpectStr("txt-backslash:postgres (verbatim)",
              RenderLiteral(MkTxt(L"a\\b"), Dialect::Postgres), L"'a\\b'");
    ExpectStr("txt-backslash:sqlserver (verbatim)",
              RenderLiteral(MkTxt(L"a\\b"), Dialect::SqlServer), L"'a\\b'");
    ExpectStr("txt-backslash:oracle (verbatim)",
              RenderLiteral(MkTxt(L"a\\b"), Dialect::Oracle), L"'a\\b'");
    ExpectStr("txt-backslash:sqlite (verbatim)",
              RenderLiteral(MkTxt(L"a\\b"), Dialect::Sqlite), L"'a\\b'");

    // ---- Text: MySQL escapes both backslash AND quote in one string ----
    ExpectStr("txt-both:mysql",
              RenderLiteral(MkTxt(L"a\\'b"), Dialect::MySQL), L"'a\\\\''b'");
    ExpectStr("txt-both:postgres",
              RenderLiteral(MkTxt(L"a\\'b"), Dialect::Postgres), L"'a\\''b'");

    // ---- Binary: dialect framing of prefix-less UC hex ----
    ExpectStr("bin:mysql (0x)",
              RenderLiteral(MkBin(L"DEADBEEF"), Dialect::MySQL), L"0xDEADBEEF");
    ExpectStr("bin:sqlserver (0x)",
              RenderLiteral(MkBin(L"DEADBEEF"), Dialect::SqlServer), L"0xDEADBEEF");
    ExpectStr("bin:sqlite (X'..')",
              RenderLiteral(MkBin(L"DEADBEEF"), Dialect::Sqlite), L"X'DEADBEEF'");
    ExpectStr("bin:postgres (bytea \\x)",
              RenderLiteral(MkBin(L"DEADBEEF"), Dialect::Postgres), L"'\\xDEADBEEF'");
    ExpectStr("bin:oracle (HEXTORAW)",
              RenderLiteral(MkBin(L"DEADBEEF"), Dialect::Oracle), L"HEXTORAW('DEADBEEF')");

    // ---- Binary empty → '' on every dialect (0x/\x need digits) ----
    for (int i = 0; i < 5; ++i)
        ExpectStr((const char*)wxString::Format(L"bin-empty:%s", wxString::FromUTF8(names[i]).wc_str()).utf8_str(),
                  RenderLiteral(MkBin(L""), all[i]), L"''");
}

// ===========================================================================
//  3. LimitOffsetClause — dialect windowing.
// ===========================================================================
static void TestLimitOffset()
{
    std::printf("[LimitOffsetClause — dialect windowing]\n");

    ExpectStr("limit:mysql", LimitOffsetClause(Dialect::MySQL, 10, 20),
              L" LIMIT 10 OFFSET 20");
    ExpectStr("limit:postgres", LimitOffsetClause(Dialect::Postgres, 10, 20),
              L" LIMIT 10 OFFSET 20");
    ExpectStr("limit:sqlite", LimitOffsetClause(Dialect::Sqlite, 10, 20),
              L" LIMIT 10 OFFSET 20");
    ExpectStr("limit:sqlserver", LimitOffsetClause(Dialect::SqlServer, 10, 20),
              L" LIMIT 10 OFFSET 20");
    // Oracle 12c+ OFFSET…FETCH form when offset>0.
    ExpectStr("limit:oracle offset>0", LimitOffsetClause(Dialect::Oracle, 10, 20),
              L" OFFSET 20 ROWS FETCH NEXT 10 ROWS ONLY");
    // Oracle offset==0 → FETCH FIRST (no OFFSET clause).
    ExpectStr("limit:oracle offset=0", LimitOffsetClause(Dialect::Oracle, 10, 0),
              L" FETCH FIRST 10 ROWS ONLY");
    // MySQL offset=0 still emits OFFSET 0 (LIMIT arm is unconditional).
    ExpectStr("limit:mysql offset=0", LimitOffsetClause(Dialect::MySQL, 5, 0),
              L" LIMIT 5 OFFSET 0");
}

int main()
{
    std::printf("== ddl_test ==\n");

    TestUnconnectedAssumption();

    auto my = CreateConnection(DbType::MySQL);
    auto pg = CreateConnection(DbType::PostgreSQL);
    if (!my || !pg) {
        std::printf("  FATAL: CreateConnection returned null\n");
        return 1;
    }

    TestCreateTable(*my, *pg);
    TestColumnChanges(*my, *pg);
    TestIndexFkAndOrdering(*my, *pg);
    TestRenderLiteral();
    TestLimitOffset();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
