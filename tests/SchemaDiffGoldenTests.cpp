// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SchemaDiffGoldenTests.cpp — WP0 characterization tests for the cross-engine
// sync feature, now carried through A1-A4:
//
//   1. PG->PG rendered CREATE TABLE DDL, byte-for-byte, for the column types
//      the A2 type-mapping fix touched (PgSql.cpp's PgTypeText/PgColumnDef,
//      the rawType-vs-kind fallback ternary at what was PgSql.cpp:63-64).
//      Fixtures are hand-built NormColumn literals with kind=ColKind::Other —
//      this is a HISTORICAL/FROZEN fixture choice (it mirrored what
//      PgConnection::GetTableSchema produced before A2 added a real
//      introspection override; PgSchemaRead.cpp now assigns real ColKind).
//      The fixture and its expected output are pinned exactly as they were —
//      MUST stay byte-identical (see the task's PG->PG golden rule) — because
//      the underlying claim being tested ("a present rawType is used
//      verbatim, regardless of kind") is unaffected by what kind PG
//      introspection assigns today. Exercised through an *unconnected*
//      PostgreSQL IConnection — same "unconnected RenderSchemaChange is pure
//      rendering" trick ddl_test.cpp already relies on — fully offline.
//
//   2. Cross-engine column comparison in db::sync::DiffSchema — UPDATED for
//      A4, per the task brief's explicit instruction (this is the one golden
//      expected to move). Before A2/A4, DiffSchema's `kind == ColKind::Other`
//      heuristic skip-and-warned EVERY cross-engine column unconditionally,
//      with a blanket "类型跨引擎不可比" excuse. Now that PG columns carry
//      real ColKind (A2) and DiffSchema does real per-column verdict-based
//      comparison (SyncTypeMap::CompareCanonical via DialectProfile::
//      Interpret, A1/A3), most columns compare cleanly with NO warning at
//      all; only the columns that genuinely need one (a real Lossy pairing,
//      or an actual cross-engine MODIFY) produce a Finding — see
//      TestCrossEngineSkipAndWarnGolden's own comment for the full trace.
//
// Same dependency-free harness as ddl_test.cpp / sync_test.cpp: an
// ExpectTrue/ExpectStr/ExpectEq assert loop, non-zero exit on any failure,
// no doctest/Catch2. Nothing here touches a live database — pg_live_test's
// env-gated skip-on-missing-creds behavior is exactly what this file exists
// to not depend on.
#include "db/DbDriver.h"
#include "db/SchemaModel.h"
#include "db/SchemaDiff.h"
#include "core/DbTypes.h"

#include <cstdio>
#include <vector>
#include <wx/string.h>

using namespace db;
using namespace db::sync;

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

// ---- small builders for schema value objects -------------------------------
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

static int CountCol(const SchemaChangeSet& ch, ColumnChange::Op op)
{
    int n = 0;
    for (const auto& c : ch.columns) if (c.op == op) ++n;
    return n;
}

// ===========================================================================
//  1. PG->PG CREATE TABLE — byte-for-byte golden, offline fixtures.
//
//  All five columns carry kind=ColKind::Other + a raw PG type string, which
//  is exactly what PgConnection::GetTableSchema produces today (no PG-specific
//  override -> DbDriver.h base-class default -> kind always Other). This
//  means PgTypeText's `(c.kind == ColKind::Other || c.rawType.IsEmpty()) ?
//  c.rawType : PgTypeFromKind(c.kind)` branch (PgSql.cpp:63-64) always takes
//  the *verbatim rawType* arm today for real PG-sourced columns. That is the
//  exact branch the upcoming type-mapping fix will touch, so this fixture is
//  the trip-wire: if the fix starts assigning real kinds to PG-introspected
//  columns without keeping PgTypeFromKind precise (e.g. it only knows generic
//  ColKind::Integer, not int2/int8 width — see the "honest gate" comment
//  above PgTypeFromKind), bigint/smallint round-tripping silently drifts to
//  "integer" and this test fails loudly instead.
// ===========================================================================
static TableSchema MeasurementsSchemaPg()
{
    TableSchema s;
    s.name = L"measurements";

    NormColumn id = Col(L"id", ColKind::Other, L"bigint");
    id.notNull = true;

    NormColumn sensor = Col(L"sensor_id", ColKind::Other, L"smallint");
    sensor.notNull = true;

    NormColumn readingAt = Col(L"reading_at", ColKind::Other, L"timestamptz");
    readingAt.notNull = true;
    readingAt.hasDefault = true; readingAt.defaultExpr = L"now()";

    NormColumn value = Col(L"value", ColKind::Other, L"numeric(10,2)");

    NormColumn label = Col(L"label", ColKind::Other, L"varchar(80)");
    label.hasDefault = true; label.defaultExpr = L"'unknown'";

    s.columns = { id, sensor, readingAt, value, label };
    s.primaryKey = { L"id" };
    return s;
}

static void TestPgToPgCreateTableGolden(IConnection& pg)
{
    std::printf("[PG->PG CREATE TABLE — golden DDL, offline fixtures]\n");

    SchemaChangeSet ch;
    ch.table = L"measurements";
    ch.createTable = true;
    ch.createSchema = MeasurementsSchemaPg();

    std::vector<wxString> ps; wxString pe;
    ExpectTrue("pg: render ok", pg.RenderSchemaChange(ch, ps, pe));
    ExpectTrue("pg: single statement (no secondary indexes/FKs)", ps.size() == 1);

    const wxString got = ps.empty() ? wxString() : ps[0];
    const wxString want =
        L"CREATE TABLE \"measurements\" (\n"
        L"  \"id\" bigint NOT NULL,\n"
        L"  \"sensor_id\" smallint NOT NULL,\n"
        L"  \"reading_at\" timestamptz NOT NULL DEFAULT now(),\n"
        L"  \"value\" numeric(10,2),\n"
        L"  \"label\" varchar(80) DEFAULT 'unknown',\n"
        L"  PRIMARY KEY (\"id\")\n"
        L")";
    ExpectStr("pg: CREATE TABLE byte-for-byte", got, want);
}

// ---------------------------------------------------------------------------
//  1b. Same five types through the ALTER COLUMN ... TYPE path (Modify), which
//  calls PgTypeText directly (not just PgColumnDef via CREATE TABLE) — the
//  other call site of the ColKind::Other fallback.
// ---------------------------------------------------------------------------
static void TestPgAlterColumnTypeGolden(IConnection& pg)
{
    std::printf("[PG ALTER COLUMN TYPE — golden per-type, offline fixtures]\n");

    struct Case { const wchar_t* col; ColKind kind; const wchar_t* raw; const wchar_t* wantType; };
    const Case cases[] = {
        { L"id",         ColKind::Other, L"bigint",         L"bigint" },
        { L"sensor_id",  ColKind::Other, L"smallint",       L"smallint" },
        { L"reading_at", ColKind::Other, L"timestamptz",    L"timestamptz" },
        { L"value",      ColKind::Other, L"numeric(10,2)",  L"numeric(10,2)" },
        { L"label",      ColKind::Other, L"varchar(80)",    L"varchar(80)" },
    };

    for (const auto& tc : cases) {
        SchemaChangeSet ch; ch.table = L"measurements";
        NormColumn mod = Col(tc.col, tc.kind, tc.raw);
        mod.notNull = true;   // forces a SET NOT NULL clause too, harmless here
        ColumnChange cc; cc.op = ColumnChange::Op::Modify; cc.column = mod;
        ch.columns.push_back(cc);

        std::vector<wxString> ps; wxString pe;
        ExpectTrue("pg-alter: render ok", pg.RenderSchemaChange(ch, ps, pe));
        ExpectTrue("pg-alter: 3 sub-clauses (TYPE/NOT NULL/DEFAULT)", ps.size() == 3);
        const wxString typeStmt = ps.empty() ? wxString() : ps[0];
        const wxString want = L"ALTER TABLE \"measurements\" ALTER COLUMN \"" +
                               wxString(tc.col) + L"\" TYPE " + wxString(tc.wantType);
        ExpectStr((const char*)(wxString(L"pg-alter: TYPE clause for ") + tc.col).utf8_str(),
                  typeStmt, want);
    }
}

// ===========================================================================
//  2. Cross-engine comparison — UPDATED for A4 (this is the one WP0-designated
//  test the task brief explicitly expected to change; see file banner). Before
//  A2, every PG column carried kind=Other unconditionally, so DiffSchema's old
//  `sc.kind == ColKind::Other || tc->kind == ColKind::Other` heuristic
//  skip-and-warned EVERY column, unconditionally, with the free-text
//  `"列 " + table + "." + col + " 类型跨引擎不可比，已跳过"`. Now that PG
//  columns carry real ColKind (A2) and DiffSchema does real per-column
//  verdict-based comparison (SyncTypeMap::CompareCanonical via each side's own
//  DialectProfile::Interpret, A1/A3/A4), most of these five columns compare
//  cleanly and produce NO warning at all — only the two columns that
//  genuinely need one (a real semantic mismatch, and a real cross-engine
//  MODIFY) get a Finding, each carrying a real reason instead of a blanket
//  "can't compare" excuse. warnings is now `std::vector<Finding>` (A4), not
//  free text — see SchemaDiff.h.
// ===========================================================================
static void TestCrossEngineSkipAndWarnGolden()
{
    std::printf("[MySQL vs PG DiffSchema — real verdict-based comparison]\n");

    // MySQL side: real kinds, as MySqlSql.cpp's MapColKind would classify them.
    TableSchema mysqlSide; mysqlSide.name = L"measurements";
    mysqlSide.columns = {
        Col(L"id",         ColKind::Integer,   L"bigint"),
        Col(L"sensor_id",  ColKind::Integer,   L"smallint"),
        Col(L"reading_at", ColKind::Timestamp, L"timestamp"),           // MySQL TIMESTAMP (tz-normalizing variant)
        Col(L"value",      ColKind::Decimal,   L"decimal(10,2)", 10, 2),
        Col(L"label",      ColKind::Varchar,   L"varchar(80)", 80),
    };

    // PG side: real kinds (A2's MapPgColKind), rawType = PG's actual
    // information_schema.columns.data_type spelling (never carries an inline
    // length like MySQL's column_type does — length/scale are separate
    // fields, as PgSchemaRead.cpp actually populates them).
    TableSchema pgSide; pgSide.name = L"measurements";
    pgSide.columns = {
        Col(L"id",         ColKind::Integer,   L"bigint"),
        Col(L"sensor_id",  ColKind::Integer,   L"smallint"),
        Col(L"reading_at", ColKind::Timestamp, L"timestamp with time zone"),  // PG timestamptz
        Col(L"value",      ColKind::Decimal,   L"numeric", 10, 2),
        Col(L"label",      ColKind::Varchar,   L"character varying", 40),    // deliberately shorter than MySQL's 80
    };

    SchemaDiffResult r = DiffSchema(mysqlSide, pgSide, Dialect::MySQL, Dialect::Postgres);

    // id/sensor_id: rawType literally identical ("bigint"/"smallint" spelled
    // the same in both engines) -> Identical, and nothing else differs -> no
    // Modify, no Finding at all (silently fine — the whole point of A4).
    // value: MySQL decimal(10,2) <-> PG numeric(10,2) -> Equivalent (same
    // kind/length/scale) -> no Finding either.
    // reading_at: MySQL TIMESTAMP <-> PG timestamptz -> Lossy (the task
    // brief's documented judgment call: both are "tz-aware" but range/DST
    // differences make this unsafe to auto-ALTER) -> skip + Finding.
    // label: MySQL varchar(80) <-> PG character varying(40) -> Equivalent
    // verdict, but the LENGTH genuinely differs -> a real cross-engine Modify,
    // rendered in PG's own type text (not MySQL's rawType) -> Finding (audit
    // trail, not a skip).
    ExpectEq("xengine: exactly 1 Modify (label — length genuinely differs)",
             CountCol(r.changes, ColumnChange::Op::Modify), 1);
    ExpectTrue("xengine: no Add/Drop (columns present on both sides)",
               CountCol(r.changes, ColumnChange::Op::Add) == 0 &&
               CountCol(r.changes, ColumnChange::Op::Drop) == 0);
    ExpectTrue("xengine: change set not Empty (the one Modify)", !r.changes.Empty());

    for (const auto& cc : r.changes.columns)
        if (cc.op == ColumnChange::Op::Modify) {
            ExpectStr("xengine: Modify column is label", cc.column.name, L"label");
            ExpectStr("xengine: Modify rawType is PG-native, mapped from MySQL's length",
                      cc.column.rawType, L"character varying(80)");
        }

    ExpectEq("xengine: exactly 2 findings (reading_at skip + label audit trail)",
             (long long)r.warnings.size(), 2);

    if (r.warnings.size() == 2) {
        const Finding& f0 = r.warnings[0];
        ExpectStr("xengine: finding[0] table", f0.table, L"measurements");
        ExpectStr("xengine: finding[0] column", f0.column, L"reading_at");
        ExpectTrue("xengine: finding[0] verdict Lossy", f0.verdict == TypeVerdict::Lossy);
        ExpectStr("xengine: finding[0] reason exact text", f0.reason,
                  L"两侧均为“带时区语义”的时间戳（MySQL TIMESTAMP / PG timestamptz），"
                  L"但取值范围（MySQL 限 1970–2038）与 DST 边界转换算法不完全一致，按 Lossy 处理，需人工核实");

        const Finding& f1 = r.warnings[1];
        ExpectStr("xengine: finding[1] table", f1.table, L"measurements");
        ExpectStr("xengine: finding[1] column", f1.column, L"label");
        ExpectTrue("xengine: finding[1] verdict Equivalent", f1.verdict == TypeVerdict::Equivalent);
        ExpectStr("xengine: finding[1] reason exact text", f1.reason,
                  L"类型种类相同，视为跨引擎语义等价");
    }
}

int main()
{
    std::printf("== SchemaDiffGoldenTests (WP0 characterization) ==\n");

    auto pg = CreateConnection(DbType::PostgreSQL);
    if (!pg) {
        std::printf("  FATAL: CreateConnection(PostgreSQL) returned null\n");
        return 1;
    }
    ExpectTrue("assumption: Postgres not connected (offline fixture)", !pg->IsConnected());

    TestPgToPgCreateTableGolden(*pg);
    TestPgAlterColumnTypeGolden(*pg);
    TestCrossEngineSkipAndWarnGolden();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
