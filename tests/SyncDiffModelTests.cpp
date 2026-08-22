// SyncDiffModelTests.cpp — T6 standalone unit tests for ui::BuildDiffTree
// (src/ui/SyncDiffModel.{h,cpp}), the pure/non-wx tree model behind the
// diff-review step's categorized tree (T7, SyncDiffPanel). Hand-builds a
// db::sync::SyncPlan with a mix of change kinds (create / structure-modify /
// data-only / drop) plus a table-missing warning that carries NO TableUnit
// (the shape a concurrent track's Bug #2 fix depends on staying visible), and
// asserts:
//   - every Table-kind DiffNode's `id` is the STABLE identifier (==
//     SyncPlan::TableUnit::table), independent of tree position;
//   - Column/Index/Constraint children come from SchemaChangeSet, not from
//     parsing rendered `ddl` text;
//   - a warning with no TableUnit still surfaces as its own tree node
//     (Bug #2) instead of silently vanishing.
//
// Same dependency-free harness as sync_test.cpp / SyncTypeMapTests.cpp: an
// ExpectTrue/ExpectStr/ExpectEq assert loop, non-zero exit on any failure, no
// doctest/Catch2. Links only swiftsql::db (SyncDiffModel.cpp is compiled
// directly into this binary — see tests/CMakeLists.txt) — no wxWindow/GUI is
// exercised, matching the model's "pure, non-wx" contract.
#include "ui/SyncDiffModel.h"

#include <cstdio>
#include <set>
#include <wx/init.h>
#include <wx/string.h>

using namespace ui;
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

// ---- small builders ---------------------------------------------------------

static NormColumn Col(const wxString& name, const wxString& raw, bool notNull = false)
{
    NormColumn c;
    c.name = name;
    c.rawType = raw;
    c.notNull = notNull;
    return c;
}

static const DiffNode* FindChild(const DiffNode& parent, DiffNodeKind kind, const wxString& label)
{
    for (const auto& c : parent.children)
        if (c->kind == kind && c->label == label) return c.get();
    return nullptr;
}

static int CountKind(const DiffNode& parent, DiffNodeKind kind)
{
    int n = 0;
    for (const auto& c : parent.children) if (c->kind == kind) ++n;
    return n;
}

static const DiffNode* FindTableNode(const DiffTree& tree, const wxString& id)
{
    for (const auto& root : tree.roots) {
        if (root->kind != DiffNodeKind::Category) continue;
        for (const auto& t : root->children)
            if (t->kind == DiffNodeKind::Table && t->id == id) return t.get();
    }
    return nullptr;
}

// Builds a plan with 4 TableUnits (create / structure-modify / data-only /
// destructive drop) plus one warning that has NO matching TableUnit — the
// exact shape SyncEngine::BuildPlan produces for a cross-engine "table
// missing on target, whole-table auto-CREATE not supported" Finding (see
// SyncEngine.cpp): a warnings entry with nothing pushed to units.
static SyncPlan BuildMixedPlan()
{
    SyncPlan plan;

    // ---- "users": brand-new table (source-only) --------------------------
    {
        SyncPlan::TableUnit u;
        u.table = L"users";
        u.ddl.push_back(L"CREATE TABLE users (id INT, email VARCHAR(80) NOT NULL)");
        u.changes.table = L"users";
        u.changes.createTable = true;
        u.changes.createSchema.name = L"users";
        u.changes.createSchema.columns.push_back(Col(L"id", L"int"));
        u.changes.createSchema.columns.push_back(Col(L"email", L"varchar(80)", true));
        NormIndex ix; ix.name = L"idx_email"; ix.columns = { L"email" }; ix.unique = true;
        u.changes.createSchema.indexes.push_back(ix);
        plan.units.push_back(std::move(u));
    }

    // ---- "orders": structural modify (add + modify column, add index) ----
    {
        SyncPlan::TableUnit u;
        u.table = L"orders";
        u.ddl.push_back(L"ALTER TABLE orders ADD COLUMN status VARCHAR(20)");
        u.changes.table = L"orders";
        ColumnChange addCol;
        addCol.op = ColumnChange::Op::Add;
        addCol.column = Col(L"status", L"varchar(20)");
        u.changes.columns.push_back(addCol);
        ColumnChange modCol;
        modCol.op = ColumnChange::Op::Modify;
        modCol.column = Col(L"total", L"decimal(12,2)", true);
        u.changes.columns.push_back(modCol);
        IndexChange addIdx;
        addIdx.op = IndexChange::Op::Add;
        addIdx.index.name = L"idx_status";
        addIdx.index.columns = { L"status" };
        u.changes.indexes.push_back(addIdx);
        plan.units.push_back(std::move(u));
    }

    // ---- "events": data-only (no structural change) -----------------------
    {
        SyncPlan::TableUnit u;
        u.table = L"events";
        u.dml.push_back(L"INSERT INTO events (id) VALUES (1)");
        u.stat.inserts = 1;
        plan.units.push_back(std::move(u));
    }

    // ---- "logs": destructive drop (target-only, drop-missing opted in) ---
    {
        SyncPlan::TableUnit u;
        u.table = L"logs";
        u.ddl.push_back(L"DROP TABLE logs");
        u.changes.table = L"logs";
        u.changes.dropTable = true;
        plan.units.push_back(std::move(u));
    }

    // ---- table-missing Finding: NO TableUnit, warning text only ----------
    plan.warnings.push_back(
        L"[不可映射] legacy_widgets：跨引擎结构同步：整表自动建表暂不支持，缺失的表需人工建表后重试");
    plan.warnings.push_back(L"表 archive 目标有·源无，未勾选删除，已跳过");

    return plan;
}

static void TestMixedPlanShape()
{
    const SyncPlan plan = BuildMixedPlan();
    const DiffTree tree = BuildDiffTree(plan);

    ExpectEq("roots: Tables category + Warning group", (long long)tree.roots.size(), 2);
    ExpectTrue("root[0] is Category", tree.roots[0]->kind == DiffNodeKind::Category);
    ExpectEq("Tables category has 4 table children", (long long)tree.roots[0]->children.size(), 4);
    ExpectTrue("root[0] carries no stable id (structural node)", tree.roots[0]->id.IsEmpty());

    // ---- stable ids survive, independent of tree/sibling position --------
    std::set<wxString> ids;
    for (const auto& t : tree.roots[0]->children) {
        ExpectTrue("table node id == table node label (stable id == plan table name)",
                   t->id == t->label);
        ids.insert(t->id);
    }
    ExpectEq("4 distinct stable table ids", (long long)ids.size(), 4);
    ExpectTrue("stable id set == plan table names",
               ids.count(L"users") && ids.count(L"orders") &&
               ids.count(L"events") && ids.count(L"logs"));

    // ---- "users": create, structured columns/index from createSchema -----
    const DiffNode* users = FindTableNode(tree, L"users");
    ExpectTrue("users node found by stable id", users != nullptr);
    if (users) {
        ExpectStr("users id", users->id, L"users");
        ExpectTrue("users change == Create", users->change == DiffChangeKind::Create);
        ExpectTrue("users checkable", users->checkable);
        ExpectTrue("users checked by default (non-destructive)", users->checked);
        ExpectTrue("users not destructive", !users->destructive);
        ExpectEq("users children: 2 columns + 1 index", (long long)users->children.size(), 3);
        const DiffNode* email = FindChild(*users, DiffNodeKind::Column, L"email");
        ExpectTrue("users has 'email' column child", email != nullptr);
        if (email) {
            ExpectTrue("email op == Add", email->op == DiffOp::Add);
            ExpectTrue("email description mentions rawType", email->summary.Contains(L"varchar(80)"));
            ExpectTrue("email description mentions NOT NULL", email->summary.Contains(L"NOT NULL"));
        }
        const DiffNode* idx = FindChild(*users, DiffNodeKind::Index, L"idx_email");
        ExpectTrue("users has 'idx_email' index child", idx != nullptr);
    }

    // ---- "orders": structure-only, add + modify column, add index --------
    const DiffNode* orders = FindTableNode(tree, L"orders");
    ExpectTrue("orders node found by stable id", orders != nullptr);
    if (orders) {
        ExpectTrue("orders change == Structure", orders->change == DiffChangeKind::Structure);
        ExpectEq("orders children: 2 columns + 1 index", (long long)orders->children.size(), 3);
        const DiffNode* status = FindChild(*orders, DiffNodeKind::Column, L"status");
        ExpectTrue("orders has 'status' Add column", status && status->op == DiffOp::Add);
        const DiffNode* total = FindChild(*orders, DiffNodeKind::Column, L"total");
        ExpectTrue("orders has 'total' Modify column", total && total->op == DiffOp::Modify);
        const DiffNode* idxStatus = FindChild(*orders, DiffNodeKind::Index, L"idx_status");
        ExpectTrue("orders has 'idx_status' Add index", idxStatus && idxStatus->op == DiffOp::Add);
    }

    // ---- "events": data-only, no structural children ----------------------
    const DiffNode* events = FindTableNode(tree, L"events");
    ExpectTrue("events node found by stable id", events != nullptr);
    if (events) {
        ExpectTrue("events change == Data", events->change == DiffChangeKind::Data);
        // T7 CHANGE OF RECORD: this assertion used to demand 0 children. The
        // data half of drill-in (DiffNodeKind::Row summaries) is now emitted by
        // default, because a data-diff table with no children is a dead end in
        // the compare grid -- expanding it would show nothing. `events` has
        // stat.inserts == 1 and no structural change, so it gets exactly one
        // Row child. The pre-T7 shape is still covered, explicitly, by
        // TestDataSummaryCanBeDisabled() below.
        ExpectEq("events has no STRUCTURAL children", (long long)CountKind(*events, DiffNodeKind::Column), 0);
        ExpectEq("events has 1 data Row child", (long long)CountKind(*events, DiffNodeKind::Row), 1);
        ExpectStr("events row child labels the insert count", events->children[0]->label, L"新增 1 行");
        ExpectTrue("events row child op == Add", events->children[0]->op == DiffOp::Add);
        ExpectTrue("events checked by default", events->checked);
    }

    // ---- "logs": destructive drop, default-unchecked -----------------------
    const DiffNode* logs = FindTableNode(tree, L"logs");
    ExpectTrue("logs node found by stable id", logs != nullptr);
    if (logs) {
        ExpectTrue("logs change == Drop", logs->change == DiffChangeKind::Drop);
        ExpectTrue("logs flagged destructive (DROP TABLE ddl)", logs->destructive);
        ExpectTrue("logs default-unchecked (destructive)", !logs->checked);
    }

    // ---- Bug #2: table-missing warning surfaces, never silently dropped --
    const DiffNode& warnGroup = *tree.roots[1];
    ExpectTrue("root[1] is Warning group", warnGroup.kind == DiffNodeKind::Warning);
    ExpectEq("warning group has 2 leaf children", (long long)warnGroup.children.size(), 2);
    ExpectStr("first warning text preserved verbatim", warnGroup.children[0]->label,
              L"[不可映射] legacy_widgets：跨引擎结构同步：整表自动建表暂不支持，缺失的表需人工建表后重试");
    ExpectStr("second warning text preserved verbatim", warnGroup.children[1]->label,
              L"表 archive 目标有·源无，未勾选删除，已跳过");
    for (const auto& w : warnGroup.children) {
        ExpectTrue("warning leaf carries no stable id (never independently executable)",
                   w->id.IsEmpty());
        ExpectTrue("warning leaf not checkable", !w->checkable);
    }
}

static void TestNoWarningsOmitsWarningGroup()
{
    SyncPlan plan;
    SyncPlan::TableUnit u;
    u.table = L"t1";
    u.ddl.push_back(L"ALTER TABLE t1 ADD COLUMN a INT");
    u.changes.table = L"t1";
    ColumnChange cc; cc.op = ColumnChange::Op::Add; cc.column = Col(L"a", L"int");
    u.changes.columns.push_back(cc);
    plan.units.push_back(std::move(u));

    const DiffTree tree = BuildDiffTree(plan);
    ExpectEq("no warnings => only the Tables category root", (long long)tree.roots.size(), 1);
}

static void TestEmptyPlanStillHasTablesCategory()
{
    const SyncPlan plan;   // no units, no warnings
    const DiffTree tree = BuildDiffTree(plan);
    ExpectEq("empty plan => Tables category present but empty", (long long)tree.roots.size(), 1);
    ExpectEq("empty plan => no table children", (long long)tree.roots[0]->children.size(), 0);
}

// Regression test for the SyncWizardDialog::OnCompareDone bug: the dialog
// used to gate its "nothing to sync" message (the localized "target already
// matches source" string in LangPack*.cpp) on plan_.Empty() alone, which
// only inspects TableUnit::ddl/dml plus preamble/postamble -- NOT
// SyncPlan::warnings (see db::sync::SyncPlan::Empty(), SyncEngine.h). A
// cross-engine "table missing on target, whole-table auto-CREATE not
// supported" Finding produces exactly this shape: zero TableUnits (nothing
// to plan) but a non-empty warnings list -- so plan.Empty() was TRUE and the
// dialog showed the false "already in sync" message while silently dropping
// the warning. This test builds that exact shape directly (no unrelated
// TableUnits at all -- matching real BuildPlan output for that Finding, and
// reusing the same warning literal BuildMixedPlan above uses) and asserts
// BuildDiffTree still surfaces it as a visible Warning-kind node -- the one
// place downstream of OnCompareDone that must never let it silently vanish.
static void TestWarningsOnlyNoUnitsSurfaceAsWarningNode()
{
    SyncPlan plan;   // units left empty on purpose -- see comment above
    ExpectTrue("sanity: plan.Empty() is true for units-empty+warnings-only "
               "(the exact condition that used to hide the warning)",
               plan.Empty());
    plan.warnings.push_back(L"[不可映射] legacy_widgets：跨引擎结构同步：整表自动建表暂不支持，缺失的表需人工建表后重试");

    const DiffTree tree = BuildDiffTree(plan);

    ExpectEq("units-empty+warnings-only => Tables category + Warning group",
             (long long)tree.roots.size(), 2);
    ExpectTrue("root[0] is the (empty) Tables category",
               tree.roots[0]->kind == DiffNodeKind::Category);
    ExpectEq("Tables category has no table children (no TableUnit was produced)",
             (long long)tree.roots[0]->children.size(), 0);

    ExpectTrue("root[1] is a Warning-kind node", tree.roots[1]->kind == DiffNodeKind::Warning);
    ExpectEq("Warning group has exactly 1 leaf", (long long)tree.roots[1]->children.size(), 1);
    if (tree.roots[1]->children.size() == 1) {
        const DiffNode& leaf = *tree.roots[1]->children[0];
        ExpectTrue("warning leaf is Warning-kind", leaf.kind == DiffNodeKind::Warning);
        ExpectStr("warning leaf carries the finding text verbatim", leaf.label,
                  L"[不可映射] legacy_widgets：跨引擎结构同步：整表自动建表暂不支持，缺失的表需人工建表后重试");
        ExpectTrue("warning leaf carries no stable id (not independently executable)",
                   leaf.id.IsEmpty());
        ExpectTrue("warning leaf not checkable", !leaf.checkable);
    }
}

// ---------------------------------------------------------------------------
// T7: DiffNodeKind::Row / DiffNodeKind::ValueDiff
// ---------------------------------------------------------------------------

// The pre-T7 tree shape (structure-only children) must still be reachable, so
// the assertion changed in TestMixedPlanShape is a deliberate default flip and
// not a silent loss of the old behaviour.
static void TestDataSummaryCanBeDisabled()
{
    const SyncPlan plan = BuildMixedPlan();
    DiffTreeOptions opts;
    opts.includeDataSummary = false;
    const DiffTree tree = BuildDiffTree(plan, opts);

    const DiffNode* events = FindTableNode(tree, L"events");
    ExpectTrue("events node still present with data summary off", events != nullptr);
    if (events)
        ExpectEq("includeDataSummary=false => pre-T7 shape, no children",
                 (long long)events->children.size(), 0);

    const DiffNode* orders = FindTableNode(tree, L"orders");
    ExpectTrue("structure children unaffected by the data-summary switch",
               orders && orders->children.size() == 3);
}

// All three counters populated => three Row children in 新增/修改/删除 order,
// zero counters skipped.
static void TestDataSummaryRowNodes()
{
    SyncPlan plan;
    SyncPlan::TableUnit u;
    u.table = L"inventory";
    u.dml.push_back(L"INSERT INTO inventory (id) VALUES (1)");
    u.stat.inserts   = 12;
    u.stat.updates   = 3;
    u.stat.deletes   = 0;      // skipped: a "删除 0 行" row would be noise
    u.stat.unchanged = 900;    // never surfaces as a node
    plan.units.push_back(std::move(u));

    const DiffTree tree = BuildDiffTree(plan);
    const DiffNode* inv = FindTableNode(tree, L"inventory");
    ExpectTrue("inventory node found", inv != nullptr);
    if (!inv) return;

    ExpectEq("zero-count categories are skipped", (long long)inv->children.size(), 2);
    ExpectTrue("child[0] is a Row node", inv->children[0]->kind == DiffNodeKind::Row);
    ExpectStr("child[0] label", inv->children[0]->label, L"新增 12 行");
    ExpectStr("child[0] summary names the statement kind", inv->children[0]->summary, L"INSERT");
    ExpectTrue("child[0] op == Add", inv->children[0]->op == DiffOp::Add);
    ExpectStr("child[1] label", inv->children[1]->label, L"修改 3 行");
    ExpectTrue("child[1] op == Modify", inv->children[1]->op == DiffOp::Modify);
    ExpectTrue("summary Row nodes carry no stable id (never independently executable)",
               inv->children[0]->id.IsEmpty() && inv->children[1]->id.IsEmpty());
    ExpectTrue("summary Row nodes are not checkable",
               !inv->children[0]->checkable && !inv->children[1]->checkable);
    ExpectEq("summary Row nodes have no ValueDiff children yet",
             (long long)inv->children[0]->children.size(), 0);
}

// ---------------------------------------------------------------------------
// T11 — the two halves that used to render placeholders
// ---------------------------------------------------------------------------

// A ColumnChange's TARGET side. Before ColumnChange carried hasCurrent/current,
// a Modify could only ever show the source definition against the placeholder
// 「（目标当前定义未随比对计划下发）」. Now both sides are real — and when the
// target side genuinely is not carried, the node says so rather than describing
// a default-constructed NormColumn as if it were a real definition.
static void TestColumnLeafCarriesBothSides()
{
    SyncPlan plan;
    SyncPlan::TableUnit u;
    u.table         = L"orders";
    u.changes.table = L"orders";
    u.ddl.push_back(L"ALTER TABLE orders MODIFY status VARCHAR(40)");

    db::ColumnChange mod;
    mod.op              = db::ColumnChange::Op::Modify;
    mod.column.name     = L"status";
    mod.column.rawType  = L"VARCHAR(40)";
    mod.hasCurrent      = true;
    mod.current.name    = L"status";
    mod.current.rawType = L"VARCHAR(20)";
    u.changes.columns.push_back(mod);

    db::ColumnChange add;                       // no target side exists
    add.op             = db::ColumnChange::Op::Add;
    add.column.name    = L"note";
    add.column.rawType = L"TEXT";
    u.changes.columns.push_back(add);

    db::ColumnChange blind;                     // Modify whose current is absent
    blind.op             = db::ColumnChange::Op::Modify;
    blind.column.name    = L"qty";
    blind.column.rawType = L"BIGINT";
    blind.hasCurrent     = false;
    u.changes.columns.push_back(blind);

    plan.units.push_back(std::move(u));

    // Hold the tree in a NAMED local: FindTableNode returns a pointer INTO it,
    // so binding it to a temporary would leave `t` dangling immediately.
    const DiffTree  tree = BuildDiffTree(plan);
    const DiffNode* t    = FindTableNode(tree, L"orders");
    ExpectTrue("orders node found", t != nullptr);
    if (!t || t->children.size() < 3) { ExpectTrue("three column leaves", false); return; }

    const DiffNode& m = *t->children[0];
    ExpectStr("Modify: before is the TARGET's current definition", m.before, L"VARCHAR(20)");
    ExpectStr("Modify: after is the SOURCE's desired definition", m.after, L"VARCHAR(40)");
    ExpectTrue("Modify: neither side is a placeholder",
               !IsPlaceholderSide(m.before) && !IsPlaceholderSide(m.after));

    const DiffNode& a = *t->children[1];
    ExpectTrue("Add: the target side is ABSENT, and says so", a.before == AbsentSide());
    ExpectStr("Add: the source side is the new definition", a.after, L"TEXT");

    const DiffNode& b = *t->children[2];
    ExpectTrue("a Modify with no current is UNAVAILABLE, not absent and not invented",
               b.before == UnavailableSide());
    ExpectTrue("and the two placeholders are distinguishable",
               AbsentSide() != UnavailableSide() &&
               IsPlaceholderSide(AbsentSide()) && IsPlaceholderSide(UnavailableSide()));
}

// The 数据对比 tab's real data: concrete Row nodes built from a table's capped
// sample of db::sync::RowChange objects. Before the rewire the sample was always
// empty, so this path had no data source at all and the tab showed counters.
static void TestSampleRowsBecomeConcreteRowNodes()
{
    using namespace db::sync;

    auto bridge = [](size_t i) { ColumnBridge b; b.srcIdx = i; b.passthrough = true; return b; };
    auto addRow = [&](RowChangeSet& set, RowChange::Op op,
                      const std::vector<db::Cell>& key, const std::vector<db::Cell>& vals) {
        RowChangeBuilder b(op, L"orders", L"row");
        for (size_t i = 0; i < key.size(); ++i)  b.AddKeyCell(key[i], bridge(i));
        for (size_t i = 0; i < vals.size(); ++i) b.AddCell(vals[i], bridge(i));
        set.Accept(std::move(b));
    };

    SyncPlan plan;
    SyncPlan::TableUnit u;
    u.table                = L"orders";
    u.layout.columns       = { L"id", L"status" };
    u.layout.pkColumns     = { L"id" };
    u.layout.valueBridges  = { bridge(0), bridge(1) };
    u.layout.keyBridges    = { bridge(0) };

    addRow(u.rows, RowChange::Op::Insert, { db::Cell{ db::CellKind::Numeric, L"1" } },
           { db::Cell{ db::CellKind::Numeric, L"1" }, db::Cell{ db::CellKind::Text, L"new" } });
    addRow(u.rows, RowChange::Op::Update, { db::Cell{ db::CellKind::Numeric, L"2" } },
           { db::Cell{ db::CellKind::Numeric, L"2" }, db::Cell{ db::CellKind::Null, L"" } });
    addRow(u.rows, RowChange::Op::Delete, { db::Cell{ db::CellKind::Numeric, L"3" } }, {});

    u.stat.inserts = 1;
    u.stat.updates = 1;
    u.stat.deletes = 1;
    plan.units.push_back(std::move(u));

    DiffTreeOptions opts;
    opts.includeDataSummary = false;      // isolate the sample rows
    const DiffTree  tree = BuildDiffTree(plan, opts);
    const DiffNode* t    = FindTableNode(tree, L"orders");
    ExpectTrue("orders node found", t != nullptr);
    if (!t) return;

    ExpectEq("one concrete Row node per sampled row", (long long)t->children.size(), 3);
    if (t->children.size() < 3) return;

    // INSERT — the target side is genuinely absent, so this is a complete
    // two-sided view and not a limitation.
    const DiffNode& ins = *t->children[0];
    ExpectTrue("insert row op == Add", ins.op == DiffOp::Add);
    ExpectStr("row label is the row IDENTITY, not its index", ins.label, L"id = 1");
    ExpectEq("one ValueDiff leaf per column", (long long)ins.children.size(), 2);
    ExpectTrue("insert: target side absent", ins.children[0]->before == AbsentSide());
    ExpectStr("insert: source value rendered", ins.children[1]->after, L"new");

    // UPDATE — the one incomplete case, and a DATA-LAYER limit: RowChange
    // carries only the values that will be WRITTEN, so the target's current
    // values never reach any consumer. It must read as "not sent", never as a
    // blank value and never as the new value echoed into both columns.
    const DiffNode& upd = *t->children[1];
    ExpectTrue("update row op == Modify", upd.op == DiffOp::Modify);
    ExpectTrue("update: target side is explicitly unavailable",
               upd.children[0]->before == UnavailableSide());
    ExpectTrue("update: and is NOT silently echoed from the source side",
               upd.children[0]->before != upd.children[0]->after);
    ExpectStr("update: a NULL renders distinctly from an empty string",
              upd.children[1]->after, L"NULL");

    // DELETE — carries only its key, which is the whole story for a delete.
    const DiffNode& del = *t->children[2];
    ExpectTrue("delete row op == Drop", del.op == DiffOp::Drop);
    ExpectStr("delete row label", del.label, L"id = 3");
    ExpectEq("delete shows its key columns only", (long long)del.children.size(), 1);
    ExpectStr("delete: the target still holds the row", del.children[0]->before, L"3");
    ExpectTrue("delete: the source side is absent", del.children[0]->after == AbsentSide());
}

// Sample rows can be switched off, and a plan with no sample degrades to
// counters rather than to an empty drill-in.
static void TestSampleRowsCanBeDisabled()
{
    SyncPlan plan;
    SyncPlan::TableUnit u;
    u.table        = L"orders";
    u.stat.inserts = 4;
    plan.units.push_back(std::move(u));

    DiffTreeOptions opts;
    opts.includeSampleRows = false;
    const DiffTree  tree = BuildDiffTree(plan, opts);
    const DiffNode* t    = FindTableNode(tree, L"orders");
    ExpectTrue("orders node found", t != nullptr);
    if (!t) return;
    ExpectEq("only the summary row remains", (long long)t->children.size(), 1);
    ExpectEq("and it has no ValueDiff children", (long long)t->children[0]->children.size(), 0);
}

// A table whose data diff detected deletes defaults to UNCHECKED, and that is
// read from the structured counter — not by scanning the capped `dml` preview
// for the word DELETE, which would miss a table with 50k deletes and 500
// sampled inserts.
static void TestRowDeletesMakeATableDestructive()
{
    SyncPlan plan;
    {
        SyncPlan::TableUnit u;
        u.table        = L"purge";
        u.stat.deletes = 50000;      // no ddl, no dml text at all
        plan.units.push_back(std::move(u));
    }
    {
        SyncPlan::TableUnit u;
        u.table        = L"grow";
        u.stat.inserts = 5;
        plan.units.push_back(std::move(u));
    }

    const DiffTree tree = BuildDiffTree(plan);
    const DiffNode* purge = FindTableNode(tree, L"purge");
    const DiffNode* grow  = FindTableNode(tree, L"grow");
    ExpectTrue("both nodes found", purge && grow);
    if (!purge || !grow) return;

    ExpectTrue("row deletes mark the table destructive", purge->destructive);
    ExpectTrue("and it defaults to unchecked", !purge->checked);
    ExpectTrue("an insert-only table is not destructive", !grow->destructive);
    ExpectTrue("and defaults to checked", grow->checked);
}

// A refused table explains itself in the drill-in, attached to the table it is
// about — not only as a plan-global warning the reader has to correlate by name.
static void TestRefusedTableCarriesItsReason()
{
    SyncPlan plan;
    {
        SyncPlan::TableUnit u;
        u.table             = L"legacy";
        u.dataVerdict       = DataVerdict::UnorderableKey;
        u.dataVerdictReason = L"表 legacy 的主键排序无法在两端保证一致，已排除数据比对";
        plan.units.push_back(std::move(u));
    }
    {
        SyncPlan::TableUnit u;
        u.table        = L"fine";
        u.dataVerdict  = DataVerdict::Ok;
        u.stat.inserts = 2;
        plan.units.push_back(std::move(u));
    }

    const DiffTree  tree   = BuildDiffTree(plan);
    const DiffNode* legacy = FindTableNode(tree, L"legacy");
    const DiffNode* fine   = FindTableNode(tree, L"fine");
    ExpectTrue("both nodes found", legacy && fine);
    if (!legacy || !fine) return;

    ExpectEq("the refused table carries exactly one Warning child",
             (long long)CountKind(*legacy, DiffNodeKind::Warning), 1);
    ExpectStr("and it is the data layer's own sentence, not a UI paraphrase",
              legacy->children[0]->label,
              L"表 legacy 的主键排序无法在两端保证一致，已排除数据比对");
    ExpectEq("an Ok table carries no warning child",
             (long long)CountKind(*fine, DiffNodeKind::Warning), 0);
}

// A concrete row with per-column before/after leaves — the shape the 数据对比
// tab renders side by side.
static void TestConcreteRowNodeAndValueDiffLeaves()
{
    std::vector<ValueDiffCell> cells = {
        { L"id",     L"42",        L"42",        false },
        { L"status", L"pending",   L"shipped",   true  },
        { L"total",  L"10.00",     L"12.50",     true  },
    };
    const std::unique_ptr<DiffNode> row = MakeRowNode(DiffOp::Modify, L"id = 42", cells);

    ExpectTrue("row node kind == Row", row->kind == DiffNodeKind::Row);
    ExpectTrue("row node op == Modify", row->op == DiffOp::Modify);
    ExpectStr("row label is the row identity, not a position", row->label, L"id = 42");
    ExpectStr("row summary counts the differing columns", row->summary, L"2/3 列不同");
    ExpectEq("one ValueDiff leaf per column", (long long)row->children.size(), 3);

    const DiffNode& id = *row->children[0];
    ExpectTrue("leaf kind == ValueDiff", id.kind == DiffNodeKind::ValueDiff);
    ExpectStr("leaf label is the column name", id.label, L"id");
    ExpectStr("leaf before == target value", id.before, L"42");
    ExpectStr("leaf after == source value", id.after, L"42");
    ExpectTrue("equal cell not marked differing", !id.differs);

    const DiffNode& status = *row->children[1];
    ExpectStr("status before (target)", status.before, L"pending");
    ExpectStr("status after (source)", status.after, L"shipped");
    ExpectTrue("changed cell marked differing", status.differs);
    ExpectStr("leaf summary renders 目标 -> 源", status.summary, L"pending  →  shipped");

    ExpectTrue("ValueDiff leaves carry no stable id", id.id.IsEmpty() && status.id.IsEmpty());
    ExpectTrue("ValueDiff leaves are not checkable", !id.checkable && !status.checkable);
}

// A row with no cells must not divide by anything or claim differences.
static void TestRowNodeWithNoCells()
{
    const std::unique_ptr<DiffNode> row = MakeRowNode(DiffOp::Drop, L"id = 7", {});
    ExpectEq("no cells => no ValueDiff children", (long long)row->children.size(), 0);
    ExpectStr("no cells => honest 0/0 summary", row->summary, L"0/0 列不同");
    ExpectTrue("op preserved", row->op == DiffOp::Drop);
}

// AppendDataSummary is additive: it must not disturb structure children of a
// table that has BOTH kinds of change (DiffChangeKind::Both).
static void TestStructureAndDataChildrenCoexist()
{
    SyncPlan plan;
    SyncPlan::TableUnit u;
    u.table = L"orders";
    u.ddl.push_back(L"ALTER TABLE orders ADD COLUMN status VARCHAR(20)");
    u.dml.push_back(L"UPDATE orders SET total = 1 WHERE id = 1");
    u.changes.table = L"orders";
    ColumnChange cc; cc.op = ColumnChange::Op::Add; cc.column = Col(L"status", L"varchar(20)");
    u.changes.columns.push_back(cc);
    u.stat.updates = 5;
    plan.units.push_back(std::move(u));

    const DiffTree tree = BuildDiffTree(plan);
    const DiffNode* orders = FindTableNode(tree, L"orders");
    ExpectTrue("orders node found", orders != nullptr);
    if (!orders) return;

    ExpectTrue("change == Both", orders->change == DiffChangeKind::Both);
    ExpectEq("structure child preserved", (long long)CountKind(*orders, DiffNodeKind::Column), 1);
    ExpectEq("data child appended", (long long)CountKind(*orders, DiffNodeKind::Row), 1);
    ExpectTrue("structure children come first",
               orders->children[0]->kind == DiffNodeKind::Column);
    ExpectStr("data child labels the update count", orders->children[1]->label, L"修改 5 行");
}

int main()
{
    // SyncDiffModel.cpp now emits the placeholder side tokens (AbsentSide /
    // UnavailableSide) for one-sided column and row differences, and those go
    // through ui::tr -> core::Lang::tr, whose lazy load reads the settings file
    // via wxStandardPaths. That needs the wx runtime initialized: in the app it
    // happens in wxApp::OnInit, and a bare console test that skips it crashes on
    // the first tr() by dereferencing a null wxAppTraits. This is NOT a GUI —
    // wxInitializer brings up wxBase only, no window is created, and the test
    // still runs headless. Same reasoning as SyncCompareRowTests.cpp's main().
    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::printf("FATAL: could not initialize wxBase\n");
        return 2;
    }

    TestMixedPlanShape();
    TestNoWarningsOmitsWarningGroup();
    TestEmptyPlanStillHasTablesCategory();
    TestWarningsOnlyNoUnitsSurfaceAsWarningNode();
    TestDataSummaryCanBeDisabled();
    TestDataSummaryRowNodes();
    TestColumnLeafCarriesBothSides();
    TestSampleRowsBecomeConcreteRowNodes();
    TestSampleRowsCanBeDisabled();
    TestRowDeletesMakeATableDestructive();
    TestRefusedTableCarriesItsReason();
    TestConcreteRowNodeAndValueDiffLeaves();
    TestRowNodeWithNoCells();
    TestStructureAndDataChildrenCoexist();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
