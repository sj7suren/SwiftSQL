// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncSelectionTests.cpp — T6 of ADR-015: standalone unit tests for
// ui::SyncSelection (src/ui/SyncSelection.{h,cpp}), the pure/non-wx model of
// the user's check-state on the data-sync compare screen.
//
// The headline test here is TestReorderInvariance(): it is the regression test
// for the bug class the component exists to prevent — Round 1 mapped checked
// rows back to the plan by ROW POSITION (`plan_.units[i]`), which would have
// silently executed the wrong table's DDL once a categorized tree started
// interleaving header rows among data rows. If anyone ever reintroduces
// position-keyed selection, shuffling the compare result will change the
// execution spec and that test goes red.
//
// Alongside it, TestKeysAreNotPositions() proves the same thing at COMPILE
// time via static_assert: the identity types physically refuse to be built
// from an integer, so an index cannot even be spelled at a call site.
//
// Also covered: the delete double-gate (master switch AND per-table check —
// deletes must be unrepresentable in the spec with the switch off), structural
// exclusion of non-executable categories, row-level selection being offered
// only for non-truncated diffs, select-all/invert over a visible subset, and
// Rebind's by-key state carry-over.
//
// Same dependency-free harness as SyncDiffModelTests.cpp / sync_test.cpp: an
// ExpectTrue/ExpectStr/ExpectEq assert loop, non-zero exit on any failure, no
// doctest/Catch2. Compiles SyncSelection.cpp directly and links only
// swiftsql::core (wxString + the src/ include root) — the model pulls in no db
// or GUI dependency at all, matching its "pure, non-wx" contract.
#include "ui/SyncSelection.h"

#include <cstdio>
#include <algorithm>
#include <type_traits>
#include <wx/string.h>

using namespace ui;

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
// Fixtures
// ---------------------------------------------------------------------------

static TableKey K(const wchar_t* name) { return TableKey(wxString(name)); }
static RowKey   R(const wchar_t* pk)   { return RowKey(wxString(pk)); }

struct DiffOpts {
    bool      hasStructure        = false;
    long long inserts             = 0;
    long long updates             = 0;
    long long deletes             = 0;
    bool      truncated           = false;
    bool      dataExecutable      = true;
    bool      structureExecutable = true;
};

static TableDiff MakeDiff(const wchar_t* name, const DiffOpts& o)
{
    TableDiff d;
    d.key                 = K(name);
    d.hasStructure        = o.hasStructure;
    d.inserts             = o.inserts;
    d.updates             = o.updates;
    d.deletes             = o.deletes;
    d.truncated           = o.truncated;
    d.dataExecutable      = o.dataExecutable;
    d.structureExecutable = o.structureExecutable;
    return d;
}

// A four-table compare result covering the interesting shapes:
//   orders    — structure + all three data categories, fully materialized
//   customers — data only, huge and TRUNCATED (no row-level selection)
//   invoices  — data only, NOT executable (blocking cross-engine findings)
//   products  — structure only, executable
static CompareResult MakeResult()
{
    CompareResult r;
    r.tables.push_back(MakeDiff(L"orders",    DiffOpts{ true, 10, 5, 3, false, true, true }));
    r.tables.push_back(MakeDiff(L"customers", DiffOpts{ false, 900000, 120, 7, true, true, true }));
    r.tables.push_back(MakeDiff(L"invoices",  DiffOpts{ false, 4, 2, 1, false, false, true }));
    r.tables.push_back(MakeDiff(L"products",  DiffOpts{ true, 0, 0, 0, false, true, true }));
    return r;
}

// Canonical, order-independent rendering of an execution spec. Used by the
// reorder test to compare two specs for exact equality.
static wxString Describe(const ExecutionSpec& spec)
{
    wxString s;
    for (const auto& t : spec.Tables()) {
        s += t.Table().Value() + L"{";
        for (ChangeCategory cat : AllCategories())
            if (t.Includes(cat)) s += CategoryName(cat) + L",";
        s += L"|";
        for (const auto& row : t.ExcludedRows()) s += row.Value() + L";";
        s += L"} ";
    }
    return s;
}

// ---------------------------------------------------------------------------
// 1. Identity types: positions cannot be spelled at all (compile-time proof)
// ---------------------------------------------------------------------------

static void TestKeysAreNotPositions()
{
    std::printf("[TestKeysAreNotPositions]\n");

    // THE structural guarantee. Every integral constructor of the identity
    // types is `= delete`d, so `sel.SetChecked(3, ...)` or `TableKey(i)` is a
    // hard compile error — a future engineer cannot reintroduce a row index as
    // an identity even by accident. These static_asserts fail the BUILD, not
    // the test run, if that protection is ever weakened.
    static_assert(!std::is_constructible_v<TableKey, int>,
                  "TableKey must never be constructible from an index");
    static_assert(!std::is_constructible_v<TableKey, unsigned>, "");
    static_assert(!std::is_constructible_v<TableKey, long>, "");
    static_assert(!std::is_constructible_v<TableKey, long long>, "");
    static_assert(!std::is_constructible_v<TableKey, std::size_t>,
                  "a container index must not be usable as a table identity");
    static_assert(!std::is_constructible_v<RowKey, std::size_t>,
                  "a grid row number must not be usable as a row identity");
    static_assert(!std::is_constructible_v<RowKey, int>, "");

    // Not implicitly convertible from a bare string either — an identity is
    // always constructed deliberately.
    static_assert(!std::is_convertible_v<wxString, TableKey>,
                  "TableKey(wxString) must stay explicit");

    // Tagged: a row identity can never stand in for a table identity.
    static_assert(!std::is_constructible_v<TableKey, RowKey>, "");
    static_assert(!std::is_constructible_v<RowKey, TableKey>, "");

    // Execution-spec types are unforgeable outside SyncSelection.
    static_assert(!std::is_default_constructible_v<ExecutionSpec>,
                  "only SyncSelection::Build() may produce an ExecutionSpec");
    static_assert(!std::is_constructible_v<TableExecSpec, TableKey>,
                  "only SyncSelection may produce a TableExecSpec");
    static_assert(!std::is_default_constructible_v<DeleteGate>,
                  "a DeleteGate must be unforgeable — only the master switch mints one");

    // And the ordinary value semantics the map relies on.
    ExpectTrue("equal names compare equal", K(L"orders") == K(L"orders"));
    ExpectTrue("different names compare unequal", K(L"orders") != K(L"products"));
    ExpectTrue("ordering is over the identity string", K(L"a") < K(L"b"));
    ExpectTrue("default-constructed key is empty", TableKey().IsEmpty());
    ExpectStr("Value() round-trips", K(L"orders").Value(), L"orders");
}

// ---------------------------------------------------------------------------
// 2. Seeded defaults
// ---------------------------------------------------------------------------

static void TestDefaultSeeding()
{
    std::printf("[TestDefaultSeeding]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());

    ExpectEq("all four tables adopted", (long long)sel.TableCount(), 4);

    ExpectTrue("orders structure checked by default",
               sel.IsChecked(K(L"orders"), ChangeCategory::Structure));
    ExpectTrue("orders inserts checked by default",
               sel.IsChecked(K(L"orders"), ChangeCategory::Inserts));
    ExpectTrue("orders updates checked by default",
               sel.IsChecked(K(L"orders"), ChangeCategory::Updates));
    ExpectTrue("orders DELETES off by default (product safety rule)",
               !sel.IsChecked(K(L"orders"), ChangeCategory::Deletes));

    ExpectTrue("master delete switch off by default", !sel.AllowDeletes());

    // A category with nothing in it is never pre-checked.
    ExpectTrue("products has no inserts -> not checked",
               !sel.IsChecked(K(L"products"), ChangeCategory::Inserts));
    ExpectTrue("products structure checked",
               sel.IsChecked(K(L"products"), ChangeCategory::Structure));

    // A blocked (non-executable) category is never pre-checked either — the UI
    // must not show a tick next to something that cannot run.
    ExpectTrue("invoices inserts blocked -> not checked",
               !sel.IsChecked(K(L"invoices"), ChangeCategory::Inserts));
    ExpectTrue("invoices updates blocked -> not checked",
               !sel.IsChecked(K(L"invoices"), ChangeCategory::Updates));

    // Keys come back sorted, and as KEYS — never as indices.
    const std::vector<TableKey> keys = sel.Keys();
    ExpectEq("Keys() returns every table", (long long)keys.size(), 4);
    ExpectStr("Keys()[0] sorted by identity", keys[0].Value(), L"customers");
    ExpectStr("Keys()[3] sorted by identity", keys[3].Value(), L"products");

    // Unknown identities are inert — no phantom selections get created.
    ExpectTrue("SetChecked on unknown table returns false",
               !sel.SetChecked(K(L"nope"), ChangeCategory::Inserts, true));
    ExpectTrue("unknown table stays unknown", sel.Find(K(L"nope")) == nullptr);
    ExpectEq("table count unchanged after unknown write", (long long)sel.TableCount(), 4);
}

// ---------------------------------------------------------------------------
// 3. The delete double-gate
// ---------------------------------------------------------------------------

static void TestDeleteDoubleGate()
{
    std::printf("[TestDeleteDoubleGate]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());

    // --- Gate 1 open, gate 2 shut: user checks deletes, master switch off ---
    ExpectTrue("check orders deletes", sel.SetChecked(K(L"orders"), ChangeCategory::Deletes, true));
    ExpectTrue("check-bit is set", sel.IsChecked(K(L"orders"), ChangeCategory::Deletes));
    ExpectTrue("but NOT active while master switch is off",
               !sel.IsActive(K(L"orders"), ChangeCategory::Deletes));

    ExecutionSpec spec = sel.Build();
    const TableExecSpec* orders = spec.Find(K(L"orders"));
    ExpectTrue("orders is in the spec (its other categories are selected)", orders != nullptr);
    if (orders) {
        ExpectTrue("spec MUST NOT contain deletes with master switch off", !orders->Deletes());
        ExpectTrue("non-delete categories unaffected", orders->Inserts() && orders->Updates());
    }
    ExpectTrue("ContainsDeletes() false across the whole spec", !spec.ContainsDeletes());
    ExpectEq("summary delete count is 0 with master switch off", sel.Summarize().deletes, 0);

    // No table anywhere may carry deletes while the switch is off, even if
    // every single delete box is checked.
    sel.SetCheckedForAll(sel.Keys(), ChangeCategory::Deletes, true);
    spec = sel.Build();
    bool anyDelete = false;
    for (const auto& t : spec.Tables()) if (t.Deletes()) anyDelete = true;
    ExpectTrue("ALL delete boxes checked + switch off => still zero deletes", !anyDelete);
    ExpectEq("summary still reports 0 deletes", sel.Summarize().deletes, 0);

    // --- Gate 2 open, gate 1 shut: master switch on, boxes unchecked ---
    sel.SetCheckedForAll(sel.Keys(), ChangeCategory::Deletes, false);
    sel.SetAllowDeletes(true);
    spec = sel.Build();
    ExpectTrue("master switch alone does NOT arm deletes", !spec.ContainsDeletes());
    ExpectTrue("orders deletes not active with box unchecked",
               !sel.IsActive(K(L"orders"), ChangeCategory::Deletes));

    // --- Both gates open ---
    sel.SetChecked(K(L"orders"), ChangeCategory::Deletes, true);
    ExpectTrue("both gates open => active", sel.IsActive(K(L"orders"), ChangeCategory::Deletes));
    spec   = sel.Build();
    orders = spec.Find(K(L"orders"));
    ExpectTrue("both gates open => deletes in spec", orders && orders->Deletes());
    ExpectTrue("ContainsDeletes() true", spec.ContainsDeletes());
    ExpectEq("summary counts the 3 deletes", sel.Summarize().deletes, 3);

    // --- Closing the master switch again retracts them ---
    sel.SetAllowDeletes(false);
    spec = sel.Build();
    ExpectTrue("closing the master switch retracts deletes from the spec",
               !spec.ContainsDeletes());
    ExpectTrue("the per-table check bit is REMEMBERED across the toggle",
               sel.IsChecked(K(L"orders"), ChangeCategory::Deletes));
    ExpectEq("summary delete count back to 0", sel.Summarize().deletes, 0);

    // A bulk "select all" sweep must never arm deletes on the user's behalf.
    SyncSelection sweep;
    sweep.Reset(MakeResult());
    sweep.SetAllTablesChecked(sweep.Keys(), true);
    sweep.SetAllowDeletes(true);
    ExpectTrue("SetAllTablesChecked does not touch the delete category",
               !sweep.IsChecked(K(L"orders"), ChangeCategory::Deletes));
    ExpectTrue("...so even with the master switch on, no deletes are planned",
               !sweep.Build().ContainsDeletes());
}

// ---------------------------------------------------------------------------
// 4. Non-executable categories are structurally excluded
// ---------------------------------------------------------------------------

static void TestNonExecutableExcluded()
{
    std::printf("[TestNonExecutableExcluded]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());
    sel.SetAllowDeletes(true);

    // Force-check everything on the blocked table, including deletes: a
    // determined (or buggy) caller must still not be able to plan it.
    for (ChangeCategory cat : AllCategories())
        sel.SetChecked(K(L"invoices"), cat, true);

    ExpectTrue("invoices inserts not active (blocking findings)",
               !sel.IsActive(K(L"invoices"), ChangeCategory::Inserts));
    ExpectTrue("invoices updates not active", !sel.IsActive(K(L"invoices"), ChangeCategory::Updates));
    ExpectTrue("invoices deletes not active", !sel.IsActive(K(L"invoices"), ChangeCategory::Deletes));

    const ExecutionSpec spec = sel.Build();
    ExpectTrue("a fully-blocked table is absent from the spec entirely",
               spec.Find(K(L"invoices")) == nullptr);

    // Structure and data are blocked independently: a table whose structure is
    // blocked can still have its executable data categories planned.
    CompareResult mixed;
    mixed.tables.push_back(
        MakeDiff(L"mixed", DiffOpts{ true, 6, 0, 0, false, /*dataExec*/ true, /*structExec*/ false }));
    SyncSelection ms;
    ms.Reset(mixed);
    ExpectTrue("blocked structure is not pre-checked",
               !ms.IsChecked(K(L"mixed"), ChangeCategory::Structure));
    ms.SetChecked(K(L"mixed"), ChangeCategory::Structure, true);   // force it anyway

    const TableExecSpec* m = ms.Build().Find(K(L"mixed"));
    ExpectTrue("mixed table present for its executable data half", m != nullptr);
    if (m) {
        ExpectTrue("blocked structure excluded even when force-checked", !m->Structure());
        ExpectTrue("executable inserts still included", m->Inserts());
    }

    // A category with zero changes is never emitted, checked or not.
    SyncSelection empt;
    empt.Reset(MakeResult());
    empt.SetChecked(K(L"products"), ChangeCategory::Inserts, true);
    const TableExecSpec* p = empt.Build().Find(K(L"products"));
    ExpectTrue("products present for its structure change", p != nullptr);
    if (p) ExpectTrue("an empty category is not emitted even when checked", !p->Inserts());

    // A table with nothing selected at all drops out of the spec completely.
    SyncSelection none;
    none.Reset(MakeResult());
    none.SetAllTablesChecked(none.Keys(), false);
    ExpectTrue("clearing every box yields an empty spec", none.Build().Empty());
    ExpectEq("empty spec has no tables", (long long)none.Build().TableCount(), 0);
}

// ---------------------------------------------------------------------------
// 5. Row-level selection only when the diff was fully materialized
// ---------------------------------------------------------------------------

static void TestRowLevelSelectionGating()
{
    std::printf("[TestRowLevelSelectionGating]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());

    // orders is fully materialized -> row-level selection is offered.
    ExpectTrue("row selection available for a non-truncated diff",
               sel.RowSelectionAvailable(K(L"orders")));
    ExpectTrue("excluding a row succeeds",
               sel.SetRowExcluded(K(L"orders"), R(L"pk=1001"), true));
    ExpectTrue("excluding a second row succeeds",
               sel.SetRowExcluded(K(L"orders"), R(L"pk=1002"), true));
    ExpectTrue("the row reads back as excluded",
               sel.IsRowExcluded(K(L"orders"), R(L"pk=1001")));
    ExpectEq("two rows excluded", (long long)sel.ExcludedRows(K(L"orders")).size(), 2);

    const TableExecSpec* orders = sel.Build().Find(K(L"orders"));
    ExpectTrue("spec carries the row exclusions", orders != nullptr);
    if (orders) ExpectEq("spec exclusion count", (long long)orders->ExcludedRows().size(), 2);
    ExpectEq("summary reports excluded rows", (long long)sel.Summarize().excludedRows, 2);

    // Re-including works.
    ExpectTrue("re-including a row succeeds",
               sel.SetRowExcluded(K(L"orders"), R(L"pk=1001"), false));
    ExpectTrue("row no longer excluded", !sel.IsRowExcluded(K(L"orders"), R(L"pk=1001")));
    sel.ClearRowExclusions(K(L"orders"));
    ExpectEq("ClearRowExclusions empties the set",
             (long long)sel.ExcludedRows(K(L"orders")).size(), 0);

    // customers is TRUNCATED -> per-row selection is refused outright, because
    // pretending the user hand-picked from 900k rows would be a lie.
    ExpectTrue("row selection NOT available for a truncated diff",
               !sel.RowSelectionAvailable(K(L"customers")));
    ExpectTrue("SetRowExcluded refused on a truncated diff",
               !sel.SetRowExcluded(K(L"customers"), R(L"pk=7"), true));
    ExpectEq("nothing was recorded for the truncated table",
             (long long)sel.ExcludedRows(K(L"customers")).size(), 0);

    const TableExecSpec* cust = sel.Build().Find(K(L"customers"));
    ExpectTrue("truncated table still plans its whole categories", cust != nullptr);
    if (cust) {
        ExpectTrue("truncated table's inserts included", cust->Inserts());
        ExpectEq("truncated table carries no row exclusions",
                 (long long)cust->ExcludedRows().size(), 0);
    }

    // Unknown table: inert.
    ExpectTrue("row selection unavailable for unknown table",
               !sel.RowSelectionAvailable(K(L"nope")));
    ExpectTrue("SetRowExcluded on unknown table returns false",
               !sel.SetRowExcluded(K(L"nope"), R(L"pk=1"), true));
    ExpectTrue("an empty row identity is refused",
               !sel.SetRowExcluded(K(L"orders"), RowKey(), true));
}

// ---------------------------------------------------------------------------
// 6. Select-all / invert over the visible set
// ---------------------------------------------------------------------------

static void TestBulkOperations()
{
    std::printf("[TestBulkOperations]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());

    // Bulk ops act on the caller-supplied VISIBLE key list only — a filtered
    // grid must not silently mutate rows the user cannot see.
    const std::vector<TableKey> visible = { K(L"orders"), K(L"products") };

    sel.SetCheckedForAll(visible, ChangeCategory::Structure, false);
    ExpectTrue("orders structure cleared", !sel.IsChecked(K(L"orders"), ChangeCategory::Structure));
    ExpectTrue("products structure cleared",
               !sel.IsChecked(K(L"products"), ChangeCategory::Structure));
    ExpectTrue("customers (not visible) untouched",
               sel.IsChecked(K(L"customers"), ChangeCategory::Inserts));

    sel.SetCheckedForAll(visible, ChangeCategory::Structure, true);
    ExpectTrue("orders structure restored", sel.IsChecked(K(L"orders"), ChangeCategory::Structure));

    // Invert flips only the visible set, and only the named category.
    sel.SetChecked(K(L"products"), ChangeCategory::Structure, false);
    sel.InvertChecked(visible, ChangeCategory::Structure);
    ExpectTrue("invert cleared orders structure",
               !sel.IsChecked(K(L"orders"), ChangeCategory::Structure));
    ExpectTrue("invert set products structure",
               sel.IsChecked(K(L"products"), ChangeCategory::Structure));
    ExpectTrue("invert did not touch the inserts category",
               sel.IsChecked(K(L"orders"), ChangeCategory::Inserts));

    // Invert on the delete category flips check bits but stays gated.
    sel.InvertChecked(visible, ChangeCategory::Deletes);
    ExpectTrue("invert set the orders delete check-bit",
               sel.IsChecked(K(L"orders"), ChangeCategory::Deletes));
    ExpectTrue("but deletes remain inactive (master switch still off)",
               !sel.IsActive(K(L"orders"), ChangeCategory::Deletes));
    ExpectTrue("and remain absent from the spec", !sel.Build().ContainsDeletes());

    // Bulk ops silently skip identities that are not in the result.
    sel.SetCheckedForAll({ K(L"ghost") }, ChangeCategory::Inserts, true);
    ExpectEq("no phantom table was created", (long long)sel.TableCount(), 4);

    // Whole-table sweep.
    SyncSelection sweep;
    sweep.Reset(MakeResult());
    sweep.SetAllTablesChecked(sweep.Keys(), false);
    ExpectTrue("sweep off clears structure", !sweep.IsChecked(K(L"orders"), ChangeCategory::Structure));
    ExpectTrue("sweep off clears inserts", !sweep.IsChecked(K(L"orders"), ChangeCategory::Inserts));
    ExpectTrue("sweep off clears updates", !sweep.IsChecked(K(L"orders"), ChangeCategory::Updates));
    sweep.SetTableChecked(K(L"orders"), true);
    ExpectTrue("SetTableChecked restores structure",
               sweep.IsChecked(K(L"orders"), ChangeCategory::Structure));
    ExpectTrue("SetTableChecked leaves deletes off",
               !sweep.IsChecked(K(L"orders"), ChangeCategory::Deletes));
}

// ---------------------------------------------------------------------------
// 7. THE regression test: selection survives a reordering of the collection
// ---------------------------------------------------------------------------
//
// Round 1 keyed selection on the row index into the underlying collection. Had
// the collection ever been reordered (sorting the grid, or a categorized tree
// interleaving header rows), the checked boxes would have pointed at DIFFERENT
// TABLES and the wrong table's DDL would have executed silently.
//
// Here we make a deliberately asymmetric selection, then re-adopt the SAME
// tables in a different order and assert the resulting execution spec is
// byte-identical. Any position-keyed shortcut fails this.
static void TestReorderInvariance()
{
    std::printf("[TestReorderInvariance]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());
    sel.SetAllowDeletes(true);

    // An asymmetric selection: every table differs from its neighbours, so any
    // off-by-one or reversed mapping produces a visibly different spec.
    sel.SetAllTablesChecked(sel.Keys(), false);
    sel.SetChecked(K(L"orders"),    ChangeCategory::Structure, true);
    sel.SetChecked(K(L"orders"),    ChangeCategory::Deletes,   true);
    sel.SetChecked(K(L"customers"), ChangeCategory::Inserts,   true);
    sel.SetChecked(K(L"products"),  ChangeCategory::Structure, true);
    sel.SetRowExcluded(K(L"orders"), R(L"pk=1001"), true);
    sel.SetRowExcluded(K(L"orders"), R(L"pk=1002"), true);

    const wxString before = Describe(sel.Build());
    ExpectStr("baseline spec is exactly the asymmetric selection made above",
              before,
              L"customers{inserts,|} orders{structure,deletes,|pk=1001;pk=1002;} "
              L"products{structure,|} ");

    // Now re-adopt the very same tables in REVERSE order — the shape a grid
    // re-sort, a category regrouping, or a re-compare would produce.
    CompareResult reversed = MakeResult();
    std::reverse(reversed.tables.begin(), reversed.tables.end());
    sel.Rebind(reversed);

    const wxString afterReverse = Describe(sel.Build());
    ExpectStr("REGRESSION: reversing the compare result changes nothing",
              afterReverse, before);

    // And a rotation, for good measure (reversal alone could be survived by a
    // symmetric bug; rotation could not).
    CompareResult rotated = MakeResult();
    std::rotate(rotated.tables.begin(), rotated.tables.begin() + 3, rotated.tables.end());
    sel.Rebind(rotated);
    ExpectStr("REGRESSION: rotating the compare result changes nothing",
              Describe(sel.Build()), before);

    // Every permutation of the four tables must yield the identical spec.
    // next_permutation enumerates all n! only when seeded with the ASCENDING
    // sequence, so sort ascending first.
    CompareResult perm = MakeResult();
    std::sort(perm.tables.begin(), perm.tables.end(),
              [](const TableDiff& a, const TableDiff& b) { return a.key < b.key; });
    bool allPermutationsStable = true;
    int  permutations          = 0;
    do {
        SyncSelection s;
        s.Reset(perm);
        s.SetAllowDeletes(true);
        s.SetAllTablesChecked(s.Keys(), false);
        s.SetChecked(K(L"orders"),    ChangeCategory::Structure, true);
        s.SetChecked(K(L"orders"),    ChangeCategory::Deletes,   true);
        s.SetChecked(K(L"customers"), ChangeCategory::Inserts,   true);
        s.SetChecked(K(L"products"),  ChangeCategory::Structure, true);
        s.SetRowExcluded(K(L"orders"), R(L"pk=1001"), true);
        s.SetRowExcluded(K(L"orders"), R(L"pk=1002"), true);
        if (Describe(s.Build()) != before) allPermutationsStable = false;
        ++permutations;
    } while (std::next_permutation(
        perm.tables.begin(), perm.tables.end(),
        [](const TableDiff& a, const TableDiff& b) { return a.key < b.key; }));

    ExpectEq("all 24 permutations of 4 tables were exercised", permutations, 24);
    ExpectTrue("REGRESSION: the spec is identical under EVERY permutation",
               allPermutationsStable);

    // The identity-keyed lookup itself must not care about order either.
    ExpectTrue("Find() still resolves orders after reordering",
               sel.Find(K(L"orders")) != nullptr);
    if (const TableDiff* d = sel.Find(K(L"orders")))
        ExpectEq("...and returns ORDERS' own stats, not a neighbour's", d->inserts, 10);
}

// ---------------------------------------------------------------------------
// 8. Rebind: state carries over by key, prunes, re-seeds
// ---------------------------------------------------------------------------

static void TestRebindSemantics()
{
    std::printf("[TestRebindSemantics]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());
    sel.SetAllTablesChecked(sel.Keys(), false);
    sel.SetChecked(K(L"orders"), ChangeCategory::Inserts, true);
    sel.SetRowExcluded(K(L"orders"), R(L"pk=1"), true);

    // A re-compare where `products` vanished and `shipments` appeared.
    CompareResult next;
    next.tables.push_back(MakeDiff(L"shipments", DiffOpts{ true, 2, 0, 0, false, true, true }));
    next.tables.push_back(MakeDiff(L"orders",    DiffOpts{ true, 10, 5, 3, false, true, true }));
    next.tables.push_back(MakeDiff(L"customers", DiffOpts{ false, 900000, 120, 7, true, true, true }));
    sel.Rebind(next);

    ExpectEq("table count follows the new result", (long long)sel.TableCount(), 3);
    ExpectTrue("vanished table pruned", sel.Find(K(L"products")) == nullptr);
    ExpectTrue("surviving table keeps its user selection",
               sel.IsChecked(K(L"orders"), ChangeCategory::Inserts));
    ExpectTrue("surviving table keeps its cleared boxes too",
               !sel.IsChecked(K(L"orders"), ChangeCategory::Structure));
    ExpectTrue("surviving table keeps its row exclusions",
               sel.IsRowExcluded(K(L"orders"), R(L"pk=1")));
    ExpectTrue("newly appeared table gets seeded defaults",
               sel.IsChecked(K(L"shipments"), ChangeCategory::Structure));
    ExpectTrue("newly appeared table's deletes still default off",
               !sel.IsChecked(K(L"shipments"), ChangeCategory::Deletes));

    // A category that became non-executable must not stay checked.
    CompareResult blocked;
    blocked.tables.push_back(
        MakeDiff(L"orders", DiffOpts{ true, 10, 5, 3, false, /*dataExec*/ false, true }));
    sel.Rebind(blocked);
    ExpectTrue("a now-blocked category is unchecked on rebind",
               !sel.IsChecked(K(L"orders"), ChangeCategory::Inserts));
    ExpectTrue("orders drops out of the spec entirely",
               sel.Build().Find(K(L"orders")) == nullptr);

    // A diff that turns truncated drops its stale row-level decisions.
    SyncSelection trunc;
    CompareResult small;
    small.tables.push_back(MakeDiff(L"t", DiffOpts{ false, 5, 0, 0, false, true, true }));
    trunc.Reset(small);
    trunc.SetRowExcluded(K(L"t"), R(L"pk=9"), true);
    ExpectTrue("row excluded while small", trunc.IsRowExcluded(K(L"t"), R(L"pk=9")));

    CompareResult big;
    big.tables.push_back(MakeDiff(L"t", DiffOpts{ false, 500000, 0, 0, true, true, true }));
    trunc.Rebind(big);
    ExpectTrue("row exclusions dropped once the diff became truncated",
               !trunc.IsRowExcluded(K(L"t"), R(L"pk=9")));
    ExpectTrue("row selection no longer offered", !trunc.RowSelectionAvailable(K(L"t")));

    // A table with no usable identity is not selectable at all.
    CompareResult anon;
    anon.tables.push_back(MakeDiff(L"", DiffOpts{ true, 1, 0, 0, false, true, true }));
    SyncSelection as;
    as.Reset(anon);
    ExpectEq("an identity-less table is refused", (long long)as.TableCount(), 0);
}

// ---------------------------------------------------------------------------
// 9. Summary counters agree with the spec
// ---------------------------------------------------------------------------

static void TestSummary()
{
    std::printf("[TestSummary]\n");

    SyncSelection sel;
    sel.Reset(MakeResult());

    SelectionSummary sum = sel.Summarize();
    // Defaults: orders (struct+ins+upd), customers (ins+upd), products (struct).
    // invoices is fully blocked and absent.
    ExpectEq("3 tables selected by default", (long long)sum.tables, 3);
    ExpectEq("2 tables carry structure", (long long)sum.structureTables, 2);
    ExpectEq("inserts summed across selected tables", sum.inserts, 10 + 900000);
    ExpectEq("updates summed across selected tables", sum.updates, 5 + 120);
    ExpectEq("deletes are 0 (default off + master switch off)", sum.deletes, 0);

    sel.SetAllowDeletes(true);
    sel.SetChecked(K(L"orders"), ChangeCategory::Deletes, true);
    sel.SetChecked(K(L"customers"), ChangeCategory::Deletes, true);
    sum = sel.Summarize();
    ExpectEq("deletes summed once both gates are open", sum.deletes, 3 + 7);

    sel.SetAllowDeletes(false);
    ExpectEq("deletes drop back to 0 the instant the master switch closes",
             sel.Summarize().deletes, 0);

    SyncSelection empty;
    sum = empty.Summarize();
    ExpectEq("empty model summarizes to zero tables", (long long)sum.tables, 0);
    ExpectEq("empty model summarizes to zero inserts", sum.inserts, 0);
    ExpectTrue("empty model builds an empty spec", empty.Build().Empty());
}

// ---------------------------------------------------------------------------

int main()
{
    TestKeysAreNotPositions();
    TestDefaultSeeding();
    TestDeleteDoubleGate();
    TestNonExecutableExcluded();
    TestRowLevelSelectionGating();
    TestBulkOperations();
    TestReorderInvariance();
    TestRebindSemantics();
    TestSummary();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
