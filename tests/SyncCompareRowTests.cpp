// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCompareRowTests.cpp — T8/T11 standalone unit tests for
// src/ui/SyncCompareRow.h, the pure half of the Navicat-style compare grid: row
// classification (类型 / 状态), the SyncPlan -> CompareResult adapter, and the
// two builders that turn a ui::ExecutionSpec into what SyncEngine::Execute
// actually runs.
//
// WHAT CHANGED, AND WHY THESE TESTS LOOK DIFFERENT NOW
// This file used to test ui::FilterPlanBySpec, which decided a statement's
// category by its LEADING SQL VERB and then dropped the ones the user had
// unchecked. That function is gone. The data layer now filters per (table,
// category) at the point of DETECTION inside the merge, so a category the user
// left unchecked never produces a db::sync::RowChange at all — there is no
// statement downstream to classify, drop, or accidentally keep.
//
// The gate therefore moved, and so did the test. The highest-stakes assertion is
// no longer "no DELETE string survived the filter" but "no delete
// AUTHORIZATION exists in the db::sync::DataExecPlan", which is a stronger
// claim: db::sync::TableDataSpec::deletes_ starts false and its only mutator
// demands a db::sync::DeleteAuthorization, whose only producer returns nullopt
// while the master switch is off. Cases covered:
//   * unchecked categories are physically absent from the DataExecPlan;
//   * an unchecked/unpermitted DELETE cannot be authorized, and — because
//     SyncSelection refuses to mint a ui::DeleteGate while the master switch is
//     off — cannot be authorized even when the user ticked the delete box first;
//   * a table with no approved category produces no spec entry at all;
//   * row exclusions travel as the encoded row key, by identity;
//   * tables are matched by stable id, so reordering the plan's units changes
//     nothing;
//   * the CompareResult adapter mirrors truncated/executable/verdict from the
//     structured carrier instead of hardcoding them (the bug this round fixed).
//
// Same dependency-free harness as SyncDiffModelTests.cpp: ExpectTrue/ExpectEq,
// non-zero exit on any failure. Links swiftsql::db (which PUBLIC-links
// swiftsql::core, needed for core::Lang behind ui::tr) and compiles
// SyncSelection.cpp directly. No wxWindow/GUI is exercised: SyncCompareRow.h is
// header-only and widget-free precisely so this test can exist.
#include "ui/SyncCompareRow.h"

#include <cstdio>
#include <wx/init.h>
#include <wx/string.h>

using namespace ui;
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

// ---------------------------------------------------------------------------
// Building real RowChangeSets
//
// These go through RowChangeBuilder + RowChangeSet::Accept because that is the
// ONLY door: RowChange has a private constructor and Accept is the only mutator
// that admits one. A test that fabricated a RowChangeSet some other way would
// be testing a shape the production code can never actually receive.
// ---------------------------------------------------------------------------

static db::Cell Num(const wchar_t* v) { return db::Cell{ db::CellKind::Numeric, v }; }
static db::Cell Txt(const wchar_t* v) { return db::Cell{ db::CellKind::Text, v }; }

// An identity bridge: passthrough, so ConvertCell returns the input untouched
// with verdict Exact after one bool test. Same-engine sync uses exactly these.
static ColumnBridge Bridge(size_t idx)
{
    ColumnBridge b;
    b.srcIdx      = idx;
    b.passthrough = true;
    return b;
}

static void AddRow(RowChangeSet& set, RowChange::Op op, const wxString& table,
                   const std::vector<db::Cell>& key, const std::vector<db::Cell>& values)
{
    RowChangeBuilder b(op, table, L"row");
    for (size_t i = 0; i < key.size(); ++i)    b.AddKeyCell(key[i], Bridge(i));
    for (size_t i = 0; i < values.size(); ++i) b.AddCell(values[i], Bridge(i));
    set.Accept(std::move(b));
}

// ---------------------------------------------------------------------------
// 类型 / 状态
// ---------------------------------------------------------------------------

static void TestRowTypeMapping()
{
    ExpectTrue("Create  -> 新建表",
               ClassifyCompareRow(DiffChangeKind::Create) == CompareRowType::NewTable);
    ExpectTrue("Drop    -> 目标多余表",
               ClassifyCompareRow(DiffChangeKind::Drop) == CompareRowType::ExtraTable);
    ExpectTrue("Structure -> 仅结构",
               ClassifyCompareRow(DiffChangeKind::Structure) == CompareRowType::StructureOnly);
    ExpectTrue("Data    -> 仅数据",
               ClassifyCompareRow(DiffChangeKind::Data) == CompareRowType::DataOnly);
    ExpectTrue("Both    -> 结构+数据",
               ClassifyCompareRow(DiffChangeKind::Both) == CompareRowType::StructureAndData);
    ExpectTrue("every type has a non-empty label",
               !CompareRowTypeLabel(CompareRowType::NewTable).IsEmpty() &&
               !CompareRowTypeLabel(CompareRowType::ExtraTable).IsEmpty() &&
               !CompareRowTypeLabel(CompareRowType::StructureOnly).IsEmpty() &&
               !CompareRowTypeLabel(CompareRowType::DataOnly).IsEmpty() &&
               !CompareRowTypeLabel(CompareRowType::StructureAndData).IsEmpty());
}

static void TestStatusMapping()
{
    TableDiff d;
    d.key = TableKey(L"t");
    d.dataStatus = DataStatus::Ok;
    ExpectTrue("clean diff -> 就绪", ClassifyCompareStatus(d) == CompareRowStatus::Ready);
    ExpectTrue("就绪 is not a warning", !CompareRowStatusIsWarning(CompareRowStatus::Ready));

    d.truncated = true;
    ExpectTrue("truncated -> 行数超限",
               ClassifyCompareStatus(d) == CompareRowStatus::RowLimitExceeded);

    // A blocked table outranks a truncated one: being unable to run at all is
    // the fact that changes what the user may do.
    d.dataExecutable = false;
    ExpectTrue("blocked outranks truncated",
               ClassifyCompareStatus(d) == CompareRowStatus::HasUnconvertibleValues);

    TableDiff s;
    s.key = TableKey(L"t");
    s.structureExecutable = false;
    ExpectTrue("a blocked STRUCTURE half also reports 含不可转换值",
               ClassifyCompareStatus(s) == CompareRowStatus::HasUnconvertibleValues);
    ExpectTrue("every non-ready status is a warning",
               CompareRowStatusIsWarning(CompareRowStatus::RowLimitExceeded) &&
               CompareRowStatusIsWarning(CompareRowStatus::HasUnconvertibleValues) &&
               CompareRowStatusIsWarning(CompareRowStatus::UnorderableKey) &&
               CompareRowStatusIsWarning(CompareRowStatus::TargetMissingTable) &&
               CompareRowStatusIsWarning(CompareRowStatus::NoPrimaryKey));
}

// The whole point of wiring db::sync::DataVerdict through: these statuses used
// to be unreachable, because nothing in the plan produced the signal and the UI
// is forbidden from recovering it by grepping warning text.
static void TestDataVerdictReachesTheStatusColumn()
{
    auto statusFor = [](DataStatus st) {
        TableDiff d;
        d.key        = TableKey(L"t");
        d.dataStatus = st;
        return ClassifyCompareStatus(d);
    };

    ExpectTrue("NoPrimaryKey   -> 无主键（已排除）",
               statusFor(DataStatus::NoPrimaryKey) == CompareRowStatus::NoPrimaryKey);
    ExpectTrue("UnorderableKey gets its OWN status, not a generic error",
               statusFor(DataStatus::UnorderableKey) == CompareRowStatus::UnorderableKey);
    ExpectTrue("TargetMissing  -> 目标缺少此表",
               statusFor(DataStatus::TargetMissing) == CompareRowStatus::TargetMissingTable);
    ExpectTrue("Blocked        -> 含不可转换值",
               statusFor(DataStatus::Blocked) == CompareRowStatus::HasUnconvertibleValues);
    ExpectTrue("NotRequested is not an error state",
               statusFor(DataStatus::NotRequested) == CompareRowStatus::Ready);

    ExpectTrue("the verdict enum maps one-for-one",
               ToDataStatus(DataVerdict::NoPrimaryKey)   == DataStatus::NoPrimaryKey &&
               ToDataStatus(DataVerdict::UnorderableKey) == DataStatus::UnorderableKey &&
               ToDataStatus(DataVerdict::TargetMissing)  == DataStatus::TargetMissing &&
               ToDataStatus(DataVerdict::Blocked)        == DataStatus::Blocked &&
               ToDataStatus(DataVerdict::Ok)             == DataStatus::Ok &&
               ToDataStatus(DataVerdict::NotRequested)   == DataStatus::NotRequested);

    ExpectTrue("every refusal has a user-facing label",
               !CompareRowStatusLabel(CompareRowStatus::NoPrimaryKey).IsEmpty() &&
               !CompareRowStatusLabel(CompareRowStatus::UnorderableKey).IsEmpty() &&
               !CompareRowStatusLabel(CompareRowStatus::TargetMissingTable).IsEmpty());
    ExpectTrue("and the labels are distinct from one another",
               CompareRowStatusLabel(CompareRowStatus::UnorderableKey) !=
                   CompareRowStatusLabel(CompareRowStatus::NoPrimaryKey) &&
               CompareRowStatusLabel(CompareRowStatus::UnorderableKey) !=
                   CompareRowStatusLabel(CompareRowStatus::HasUnconvertibleValues));
}

// ---------------------------------------------------------------------------
// SyncPlan -> CompareResult
// ---------------------------------------------------------------------------

static SyncPlan BuildPlan()
{
    SyncPlan plan;
    {
        SyncPlan::TableUnit u;
        u.table = L"orders";
        u.ddl.push_back(L"ALTER TABLE orders ADD COLUMN status VARCHAR(20)");
        AddRow(u.rows, RowChange::Op::Insert, L"orders", { Num(L"1") }, { Num(L"1"), Txt(L"new") });
        AddRow(u.rows, RowChange::Op::Update, L"orders", { Num(L"3") }, { Num(L"3"), Txt(L"upd") });
        AddRow(u.rows, RowChange::Op::Delete, L"orders", { Num(L"9") }, {});
        u.stat.inserts = 1;
        u.stat.updates = 1;
        u.stat.deletes = 1;
        u.dataVerdict  = DataVerdict::Ok;
        plan.units.push_back(std::move(u));
    }
    {
        SyncPlan::TableUnit u;
        u.table = L"events";
        AddRow(u.rows, RowChange::Op::Insert, L"events", { Num(L"7") }, { Num(L"7") });
        u.stat.inserts = 1;
        u.dataVerdict  = DataVerdict::Ok;
        plan.units.push_back(std::move(u));
    }
    plan.preamble.push_back(L"SET FOREIGN_KEY_CHECKS=0");
    plan.postamble.push_back(L"SET FOREIGN_KEY_CHECKS=1");
    plan.warnings.push_back(L"表 archive 目标有·源无，未勾选删除，已跳过");
    return plan;
}

static void TestMakeCompareResultFromPlan()
{
    const CompareResult r = MakeCompareResultFromPlan(BuildPlan());
    ExpectEq("one TableDiff per unit", (long long)r.tables.size(), 2);

    const TableDiff& orders = r.tables[0];
    ExpectTrue("key is the stable table name", orders.key == TableKey(L"orders"));
    ExpectTrue("ddl present -> hasStructure", orders.hasStructure);
    ExpectEq("inserts mirrored from the RowChangeSet", orders.inserts, 1);
    ExpectEq("updates mirrored from the RowChangeSet", orders.updates, 1);
    ExpectEq("deletes mirrored from the RowChangeSet", orders.deletes, 1);
    ExpectTrue("a small diff is not truncated", !orders.truncated);
    ExpectTrue("an unblocked set is executable", orders.dataExecutable);
    ExpectTrue("verdict carried through", orders.dataStatus == DataStatus::Ok);
    ExpectTrue("plan-global warnings are NOT attributed to a table",
               orders.findings.empty());

    ExpectTrue("data-only table has no structure half", !r.tables[1].hasStructure);
}

// The two hardcodes this round removed. Both used to be constants in the
// adapter; both are now mirrored from the carrier, and both are wrong in the
// dangerous direction if they are not.
static void TestExecutabilityAndTruncationAreMirroredNotAssumed()
{
    SyncPlan plan;
    {
        // Blocked: a value that cannot cross into the target engine. The set
        // becomes permanently non-executable and there is no path back.
        SyncPlan::TableUnit u;
        u.table = L"blocked";
        Finding f;
        f.table  = L"blocked";
        f.column = L"created_at";
        f.reason = L"0000-00-00 无法转换为 PostgreSQL 的 date";
        u.rows.AddFinding(f, ValueVerdict::Unrepresentable);
        u.dataVerdict       = DataVerdict::Blocked;
        u.dataVerdictReason = L"blocked.created_at: 0000-00-00 无法转换";
        plan.units.push_back(std::move(u));
    }
    {
        // Truncated: more differing rows than the sample cap.
        SyncPlan::TableUnit u;
        u.table = L"huge";
        for (size_t i = 0; i <= RowChangeSet::kSampleCap; ++i)
            AddRow(u.rows, RowChange::Op::Insert, L"huge", { Num(L"1") }, { Num(L"1") });
        u.dataVerdict = DataVerdict::Ok;
        plan.units.push_back(std::move(u));
    }

    const CompareResult r = MakeCompareResultFromPlan(plan);

    ExpectTrue("a blocked set arrives as NOT executable", !r.tables[0].dataExecutable);
    ExpectTrue("and reads as 含不可转换值",
               ClassifyCompareStatus(r.tables[0]) == CompareRowStatus::HasUnconvertibleValues);
    ExpectTrue("the per-table verdict reason IS attributed to its table",
               !r.tables[0].findings.empty());

    ExpectTrue("a capped diff arrives as truncated", r.tables[1].truncated);
    ExpectTrue("and reads as 行数超限",
               ClassifyCompareStatus(r.tables[1]) == CompareRowStatus::RowLimitExceeded);

    // The consequence that matters: SyncSelection refuses row-level selection on
    // a truncated table, so the user is never shown hand-picking they do not have.
    SyncSelection sel;
    sel.Reset(r);
    ExpectTrue("row-level selection is refused on the truncated table",
               !sel.RowSelectionAvailable(TableKey(L"huge")));
    ExpectTrue("a blocked table has no executable data category",
               !sel.IsActive(TableKey(L"blocked"), ChangeCategory::Inserts));
}

// ---------------------------------------------------------------------------
// ExecutionSpec -> DataExecPlan — the gate before execution
// ---------------------------------------------------------------------------

static void TestDataPlanKeepsOnlyCheckedCategories()
{
    const SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));
    // Reset seeds structure/inserts/updates on, deletes OFF (product default).

    const DataExecPlan data = BuildDataExecPlan(sel.Build());
    ExpectEq("both tables are authorized to write", (long long)data.tables.size(), 2);

    const TableDataSpec* orders = data.Find(L"orders");
    ExpectTrue("lookup is by table name", orders != nullptr);
    ExpectTrue("inserts authorized", orders && orders->Inserts());
    ExpectTrue("updates authorized", orders && orders->Updates());
    ExpectTrue("deletes NOT authorized under the default selection",
               orders && !orders->Deletes());
}

static void TestStructurePlanKeepsOnlyCheckedDdl()
{
    const SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));

    const SyncPlan out = FilterStructureBySpec(plan, sel.Build());
    ExpectEq("both tables survive", (long long)out.units.size(), 2);
    ExpectTrue("plan-global preamble passes through", out.preamble.size() == 1);
    ExpectTrue("plan-global postamble passes through", out.postamble.size() == 1);
    ExpectTrue("warnings pass through", out.warnings.size() == 1);
    ExpectEq("structure checked -> ddl kept", (long long)out.units[0].ddl.size(), 1);

    // The `dml` preview vector is never handed to the executor.
    ExpectEq("the preview dml vector is not propagated into the execution plan",
             (long long)out.units[0].dml.size(), 0);

    // Uncheck structure: the DDL goes, but the unit must REMAIN, because
    // SyncEngine::Execute re-derives the data diff from the unit's dataSpec.
    sel.SetChecked(TableKey(L"orders"), ChangeCategory::Structure, false);
    const SyncPlan noDdl = FilterStructureBySpec(plan, sel.Build());
    const SyncPlan::TableUnit* o = nullptr;
    for (const auto& u : noDdl.units) if (u.table == L"orders") o = &u;
    ExpectTrue("the unit survives for its data half", o != nullptr);
    ExpectEq("but carries no ddl", o ? (long long)o->ddl.size() : -1, 0);
}

// The single highest-stakes assertion in this file.
static void TestDeletesCannotBeAuthorizedWithoutTheMasterSwitch()
{
    const SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));

    // The user ticks the delete box while the master switch is still off.
    sel.SetChecked(TableKey(L"orders"), ChangeCategory::Deletes, true);
    ExpectTrue("the raw check bit did flip",
               sel.IsChecked(TableKey(L"orders"), ChangeCategory::Deletes));
    ExpectTrue("but the category is not ACTIVE",
               !sel.IsActive(TableKey(L"orders"), ChangeCategory::Deletes));

    const ExecutionSpec off = sel.Build();
    ExpectTrue("spec contains no deletes while the switch is off", !off.ContainsDeletes());

    const DataExecPlan data = BuildDataExecPlan(off);
    for (const TableDataSpec& t : data.tables)
        ExpectTrue("no table is authorized to delete", !t.Deletes());

    // Same selection, switch on: now — and only now — the authorization exists.
    sel.SetAllowDeletes(true);
    const DataExecPlan armed = BuildDataExecPlan(sel.Build());
    const TableDataSpec* orders = armed.Find(L"orders");
    ExpectTrue("with the master switch on, orders may delete", orders && orders->Deletes());

    const TableDataSpec* events = armed.Find(L"events");
    ExpectTrue("a table whose delete box was never ticked still may not",
               events && !events->Deletes());
}

static void TestUnselectedTableProducesNoAuthorization()
{
    const SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));
    sel.SetChecked(TableKey(L"events"), ChangeCategory::Inserts, false);

    const DataExecPlan data = BuildDataExecPlan(sel.Build());
    ExpectTrue("events had only inserts, so unchecking them removes it entirely",
               data.Find(L"events") == nullptr);
    ExpectTrue("orders is untouched", data.Find(L"orders") != nullptr);
}

static void TestNothingSelectedYieldsEmptyPlans()
{
    const SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));
    sel.SetAllTablesChecked(sel.Keys(), false);

    ExpectTrue("the spec itself is empty", sel.Build().Empty());
    ExpectEq("no structure units", (long long)FilterStructureBySpec(plan, sel.Build()).units.size(), 0);
    ExpectTrue("no data authorization", BuildDataExecPlan(sel.Build()).Empty());
}

// Row exclusions must travel as the encoded row key, unchanged — the UI's
// ui::RowKey and the data layer's db::sync::EncodeRowKey() output are the same
// string by contract, so no translation step exists to get wrong.
static void TestRowExclusionsTravelByIdentity()
{
    const SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));

    // The exact shape EncodeRowKey produces: `col=value` joined by \x1F.
    const wxString encoded = L"id=42";
    ExpectTrue("the table accepts row-level selection",
               sel.RowSelectionAvailable(TableKey(L"orders")));
    ExpectTrue("the row is excluded",
               sel.SetRowExcluded(TableKey(L"orders"), RowKey(encoded), true));

    const DataExecPlan data = BuildDataExecPlan(sel.Build());
    const TableDataSpec* orders = data.Find(L"orders");
    ExpectTrue("the exclusion reached the db-layer spec",
               orders && orders->ExcludedRows().count(encoded) == 1);
    ExpectTrue("and its string is untouched",
               orders && *orders->ExcludedRows().begin() == encoded);
}

// The Round 1 bug, at the level these functions operate on: tables are matched
// by stable id, so the order of SyncPlan::units must not affect the outcome.
static void TestBuildersAreIndependentOfUnitOrder()
{
    SyncPlan plan = BuildPlan();
    SyncSelection sel;
    sel.Reset(MakeCompareResultFromPlan(plan));
    sel.SetChecked(TableKey(L"events"), ChangeCategory::Inserts, false);
    sel.SetChecked(TableKey(L"events"), ChangeCategory::Updates, false);
    sel.SetChecked(TableKey(L"events"), ChangeCategory::Structure, false);

    const SyncPlan a = FilterStructureBySpec(plan, sel.Build());
    std::swap(plan.units[0], plan.units[1]);
    const SyncPlan b = FilterStructureBySpec(plan, sel.Build());

    ExpectEq("same unit count after reordering the plan",
             (long long)a.units.size(), (long long)b.units.size());
    ExpectTrue("the surviving table is 'orders' either way",
               a.units.size() == 1 && b.units.size() == 1 &&
               a.units[0].table == L"orders" && b.units[0].table == L"orders");

    // The data half is keyed off the spec, which is sorted by TableKey and does
    // not consult the plan at all — so it cannot depend on unit order by
    // construction. Asserted anyway, because "by construction" is the claim.
    const DataExecPlan d = BuildDataExecPlan(sel.Build());
    ExpectEq("one authorized table", (long long)d.tables.size(), 1);
    ExpectTrue("and it is orders", d.Find(L"orders") != nullptr);
}

int main()
{
    // The label helpers go through ui::tr -> core::Lang::tr, whose lazy load
    // reads the settings file via wxStandardPaths, which needs the wx runtime
    // initialized. In the app that happens in wxApp::OnInit; a bare console
    // test has to do it itself or the first tr() dereferences a null
    // wxAppTraits. This is NOT a GUI: wxInitializer brings up wxBase only, no
    // window is created, and the test still runs headless.
    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::printf("FATAL: could not initialize wxBase\n");
        return 2;
    }

    TestRowTypeMapping();
    TestStatusMapping();
    TestDataVerdictReachesTheStatusColumn();
    TestMakeCompareResultFromPlan();
    TestExecutabilityAndTruncationAreMirroredNotAssumed();
    TestDataPlanKeepsOnlyCheckedCategories();
    TestStructurePlanKeepsOnlyCheckedDdl();
    TestDeletesCannotBeAuthorizedWithoutTheMasterSwitch();
    TestUnselectedTableProducesNoAuthorization();
    TestNothingSelectedYieldsEmptyPlans();
    TestRowExclusionsTravelByIdentity();
    TestBuildersAreIndependentOfUnitOrder();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
