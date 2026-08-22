// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncRowSelectionTests.cpp — the two functional gaps this round closed on the
// data-sync compare screen, both of which sit directly on the user's wording:
//
//   「界面上要有明确的对比」  — an UPDATE must render the target's BEFORE value
//                              next to the source's AFTER value, not the new
//                              value against a placeholder;
//   「让用户去勾选哪些可以同步还是所有同步」
//                            — per-ROW selection must exist and must actually
//                              reach the execution plan.
//
// WHY A SEPARATE FILE RATHER THAN MORE CASES IN THE TWO NEIGHBOURS
// The behaviour under test spans BOTH pure UI models at once: it starts at
// ui::AppendSampleRows (SyncDiffModel.cpp), carries an identity through
// ui::SyncSelection (SyncSelection.cpp), and ends in a db::sync::DataExecPlan.
// SyncDiffModelTests links the first, SyncCompareRowTests the second; neither
// can see the whole chain, and the chain is precisely what regressed before.
// This target compiles both, so an end-to-end assertion is possible in one
// process — and both existing files stay well under the charter's 1000-line cap.
//
// Offline by construction: a canned-row IConnection stub, no server, no GUI.
// Same dependency-free harness as sync_test.cpp / SyncRowIdentityTests.cpp.
#include "db/DataSync.h"
#include "db/DbDriver.h"
#include "ui/SyncCompareRow.h"
#include "ui/SyncDiffModel.h"
#include "ui/SyncSelection.h"

#include <atomic>
#include <cstdio>
#include <vector>
#include <wx/init.h>
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

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ===========================================================================
//  Stub IConnection — canned rows, no wire protocol.
// ===========================================================================
class StubConn : public IConnection {
public:
    Dialect                        dialect_ = Dialect::Postgres;
    std::vector<std::vector<Cell>> rows_;

    Dialect GetDialect() const override { return dialect_; }

    bool StreamRows(const wxString&, const QualifiedName&, const StreamOptions&,
                    const RowSink& sink, wxString&) override
    {
        for (const auto& r : rows_)
            if (!sink(r)) return true;
        return true;
    }

    bool Connect(const core::ConnectionProfile&, wxString&) override { return true; }
    void Disconnect() override {}
    bool IsConnected() const override { return true; }
    bool Execute(const wxString&, QueryResult&, wxString&) override { return true; }
    bool ListDatabases(std::vector<wxString>&, wxString&) override { return true; }
    bool ListTables(const wxString&, std::vector<TableInfo>&, wxString&) override { return true; }
    bool GetColumns(const wxString&, const wxString&, std::vector<ColumnInfo>&,
                    wxString&) override { return true; }
    bool GetForeignKeys(const wxString&, const wxString&, std::vector<ForeignKey>&,
                        wxString&) override { return true; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<IndexInfo>&,
                    wxString&) override { return true; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&, wxString&)
        override { return true; }
    wxString ServerVersion() const override { return L"stub"; }
};

static Cell Num(const wxString& t) { return Cell{ CellKind::Numeric, t }; }
static Cell Txt(const wxString& t) { return Cell{ CellKind::Text, t }; }

static NormColumn Col(const wxString& name, ColKind kind, const wxString& raw)
{
    NormColumn c; c.name = name; c.kind = kind; c.rawType = raw;
    return c;
}

// `id` int PK + `v` text + `w` text. Three columns on purpose: an UPDATE that
// changes ONE of them is the case where per-cell `differs` marking either works
// or floods the row.
static TableSchema OrdersSchema()
{
    TableSchema s;
    s.name = L"orders";
    s.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
    s.columns.push_back(Col(L"v",  ColKind::Varchar, L"varchar(40)"));
    s.columns.push_back(Col(L"w",  ColKind::Varchar, L"varchar(40)"));
    s.primaryKey.push_back(L"id");
    return s;
}

static DataDiffSpec OrdersSpec()
{
    DataDiffSpec s;
    s.srcDb = L"s"; s.tgtDb = L"t";
    s.srcSchema = OrdersSchema();
    s.tgtSchema = OrdersSchema();
    return s;
}

// Run a compare pass over two canned row sets.
static bool Compare(const std::vector<std::vector<Cell>>& srcRows,
                    const std::vector<std::vector<Cell>>& tgtRows,
                    RowLayout& layout, RowChangeSet& set)
{
    std::atomic<bool> stop{false};
    StubConn src, tgt;
    src.dialect_ = tgt.dialect_ = Dialect::Postgres;
    src.rows_ = srcRows;
    tgt.rows_ = tgtRows;

    DataSyncOptions opt;
    opt.insert = opt.update = opt.deleteMissing = true;
    wxString err;
    return DiffRowChanges(src, tgt, OrdersSpec(), opt, layout, set, err, stop);
}

// Find the first Row-kind child with a given op.
static const ui::DiffNode* FindRow(const ui::DiffNode& table, ui::DiffOp op)
{
    for (const auto& c : table.children)
        if (c->kind == ui::DiffNodeKind::Row && !c->rowKey.IsEmpty() && c->op == op)
            return c.get();
    return nullptr;
}

static const ui::DiffNode* FindLeaf(const ui::DiffNode& row, const wxString& column)
{
    for (const auto& c : row.children)
        if (c->kind == ui::DiffNodeKind::ValueDiff && c->label == column) return c.get();
    return nullptr;
}

// ===========================================================================
//  1. GAP 1 — an UPDATE renders the target's BEFORE against the source's AFTER
// ===========================================================================
static void TestUpdateRendersBothSides()
{
    std::printf("\n[1] UPDATE rows render a real before/after pair\n");

    // id=2 differs in `v` only; `w` is identical on both sides. That asymmetry
    // is the whole test: the row IS a difference, but only ONE of its columns
    // is, and the UI must say which.
    RowLayout layout; RowChangeSet set;
    ExpectTrue("compare pass ok",
               Compare({ { Num(L"2"), Txt(L"new"), Txt(L"same") } },
                       { { Num(L"2"), Txt(L"old"), Txt(L"same") } }, layout, set));
    ExpectEq("one update detected", set.Stat().updates, 1);

    ui::DiffNode table;
    table.kind = ui::DiffNodeKind::Table;
    table.id = table.label = L"orders";
    ui::AppendSampleRows(table, set, layout);

    const ui::DiffNode* row = FindRow(table, ui::DiffOp::Modify);
    ExpectTrue("the update reached the tree as a concrete row", row != nullptr);
    if (!row) return;

    const ui::DiffNode* v = FindLeaf(*row, L"v");
    const ui::DiffNode* w = FindLeaf(*row, L"w");
    ExpectTrue("both column leaves present", v && w);
    if (!v || !w) return;

    // The headline: neither side is a placeholder any more.
    ExpectStr("before == the TARGET's current value", v->before, L"old");
    ExpectStr("after  == the SOURCE's new value",     v->after,  L"new");
    ExpectTrue("no placeholder on either side of a changed column",
               !ui::IsPlaceholderSide(v->before) && !ui::IsPlaceholderSide(v->after));
    ExpectStr("specifically: NOT the stale 未随比对下发 marker",
              v->before == ui::UnavailableSide() ? L"<placeholder>" : L"<real>", L"<real>");

    // Per-cell marking: only the column that actually moved lights up.
    ExpectTrue("the changed column is marked differing", v->differs);
    ExpectTrue("the UNCHANGED column is NOT marked differing", !w->differs);
    ExpectStr("...and both sides of it are the same real value", w->before, w->after);
    ExpectStr("row summary counts only the differing columns", row->summary, L"1/3 列不同");
}

// ===========================================================================
//  2. GAP 1 — the placeholder stays REACHABLE when prior values are absent
// ===========================================================================
//
// Prior values are retained only for rows that enter the sample
// (RowChangeBuilder::KeepPriorValues defaults OFF so a 500k-row diff does not
// materialize 500k target rows). A row built without them must say so rather
// than have a target value invented for it — this is the fallback branch, and a
// fallback nothing exercises is a fallback that rots.
static void TestUpdateWithoutPriorValuesFallsBackHonestly()
{
    std::printf("\n[2] an UPDATE with no prior values renders the honest placeholder\n");

    RowLayout layout;
    layout.columns   = { L"id", L"v" };
    layout.pkColumns = { L"id" };

    ColumnBridge br;                 // passthrough by default: same-engine
    br.toDialect = Dialect::Postgres;

    // Deliberately NO AttachPriorValues: exactly the shape a row built outside
    // the sampling path has.
    RowChangeBuilder b(RowChange::Op::Update, L"orders", L"id=9");
    b.AddCell(Num(L"9"), br);
    b.AddCell(Txt(L"new"), br);
    b.AddKeyCell(Num(L"9"), br);

    RowChangeSet set;
    ExpectTrue("row accepted", set.Accept(std::move(b)));
    ExpectTrue("...and it genuinely carries no prior values",
               !set.Sample().empty() && !set.Sample()[0].HasPriorValues());

    ui::DiffNode table;
    table.kind = ui::DiffNodeKind::Table;
    table.id = table.label = L"orders";
    ui::AppendSampleRows(table, set, layout);

    const ui::DiffNode* row = FindRow(table, ui::DiffOp::Modify);
    ExpectTrue("row node built", row != nullptr);
    if (!row) return;

    const ui::DiffNode* v = FindLeaf(*row, L"v");
    ExpectTrue("column leaf present", v != nullptr);
    if (!v) return;

    ExpectStr("target side states it did not reach the UI", v->before, ui::UnavailableSide());
    ExpectStr("source side is still the real value", v->after, L"new");
    ExpectTrue("an unknown side is NOT rendered as agreement", v->differs);
}

// ===========================================================================
//  3. GAP 2 — the row identity is RowChange::RowKey() verbatim
// ===========================================================================
static void TestRowKeyIsCarriedNotRederived()
{
    std::printf("\n[3] DiffNode::rowKey is the data layer's key, copied\n");

    RowLayout layout; RowChangeSet set;
    ExpectTrue("compare pass ok",
               Compare({ { Num(L"2"), Txt(L"new"), Txt(L"x") },
                         { Num(L"3"), Txt(L"i"),   Txt(L"y") } },
                       { { Num(L"2"), Txt(L"old"), Txt(L"x") },
                         { Num(L"4"), Txt(L"d"),   Txt(L"z") } }, layout, set));

    ui::DiffNode table;
    table.kind = ui::DiffNodeKind::Table;
    table.id = table.label = L"orders";
    ui::AppendSampleRows(table, set, layout);

    // Every concrete row's key must EQUAL the RowChange it came from, by string.
    int matched = 0;
    for (const RowChange& r : set.Sample()) {
        for (const auto& c : table.children) {
            if (c->kind != ui::DiffNodeKind::Row || c->rowKey.IsEmpty()) continue;
            if (c->rowKey == r.RowKey()) { ++matched; break; }
        }
    }
    ExpectEq("every sampled row has a node carrying its key verbatim",
             matched, (long long)set.Sample().size());

    const ui::DiffNode* up = FindRow(table, ui::DiffOp::Modify);
    ExpectTrue("update row found", up != nullptr);
    if (up) {
        ExpectStr("the key is the canonical encoding", up->rowKey, L"id=2");
        // The LABEL is display text and must never be mistaken for the key.
        ExpectTrue("label and key are different strings", up->label != up->rowKey);
        ExpectStr("label is the human form", up->label, L"id = 2");
    }

    // A SUMMARY row is a counter, not a row: it must carry no key, or the grid
    // would offer a checkbox for「修改 5 行」.
    ui::DiffNode counters;
    counters.kind = ui::DiffNodeKind::Table;
    counters.id = L"orders";
    RowStat st; st.updates = 5;
    ui::AppendDataSummary(counters, st);
    ExpectTrue("summary rows carry NO row key",
               !counters.children.empty() && counters.children[0]->rowKey.IsEmpty());
}

// ===========================================================================
//  4. GAP 2 — a truncated diff offers no row selection, and SAYS WHY
// ===========================================================================
static void TestTruncatedDiffExplainsItself()
{
    std::printf("\n[4] a truncated diff refuses row selection and explains it\n");

    // 600 differing rows => 100 past RowChangeSet::kSampleCap.
    std::vector<std::vector<Cell>> srcRows, tgtRows;
    for (int i = 0; i < 600; ++i) {
        const wxString id = wxString::Format(L"%06d", i);
        srcRows.push_back({ Num(id), Txt(L"new"), Txt(L"x") });
        tgtRows.push_back({ Num(id), Txt(L"old"), Txt(L"x") });
    }

    RowLayout layout; RowChangeSet set;
    ExpectTrue("compare pass ok", Compare(srcRows, tgtRows, layout, set));
    ExpectTrue("the set really is truncated", set.Truncated());

    ui::DiffNode table;
    table.kind = ui::DiffNodeKind::Table;
    table.id = table.label = L"orders";
    ui::AppendSampleRows(table, set, layout);

    // (a) The UI is not merely disabled — it is LEGIBLE. A warning child exists
    //     and it names both the cause and the consequence.
    const ui::DiffNode* warn = nullptr;
    for (const auto& c : table.children)
        if (c->kind == ui::DiffNodeKind::Warning) { warn = c.get(); break; }
    ExpectTrue("a truncated table carries a warning child", warn != nullptr);
    if (warn) {
        // Consequence in the label (the narrow 表名 column clips), cause in the
        // summary beside it.
        ExpectTrue("...saying row-level checking is unavailable, up front",
                   warn->label.Contains(L"逐行"));
        ExpectTrue("...and naming the cap that caused it", warn->summary.Contains(L"500"));
        ExpectTrue("the explanation comes BEFORE the sample it qualifies",
                   table.children[0].get() == warn);
    }

    // (b) The model REFUSES row-level selection for it, so the missing
    //     checkboxes are not a rendering accident.
    ui::CompareResult res;
    res.tables.push_back(ui::MakeTableDiff(L"orders", set));
    ui::SyncSelection sel;
    sel.Reset(res);

    const ui::TableKey key(wxString(L"orders"));
    ExpectTrue("TableDiff mirrors the truncation", sel.Find(key)->truncated);
    ExpectTrue("row selection is UNAVAILABLE", !sel.RowSelectionAvailable(key));

    const ui::DiffNode* row = FindRow(table, ui::DiffOp::Modify);
    ExpectTrue("a sample row exists to try to exclude", row != nullptr);
    if (row) {
        ExpectTrue("SetRowExcluded is refused on a truncated table",
                   !sel.SetRowExcluded(key, ui::RowKey(row->rowKey), true));
        ExpectTrue("...and nothing was recorded",
                   !sel.IsRowExcluded(key, ui::RowKey(row->rowKey)));
    }
    ExpectEq("no exclusions leak into the spec",
             (long long)sel.Summarize().excludedRows, 0);
}

// ===========================================================================
//  5. GAP 2 — an excluded row does NOT reach the DataExecPlan
// ===========================================================================
//
// The end-to-end assertion: grid identity -> SyncSelection -> ExecutionSpec ->
// db::sync::TableDataSpec. This is the chain that was plumbed but had no
// producer at its head.
static void TestExcludedRowIsAbsentFromExecPlan()
{
    std::printf("\n[5] an excluded row does not reach the execution plan\n");

    RowLayout layout; RowChangeSet set;
    ExpectTrue("compare pass ok",
               Compare({ { Num(L"1"), Txt(L"a"), Txt(L"x") },     // update
                         { Num(L"2"), Txt(L"b"), Txt(L"x") },     // update
                         { Num(L"5"), Txt(L"n"), Txt(L"x") } },   // insert
                       { { Num(L"1"), Txt(L"A"), Txt(L"x") },
                         { Num(L"2"), Txt(L"B"), Txt(L"x") },
                         { Num(L"9"), Txt(L"d"), Txt(L"x") } },   // delete
                       layout, set));
    ExpectEq("two updates", set.Stat().updates, 2);
    ExpectEq("one insert",  set.Stat().inserts, 1);
    ExpectEq("one delete",  set.Stat().deletes, 1);
    ExpectTrue("not truncated, so row selection is on offer", !set.Truncated());

    ui::DiffNode table;
    table.kind = ui::DiffNodeKind::Table;
    table.id = table.label = L"orders";
    ui::AppendSampleRows(table, set, layout);

    ui::CompareResult res;
    res.tables.push_back(ui::MakeTableDiff(L"orders", set));
    ui::SyncSelection sel;
    sel.Reset(res);
    sel.SetAllowDeletes(true);
    sel.SetChecked(ui::TableKey(wxString(L"orders")), ui::ChangeCategory::Deletes, true);

    const ui::TableKey key(wxString(L"orders"));
    ExpectTrue("row selection IS available here", sel.RowSelectionAvailable(key));

    // Exclude one specific UPDATE, by the key the node carried.
    const ui::DiffNode* victim = nullptr;
    for (const auto& c : table.children)
        if (c->kind == ui::DiffNodeKind::Row && c->rowKey == L"id=1") victim = c.get();
    ExpectTrue("the row to exclude was found by its key", victim != nullptr);
    if (!victim) return;

    ExpectTrue("SetRowExcluded accepted", sel.SetRowExcluded(key, ui::RowKey(victim->rowKey), true));

    const ui::ExecutionSpec spec = sel.Build();
    const ui::TableExecSpec* ts  = spec.Find(key);
    ExpectTrue("the table is still in the spec", ts != nullptr);
    if (!ts) return;
    ExpectEq("exactly one row excluded", (long long)ts->ExcludedRows().size(), 1);

    const DataExecPlan plan = ui::BuildDataExecPlan(spec);
    ExpectEq("one table in the data plan", (long long)plan.tables.size(), 1);
    if (plan.tables.empty()) return;

    const TableDataSpec& t = plan.tables[0];
    ExpectEq("the exclusion crossed the layer boundary",
             (long long)t.ExcludedRows().size(), 1);
    ExpectTrue("...as the SAME string the data layer produced",
               t.ExcludedRows().count(L"id=1") != 0);

    // The point of "verbatim": the string must equal EncodeRowKey over the RAW
    // key cells, which is what DataSyncExec matches against. Re-encoding
    // RowChange::Key() (post-conversion cells) is the silent-miss bug.
    const std::vector<Cell> rawKey = { Num(L"1") };
    ExpectTrue("the excluded key == EncodeRowKey(rawKey), byte for byte",
               t.ExcludedRows().count(EncodeRowKey(layout.pkColumns, rawKey)) != 0);

    // ...and the row the user LEFT checked is untouched.
    ExpectTrue("the other update was NOT excluded", t.ExcludedRows().count(L"id=2") == 0);
    ExpectTrue("deletes are still authorized", t.Deletes());
}

// ===========================================================================
//  6. GAP 2 — exclusions are counted per category for the UI's numbers
// ===========================================================================
static void TestExcludedRowsAreCountedPerCategory()
{
    std::printf("\n[6] exclusions are attributed to the right category\n");

    RowLayout layout; RowChangeSet set;
    ExpectTrue("compare pass ok",
               Compare({ { Num(L"1"), Txt(L"a"), Txt(L"x") },     // update
                         { Num(L"5"), Txt(L"n"), Txt(L"x") } },   // insert
                       { { Num(L"1"), Txt(L"A"), Txt(L"x") },
                         { Num(L"9"), Txt(L"d"), Txt(L"x") } },   // delete
                       layout, set));

    ui::DiffNode table;
    table.kind = ui::DiffNodeKind::Table;
    table.id = table.label = L"orders";
    ui::AppendSampleRows(table, set, layout);

    ui::CompareResult res;
    res.tables.push_back(ui::MakeTableDiff(L"orders", set));
    ui::SyncSelection sel;
    sel.Reset(res);

    const ui::TableKey key(wxString(L"orders"));
    ExpectEq("nothing excluded to begin with",
             ui::CountExcludedRows(table, sel).Total(), 0);

    // One of each, identified through the node that carries both the op and the
    // key — the join SyncSelection deliberately cannot make on its own.
    for (const auto& c : table.children) {
        if (c->kind != ui::DiffNodeKind::Row || c->rowKey.IsEmpty()) continue;
        sel.SetRowExcluded(key, ui::RowKey(c->rowKey), true);
    }

    const ui::ExcludedRowCounts ex = ui::CountExcludedRows(table, sel);
    ExpectEq("one excluded insert", ex.inserts, 1);
    ExpectEq("one excluded update", ex.updates, 1);
    ExpectEq("one excluded delete", ex.deletes, 1);
    ExpectEq("three in total",      ex.Total(),  3);

    // The counts are exact, not an estimate: on a non-truncated diff every
    // counted row is in the sample, so excluding all of them zeroes the table.
    ExpectEq("excluded inserts == detected inserts", ex.inserts, set.Stat().inserts);
    ExpectEq("excluded updates == detected updates", ex.updates, set.Stat().updates);
    ExpectEq("excluded deletes == detected deletes", ex.deletes, set.Stat().deletes);

    // Re-including one row is not a one-way door.
    for (const auto& c : table.children)
        if (c->kind == ui::DiffNodeKind::Row && c->rowKey == L"id=1")
            sel.SetRowExcluded(key, ui::RowKey(c->rowKey), false);
    ExpectEq("re-including drops the update back out of the count",
             ui::CountExcludedRows(table, sel).updates, 0);
    ExpectEq("...and only that one", ui::CountExcludedRows(table, sel).Total(), 2);
}

// ===========================================================================
//  7. GAP 2 — the whole-tree count, as the page footer reads it
// ===========================================================================
static void TestTreeWideExclusionCount()
{
    std::printf("\n[7] the tree-wide count sums only Table nodes\n");

    RowLayout layout; RowChangeSet set;
    ExpectTrue("compare pass ok",
               Compare({ { Num(L"1"), Txt(L"a"), Txt(L"x") } },
                       { { Num(L"1"), Txt(L"A"), Txt(L"x") } }, layout, set));

    SyncPlan plan;
    {
        SyncPlan::TableUnit u;
        u.table       = L"orders";
        u.layout      = layout;
        u.rows        = set;
        u.stat.updates = 1;
        u.dataVerdict = DataVerdict::Ok;
        plan.units.push_back(std::move(u));
    }
    plan.warnings.push_back(L"某个没有 TableUnit 的提示");

    const ui::DiffTree tree = ui::BuildDiffTree(plan);
    ui::SyncSelection  sel;
    sel.Reset(ui::MakeCompareResultFromPlan(plan));

    ExpectEq("nothing excluded initially", ui::CountExcludedRows(tree, sel).Total(), 0);

    sel.SetRowExcluded(ui::TableKey(wxString(L"orders")), ui::RowKey(wxString(L"id=1")), true);
    const ui::ExcludedRowCounts ex = ui::CountExcludedRows(tree, sel);
    ExpectEq("the tree-wide walk finds it", ex.Total(), 1);
    ExpectEq("...as an update", ex.updates, 1);

    // The Warning ROOT group (a plan warning with no TableUnit) must not be
    // walked as if it were a table — it carries no id and nothing selectable.
    ExpectTrue("the plan warning still has its own root",
               tree.roots.size() == 2 && tree.roots[1]->kind == ui::DiffNodeKind::Warning);
}

int main()
{
    // ui::tr -> core::Lang::tr lazily reads the settings file via
    // wxStandardPaths, which needs the wx runtime up. wxInitializer brings up
    // wxBase only: no window is created and this stays headless. Same reasoning
    // as SyncDiffModelTests.cpp / SyncCompareRowTests.cpp.
    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::printf("FATAL: could not initialize wxBase\n");
        return 2;
    }

    std::printf("=== SyncRowSelectionTests ===\n");
    TestUpdateRendersBothSides();
    TestUpdateWithoutPriorValuesFallsBackHonestly();
    TestRowKeyIsCarriedNotRederived();
    TestTruncatedDiffExplainsItself();
    TestExcludedRowIsAbsentFromExecPlan();
    TestExcludedRowsAreCountedPerCategory();
    TestTreeWideExclusionCount();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
