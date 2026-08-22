// SyncSelectionAdapterTests.cpp — ADR-015 T6, integration half: exercises
// ui::MakeTableDiff (src/ui/SyncSelectionAdapter.h) against REAL
// db::sync::RowChangeSet values (src/db/RowChange.h, T3), then drives the
// resulting ui::SyncSelection end-to-end.
//
// This is deliberately a separate binary from SyncSelectionTests. That one
// links only swiftsql::core and must KEEP doing so: it is a standing fitness
// function proving ui::SyncSelection depends on nothing but wxString. All
// data-layer knowledge is confined to the adapter, and therefore to this file.
//
// What it pins, on real data-layer objects rather than hand-set booleans:
//   - RowChangeSet::Executable() (derived, no path back to true) reaching the
//     selection model, and a blocked set being structurally absent from the
//     execution spec even when every one of its boxes is force-checked;
//   - RowChangeSet::Truncated() suppressing row-level selection;
//   - the delete double-gate holding on a real, executable, delete-bearing set.
//
// Same dependency-free harness as SyncSelectionTests.cpp / sync_test.cpp.
#include "ui/SyncSelectionAdapter.h"

#include <cstdio>
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
// Fixtures — real RowChangeSet objects built through the real gate
// ---------------------------------------------------------------------------

// A default-constructed ColumnBridge is `passthrough`, so ConvertCell returns
// the input untouched with verdict Exact. That keeps these fixtures about the
// SELECTION contract rather than about value conversion, which SyncValueMapTests
// already covers exhaustively.
static ColumnBridge PassthroughBridge()
{
    return ColumnBridge{};
}

// Cell/CellKind live in namespace db (SyncTypes.h), not db::sync.
static db::Cell Text(const wchar_t* s)
{
    db::Cell c;
    c.kind = db::CellKind::Text;
    c.text = s;
    return c;
}

// Accept `n` emittable rows of `op` into `set`.
static void AcceptRows(RowChangeSet& set, RowChange::Op op, const wchar_t* table, int n)
{
    for (int i = 0; i < n; ++i) {
        RowChangeBuilder b(op, table, wxString::Format(L"%s(id=%d)", table, i));
        b.AddKeyCell(Text(L"k"), PassthroughBridge());
        b.AddCell(Text(L"v"), PassthroughBridge());
        set.Accept(std::move(b));
    }
}

static Finding MakeFinding(const wchar_t* table, const wchar_t* column, const wchar_t* reason)
{
    Finding f;
    f.table  = table;
    f.column = column;
    f.reason = reason;
    return f;
}

// ---------------------------------------------------------------------------

static void TestEmptySetMapping()
{
    std::printf("[TestEmptySetMapping]\n");

    RowChangeSet    rows;
    const TableDiff d = MakeTableDiff(L"orders", rows);

    ExpectStr("key is the stable table name", d.key.Value(), L"orders");
    ExpectEq("no inserts", d.inserts, 0);
    ExpectEq("no updates", d.updates, 0);
    ExpectEq("no deletes", d.deletes, 0);
    ExpectTrue("an empty set is not truncated", !d.truncated);
    ExpectTrue("an empty set is executable", d.dataExecutable);
    ExpectTrue("structure defaults off", !d.hasStructure);
    ExpectEq("no findings", (long long)d.findings.size(), 0);

    // Nothing to do => the table never reaches the execution spec.
    CompareResult r;
    r.tables.push_back(d);
    SyncSelection sel;
    sel.Reset(r);
    ExpectTrue("a table with no changes yields an empty spec", sel.Build().Empty());
}

static void TestCountersAndStructureHalf()
{
    std::printf("[TestCountersAndStructureHalf]\n");

    RowChangeSet rows;
    AcceptRows(rows, RowChange::Op::Insert, L"orders", 4);
    AcceptRows(rows, RowChange::Op::Update, L"orders", 2);
    AcceptRows(rows, RowChange::Op::Delete, L"orders", 3);

    ExpectEq("data layer counted the inserts", rows.Stat().inserts, 4);
    const TableDiff d = MakeTableDiff(L"orders", rows, /*hasStructure*/ true);

    ExpectEq("inserts mapped", d.inserts, 4);
    ExpectEq("updates mapped", d.updates, 2);
    ExpectEq("deletes mapped", d.deletes, 3);
    ExpectTrue("structure half passed through", d.hasStructure);
    ExpectTrue("structure executable by default", d.structureExecutable);
    ExpectTrue("9 rows is well under the sample cap -> not truncated", !d.truncated);

    CompareResult r;
    r.tables.push_back(d);
    SyncSelection sel;
    sel.Reset(r);

    const TableExecSpec* spec = sel.Build().Find(TableKey(L"orders"));
    ExpectTrue("table is in the spec", spec != nullptr);
    if (spec) {
        ExpectTrue("structure selected by default", spec->Structure());
        ExpectTrue("inserts selected by default", spec->Inserts());
        ExpectTrue("updates selected by default", spec->Updates());
        ExpectTrue("deletes NOT selected by default", !spec->Deletes());
    }

    // The delete double-gate, end to end on a real RowChangeSet.
    sel.SetChecked(TableKey(L"orders"), ChangeCategory::Deletes, true);
    ExpectTrue("real set + checked box + switch OFF => no deletes",
               !sel.Build().ContainsDeletes());
    sel.SetAllowDeletes(true);
    ExpectTrue("real set + checked box + switch ON => deletes planned",
               sel.Build().ContainsDeletes());
    ExpectEq("summary counts the real delete rows", sel.Summarize().deletes, 3);
}

static void TestBlockedSetIsStructurallyExcluded()
{
    std::printf("[TestBlockedSetIsStructurallyExcluded]\n");

    RowChangeSet rows;
    AcceptRows(rows, RowChange::Op::Insert, L"invoices", 5);
    ExpectTrue("set is executable before the blocking finding", rows.Executable());

    // A blocking, non-row Finding — e.g. BuildBridges deciding a column pair
    // cannot be converted at all.
    rows.AddFinding(MakeFinding(L"invoices", L"amount", L"numeric(38,10) has no safe target type"),
                    ValueVerdict::Unrepresentable);
    ExpectTrue("data layer marked the set non-executable", !rows.Executable());

    const TableDiff d = MakeTableDiff(L"invoices", rows, /*hasStructure*/ true);
    ExpectTrue("non-executable state reached the selection model", !d.dataExecutable);
    ExpectTrue("its rows are still COUNTED (they must stay visible)", d.inserts == 5);
    ExpectEq("the finding came through for display", (long long)d.findings.size(), 1);
    if (!d.findings.empty())
        ExpectStr("finding text is table.column: reason", d.findings[0],
                  L"invoices.amount: numeric(38,10) has no safe target type");

    CompareResult r;
    r.tables.push_back(d);
    SyncSelection sel;
    sel.Reset(r);
    sel.SetAllowDeletes(true);

    ExpectTrue("blocked data category not pre-checked",
               !sel.IsChecked(TableKey(L"invoices"), ChangeCategory::Inserts));

    // Force every box on anyway — a blocked set must still be unplannable.
    for (ChangeCategory cat : AllCategories())
        sel.SetChecked(TableKey(L"invoices"), cat, true);

    const TableExecSpec* spec = sel.Build().Find(TableKey(L"invoices"));
    ExpectTrue("table present only for its still-executable structure half", spec != nullptr);
    if (spec) {
        ExpectTrue("structure half still plannable", spec->Structure());
        ExpectTrue("blocked inserts excluded despite being force-checked", !spec->Inserts());
        ExpectTrue("blocked updates excluded", !spec->Updates());
        ExpectTrue("blocked deletes excluded", !spec->Deletes());
    }
    ExpectEq("no blocked rows are counted as planned inserts", sel.Summarize().inserts, 0);

    // And when the structure half is blocked too, the table vanishes entirely.
    const TableDiff both = MakeTableDiff(L"invoices", rows, true, /*structureExecutable*/ false);
    CompareResult r2;
    r2.tables.push_back(both);
    SyncSelection sel2;
    sel2.Reset(r2);
    for (ChangeCategory cat : AllCategories())
        sel2.SetChecked(TableKey(L"invoices"), cat, true);
    ExpectTrue("a fully blocked table cannot appear in the spec at all",
               sel2.Build().Empty());
}

static void TestTruncationSuppressesRowSelection()
{
    std::printf("[TestTruncationSuppressesRowSelection]\n");

    RowChangeSet small;
    AcceptRows(small, RowChange::Op::Insert, L"small", 10);
    ExpectTrue("data layer says small set is not truncated", !small.Truncated());

    RowChangeSet big;
    AcceptRows(big, RowChange::Op::Insert, L"big",
               static_cast<int>(RowChangeSet::kSampleCap) + 1);
    ExpectTrue("data layer says over-cap set IS truncated", big.Truncated());
    ExpectEq("all rows still counted, not just the sampled ones", big.Stat().inserts,
             (long long)RowChangeSet::kSampleCap + 1);

    CompareResult r;
    r.tables.push_back(MakeTableDiff(L"small", small));
    r.tables.push_back(MakeTableDiff(L"big", big));
    SyncSelection sel;
    sel.Reset(r);

    ExpectTrue("row selection offered for the fully materialized diff",
               sel.RowSelectionAvailable(TableKey(L"small")));
    ExpectTrue("row exclusion accepted there",
               sel.SetRowExcluded(TableKey(L"small"), RowKey(L"k=3"), true));

    ExpectTrue("row selection NOT offered for the truncated diff",
               !sel.RowSelectionAvailable(TableKey(L"big")));
    ExpectTrue("row exclusion refused there",
               !sel.SetRowExcluded(TableKey(L"big"), RowKey(L"k=3"), true));

    const TableExecSpec* bigSpec = sel.Build().Find(TableKey(L"big"));
    ExpectTrue("truncated table still plans its whole insert category",
               bigSpec && bigSpec->Inserts());
    if (bigSpec)
        ExpectEq("and carries no row-level decisions",
                 (long long)bigSpec->ExcludedRows().size(), 0);
}

int main()
{
    TestEmptySetMapping();
    TestCountersAndStructureHalf();
    TestBlockedSetIsStructurallyExcluded();
    TestTruncationSuppressesRowSelection();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
