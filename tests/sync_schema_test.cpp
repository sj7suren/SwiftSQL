// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sync_schema_test.cpp — offline unit tests for the STRUCTURAL half of the
// cross-database synchronization suite: db::sync::DiffSchema, a pure function
// over TableSchema value objects. Nothing here builds a plan, streams a row or
// touches a connection — if a test needs a StubConn it belongs in one of the
// sibling suites (sync_data_test / sync_exec_test / sync_plan_test).
//
// Covers: whole-table create/drop, column Add/Drop/Modify (and the positional
// hint), index and FK signatures, table options degrading to a warning, the
// cross-dialect verdict gate (SyncTypeMap/DialectProfile via CompareCanonical +
// MayAutoAlter), and the target-current-definition snapshot a ColumnChange
// carries.
//
// Shared harness (assert loop, Col/Count* helpers, stub connections) lives in
// sync_stub.h. See that file for why this suite is four TUs.
#include "sync_stub.h"

using namespace db;
using namespace db::sync;
using namespace synctest;

// ===========================================================================
//  A1. DiffSchema — pure structural diff.
// ===========================================================================
static void TestDiffSchema()
{
    std::printf("[DiffSchema — structural]\n");

    // ---- whole-table create / drop / empty ----
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        TableSchema tgt; tgt.name = L"t";   // no columns → target missing table
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectTrue("create: createTable set", r.changes.createTable);
        ExpectTrue("create: carries source schema",
                   r.changes.createSchema.columns.size() == 1);
        ExpectTrue("create: not a drop", !r.changes.dropTable);
    }
    {
        TableSchema src; src.name = L"t";   // source missing
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectTrue("drop: dropTable set (opt-in)", r.changes.dropTable);
        ExpectTrue("drop: not a create", !r.changes.createTable);
    }
    {
        TableSchema src, tgt;               // both empty
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectTrue("both empty: change set Empty()", r.changes.Empty());
    }

    // ---- column Add / Drop with positional hint ----
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"id",   ColKind::Integer, L"int"));
        src.columns.push_back(Col(L"name", ColKind::Varchar, L"varchar(50)", 50));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"id",   ColKind::Integer, L"int"));
        tgt.columns.push_back(Col(L"gone", ColKind::Text,    L"text"));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("addcol: one Add", CountCol(r.changes, ColumnChange::Op::Add), 1);
        ExpectEq("addcol: one Drop", CountCol(r.changes, ColumnChange::Op::Drop), 1);
        // Add of `name` keeps its order (after `id`).
        //
        // Count what was inspected, then assert the property. A bare
        // `for`+`if` over the change set executes ZERO assertions if the diff
        // ever returns no Add at all — and a suite that asserted nothing still
        // reports green. The trailing count turns "we looked at nothing" into a
        // failure.
        int addsInspected = 0;
        for (const auto& c : r.changes.columns)
            if (c.op == ColumnChange::Op::Add) {
                ++addsInspected;
                ExpectStr("addcol: afterColumn hint", c.afterColumn, L"id");
            }
        ExpectEq("addcol: the afterColumn hint was actually checked",
                 addsInspected, 1);
    }

    // ---- Modify triggers (same dialect: exact rawType) ----
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"a", ColKind::Varchar, L"varchar(80)", 80));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"a", ColKind::Varchar, L"varchar(40)", 40));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("modify: rawType differ", CountCol(r.changes, ColumnChange::Op::Modify), 1);
    }
    {
        TableSchema src; src.name = L"t";
        NormColumn s = Col(L"a", ColKind::Integer, L"int"); s.notNull = true;
        src.columns.push_back(s);
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"a", ColKind::Integer, L"int"));   // notNull=false
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("modify: notNull differ", CountCol(r.changes, ColumnChange::Op::Modify), 1);
    }
    {
        TableSchema src; src.name = L"t";
        NormColumn s = Col(L"a", ColKind::Integer, L"int");
        s.hasDefault = true; s.defaultExpr = L"1";
        src.columns.push_back(s);
        TableSchema tgt; tgt.name = L"t";
        NormColumn t = Col(L"a", ColKind::Integer, L"int");
        t.hasDefault = true; t.defaultExpr = L"2";
        tgt.columns.push_back(t);
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("modify: defaultExpr differ", CountCol(r.changes, ColumnChange::Op::Modify), 1);
    }
    {
        TableSchema src; src.name = L"t";
        NormColumn s = Col(L"a", ColKind::Integer, L"int"); s.autoIncrement = true;
        src.columns.push_back(s);
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"a", ColKind::Integer, L"int"));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("modify: autoIncrement differ", CountCol(r.changes, ColumnChange::Op::Modify), 1);
    }
    {
        // Identical → no change.
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"a", ColKind::Integer, L"int"));
        TableSchema tgt = src;
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("modify: identical → none", CountCol(r.changes, ColumnChange::Op::Modify), 0);
        ExpectTrue("modify: identical Empty()", r.changes.Empty());
    }

    // ---- index Add / Drop by signature ----
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        NormIndex si; si.name = L"ix_a"; si.columns = { L"id" }; si.unique = true;
        src.indexes.push_back(si);
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        NormIndex ti; ti.name = L"ix_old"; ti.columns = { L"id" }; ti.unique = false;
        tgt.indexes.push_back(ti);   // different unique flag → different signature
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("index: one Add", CountIdx(r.changes, IndexChange::Op::Add), 1);
        ExpectEq("index: one Drop", CountIdx(r.changes, IndexChange::Op::Drop), 1);
    }

    // ---- FK Add / Drop by signature ----
    {
        TableSchema src; src.name = L"orders";
        src.columns.push_back(Col(L"cid", ColKind::Integer, L"int"));
        NormForeignKey sf; sf.name = L"fk1"; sf.columns = { L"cid" };
        sf.refTable = L"customers"; sf.refColumns = { L"id" };
        src.foreignKeys.push_back(sf);
        TableSchema tgt; tgt.name = L"orders";
        tgt.columns.push_back(Col(L"cid", ColKind::Integer, L"int"));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("fk: one Add", CountFk(r.changes, FkChange::Op::Add), 1);
        ExpectEq("fk: zero Drop", CountFk(r.changes, FkChange::Op::Drop), 0);
    }

    // ---- table options → warning only, no structural change ----
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        src.engine = L"InnoDB"; src.charset = L"utf8mb4";
        TableSchema tgt = src;
        tgt.engine = L"MyISAM"; tgt.charset = L"latin1";
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::MySQL);
        ExpectTrue("options: engine warning", HasWarning(r.warnings, L"engine"));
        ExpectTrue("options: charset warning", HasWarning(r.warnings, L"charset"));
        ExpectTrue("options: no structural change", r.changes.columns.empty());
    }
}

// ===========================================================================
//  A1b/A4. DiffSchema — cross-dialect, real verdict-based comparison
//  (SyncTypeMap/DialectProfile, A1/A3) instead of the old "any kind mismatch
//  across engines that isn't ColKind::Other is a Modify" heuristic. That old
//  heuristic is what let a MySQL int diff against a PG varchar and come back
//  "Modify" (case 3 below) — genuinely unsafe, and exactly what the new
//  verdict gate (MayAutoAlter) is designed to catch instead.
// ===========================================================================
static void TestDiffSchemaCrossDialect()
{
    std::printf("[DiffSchema — cross-dialect]\n");

    // JSON <-> jsonb: previously blocked (both sides were ColKind::Other pre-
    // A2), now correctly recognized Equivalent by CompareCanonical — no Modify
    // needed (nothing actually differs), and no warning either.
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"j", ColKind::Json, L"json"));     // MySQL json
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"j", ColKind::Json, L"jsonb"));    // PG jsonb
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect: json<->jsonb → no Modify (already equal)",
                 CountCol(r.changes, ColumnChange::Op::Modify), 0);
        ExpectTrue("xdialect: json<->jsonb → no warning either", r.warnings.empty());
    }
    // MySQL unsigned integer <-> PG integer: still correctly blocked — but now
    // for a documented reason (PG has no unsigned type), not a blanket
    // "Other-kind, can't compare" skip.
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"n", ColKind::Integer, L"int unsigned"));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"n", ColKind::Integer, L"integer"));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect: unsigned int <-> integer → no Modify (Unmappable)",
                 CountCol(r.changes, ColumnChange::Op::Modify), 0);
        ExpectTrue("xdialect: unsigned int <-> integer → warning",
                   HasWarning(r.warnings, L"无符号"));
    }
    // Same kind + length across engines → seen as equal (no Modify).
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"s", ColKind::Varchar, L"varchar(80)", 80));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"s", ColKind::Varchar, L"character varying(80)", 80));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect: same kind+len → equal",
                 CountCol(r.changes, ColumnChange::Op::Modify), 0);
    }
    // Different kind (both known) across engines: NOT a Modify anymore — no
    // documented safe cast rule exists from Integer to Varchar, so
    // CompareCanonical returns Unmappable and MayAutoAlter refuses it. This is
    // the deliberate behavior change vs. the pre-A4 heuristic (which allowed
    // it purely because kind != Other on either side).
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"n", ColKind::Integer, L"int"));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"n", ColKind::Varchar, L"character varying(10)", 10));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect: kind differ → NOT Modify (Unmappable, not a safe cast)",
                 CountCol(r.changes, ColumnChange::Op::Modify), 0);
        ExpectTrue("xdialect: kind differ → warning", !r.warnings.empty());
    }
    // Different length across engines (same known kind, Equivalent verdict) →
    // Modify, rendered in the TARGET dialect's own type text (not the
    // source's foreign rawType — A2's PgTypeText ternary fix depends on this).
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"s", ColKind::Varchar, L"varchar(80)", 80));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"s", ColKind::Varchar, L"character varying(40)", 40));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect: length differ → Modify",
                 CountCol(r.changes, ColumnChange::Op::Modify), 1);
        for (const auto& cc : r.changes.columns)
            if (cc.op == ColumnChange::Op::Modify)
                ExpectStr("xdialect: Modify rawType is PG-native (not MySQL's)",
                          cc.column.rawType, L"character varying(80)");
    }

    // ---- THE P0 REGRESSION: int PK -> auto-created bigint is a NO-OP re-diff --
    // The exact shape the user hit. Auto-create translated MySQL `int` -> PG
    // `bigint` (correct, ruling), but the re-diff used to compare the source-
    // canonical width (MySQL int numeric_precision = 10) against the target-
    // canonical width (PG bigint numeric_precision = 64) and call 10 != 64 a
    // Modify — a PHANTOM, since bigint is exactly what the create produced. Worse,
    // PG's Modify renderer then emitted `ALTER COLUMN "id" DROP DEFAULT` on the
    // identity column, which PG rejects ("column ... is an identity column").
    // With the round-trip unification the source rendered into PG ("bigint")
    // equals the target's own bigint, so re-diff produces ZERO changes.
    {
        TableSchema src; src.name = L"t";
        // MySQL side, as introspected: int carries numeric_precision 10, auto_increment.
        NormColumn si = Col(L"id", ColKind::Integer, L"int", 10);
        si.notNull = true; si.autoIncrement = true;
        src.columns.push_back(si);
        src.primaryKey = { L"id" };
        TableSchema tgt; tgt.name = L"t";
        // PG side, exactly as the auto-create left it and introspection reads back:
        // bigint (numeric_precision 64), GENERATED AS IDENTITY (autoIncrement).
        NormColumn ti = Col(L"id", ColKind::Integer, L"bigint", 64);
        ti.notNull = true; ti.autoIncrement = true;
        tgt.columns.push_back(ti);
        tgt.primaryKey = { L"id" };
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect(P0): int PK -> auto-created bigint identity → NO phantom Modify",
                 CountCol(r.changes, ColumnChange::Op::Modify), 0);
        ExpectEq("xdialect(P0): no spurious column change of any kind",
                 (long long)r.changes.columns.size(), 0);
    }
    // Negative control: a GENUINE type change on the SAME identity shape must
    // still flag. Widening the source column to varchar (a real, different type)
    // is not the "target already holds what we'd create" case, so the diff must
    // NOT go blind — idempotency must not become blindness.
    {
        TableSchema src; src.name = L"t";
        src.columns.push_back(Col(L"code", ColKind::Varchar, L"varchar(50)", 50));
        TableSchema tgt; tgt.name = L"t";
        tgt.columns.push_back(Col(L"code", ColKind::Varchar, L"character varying(80)", 80));
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectEq("xdialect(control): a real varchar length change still flags a Modify",
                 CountCol(r.changes, ColumnChange::Op::Modify), 1);
    }

    // ---- cross-engine whole-table CREATE (A5) --------------------------------
    // A table present on MySQL, entirely absent on PG: DiffSchema now translates
    // every column's type through Interpret(MySQL)->Render(PG) and sets
    // createTable with a TARGET-NATIVE createSchema (no foreign rawType leaks).
    {
        TableSchema src; src.name = L"neu";
        src.columns.push_back(Col(L"id",   ColKind::Integer, L"int"));
        src.columns.push_back(Col(L"name", ColKind::Varchar, L"varchar(80)", 80));
        src.columns.push_back(Col(L"flag", ColKind::Boolean, L"tinyint(1)"));
        src.primaryKey = { L"id" };                          // ruling 5: PK carried
        TableSchema tgt; tgt.name = L"neu";                  // no columns → missing on PG
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectTrue("xcreate: createTable set for a fully mappable table",
                   r.changes.createTable);
        ExpectEq("xcreate: all columns carried",
                 (long long)r.changes.createSchema.columns.size(), 3);
        ExpectTrue("xcreate: PK carried",
                   r.changes.createSchema.primaryKey.size() == 1 &&
                   r.changes.createSchema.primaryKey[0] == L"id");
        // The load-bearing property: rawType is now PG-native, so the PG driver's
        // RenderCreateTable emits correct PG DDL with no new render path.
        const NormColumn* nc = r.changes.createSchema.FindColumn(L"name");
        ExpectTrue("xcreate: name column present", nc != nullptr);
        if (nc) ExpectStr("xcreate: rawType translated to PG-native",
                          nc->rawType, L"character varying(80)");
        const NormColumn* fc = r.changes.createSchema.FindColumn(L"flag");
        if (fc) ExpectStr("xcreate: tinyint(1) → PG boolean",
                          fc->rawType, L"boolean");
    }

    // ---- cross-engine CREATE — carries FKs out, warns (ruling 2) --------------
    {
        TableSchema src; src.name = L"orders";
        src.columns.push_back(Col(L"cid", ColKind::Integer, L"int"));
        src.primaryKey = { L"cid" };
        NormForeignKey sf; sf.name = L"fk1"; sf.columns = { L"cid" };
        sf.refTable = L"customers"; sf.refColumns = { L"id" };
        src.foreignKeys.push_back(sf);
        TableSchema tgt; tgt.name = L"orders";               // missing on PG
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectTrue("xcreate(fk): createTable set", r.changes.createTable);
        ExpectTrue("xcreate(fk): FKs omitted from the CREATE",
                   r.changes.createSchema.foreignKeys.empty());
        ExpectTrue("xcreate(fk): a warning names the FK omission",
                   HasWarning(r.warnings, L"外键"));
    }

    // ---- cross-engine CREATE — Unmappable column refuses the WHOLE table
    // (ruling 1). MySQL `int unsigned` has no safe PG type. ---------------------
    {
        TableSchema src; src.name = L"badnew";
        src.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
        src.columns.push_back(Col(L"n",  ColKind::Integer, L"int unsigned"));
        TableSchema tgt; tgt.name = L"badnew";               // missing on PG
        SchemaDiffResult r = DiffSchema(src, tgt, Dialect::MySQL, Dialect::Postgres);
        ExpectTrue("xcreate(refuse): NOT created (whole table refused)",
                   !r.changes.createTable);
        ExpectTrue("xcreate(refuse): no createSchema columns leaked",
                   r.changes.createSchema.columns.empty());
        ExpectTrue("xcreate(refuse): Finding names the offending column",
                   HasWarning(r.warnings, L"无法自动建表"));
    }
}

// ===========================================================================
//  A6. T11 — a ColumnChange snapshots the TARGET's current definition.
//
//  This block used to sit in the plan-verdict tests, which is where it was
//  written but not what it is about: it calls DiffSchema directly and never
//  builds a plan. It belongs here with the rest of the structural diff.
// ===========================================================================
static void TestColumnCurrentSnapshot()
{
    std::printf("[DiffSchema — ColumnChange carries the target's current column]\n");

    {
        TableSchema s; s.name = L"t";
        s.columns.push_back(Col(L"c", ColKind::Varchar, L"varchar(80)", 80));
        TableSchema t; t.name = L"t";
        t.columns.push_back(Col(L"c", ColKind::Varchar, L"varchar(20)", 20));
        SchemaDiffResult r = DiffSchema(s, t, Dialect::MySQL, Dialect::MySQL);
        ExpectEq("cur: one Modify", (long long)r.changes.columns.size(), 1);
        if (r.changes.columns.size() == 1) {
            const ColumnChange& cc = r.changes.columns[0];
            ExpectTrue("cur: Modify carries the target's current column", cc.hasCurrent);
            ExpectStr("cur: desired definition", cc.column.rawType, L"varchar(80)");
            ExpectStr("cur: current definition", cc.current.rawType, L"varchar(20)");
        }
    }
}

int main()
{
    std::printf("== sync_schema_test ==\n");
    TestDiffSchema();
    TestDiffSchemaCrossDialect();
    TestColumnCurrentSnapshot();
    return Report("sync_schema_test");
}
