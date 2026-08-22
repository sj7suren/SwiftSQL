// dialect_profile_test.cpp — golden-DDL unit tests (a fitness function) for the
// five ADR-014 DialectProfile implementations (MySql / Pg / Sqlite / SqlServer /
// Oracle). For each dialect we take the profile from GetDialectProfile(DbType),
// build a db::TableEdit scenario, call RenderAlter(edit, stmts, err), and assert
// each produced statement byte-for-byte against the DDL *we* believe is correct for
// that dialect — not against a recording of the profile's own output. Where the two
// disagree, the divergence is called out with a "KNOWN DEVIATION" comment and, if it
// is an outright bug, pinned to the profile's *current* output so the suite stays
// green while the bug list drives the fix (see the report handed to the CTO).
//
// Pure rendering: RenderAlter never touches a connection handle or a vendor header,
// so this links only swiftsql::db (which supplies the profiles + wxString) and runs
// headless in CI, mirroring the other self-contained test binaries here. Harness is
// the same dependency-free assert loop (no doctest/Catch2 vendored). All expected
// strings are kept ASCII on purpose: MSVC without a BOM mis-decodes UTF-8 string
// literals, and the earlier binary-export test deliberately routes CJK through an
// explicit-byte helper for the same reason — so column comments here use ASCII text
// and the SQLite honest-skip note (which is Chinese in the profile) is checked
// structurally (prefix + substring) rather than by full-string equality.
//
// The suite is split across two translation units to stay under the ≤1000-line
// charter cap: this file carries the column-level tests + main(); the table-level
// tests (indexes / FKs / options / comment / triggers / RenderCreate) live in
// dialect_profile_test_ext.cpp and run via TableScopeTests(). The shared harness
// (g_checks/g_fails, Expect*, Render* helpers) lives in dialect_profile_test.h.
#include "dialect_profile_test.h"

// Table-level tests live in dialect_profile_test_ext.cpp; main() invokes them
// through this forward declaration after the column-level tests below.
void TableScopeTests();

// ===========================================================================
// MySQL — one ALTER TABLE, comma-joined clauses; backtick quoting; `db`.`table`.
// ===========================================================================
static void TestMySql()
{
    std::printf("-- MySQL --\n");
    const DbType T = DbType::MySQL;

    // 1) ADD varchar(80) NOT NULL DEFAULT 'guest' COMMENT '...'.
    {
        ColumnModel m;
        m.name = L"email"; m.type = L"varchar"; m.length = L"80";
        m.notNull = true; m.defaultVal = L"guest"; m.comment = L"user email";
        auto s = RenderOne(T, L"app", L"users", Add(m));
        ExpectCount("mysql.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.add varchar(80) NOT NULL DEFAULT COMMENT", s[0],
                L"ALTER TABLE `app`.`users` ADD COLUMN `email` varchar(80) NOT NULL "
                L"DEFAULT 'guest' COMMENT 'user email'");
    }

    // 2) ADD decimal(10,2) — precision + scale spliced correctly.
    {
        ColumnModel m;
        m.name = L"amount"; m.type = L"decimal"; m.length = L"10"; m.scale = L"2";
        m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Add(m));
        ExpectCount("mysql.add decimal: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.add decimal(10,2)", s[0],
                L"ALTER TABLE `app`.`users` ADD COLUMN `amount` decimal(10,2) NOT NULL");
    }

    // 3) MODIFY + rename → single CHANGE COLUMN `old` `new` <body>.
    {
        ColumnModel m;
        m.origName = L"old_email"; m.name = L"email";
        m.type = L"varchar"; m.length = L"120"; m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Modify(m));
        ExpectCount("mysql.rename: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.CHANGE COLUMN old->new + retype", s[0],
                L"ALTER TABLE `app`.`users` CHANGE COLUMN `old_email` `email` "
                L"varchar(120) NOT NULL");
    }

    // 4) DROP COLUMN.
    {
        auto s = RenderOne(T, L"app", L"users", Drop(L"email"));
        ExpectCount("mysql.drop: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.DROP COLUMN", s[0],
                L"ALTER TABLE `app`.`users` DROP COLUMN `email`");
    }

    // 4b) Column reorder → MODIFY … AFTER `x` / MODIFY … FIRST (positioned Modify).
    {
        ColumnModel m; m.origName = L"c"; m.name = L"c"; m.type = L"int"; m.notNull = true;
        ColumnEdit e; e.op = ColumnEdit::Modify; e.model = m;
        e.positioned = true; e.afterColumn = L"a";
        auto s = RenderOne(T, L"app", L"users", e);
        if (s.size() == 1)
            ExpectEq("mysql.MODIFY AFTER (reorder)", s[0],
                L"ALTER TABLE `app`.`users` MODIFY COLUMN `c` int NOT NULL AFTER `a`");

        ColumnEdit f; f.op = ColumnEdit::Modify; f.model = m; f.positioned = true;  // "" = FIRST
        auto s2 = RenderOne(T, L"app", L"users", f);
        if (s2.size() == 1)
            ExpectEq("mysql.MODIFY FIRST (reorder)", s2[0],
                L"ALTER TABLE `app`.`users` MODIFY COLUMN `c` int NOT NULL FIRST");
        ExpectTrue("mysql.SupportsColumnReorder", GetDialectProfile(T).SupportsColumnReorder());
        ExpectTrue("pg.!SupportsColumnReorder",
                   !GetDialectProfile(DbType::PostgreSQL).SupportsColumnReorder());
    }

    // 5) Dialect attrs: unsigned (attrs bag) + autoIncrement.
    {
        ColumnModel m;
        m.name = L"id"; m.type = L"int"; m.notNull = true; m.autoIncrement = true;
        m.attrs.push_back({ L"unsigned", L"1" });
        auto s = RenderOne(T, L"app", L"users", Add(m));
        ExpectCount("mysql.unsigned+ai: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.int unsigned NOT NULL AUTO_INCREMENT", s[0],
                L"ALTER TABLE `app`.`users` ADD COLUMN `id` int unsigned NOT NULL "
                L"AUTO_INCREMENT");
    }

    // 5b) Numeric DEFAULT stays unquoted (RenderDefault recognizes numbers).
    {
        ColumnModel m;
        m.name = L"qty"; m.type = L"int"; m.notNull = true; m.defaultVal = L"0";
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("mysql.numeric DEFAULT unquoted", s[0],
                L"ALTER TABLE `app`.`users` ADD COLUMN `qty` int NOT NULL DEFAULT 0");
    }

    // FINDING (documented, not a syntax error): an *empty-string* default cannot be
    // expressed. RenderColumnBody guards `!defaultVal.IsEmpty()`, so defaultVal=""
    // (a DEFAULT '') is indistinguishable from "no default" and NO DEFAULT clause is
    // emitted. Pinned to current behavior; see report item M1.
    {
        ColumnModel m;
        m.name = L"tag"; m.type = L"varchar"; m.length = L"10";
        m.notNull = true; m.defaultVal = L"";   // intent: DEFAULT ''
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("mysql.empty DEFAULT '' is DROPPED (finding M1)", s[0],
                L"ALTER TABLE `app`.`users` ADD COLUMN `tag` varchar(10) NOT NULL");
    }
}

// ===========================================================================
// PostgreSQL — one statement per change; double-quote quoting; defaults verbatim.
// ===========================================================================
static void TestPg()
{
    std::printf("-- PostgreSQL --\n");
    const DbType T = DbType::PostgreSQL;

    // 1) ADD varchar(80) NOT NULL DEFAULT 'guest'. PG defaults are emitted verbatim,
    //    so the model must already carry the quotes.
    {
        ColumnModel m;
        m.name = L"email"; m.type = L"varchar"; m.length = L"80";
        m.notNull = true; m.defaultVal = L"'guest'";
        auto s = RenderOne(T, L"app", L"users", Add(m));
        ExpectCount("pg.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("pg.ADD COLUMN varchar(80) NOT NULL DEFAULT", s[0],
                L"ALTER TABLE \"app\".\"users\" ADD COLUMN \"email\" varchar(80) "
                L"NOT NULL DEFAULT 'guest'");
    }

    // 2) ADD numeric(10,2).
    {
        ColumnModel m;
        m.name = L"amount"; m.type = L"numeric"; m.length = L"10"; m.scale = L"2";
        m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("pg.ADD numeric(10,2)", s[0],
                L"ALTER TABLE \"app\".\"users\" ADD COLUMN \"amount\" numeric(10,2) NOT NULL");
    }

    // 2b) Alias resolution: user types "decimal" -> canonical "numeric".
    {
        ColumnModel m;
        m.name = L"amt2"; m.type = L"decimal"; m.length = L"10"; m.scale = L"2";
        m.notNull = false;
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("pg.decimal alias -> numeric(10,2)", s[0],
                L"ALTER TABLE \"app\".\"users\" ADD COLUMN \"amt2\" numeric(10,2)");
    }

    // 3) MODIFY + rename → RENAME COLUMN, then ALTER COLUMN TYPE / SET NOT NULL
    //    (three statements). FIXED (report P2): no default is supplied, so NO
    //    DROP DEFAULT is emitted — an untouched default is left intact.
    {
        ColumnModel m;
        m.origName = L"old_email"; m.name = L"email";
        m.type = L"varchar"; m.length = L"120"; m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Modify(m));
        ExpectCount("pg.rename+retype: 3 stmts (no DROP DEFAULT)", s.size(), 3);
        if (s.size() == 3) {
            ExpectEq("pg.RENAME COLUMN", s[0],
                L"ALTER TABLE \"app\".\"users\" RENAME COLUMN \"old_email\" TO \"email\"");
            ExpectEq("pg.ALTER COLUMN TYPE", s[1],
                L"ALTER TABLE \"app\".\"users\" ALTER COLUMN \"email\" TYPE varchar(120)");
            ExpectEq("pg.ALTER COLUMN SET NOT NULL", s[2],
                L"ALTER TABLE \"app\".\"users\" ALTER COLUMN \"email\" SET NOT NULL");
        }
    }

    // 4) DROP COLUMN.
    {
        auto s = RenderOne(T, L"app", L"users", Drop(L"email"));
        if (s.size() == 1)
            ExpectEq("pg.DROP COLUMN", s[0],
                L"ALTER TABLE \"app\".\"users\" DROP COLUMN \"email\"");
    }
}

// ===========================================================================
// SQLite — ADD/RENAME only; honest "-- ..." skip for type/constraint changes.
// ===========================================================================
static void TestSqlite()
{
    std::printf("-- SQLite --\n");
    const DbType T = DbType::Sqlite;

    // 1) ADD (bare table, no db). Type emitted verbatim (dynamic typing), default
    //    verbatim.
    {
        ColumnModel m;
        m.name = L"note"; m.type = L"varchar"; m.length = L"80";
        m.notNull = true; m.defaultVal = L"'x'";
        auto s = RenderOne(T, L"", L"t", Add(m));
        if (s.size() == 1)
            ExpectEq("sqlite.ADD COLUMN varchar(80) NOT NULL DEFAULT", s[0],
                L"ALTER TABLE \"t\" ADD COLUMN \"note\" varchar(80) NOT NULL DEFAULT 'x'");
    }

    // 4) DROP COLUMN.
    {
        auto s = RenderOne(T, L"", L"t", Drop(L"note"));
        if (s.size() == 1)
            ExpectEq("sqlite.DROP COLUMN", s[0],
                L"ALTER TABLE \"t\" DROP COLUMN \"note\"");
    }

    // 6) HONEST SKIP: a type/constraint change (no rename) must NOT emit ALTER COLUMN;
    //    it emits a single "-- " note. Checked structurally (the note is Chinese in
    //    the profile).
    {
        ColumnModel m;
        m.name = L"note"; m.type = L"INTEGER"; m.notNull = true;  // no origName -> no rename
        auto s = RenderOne(T, L"", L"t", Modify(m));
        ExpectCount("sqlite.modify-type: 1 stmt (skip note only)", s.size(), 1);
        if (s.size() == 1) {
            ExpectTrue("sqlite.skip note starts with '-- '", s[0].StartsWith(L"-- "));
            ExpectTrue("sqlite.skip note has NO 'ALTER COLUMN'",
                       !s[0].Contains(L"ALTER COLUMN"));
            ExpectTrue("sqlite.skip note references the column",
                       s[0].Contains(L"\"note\""));
        }
    }

    // 6b) A pure rename emits a real RENAME COLUMN; the honest note still follows.
    {
        ColumnModel m;
        m.origName = L"old"; m.name = L"new"; m.type = L"TEXT";
        auto s = RenderOne(T, L"", L"t", Modify(m));
        ExpectCount("sqlite.rename: 2 stmts (rename + note)", s.size(), 2);
        if (s.size() == 2) {
            ExpectEq("sqlite.RENAME COLUMN", s[0],
                L"ALTER TABLE \"t\" RENAME COLUMN \"old\" TO \"new\"");
            ExpectTrue("sqlite.rename trailing note is a comment",
                       s[1].StartsWith(L"-- "));
        }
    }
}

// ===========================================================================
// SQL Server — sp_rename for renames; ALTER COLUMN for retype; ADD (no COLUMN
// keyword) via the AddColumnClause override (report S1, fixed).
// ===========================================================================
static void TestSqlServer()
{
    std::printf("-- SQL Server --\n");
    const DbType T = DbType::SqlServer;

    // 1) ADD. FIXED (report S1): correct T-SQL has no COLUMN keyword after ADD.
    //    SqlServerProfile overrides AddColumnClause to emit `ADD <body>`.
    {
        ColumnModel m;
        m.name = L"email"; m.type = L"nvarchar"; m.length = L"80";
        m.notNull = true; m.defaultVal = L"'guest'";
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("mssql.ADD (no COLUMN keyword)", s[0],
                L"ALTER TABLE \"app\".\"users\" ADD \"email\" nvarchar(80) "
                L"NOT NULL DEFAULT 'guest'");
    }

    // 3) MODIFY + rename → sp_rename (a proc call, NOT ALTER TABLE), then ALTER COLUMN.
    {
        ColumnModel m;
        m.origName = L"old_email"; m.name = L"email";
        m.type = L"nvarchar"; m.length = L"120"; m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Modify(m));
        ExpectCount("mssql.rename+retype: 2 stmts", s.size(), 2);
        if (s.size() == 2) {
            // FIXED (report S2): the old-name argument is now the idiomatic UNQUOTED
            // 'schema.table.col' literal — sp_rename parses that string itself, so the
            // ANSI-double-quoted form was fragile.
            ExpectEq("mssql.EXEC sp_rename", s[0],
                L"EXEC sp_rename 'app.users.old_email', 'email', 'COLUMN'");
            ExpectEq("mssql.ALTER COLUMN retype", s[1],
                L"ALTER TABLE \"app\".\"users\" ALTER COLUMN \"email\" nvarchar(120) NOT NULL");
        }
    }

    // 4) DROP COLUMN — this form IS valid T-SQL.
    {
        auto s = RenderOne(T, L"app", L"users", Drop(L"email"));
        if (s.size() == 1)
            ExpectEq("mssql.DROP COLUMN", s[0],
                L"ALTER TABLE \"app\".\"users\" DROP COLUMN \"email\"");
    }
}

// ===========================================================================
// Oracle — parenthesized MODIFY (...) for retype; RENAME COLUMN; DEFAULT before
// NOT NULL; ADD (no COLUMN keyword) via AddColumnClause override (report O1, fixed).
// ===========================================================================
static void TestOracle()
{
    std::printf("-- Oracle --\n");
    const DbType T = DbType::Oracle;

    // 1) ADD. FIXED (report O1): Oracle has no `ADD COLUMN`; OracleProfile overrides
    //    AddColumnClause to emit `ADD <body>`. Column-body order is DEFAULT before
    //    NOT NULL (correct for Oracle).
    {
        ColumnModel m;
        m.name = L"email"; m.type = L"VARCHAR2"; m.length = L"80";
        m.notNull = true; m.defaultVal = L"'guest'";
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("oracle.ADD (no COLUMN keyword)", s[0],
                L"ALTER TABLE \"app\".\"users\" ADD \"email\" VARCHAR2(80) "
                L"DEFAULT 'guest' NOT NULL");
    }

    // 2) ADD NUMBER(10,2).
    {
        ColumnModel m;
        m.name = L"amount"; m.type = L"NUMBER"; m.length = L"10"; m.scale = L"2";
        m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Add(m));
        if (s.size() == 1)
            ExpectEq("oracle.ADD NUMBER(10,2) (no COLUMN keyword)", s[0],
                L"ALTER TABLE \"app\".\"users\" ADD \"amount\" NUMBER(10,2) NOT NULL");
    }

    // 3) MODIFY + rename → RENAME COLUMN, then MODIFY (col type NOT NULL).
    {
        ColumnModel m;
        m.origName = L"old_email"; m.name = L"email";
        m.type = L"VARCHAR2"; m.length = L"120"; m.notNull = true;
        auto s = RenderOne(T, L"app", L"users", Modify(m));
        ExpectCount("oracle.rename+retype: 2 stmts", s.size(), 2);
        if (s.size() == 2) {
            ExpectEq("oracle.RENAME COLUMN", s[0],
                L"ALTER TABLE \"app\".\"users\" RENAME COLUMN \"old_email\" TO \"email\"");
            ExpectEq("oracle.MODIFY (col type NOT NULL)", s[1],
                L"ALTER TABLE \"app\".\"users\" MODIFY (\"email\" VARCHAR2(120) NOT NULL)");
        }
    }

    // 4) DROP COLUMN — valid Oracle.
    {
        auto s = RenderOne(T, L"app", L"users", Drop(L"email"));
        if (s.size() == 1)
            ExpectEq("oracle.DROP COLUMN", s[0],
                L"ALTER TABLE \"app\".\"users\" DROP COLUMN \"email\"");
    }
}

int main()
{
    std::printf("== SwiftSQL DialectProfile golden-DDL tests ==\n");
    TestMySql();
    TestPg();
    TestSqlite();
    TestSqlServer();
    TestOracle();
    TableScopeTests();
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
