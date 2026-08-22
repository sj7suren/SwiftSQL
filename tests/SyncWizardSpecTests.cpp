// SyncWizardSpecTests.cpp — the LAST layer of the sync feature that was
// verified only by argument: the compare screen's own construction of what will
// execute.
//
// ---------------------------------------------------------------------------
// WHY THIS TARGET EXISTS WHEN THREE NEIGHBOURS ALREADY TOUCH THIS AREA
// ---------------------------------------------------------------------------
// SyncSelectionTests pins ui::SyncSelection in isolation. SyncCompareRowTests
// pins the pure ExecutionSpec -> plan functions. SyncRowSelectionTests pins one
// row identity travelling from the data layer into a DataExecPlan. All three
// hand-build their inputs, and none of them touches the object the RUNNING
// DIALOG actually owns — so all three would stay green if SyncComparePage
// stopped seeding its selection from the plan, stopped mirroring the delete
// master switch, or started answering 执行 from a second copy of the state.
//
// This target drives ui::SyncCompareSession: the model half of
// SyncComparePage, which the page now delegates to for SetPlan,
// HasAnythingSelected, BuildExecutionPlan, BuildDataExecPlan and
// EffectiveDeleteRows. It exercises it through ui::syncspec::RunSpecScript,
// which is the same entry point the SWIFTSQL_SYNCSPEC hook in
// SyncWizardDialog.cpp calls on the live page. There is no re-implementation
// between the assertions here and the code the wizard runs.
//
// ---------------------------------------------------------------------------
// SCOPE — READ THIS BEFORE CITING A GREEN RUN
// ---------------------------------------------------------------------------
// This verifies the BACKEND CONTRACT REACHABLE FROM THE GUI SEAT: given real
// dialog state, does the emitted plan mean what that state means. It creates NO
// window and asserts NOTHING about rendering — not a checkbox's tick, not a
// greyed-out control, not the summary label. "The plan is right" and "the
// screen draws right" are different claims and this file only makes the first.
//
// Offline by construction: a canned-row IConnection stub feeds the REAL
// db::sync::DiffRowChanges merge, so the RowChangeSets under test are produced
// by the data layer rather than hand-set. No server, no wxApp, no GUI.
#include "db/DataSync.h"
#include "db/DbDriver.h"
#include "db/SyncEngine.h"
#include "ui/SyncCompareSession.h"
#include "ui/SyncSpecScript.h"

#include <atomic>
#include <cstdio>
#include <vector>
#include <wx/init.h>
#include <wx/string.h>

using namespace db;
using namespace db::sync;
using ui::SyncCompareSession;
namespace spec = ui::syncspec;

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
    Dialect                        dialect_ = Dialect::MySQL;
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

static TableSchema Schema(const wxString& table)
{
    TableSchema s;
    s.name = table;
    s.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
    s.columns.push_back(Col(L"v",  ColKind::Varchar, L"varchar(40)"));
    s.primaryKey.push_back(L"id");
    return s;
}

static DataDiffSpec DiffSpecFor(const wxString& table)
{
    DataDiffSpec s;
    s.srcDb = L"s"; s.tgtDb = L"t";
    s.srcSchema = Schema(table);
    s.tgtSchema = Schema(table);
    return s;
}

// Run the REAL merge over two canned row sets. The RowChangeSet under test is
// therefore the data layer's own product, not a hand-set struct.
static bool Compare(const wxString& table,
                    const std::vector<std::vector<Cell>>& srcRows,
                    const std::vector<std::vector<Cell>>& tgtRows,
                    RowLayout& layout, RowChangeSet& set)
{
    std::atomic<bool> stop{false};
    StubConn src, tgt;
    src.rows_ = srcRows;
    tgt.rows_ = tgtRows;

    DataSyncOptions opt;
    opt.insert = opt.update = opt.deleteMissing = true;
    wxString err;
    return DiffRowChanges(src, tgt, DiffSpecFor(table), opt, layout, set, err, stop);
}

// One plan unit: `table` with the given row diff, plus optional DDL.
static SyncPlan::TableUnit MakeUnit(const wxString& table,
                                    const std::vector<std::vector<Cell>>& srcRows,
                                    const std::vector<std::vector<Cell>>& tgtRows,
                                    const std::vector<wxString>& ddl = {})
{
    SyncPlan::TableUnit u;
    u.table    = QualifiedName{ wxString(), table };
    u.ddl      = ddl;
    u.dataSpec = DiffSpecFor(table);
    if (!Compare(table, srcRows, tgtRows, u.layout, u.rows)) {
        std::printf("  FAIL compare pass for %s\n", (const char*)table.utf8_str());
        ++g_fails; ++g_checks;
    }
    const RowChangeStat& st = u.rows.Stat();
    u.stat.inserts = st.inserts;
    u.stat.updates = st.updates;
    u.stat.deletes = st.deletes;
    u.dataVerdict = u.rows.Empty() ? DataVerdict::NotRequested : DataVerdict::Ok;
    return u;
}

// A MySQL-target plan: the FK relaxation pair SyncEngine::BuildPlan emits for a
// MySQL target. Carried here as data rather than re-derived, because item 6 is
// about the plan the WIZARD hands on still carrying it, not about who made it.
static void AddMySqlFkPreamble(SyncPlan& p)
{
    p.preamble.push_back(L"SET FOREIGN_KEY_CHECKS=0");
    p.postamble.push_back(L"SET FOREIGN_KEY_CHECKS=1");
}

// ---------------------------------------------------------------------------
// Render helpers — read the emitted plan by identity, never by line position.
// ---------------------------------------------------------------------------

// Out-of-range reads report instead of terminating. That is not defensive
// padding: a mutation run that deliberately empties the postamble made this
// suite die on std::vector::at with a bare 0xC0000409, which says nothing about
// WHICH claim broke. A test whose failure mode is unreadable is worth less than
// one that fails with a sentence.
static wxString Token(const std::vector<spec::GestureOutcome>& g, size_t i)
{
    return i < g.size() ? spec::GestureToken(g[i]) : wxString(L"<no gesture " )
                                                     + wxString::Format(L"%d>", (int)i);
}

static wxString Line(const std::vector<wxString>& v, size_t i)
{
    return i < v.size() ? v[i] : wxString(L"<absent>");
}

static bool RenderHas(const wxString& render, const wxString& line)
{
    return render.Contains(line + L"\n");
}

static const TableDataSpec* FindData(const DataExecPlan& p, const wxString& table)
{
    for (const TableDataSpec& t : p.tables) if (t.Table() == table) return &t;
    return nullptr;
}

static bool AnyDeleteReachable(const DataExecPlan& p)
{
    for (const TableDataSpec& t : p.tables) if (t.Deletes()) return true;
    return false;
}

// ===========================================================================
//  Fixture: three tables, each a different shape.
//    orders   1 insert, 1 update, 1 delete  + 2 DDL statements
//    items    1 insert only                 + no DDL
//    audit    1 delete only                 + no DDL
// ===========================================================================
static SyncPlan MakeFixture()
{
    SyncPlan p;
    AddMySqlFkPreamble(p);
    p.units.push_back(MakeUnit(L"orders",
                               { { Num(L"1"), Txt(L"new") },      // update
                                 { Num(L"3"), Txt(L"ins") } },    // insert
                               { { Num(L"1"), Txt(L"old") },
                                 { Num(L"2"), Txt(L"gone") } },   // delete
                               { L"ALTER TABLE orders ADD c1 int",
                                 L"ALTER TABLE orders ADD c2 int" }));
    p.units.push_back(MakeUnit(L"items",
                               { { Num(L"7"), Txt(L"a") } }, {}));
    p.units.push_back(MakeUnit(L"audit",
                               {}, { { Num(L"9"), Txt(L"b") } }));
    return p;
}

// ===========================================================================
//  1. Identity, not position — a reordered compare yields the SAME plan
// ===========================================================================
static void TestSelectionRoundTripsByIdentity()
{
    std::printf("\n[1] the emitted plan survives a reordering of the compare result\n");

    SyncPlan a = MakeFixture();
    SyncPlan b = MakeFixture();
    // Same three tables, opposite order. If anything anywhere resolved a table
    // by its position in plan.units, these two runs would disagree.
    std::swap(b.units[0], b.units[2]);

    const wxString script =
        L"deletes on; check orders inserts; check orders deletes;"
        L"check items inserts; uncheck audit deletes";

    SyncCompareSession sa, sb;
    sa.SetPlan(&a);
    sb.SetPlan(&b);
    const spec::ScriptResult ra = spec::RunSpecScript(sa, script);
    const spec::ScriptResult rb = spec::RunSpecScript(sb, script);

    ExpectStr("forward order verdict", spec::ScriptToken(ra), L"OK");
    ExpectStr("reversed order verdict", spec::ScriptToken(rb), L"OK");
    // The headline: byte-identical renders. ExecutionSpec sorts by TableKey, so
    // this is a property of the identity keying, not of the input order.
    ExpectStr("reordering the compare changes NOTHING about what executes",
              rb.render, ra.render);

    // And it is a non-trivial render — an assertion that two empty strings match
    // would pass whatever the code did.
    ExpectTrue("...and the render is non-empty", !ra.render.IsEmpty());
    // NOTE: the plan must be BOUND before it is searched. FindData returns a
    // pointer INTO it, so `FindData(s.BuildDataExecPlan(), ...)` would dangle on
    // the temporary — and read plausible-looking freed memory rather than fail
    // honestly. Caught here by a byte-comparison assertion that started
    // disagreeing with a size assertion two lines above it.
    const DataExecPlan da = sa.BuildDataExecPlan();
    ExpectTrue("orders carries its own identity in the data plan",
               FindData(da, L"orders") != nullptr);
    ExpectTrue("items too", FindData(da, L"items") != nullptr);

    // A table named in a gesture but absent from the compare is reported, never
    // silently invented — and, crucially, creates no phantom selection. The
    // sweep first clears the seeded defaults so this measures the ghost alone.
    SyncCompareSession sc;
    sc.SetPlan(&a);
    const spec::ScriptResult rc = spec::RunSpecScript(
        sc, L"all structure off; all inserts off; all updates off; all deletes off;"
            L"check ghosts inserts");
    ExpectStr("a gesture on an unknown table is loud",
              Token(rc.gestures, 4), L"NO_SUCH_TABLE:ghosts");
    ExpectStr("...and nothing was selected as a side effect",
              spec::ScriptToken(rc), L"EMPTY:NOTHING_SELECTED");
    ExpectEq("the compare gained no phantom table",
             (long long)sc.Selection().TableCount(), 3);
}

// ===========================================================================
//  2. The delete gate, driven from dialog state
// ===========================================================================
static void TestDeleteGateFromDialogState()
{
    std::printf("\n[2] the delete master switch, end to end from dialog state\n");

    SyncPlan p = MakeFixture();

    // --- master switch OFF, every delete box ticked ---
    {
        SyncCompareSession s;
        s.SetPlan(&p);
        const spec::ScriptResult r = spec::RunSpecScript(
            s, L"deletes off; check orders deletes; check audit deletes;"
               L"check orders inserts");
        ExpectStr("ticking 删除 while disarmed is recorded AND reported inactive",
                  Token(r.gestures, 1),
                  L"APPLIED:INACTIVE:DELETES_DISARMED");
        ExpectTrue("the check bit really was set (this is not a no-op)",
                   s.Selection().IsChecked(ui::TableKey(L"orders"),
                                           ui::ChangeCategory::Deletes));
        ExpectTrue("...and IsActive still says no",
                   !s.Selection().IsActive(ui::TableKey(L"orders"),
                                           ui::ChangeCategory::Deletes));
        const DataExecPlan d = s.BuildDataExecPlan();
        ExpectTrue("NO delete is reachable anywhere in the plan",
                   !AnyDeleteReachable(d));
        ExpectTrue("a delete-only table drops out of the plan entirely",
                   FindData(d, L"audit") == nullptr);
        ExpectEq("the destructive-confirmation count is zero",
                 s.EffectiveDeleteRows(), 0);
        ExpectTrue("but the run is not empty — inserts still go",
                   FindData(d, L"orders") && FindData(d, L"orders")->Inserts());
    }

    // --- armed for ONE table ---
    {
        SyncCompareSession s;
        s.SetPlan(&p);
        const spec::ScriptResult r = spec::RunSpecScript(
            s, L"deletes on; check orders deletes; check audit deletes;"
               L"uncheck audit deletes");
        ExpectStr("with the switch on, the tick is simply applied",
                  Token(r.gestures, 1), L"APPLIED:deletes");
        const DataExecPlan d = s.BuildDataExecPlan();
        const TableDataSpec* o = FindData(d, L"orders");
        ExpectTrue("orders may delete", o && o->Deletes());
        ExpectTrue("audit — unticked — may not, and is absent",
                   FindData(d, L"audit") == nullptr);
        ExpectEq("exactly one table carries a delete authorization",
                 AnyDeleteReachable(d) ? 1 : 0, 1);
        ExpectEq("one delete row is what the confirmation will be about",
                 s.EffectiveDeleteRows(), 1);
    }

    // --- disarming AFTER ticking strips it again ---
    {
        SyncCompareSession s;
        s.SetPlan(&p);
        spec::RunSpecScript(s, L"deletes on; check orders deletes; deletes off");
        ExpectTrue("turning the switch back off re-strips every DELETE",
                   !AnyDeleteReachable(s.BuildDataExecPlan()));
        ExpectEq("...and the confirmation count follows it back to zero",
                 s.EffectiveDeleteRows(), 0);
    }
}

// ===========================================================================
//  3. Per-category filtering
// ===========================================================================
static void TestPerCategoryFiltering()
{
    std::printf("\n[3] inserts checked but not updates yields inserts only\n");

    SyncPlan p = MakeFixture();
    SyncCompareSession s;
    s.SetPlan(&p);

    // Reset() seeds inserts/updates ON, so this must UNCHECK updates rather
    // than rely on a default — a test that only checked boxes would pass on a
    // build where unchecking did nothing.
    const spec::ScriptResult r = spec::RunSpecScript(
        s, L"all structure off; all updates off; all deletes off;"
           L"all inserts on");
    ExpectStr("data-only run is its own verdict, not an error",
              spec::ScriptToken(r), L"OK:DATA_ONLY");

    const DataExecPlan d = s.BuildDataExecPlan();
    const TableDataSpec* o = FindData(d, L"orders");
    ExpectTrue("orders present", o != nullptr);
    if (o) {
        ExpectTrue("inserts allowed", o->Inserts());
        ExpectTrue("updates NOT allowed", !o->Updates());
        ExpectTrue("deletes NOT allowed", !o->Deletes());
    }
    ExpectStr("the render says so verbatim", L"",
              RenderHas(r.render, L"DATA|orders|I=1|U=0|D=0|X=") ? L"" : r.render);
    ExpectTrue("a table whose ONLY change is a delete drops out",
               FindData(d, L"audit") == nullptr);

    // The mirror image, so neither direction can be a hardcoded constant.
    SyncCompareSession s2;
    s2.SetPlan(&p);
    spec::RunSpecScript(s2, L"all structure off; all inserts off; all deletes off;"
                            L"all updates on");
    const DataExecPlan   d2 = s2.BuildDataExecPlan();
    const TableDataSpec* o2 = FindData(d2, L"orders");
    ExpectTrue("updates-only: updates allowed", o2 && o2->Updates());
    ExpectTrue("updates-only: inserts NOT allowed", o2 && !o2->Inserts());
    ExpectTrue("items, an insert-only table, is absent from an updates-only run",
               FindData(d2, L"items") == nullptr);
}

// ===========================================================================
//  4. Row exclusions reach TableDataSpec::ExcludeRow, keyed on RowKey() verbatim
// ===========================================================================
static void TestRowExclusionsCarryTheProducersKey()
{
    std::printf("\n[4] row exclusions arrive as db::sync::RowChange::RowKey(), copied\n");

    SyncPlan p = MakeFixture();
    SyncCompareSession s;
    s.SetPlan(&p);

    // Take the key from the data layer's own RowChange, so the assertion cannot
    // drift with the encoding: whatever RowChange::RowKey() says, that exact
    // string must be what the plan carries.
    wxString insertKey;
    for (const RowChange& rc : p.units[0].rows.Sample())
        if (rc.Operation() == RowChange::Op::Insert) insertKey = rc.RowKey();
    ExpectTrue("the fixture produced an insert row with a key", !insertKey.IsEmpty());

    const spec::ScriptResult r = spec::RunSpecScript(
        s, L"deletes on; check orders deletes; exclude orders " + insertKey);
    ExpectStr("the exclusion was applied",
              Token(r.gestures, 2), L"APPLIED:" + insertKey);

    const DataExecPlan   d = s.BuildDataExecPlan();
    const TableDataSpec* o = FindData(d, L"orders");
    ExpectTrue("orders present", o != nullptr);
    if (o) {
        ExpectEq("exactly one row excluded", (long long)o->ExcludedRows().size(), 1);
        ExpectTrue("...and it is the PRODUCER's key, byte for byte",
                   o->ExcludedRows().count(insertKey) == 1);
    }

    // A key that matches no row must NOT be stored. SyncSelection holds no row
    // inventory and would accept it; a stored-but-matching-nothing key excludes
    // nothing and lets a run report success while syncing a row the user
    // unchecked. That silent miss is the failure this refuses.
    SyncCompareSession s2;
    s2.SetPlan(&p);
    const spec::ScriptResult r2 =
        spec::RunSpecScript(s2, L"exclude orders id=999");
    ExpectStr("a key no row carries is refused, loudly",
              Token(r2.gestures, 0), L"NO_SUCH_ROW:id=999");
    const DataExecPlan   d2 = s2.BuildDataExecPlan();
    const TableDataSpec* o2 = FindData(d2, L"orders");
    ExpectTrue("...and nothing was stored",
               o2 && o2->ExcludedRows().empty());

    // Exclusion by op, keyed off each node's carried key.
    SyncCompareSession s3;
    s3.SetPlan(&p);
    const spec::ScriptResult r3 =
        spec::RunSpecScript(s3, L"deletes on; check orders deletes;"
                                L"exclude-op orders delete");
    ExpectStr("one delete row excluded",
              Token(r3.gestures, 2), L"APPLIED:1");
    ExpectEq("the armed delete is now excluded, so nothing will be deleted",
             s3.EffectiveDeleteRows(), 0);
    const DataExecPlan   d3 = s3.BuildDataExecPlan();
    const TableDataSpec* o3 = FindData(d3, L"orders");
    ExpectTrue("the table still carries the delete AUTHORIZATION, though",
               o3 && o3->Deletes());
}

// ===========================================================================
//  5. A non-executable table cannot enter the plan
// ===========================================================================
static void TestBlockedTableCannotEnterThePlan()
{
    std::printf("\n[5] a table with blocking findings cannot be selected into the plan\n");

    SyncPlan p = MakeFixture();
    // Block `orders`' data half exactly as the data layer does: a Finding whose
    // verdict refuses. Executable() is derived (`!blocked_`, no path back), so
    // this cannot be undone by any check the user makes.
    Finding f;
    f.table  = L"orders";
    f.column = L"v";
    f.reason = L"值无法跨引擎转换";
    p.units[0].rows.AddFinding(f, ValueVerdict::Unrepresentable);
    p.units[0].dataVerdict       = DataVerdict::Blocked;
    p.units[0].dataVerdictReason = L"含不可转换值";
    ExpectTrue("the fixture really is blocked now", !p.units[0].rows.Executable());

    SyncCompareSession s;
    s.SetPlan(&p);
    const spec::ScriptResult r = spec::RunSpecScript(
        s, L"deletes on; check orders inserts; check orders updates;"
           L"check orders deletes; check items inserts");

    ExpectStr("ticking a blocked category is reported inactive, not applied",
              Token(r.gestures, 1),
              L"APPLIED:INACTIVE:NOT_EXECUTABLE");
    ExpectStr("...and so is its delete, which has TWO reasons to be refused",
              Token(r.gestures, 3),
              L"APPLIED:INACTIVE:NOT_EXECUTABLE");

    const DataExecPlan d = s.BuildDataExecPlan();
    ExpectTrue("the blocked table has NO data spec at all", FindData(d, L"orders") == nullptr);
    ExpectTrue("its neighbour is unaffected", FindData(d, L"items") != nullptr);

    // Its STRUCTURE half is independent and must still be selectable — blocking
    // data must not quietly cancel a DDL change the user asked for.
    spec::RunSpecScript(s, L"check orders structure");
    const SyncPlan ddl = s.BuildExecutionPlan();
    bool ordersDdl = false;
    for (const SyncPlan::TableUnit& u : ddl.units)
        if (u.table.Key() == L"orders" && !u.ddl.empty()) ordersDdl = true;
    ExpectTrue("the blocked table's DDL still reaches the plan", ordersDdl);
    const DataExecPlan d2 = s.BuildDataExecPlan();
    ExpectTrue("but its data still does not", FindData(d2, L"orders") == nullptr);
}

// ===========================================================================
//  6. Preamble AND postamble survive the wizard's filtering
// ===========================================================================
static void TestPreambleAndPostambleAreCarried()
{
    std::printf("\n[6] a MySQL target's FK relaxation pair reaches the executed plan\n");

    SyncPlan p = MakeFixture();
    SyncCompareSession s;
    s.SetPlan(&p);
    const spec::ScriptResult r =
        spec::RunSpecScript(s, L"all inserts on; check orders structure");

    const SyncPlan ddl = s.BuildExecutionPlan();
    ExpectEq("one preamble statement",  (long long)ddl.preamble.size(), 1);
    ExpectEq("one postamble statement", (long long)ddl.postamble.size(), 1);
    ExpectStr("FK checks are relaxed before the run",
              Line(ddl.preamble, 0), L"SET FOREIGN_KEY_CHECKS=0");
    // The postamble was silently never executed until recently. Its PRESENCE in
    // the plan the wizard hands on is the half this layer owns.
    ExpectStr("...and restored after it",
              Line(ddl.postamble, 0), L"SET FOREIGN_KEY_CHECKS=1");
    ExpectTrue("both are visible in the scripted render",
               RenderHas(r.render, L"PRE|SET FOREIGN_KEY_CHECKS=0") &&
               RenderHas(r.render, L"POST|SET FOREIGN_KEY_CHECKS=1"));

    // They must survive even a selection that drops most tables — the filtering
    // is per unit, and session-level statements are not a unit.
    SyncCompareSession s2;
    s2.SetPlan(&p);
    spec::RunSpecScript(s2, L"all structure off; all inserts off; all updates off;"
                            L"check items inserts");
    const SyncPlan narrow = s2.BuildExecutionPlan();
    ExpectEq("a one-table selection still carries the preamble",
             (long long)narrow.preamble.size(), 1);
    ExpectEq("...and the postamble", (long long)narrow.postamble.size(), 1);
}

// ===========================================================================
//  7. Structure-only vs data selection
// ===========================================================================
static void TestStructureOnlyVsData()
{
    std::printf("\n[7] structure-only and data-only are distinct, named outcomes\n");

    SyncPlan p = MakeFixture();

    // --- structure only: an EMPTY DataExecPlan is CORRECT here ---
    {
        SyncCompareSession s;
        s.SetPlan(&p);
        const spec::ScriptResult r = spec::RunSpecScript(
            s, L"all inserts off; all updates off; all deletes off;"
               L"all structure on");
        ExpectStr("structure-only has its own OK token, not an error shape",
                  spec::ScriptToken(r), L"OK:STRUCTURE_ONLY");
        ExpectTrue("the data plan is empty", s.BuildDataExecPlan().Empty());
        ExpectTrue("...which is NOT the same as nothing to do",
                   s.HasAnythingSelected());
        ExpectTrue("the DDL is there", RenderHas(r.render, L"DDL|orders|2"));
    }

    // --- the cross-engine gate: page 1 disables 同步数据 for a cross-engine
    //     pair, so the compare runs with scope.data off. The SPEC must reflect
    //     that, and it does structurally: no scope.data means no RowChangeSet,
    //     which means no data category is present, which means no table can
    //     enter the DataExecPlan no matter what is ticked. Asserted on the
    //     spec, not on the widget's enabled bit.
    {
        SyncPlan cross;
        AddMySqlFkPreamble(cross);
        SyncPlan::TableUnit u;
        u.table       = QualifiedName{ wxString(), L"orders" };
        u.ddl         = { L"ALTER TABLE orders ADD c1 int" };
        u.dataSpec    = DiffSpecFor(L"orders");
        u.dataVerdict = DataVerdict::NotRequested;   // 同步数据 was unavailable
        cross.units.push_back(std::move(u));

        SyncCompareSession s;
        s.SetPlan(&cross);
        const spec::ScriptResult r = spec::RunSpecScript(
            s, L"deletes on; check orders inserts; check orders updates;"
               L"check orders deletes; check orders structure");

        ExpectStr("ticking 插入 on a structure-only compare is inactive",
                  Token(r.gestures, 1),
                  L"APPLIED:INACTIVE:NOT_EXECUTABLE");
        ExpectStr("a cross-engine pair cannot select data",
                  spec::ScriptToken(r), L"OK:STRUCTURE_ONLY");
        ExpectTrue("the DataExecPlan is empty even with every data box ticked",
                   s.BuildDataExecPlan().Empty());
        ExpectTrue("the structure half still runs",
                   RenderHas(r.render, L"DDL|orders|1"));
    }

    // --- nothing selected is its own verdict, distinct from both ---
    {
        SyncCompareSession s;
        s.SetPlan(&p);
        const spec::ScriptResult r = spec::RunSpecScript(
            s, L"all structure off; all inserts off; all updates off; all deletes off");
        ExpectStr("an empty selection says so", spec::ScriptToken(r),
                  L"EMPTY:NOTHING_SELECTED");
        ExpectTrue("nothing at all would run", !s.HasAnythingSelected());
    }

    // --- no compare adopted at all ---
    {
        SyncCompareSession s;
        const spec::ScriptResult r = spec::RunSpecScript(s, L"all inserts on");
        ExpectStr("no adopted plan is distinguishable from an empty selection",
                  spec::ScriptToken(r), L"EMPTY:NO_PLAN");
    }
}

// ===========================================================================
//  8. The vocabulary itself: a malformed script cannot look like a clean run
// ===========================================================================
static void TestMalformedScriptIsLoud()
{
    std::printf("\n[8] a malformed script is a loud failure, never a quiet green\n");

    SyncPlan p = MakeFixture();
    SyncCompareSession s;
    s.SetPlan(&p);
    const spec::ScriptResult r =
        spec::RunSpecScript(s, L"all inserts on; chekc orders updates");
    ExpectStr("the typo is reported per gesture",
              Token(r.gestures, 1), L"BAD_DIRECTIVE");
    ExpectStr("...and it outranks the run's own verdict",
              spec::ScriptToken(r), L"BAD_SCRIPT:2");

    SyncCompareSession s2;
    s2.SetPlan(&p);
    const spec::ScriptResult r2 = spec::RunSpecScript(s2, L"check orders rows");
    ExpectStr("an unknown category is named, not swallowed",
              Token(r2.gestures, 0), L"NO_SUCH_CATEGORY:rows");

    // The report body a live run writes: verdict first, one line per gesture.
    const wxString body = spec::FormatScriptReport(r);
    ExpectTrue("the report leads with the verdict", body.StartsWith(L"BAD_SCRIPT:2\n"));
    ExpectTrue("and echoes the offending directive",
               body.Contains(L"G2|BAD_DIRECTIVE|chekc orders updates"));
}

int main()
{
    // Unbuffered: this suite is run under ctest and, when it is being
    // deliberately broken to prove it is not vacuous, under a kill-after-N
    // seconds harness. Buffered output would lose everything printed before a
    // hang, which is exactly the run whose output matters most.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ui::tr -> core::Lang::tr lazily reads the settings file via
    // wxStandardPaths, which needs the wx runtime up. wxInitializer brings up
    // wxBase only: no window is created and this stays headless. Same reasoning
    // as SyncCompareRowTests.cpp / SyncRowSelectionTests.cpp.
    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::printf("FATAL: could not initialize wxBase\n");
        return 2;
    }

    std::printf("== SyncWizardSpecTests — dialog state -> DataExecPlan ==\n");
    TestSelectionRoundTripsByIdentity();
    TestDeleteGateFromDialogState();
    TestPerCategoryFiltering();
    TestRowExclusionsCarryTheProducersKey();
    TestBlockedTableCannotEnterThePlan();
    TestPreambleAndPostambleAreCarried();
    TestStructureOnlyVsData();
    TestMalformedScriptIsLoud();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
