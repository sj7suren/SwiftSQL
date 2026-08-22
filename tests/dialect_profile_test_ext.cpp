// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// dialect_profile_test_ext.cpp — table-scope half of the golden-DDL DialectProfile
// suite (see dialect_profile_test.cpp for the suite rationale). This translation
// unit carries the table-level tests — indexes, foreign keys, table options, table
// comment, triggers, and full RenderCreate — and exposes them to main() through the
// single entry point TableScopeTests(). It shares the assert harness + render
// helpers with the column-level TU via dialect_profile_test.h (C++17 inline
// definitions), so g_checks/g_fails accumulate across both files into one total.
#include "dialect_profile_test.h"

// ===========================================================================
// MySQL indexes — Add/Drop fold into ONE ALTER TABLE; kind prefixes INDEX; USING
// only for BTREE/HASH (suppressed for FULLTEXT/SPATIAL); comment escaped.
// ===========================================================================
static void TestMySqlIndexes()
{
    std::printf("-- MySQL indexes --\n");
    const DbType T = DbType::MySQL;

    // 1) Plain, single-column index.
    {
        IndexModel m; m.name = L"idx_name"; m.columns = { L"name" };
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        ExpectCount("mysql.idx.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.ADD INDEX (plain)", s[0],
                L"ALTER TABLE `app`.`users` ADD INDEX `idx_name` (`name`)");
    }

    // 2) UNIQUE index.
    {
        IndexModel m; m.name = L"uq_email"; m.columns = { L"email" }; m.type = L"UNIQUE";
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        if (s.size() == 1)
            ExpectEq("mysql.ADD UNIQUE INDEX", s[0],
                L"ALTER TABLE `app`.`users` ADD UNIQUE INDEX `uq_email` (`email`)");
    }

    // 3) BTREE method + multi-column.
    {
        IndexModel m; m.name = L"idx_ab"; m.columns = { L"a", L"b" }; m.method = L"BTREE";
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        if (s.size() == 1)
            ExpectEq("mysql.ADD INDEX (a,b) USING BTREE", s[0],
                L"ALTER TABLE `app`.`users` ADD INDEX `idx_ab` (`a`, `b`) USING BTREE");
    }

    // 4) FULLTEXT with a comment — USING is SUPPRESSED even though method is set.
    {
        IndexModel m; m.name = L"ft_body"; m.columns = { L"body" };
        m.type = L"FULLTEXT"; m.method = L"BTREE"; m.comment = L"search";
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        if (s.size() == 1)
            ExpectEq("mysql.ADD FULLTEXT INDEX (no USING) COMMENT", s[0],
                L"ALTER TABLE `app`.`users` ADD FULLTEXT INDEX `ft_body` (`body`) "
                L"COMMENT 'search'");
    }

    // 5) Comment with an apostrophe is doubled.
    {
        IndexModel m; m.name = L"idx_c"; m.columns = { L"c" }; m.comment = L"a'b";
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        if (s.size() == 1)
            ExpectEq("mysql.index COMMENT apostrophe escaped", s[0],
                L"ALTER TABLE `app`.`users` ADD INDEX `idx_c` (`c`) COMMENT 'a''b'");
    }

    // 6) DROP INDEX.
    {
        auto s = RenderIdx(T, L"app", L"users", IdxDrop(L"idx_name"));
        ExpectCount("mysql.idx.drop: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.DROP INDEX", s[0],
                L"ALTER TABLE `app`.`users` DROP INDEX `idx_name`");
    }

    // 7) Metadata drives the UI dropdowns.
    {
        auto types = GetDialectProfile(T).IndexTypes();
        ExpectCount("mysql.IndexTypes count", types.size(), 3);
        auto methods = GetDialectProfile(T).IndexMethods();
        ExpectCount("mysql.IndexMethods count", methods.size(), 2);
    }
}

// ===========================================================================
// PostgreSQL indexes — the ANSI SeparateAlterProfile default: standalone
// CREATE INDEX / DROP INDEX statements (not ALTER TABLE clauses).
// ===========================================================================
static void TestPgIndexes()
{
    std::printf("-- PostgreSQL indexes --\n");
    const DbType T = DbType::PostgreSQL;

    // 1) Plain index → CREATE INDEX ... ON t (...).
    {
        IndexModel m; m.name = L"idx_name"; m.columns = { L"name" };
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        ExpectCount("pg.idx.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("pg.CREATE INDEX", s[0],
                L"CREATE INDEX \"idx_name\" ON \"app\".\"users\" (\"name\")");
    }

    // 2) UNIQUE index.
    {
        IndexModel m; m.name = L"uq_email"; m.columns = { L"email" }; m.type = L"UNIQUE";
        auto s = RenderIdx(T, L"app", L"users", IdxAdd(m));
        if (s.size() == 1)
            ExpectEq("pg.CREATE UNIQUE INDEX", s[0],
                L"CREATE UNIQUE INDEX \"uq_email\" ON \"app\".\"users\" (\"email\")");
    }

    // 3) DROP INDEX (PG drops by name, no ON clause).
    {
        auto s = RenderIdx(T, L"app", L"users", IdxDrop(L"idx_name"));
        if (s.size() == 1)
            ExpectEq("pg.DROP INDEX", s[0], L"DROP INDEX \"idx_name\"");
    }
}

// ===========================================================================
// MySQL foreign keys — Add/Drop fold into ONE ALTER TABLE; ADD CONSTRAINT … /
// DROP FOREIGN KEY <name>; referenced-schema qualifier; ON DELETE/UPDATE.
// ===========================================================================
static void TestMySqlFks()
{
    std::printf("-- MySQL foreign keys --\n");
    const DbType T = DbType::MySQL;

    // 1) Single-column FK with both referential actions.
    {
        ForeignKeyModel m;
        m.name = L"fk_user"; m.columns = { L"user_id" };
        m.refTable = L"users"; m.refColumns = { L"id" };
        m.onDelete = L"CASCADE"; m.onUpdate = L"RESTRICT";
        auto s = RenderFk(T, L"app", L"orders", FkAdd(m));
        ExpectCount("mysql.fk.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.ADD CONSTRAINT FK ON DELETE/UPDATE", s[0],
                L"ALTER TABLE `app`.`orders` ADD CONSTRAINT `fk_user` "
                L"FOREIGN KEY (`user_id`) REFERENCES `users` (`id`) "
                L"ON DELETE CASCADE ON UPDATE RESTRICT");
    }

    // 2) Referenced schema qualifier + multi-column.
    {
        ForeignKeyModel m;
        m.name = L"fk_ab"; m.columns = { L"a", L"b" };
        m.refDb = L"ref"; m.refTable = L"t"; m.refColumns = { L"x", L"y" };
        auto s = RenderFk(T, L"app", L"orders", FkAdd(m));
        if (s.size() == 1)
            ExpectEq("mysql.FK refDb.refTable multi-col", s[0],
                L"ALTER TABLE `app`.`orders` ADD CONSTRAINT `fk_ab` "
                L"FOREIGN KEY (`a`, `b`) REFERENCES `ref`.`t` (`x`, `y`)");
    }

    // 3) DROP → MySQL's DROP FOREIGN KEY (not DROP CONSTRAINT).
    {
        auto s = RenderFk(T, L"app", L"orders", FkDrop(L"fk_user"));
        if (s.size() == 1)
            ExpectEq("mysql.DROP FOREIGN KEY", s[0],
                L"ALTER TABLE `app`.`orders` DROP FOREIGN KEY `fk_user`");
    }
}

// ===========================================================================
// PostgreSQL foreign keys — standalone ALTER TABLE ADD/DROP CONSTRAINT (the ANSI
// SeparateAlterProfile default). DROP uses DROP CONSTRAINT, not DROP FOREIGN KEY.
// ===========================================================================
static void TestPgFks()
{
    std::printf("-- PostgreSQL foreign keys --\n");
    const DbType T = DbType::PostgreSQL;

    {
        ForeignKeyModel m;
        m.name = L"fk_user"; m.columns = { L"user_id" };
        m.refTable = L"users"; m.refColumns = { L"id" }; m.onDelete = L"CASCADE";
        auto s = RenderFk(T, L"app", L"orders", FkAdd(m));
        ExpectCount("pg.fk.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("pg.ADD CONSTRAINT FK ON DELETE", s[0],
                L"ALTER TABLE \"app\".\"orders\" ADD CONSTRAINT \"fk_user\" "
                L"FOREIGN KEY (\"user_id\") REFERENCES \"users\" (\"id\") "
                L"ON DELETE CASCADE");
    }
    {
        auto s = RenderFk(T, L"app", L"orders", FkDrop(L"fk_user"));
        if (s.size() == 1)
            ExpectEq("pg.DROP CONSTRAINT", s[0],
                L"ALTER TABLE \"app\".\"orders\" DROP CONSTRAINT \"fk_user\"");
    }
}

// ===========================================================================
// SQLite foreign keys — cannot ADD/DROP a FK on an existing table; honest "-- "
// skip note (checked structurally, the note is Chinese), never invalid SQL.
// ===========================================================================
static void TestSqliteFks()
{
    std::printf("-- SQLite foreign keys --\n");
    const DbType T = DbType::Sqlite;

    ForeignKeyModel m;
    m.name = L"fk_user"; m.columns = { L"user_id" };
    m.refTable = L"users"; m.refColumns = { L"id" };
    auto s = RenderFk(T, L"", L"orders", FkAdd(m));
    ExpectCount("sqlite.fk.add: 1 stmt (skip note only)", s.size(), 1);
    if (s.size() == 1) {
        ExpectTrue("sqlite.fk skip note starts with '-- '", s[0].StartsWith(L"-- "));
        ExpectTrue("sqlite.fk skip note has NO 'ALTER TABLE'",
                   !s[0].Contains(L"ALTER TABLE"));
        ExpectTrue("sqlite.fk skip note references the constraint",
                   s[0].Contains(L"\"fk_user\""));
    }
}

// ===========================================================================
// Table options (选项 tab) — MySQL emits ALTER TABLE … ENGINE=… etc.; dialects
// without alterable table options (PG here) stay silent (base no-op).
// ===========================================================================
static void TestTableOptions()
{
    std::printf("-- table options --\n");

    // 1) MySQL: engine + charset only.
    {
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.hasOptions = true; e.options.engine = L"InnoDB"; e.options.charset = L"utf8mb4";
        auto s = RenderEdit(DbType::MySQL, e);
        ExpectCount("mysql.options: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.ALTER TABLE ENGINE + CHARSET", s[0],
                L"ALTER TABLE `app`.`users` ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    // 2) MySQL: all four fields, in fixed order.
    {
        TableEdit e; e.db = L"app"; e.table = L"users"; e.hasOptions = true;
        e.options.engine = L"InnoDB"; e.options.charset = L"utf8mb4";
        e.options.collation = L"utf8mb4_bin"; e.options.rowFormat = L"DYNAMIC";
        auto s = RenderEdit(DbType::MySQL, e);
        if (s.size() == 1)
            ExpectEq("mysql.ALTER TABLE full options", s[0],
                L"ALTER TABLE `app`.`users` ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                L"COLLATE=utf8mb4_bin ROW_FORMAT=DYNAMIC");
    }

    // 2b) MySQL: numeric storage hints (max_rows / auto_increment) + checksum flag,
    //     appended after the existing engine/charset/collation/row_format clauses.
    {
        TableEdit e; e.db = L"app"; e.table = L"users"; e.hasOptions = true;
        e.options.maxRows = L"100000"; e.options.autoIncrement = L"50";
        e.options.checksum = L"1";
        auto s = RenderEdit(DbType::MySQL, e);
        ExpectCount("mysql.options numeric+checksum: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.ALTER TABLE MAX_ROWS/AUTO_INCREMENT/CHECKSUM", s[0],
                L"ALTER TABLE `app`.`users` MAX_ROWS=100000 AUTO_INCREMENT=50 CHECKSUM=1");
    }

    // 2c) maxRows="0" means unlimited → the MAX_ROWS clause is omitted entirely.
    {
        TableEdit e; e.db = L"app"; e.table = L"users"; e.hasOptions = true;
        e.options.maxRows = L"0"; e.options.checksum = L"0";
        auto s = RenderEdit(DbType::MySQL, e);
        ExpectCount("mysql.options maxRows=0 dropped: 1 stmt", s.size(), 1);
        if (s.size() == 1) {
            ExpectTrue("mysql.maxRows=0 emits NO MAX_ROWS", !s[0].Contains(L"MAX_ROWS"));
            // CHECKSUM=0 is still meaningful and IS emitted (unlike maxRows=0).
            ExpectEq("mysql.CHECKSUM=0 kept", s[0],
                L"ALTER TABLE `app`.`users` CHECKSUM=0");
        }
    }

    // 3) A column Add + options → column ALTER first, options ALTER second (extras
    //    render after, and independently of, the main ALTER).
    {
        ColumnModel m; m.name = L"age"; m.type = L"int";
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.columns.push_back(Add(m));
        e.hasOptions = true; e.options.engine = L"InnoDB";
        auto s = RenderEdit(DbType::MySQL, e);
        ExpectCount("mysql.column+options: 2 stmts", s.size(), 2);
        if (s.size() == 2) {
            ExpectEq("mysql.col add first", s[0],
                L"ALTER TABLE `app`.`users` ADD COLUMN `age` int");
            ExpectEq("mysql.options second", s[1],
                L"ALTER TABLE `app`.`users` ENGINE=InnoDB");
        }
    }

    // 4) PG has no alterable table options here → base no-op → nothing emitted.
    {
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.hasOptions = true; e.options.engine = L"InnoDB";   // meaningless for PG
        auto s = RenderEdit(DbType::PostgreSQL, e);
        ExpectCount("pg.options ignored (no-op): 0 stmts", s.size(), 0);
    }

    // 5) Option specs drive the UI dropdowns.
    {
        ExpectCount("mysql.TableOptionSpecs count",
                    GetDialectProfile(DbType::MySQL).TableOptionSpecs().size(), 8);
        ExpectCount("pg.TableOptionSpecs count (none)",
                    GetDialectProfile(DbType::PostgreSQL).TableOptionSpecs().size(), 0);
    }
}

// ===========================================================================
// Table comment (注释 tab) — MySQL: ALTER TABLE … COMMENT='…'; PG/Oracle:
// COMMENT ON TABLE … IS '…'; SQLite: no table comment → base no-op (silent).
// ===========================================================================
static void TestTableComment()
{
    std::printf("-- table comment --\n");

    {
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.hasComment = true; e.comment = L"user table";
        auto s = RenderEdit(DbType::MySQL, e);
        ExpectCount("mysql.comment: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.ALTER TABLE COMMENT", s[0],
                L"ALTER TABLE `app`.`users` COMMENT = 'user table'");
    }

    // Apostrophe in the comment is doubled.
    {
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.hasComment = true; e.comment = L"it's ours";
        auto s = RenderEdit(DbType::MySQL, e);
        if (s.size() == 1)
            ExpectEq("mysql.comment apostrophe escaped", s[0],
                L"ALTER TABLE `app`.`users` COMMENT = 'it''s ours'");
    }

    {
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.hasComment = true; e.comment = L"user table";
        auto s = RenderEdit(DbType::PostgreSQL, e);
        if (s.size() == 1)
            ExpectEq("pg.COMMENT ON TABLE", s[0],
                L"COMMENT ON TABLE \"app\".\"users\" IS 'user table'");
    }

    {
        TableEdit e; e.db = L"app"; e.table = L"users";
        e.hasComment = true; e.comment = L"user table";
        auto s = RenderEdit(DbType::Oracle, e);
        if (s.size() == 1)
            ExpectEq("oracle.COMMENT ON TABLE", s[0],
                L"COMMENT ON TABLE \"app\".\"users\" IS 'user table'");
    }

    // SQLite has no table comment → base no-op → nothing emitted.
    {
        TableEdit e; e.table = L"users";
        e.hasComment = true; e.comment = L"user table";
        auto s = RenderEdit(DbType::Sqlite, e);
        ExpectCount("sqlite.comment ignored (no-op): 0 stmts", s.size(), 0);
    }
}

// ===========================================================================
// Triggers (触发器 tab) — MySQL/SQLite render the CREATE TRIGGER … FOR EACH ROW
// form; PG/SQL Server/Oracle report SupportsTriggers()=false and honest-skip.
// ===========================================================================
static void TestTriggers()
{
    std::printf("-- triggers --\n");

    // 1) MySQL add — CREATE TRIGGER … FOR EACH ROW <body>.
    {
        TriggerModel m; m.name = L"trg_ins"; m.timing = L"BEFORE"; m.event = L"INSERT";
        m.body = L"SET NEW.created = NOW()";
        auto s = RenderTrig(DbType::MySQL, L"app", L"users", TrigAdd(m));
        ExpectCount("mysql.trigger.add: 1 stmt", s.size(), 1);
        if (s.size() == 1)
            ExpectEq("mysql.CREATE TRIGGER FOR EACH ROW", s[0],
                L"CREATE TRIGGER `trg_ins` BEFORE INSERT ON `app`.`users` "
                L"FOR EACH ROW SET NEW.created = NOW()");
    }

    // 2) MySQL drop.
    {
        auto s = RenderTrig(DbType::MySQL, L"app", L"users", TrigDrop(L"trg_ins"));
        if (s.size() == 1)
            ExpectEq("mysql.DROP TRIGGER", s[0], L"DROP TRIGGER `trg_ins`");
    }

    // 3) SQLite add — same base form, bare (unqualified) table.
    {
        TriggerModel m; m.name = L"trg_upd"; m.timing = L"AFTER"; m.event = L"UPDATE";
        m.body = L"UPDATE audit SET n = n + 1";
        auto s = RenderTrig(DbType::Sqlite, L"", L"t", TrigAdd(m));
        if (s.size() == 1)
            ExpectEq("sqlite.CREATE TRIGGER", s[0],
                L"CREATE TRIGGER \"trg_upd\" AFTER UPDATE ON \"t\" "
                L"FOR EACH ROW UPDATE audit SET n = n + 1");
    }

    // 4) PG is trigger-unsupported here → honest "-- " skip, never CREATE TRIGGER.
    {
        TriggerModel m; m.name = L"trg_ins"; m.timing = L"BEFORE"; m.event = L"INSERT";
        m.body = L"x";
        auto s = RenderTrig(DbType::PostgreSQL, L"app", L"users", TrigAdd(m));
        ExpectCount("pg.trigger: 1 stmt (skip note)", s.size(), 1);
        if (s.size() == 1) {
            ExpectTrue("pg.trigger skip starts with '-- '", s[0].StartsWith(L"-- "));
            ExpectTrue("pg.trigger skip has NO 'CREATE TRIGGER'",
                       !s[0].Contains(L"CREATE TRIGGER"));
        }
    }

    // 5) SupportsTriggers() gates the tab per dialect.
    {
        ExpectTrue("mysql.SupportsTriggers", GetDialectProfile(DbType::MySQL).SupportsTriggers());
        ExpectTrue("sqlite.SupportsTriggers", GetDialectProfile(DbType::Sqlite).SupportsTriggers());
        ExpectTrue("pg.!SupportsTriggers", !GetDialectProfile(DbType::PostgreSQL).SupportsTriggers());
        ExpectTrue("mssql.!SupportsTriggers", !GetDialectProfile(DbType::SqlServer).SupportsTriggers());
        ExpectTrue("oracle.!SupportsTriggers", !GetDialectProfile(DbType::Oracle).SupportsTriggers());
        ExpectCount("mysql.TriggerTimings count",
                    GetDialectProfile(DbType::MySQL).TriggerTimings().size(), 2);
        ExpectCount("mysql.TriggerEvents count",
                    GetDialectProfile(DbType::MySQL).TriggerEvents().size(), 3);
    }
}

// ===========================================================================
// RenderCreate (TABLE DDL tab) — full CREATE TABLE. MySQL inlines everything
// (PK / index / FK / options / comment), triggers follow; the SeparateAlter
// dialects inline columns+PK+FK and emit index/comment as follow-on statements.
// ===========================================================================
static TableModel SampleModel(const wxString& intType, const wxString& varType)
{
    TableModel m;
    m.db = L"app"; m.table = L"users";
    { ColumnModel c; c.name = L"id"; c.type = intType; c.notNull = true;
      c.autoIncrement = true; c.attrs.push_back({ L"unsigned", L"1" }); m.columns.push_back(c); }
    { ColumnModel c; c.name = L"name"; c.type = varType; c.length = L"80";
      c.notNull = true; m.columns.push_back(c); }
    { ColumnModel c; c.name = L"org_id"; c.type = intType; m.columns.push_back(c); }
    m.primaryKey = { L"id" };
    { ForeignKeyModel fk; fk.name = L"fk_org"; fk.columns = { L"org_id" };
      fk.refTable = L"orgs"; fk.refColumns = { L"id" }; m.fks.push_back(fk); }
    return m;
}

static void TestRenderCreate()
{
    std::printf("-- RenderCreate (full CREATE TABLE) --\n");

    // 1) MySQL — everything inline + options/comment tail; trigger as follow-on.
    {
        TableModel m = SampleModel(L"int", L"varchar");
        { IndexModel idx; idx.name = L"uq_name"; idx.columns = { L"name" };
          idx.type = L"UNIQUE"; m.indexes.push_back(idx); }
        m.hasOptions = true; m.options.engine = L"InnoDB"; m.options.charset = L"utf8mb4";
        m.comment = L"users";
        { TriggerModel t; t.name = L"trg_ins"; t.timing = L"BEFORE"; t.event = L"INSERT";
          t.body = L"SET NEW.x = 1"; m.triggers.push_back(t); }
        auto s = RenderCreateOf(DbType::MySQL, m);
        ExpectCount("mysql.create: 2 stmts (create + trigger)", s.size(), 2);
        if (s.size() == 2) {
            ExpectEq("mysql.CREATE TABLE inline", s[0],
                L"CREATE TABLE `app`.`users` (\n"
                L"  `id` int unsigned NOT NULL AUTO_INCREMENT,\n"
                L"  `name` varchar(80) NOT NULL,\n"
                L"  `org_id` int,\n"
                L"  PRIMARY KEY (`id`),\n"
                L"  UNIQUE INDEX `uq_name` (`name`),\n"
                L"  CONSTRAINT `fk_org` FOREIGN KEY (`org_id`) REFERENCES `orgs` (`id`)\n"
                L") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='users'");
            ExpectEq("mysql.create trigger follow-on", s[1],
                L"CREATE TRIGGER `trg_ins` BEFORE INSERT ON `app`.`users` "
                L"FOR EACH ROW SET NEW.x = 1");
        }
    }

    // 2) PostgreSQL — columns+PK+inline FK in CREATE; index & comment as follow-ons.
    {
        TableModel m = SampleModel(L"integer", L"varchar");
        { IndexModel idx; idx.name = L"idx_name"; idx.columns = { L"name" };
          m.indexes.push_back(idx); }
        m.comment = L"users";
        auto s = RenderCreateOf(DbType::PostgreSQL, m);
        ExpectCount("pg.create: 3 stmts (create + index + comment)", s.size(), 3);
        if (s.size() == 3) {
            // PG renders autoIncrement as GENERATED … AS IDENTITY (correct PG DDL) —
            // the column-body concern, exercised end-to-end here inside CREATE TABLE.
            ExpectEq("pg.CREATE TABLE (FK inline, no options)", s[0],
                L"CREATE TABLE \"app\".\"users\" (\n"
                L"  \"id\" integer GENERATED BY DEFAULT AS IDENTITY NOT NULL,\n"
                L"  \"name\" varchar(80) NOT NULL,\n"
                L"  \"org_id\" integer,\n"
                L"  PRIMARY KEY (\"id\"),\n"
                L"  CONSTRAINT \"fk_org\" FOREIGN KEY (\"org_id\") REFERENCES \"orgs\" (\"id\")\n"
                L")");
            ExpectEq("pg.create index follow-on", s[1],
                L"CREATE INDEX \"idx_name\" ON \"app\".\"users\" (\"name\")");
            ExpectEq("pg.create comment follow-on", s[2],
                L"COMMENT ON TABLE \"app\".\"users\" IS 'users'");
        }
    }

    // 3) SQLite — comment is a no-op (dropped); index follows the CREATE.
    {
        TableModel m; m.table = L"t";
        { ColumnModel c; c.name = L"id"; c.type = L"INTEGER"; c.notNull = true;
          m.columns.push_back(c); }
        { ColumnModel c; c.name = L"name"; c.type = L"TEXT"; m.columns.push_back(c); }
        m.primaryKey = { L"id" };
        { IndexModel idx; idx.name = L"idx_name"; idx.columns = { L"name" };
          m.indexes.push_back(idx); }
        m.comment = L"ignored";   // SQLite has no table comment → dropped
        auto s = RenderCreateOf(DbType::Sqlite, m);
        ExpectCount("sqlite.create: 2 stmts (create + index, comment dropped)", s.size(), 2);
        if (s.size() == 2) {
            ExpectEq("sqlite.CREATE TABLE", s[0],
                L"CREATE TABLE \"t\" (\n"
                L"  \"id\" INTEGER NOT NULL,\n"
                L"  \"name\" TEXT,\n"
                L"  PRIMARY KEY (\"id\")\n"
                L")");
            ExpectEq("sqlite.create index follow-on", s[1],
                L"CREATE INDEX \"idx_name\" ON \"t\" (\"name\")");
        }
    }
}

// ===========================================================================
// A3 — DialectProfile::Interpret/Render cross-engine type mapping. Thin
// wiring tests only (the real MySQL<->PostgreSQL semantics are already
// exhaustively covered offline by SyncTypeMapTests, A1) — this just proves
// MySqlProfile/PgProfile actually delegate to SyncTypeMap rather than falling
// through to the base-class honest-gate default, and that the base default
// itself behaves as documented for a dialect that hasn't opted in.
// ===========================================================================
static void TestTypeMapping()
{
    std::printf("-- A3: DialectProfile::Interpret/Render --\n");

    // MySQL Interpret: unsigned parsed out of rawType (delegates to SyncTypeMap).
    {
        NormColumn c; c.kind = ColKind::Integer; c.rawType = L"int(10) unsigned";
        CanonicalType t = GetDialectProfile(DbType::MySQL).Interpret(c);
        ExpectTrue("mysql.Interpret: unsigned parsed", t.isUnsigned);
    }
    // MySQL Render: varchar(n).
    {
        CanonicalType t; t.kind = ColKind::Varchar; t.length = 80;
        wxString out, why;
        ExpectTrue("mysql.Render: ok", GetDialectProfile(DbType::MySQL).Render(t, out, why));
        ExpectEq("mysql.Render: varchar(80)", out, L"varchar(80)");
    }
    // Postgres Interpret: withTimeZone parsed out of rawType.
    {
        NormColumn c; c.kind = ColKind::Timestamp; c.rawType = L"timestamp with time zone";
        CanonicalType t = GetDialectProfile(DbType::PostgreSQL).Interpret(c);
        ExpectTrue("pg.Interpret: withTimeZone parsed", t.withTimeZone);
    }
    // Postgres Render: character varying(n).
    {
        CanonicalType t; t.kind = ColKind::Varchar; t.length = 80;
        wxString out, why;
        ExpectTrue("pg.Render: ok", GetDialectProfile(DbType::PostgreSQL).Render(t, out, why));
        ExpectEq("pg.Render: character varying(80)", out, L"character varying(80)");
    }
    // Round-trip through the abstraction: MySQL bigint -> canonical -> PG bigint.
    {
        NormColumn c; c.kind = ColKind::Integer; c.rawType = L"bigint";
        CanonicalType t = GetDialectProfile(DbType::MySQL).Interpret(c);
        wxString out, why;
        ExpectTrue("mysql->pg round-trip: Render ok",
                   GetDialectProfile(DbType::PostgreSQL).Render(t, out, why));
        ExpectEq("mysql->pg round-trip: bigint", out, L"bigint");
    }
    // A dialect that hasn't opted in (SQLite) — base-default honest gate:
    // Interpret is a plain passthrough, Render always refuses.
    {
        NormColumn c; c.kind = ColKind::Varchar; c.rawType = L"TEXT"; c.length = 40;
        CanonicalType t = GetDialectProfile(DbType::Sqlite).Interpret(c);
        ExpectTrue("sqlite.Interpret: base-default passthrough kind",
                   t.kind == ColKind::Varchar && t.length == 40);
        wxString out, why;
        ExpectTrue("sqlite.Render: base default refuses (honest gate)",
                   !GetDialectProfile(DbType::Sqlite).Render(t, out, why));
        ExpectTrue("sqlite.Render: why explains", !why.IsEmpty());
    }
}

// Aggregate entry point invoked by main() in dialect_profile_test.cpp — runs every
// table-scope test in the original suite order.
void TableScopeTests()
{
    TestMySqlIndexes();
    TestPgIndexes();
    TestMySqlFks();
    TestPgFks();
    TestSqliteFks();
    TestTableOptions();
    TestTableComment();
    TestTriggers();
    TestRenderCreate();
    TestTypeMapping();
}
