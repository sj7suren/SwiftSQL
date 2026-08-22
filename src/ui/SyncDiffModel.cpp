// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncDiffModel.cpp — see header.
#include "ui/SyncDiffModel.h"

namespace ui {

namespace {

// Statement-text danger scan — same criterion the wizard's SQL preview page
// uses (DROP / DELETE / TRUNCATE), duplicated here (rather than shared)
// because its purpose is different from the structural classification below:
// this decides a table row's DEFAULT CHECK STATE, which is inherently about
// what the rendered SQL will actually do, not about how the change is
// categorized (that part reads db::SchemaChangeSet, never this).
bool IsDangerStmt(const wxString& s)
{
    const wxString u = s.Upper();
    return u.Contains(L"DROP ") || u.Contains(L"DELETE ") || u.Contains(L"TRUNCATE");
}

wxString DescribeColumn(const db::NormColumn& c)
{
    wxString d = c.rawType.IsEmpty() ? wxString(L"(unknown type)") : c.rawType;
    if (c.notNull) d += L" NOT NULL";
    if (c.hasDefault) d += L" DEFAULT " + c.defaultExpr;
    if (c.autoIncrement) d += L" AUTO_INCREMENT";
    return d;
}

wxString DescribeIndex(const db::NormIndex& ix)
{
    wxString d = ix.primary ? wxString(L"PRIMARY KEY") : (ix.unique ? wxString(L"UNIQUE") : wxString(L"INDEX"));
    d += L" (";
    for (size_t i = 0; i < ix.columns.size(); ++i) {
        if (i) d += L", ";
        d += ix.columns[i];
    }
    d += L")";
    return d;
}

wxString DescribeFk(const db::NormForeignKey& fk)
{
    wxString d = L"FOREIGN KEY (";
    for (size_t i = 0; i < fk.columns.size(); ++i) {
        if (i) d += L", ";
        d += fk.columns[i];
    }
    d += L") REFERENCES " + fk.refTable + L" (";
    for (size_t i = 0; i < fk.refColumns.size(); ++i) {
        if (i) d += L", ";
        d += fk.refColumns[i];
    }
    d += L")";
    return d;
}

std::unique_ptr<DiffNode> MakeLeaf(DiffNodeKind kind, DiffOp op, const wxString& label,
                                   const wxString& summary)
{
    auto n = std::make_unique<DiffNode>();
    n->kind = kind;
    n->op = op;
    n->label = label;
    n->summary = summary;
    return n;
}

DiffOp ToOp(db::ColumnChange::Op op)
{
    switch (op) {
    case db::ColumnChange::Op::Add:    return DiffOp::Add;
    case db::ColumnChange::Op::Modify: return DiffOp::Modify;
    case db::ColumnChange::Op::Drop:   return DiffOp::Drop;
    }
    return DiffOp::Add;
}

DiffOp ToOp(db::IndexChange::Op op)
{
    return op == db::IndexChange::Op::Add ? DiffOp::Add : DiffOp::Drop;
}

DiffOp ToOp(db::FkChange::Op op)
{
    return op == db::FkChange::Op::Add ? DiffOp::Add : DiffOp::Drop;
}

// Column/Index/Constraint children for a table whose SchemaChangeSet carries
// a brand-new CREATE (source-only table): every column/index/FK of the
// source schema, shown as an Add.
void AppendCreateChildren(DiffNode& table, const db::TableSchema& schema)
{
    for (const auto& c : schema.columns) {
        auto n = MakeLeaf(DiffNodeKind::Column, DiffOp::Add, c.name, DescribeColumn(c));
        // A brand-new table: every column is source-only, and that is a fact,
        // not a missing value.
        n->before  = AbsentSide();
        n->after   = DescribeColumn(c);
        n->differs = true;
        table.children.push_back(std::move(n));
    }
    for (const auto& ix : schema.indexes)
        table.children.push_back(MakeLeaf(DiffNodeKind::Index, DiffOp::Add, ix.name, DescribeIndex(ix)));
    for (const auto& fk : schema.foreignKeys)
        table.children.push_back(MakeLeaf(DiffNodeKind::Constraint, DiffOp::Add, fk.name, DescribeFk(fk)));
}

// Column/Index/Constraint children for an ordinary (non-create, non-drop)
// structural diff — the actual field-level detail the diff-review "点开表看
// 哪里不一致" requirement asks for, read straight from SchemaChangeSet.
// One column change as a TRUE two-sided leaf.
//
// `before` (target) used to be unavailable: db::ColumnChange carried only the
// desired column, so the 结构对比 tab could show the source definition and a
// placeholder, and nothing else. ColumnChange now carries `hasCurrent`/`current`
// — the target's definition as it exists right now — so a Modify renders both
// real definitions side by side. `hasCurrent` is checked rather than assumed:
// it is false exactly when there is no target column to describe (an Add), and
// a default-constructed NormColumn would otherwise render as a plausible-looking
// but entirely fictional "(unknown type)" definition.
std::unique_ptr<DiffNode> MakeColumnLeaf(const db::ColumnChange& cc)
{
    // On a Drop, ColumnChange::column carries the NAME only (see SchemaModel.h),
    // so describing it would print a fabricated "(unknown type)". The target's
    // real definition is `current`; that is the side a Drop is about.
    const wxString desired = DescribeColumn(cc.column);
    const wxString current = cc.hasCurrent ? DescribeColumn(cc.current) : UnavailableSide();

    auto n = MakeLeaf(DiffNodeKind::Column, ToOp(cc.op), cc.column.name,
                      cc.op == db::ColumnChange::Op::Drop ? current : desired);

    switch (n->op) {
    case DiffOp::Add:                                  // source only
        n->before = AbsentSide();
        n->after  = desired;
        break;
    case DiffOp::Drop:                                 // target only
        n->before = current;
        n->after  = AbsentSide();
        break;
    case DiffOp::Modify:                               // both sides real
        n->before = current;
        n->after  = desired;
        break;
    }
    n->differs = true;                                 // it is in the change set
    return n;
}

void AppendChangeChildren(DiffNode& table, const db::SchemaChangeSet& changes)
{
    for (const auto& cc : changes.columns)
        table.children.push_back(MakeColumnLeaf(cc));
    for (const auto& ic : changes.indexes)
        table.children.push_back(
            MakeLeaf(DiffNodeKind::Index, ToOp(ic.op), ic.index.name, DescribeIndex(ic.index)));
    for (const auto& fc : changes.foreignKeys)
        table.children.push_back(
            MakeLeaf(DiffNodeKind::Constraint, ToOp(fc.op), fc.fk.name, DescribeFk(fc.fk)));
}

std::unique_ptr<DiffNode> BuildTableNode(const db::sync::SyncPlan::TableUnit& u,
                                         const DiffTreeOptions& opts)
{
    auto node = std::make_unique<DiffNode>();
    node->kind = DiffNodeKind::Table;
    // id is the STABLE identity (schema-qualified, escaped) and must match the
    // ui::TableKey the selection model holds; label is what a human reads. They
    // were the same string only while identity was a bare name.
    node->id = u.table.Key();    // STABLE identifier — see SyncDiffModel.h
    node->label = u.table.Display();
    node->checkable = true;

    // Destructive = the DDL drops/truncates something, OR the data diff detected
    // rows that only a DELETE could reconcile. The second half is read from the
    // STRUCTURED counter rather than by scanning `u.dml` for the word DELETE:
    // that vector is a <=500-statement preview, so a table with 50k deletes and
    // 500 sampled inserts would have scanned as non-destructive.
    bool danger = u.stat.deletes > 0;
    for (const auto& d : u.ddl) if (IsDangerStmt(d)) danger = true;
    node->destructive = danger;
    node->checked = !danger;     // destructive rows default unchecked (unchanged behaviour)

    const bool hasStruct = !u.ddl.empty();
    // Read the COUNTERS, not the rendered `dml` preview it used to read: that
    // vector is capped at 500 statements and is empty for a table whose diff was
    // counted but not sampled, so a real data difference could classify as
    // 仅结构. u.stat mirrors u.rows.Stat(), and is also what AppendDataSummary
    // renders — so the node's classification and its children cannot disagree.
    const bool hasData = (u.stat.inserts + u.stat.updates + u.stat.deletes) > 0;

    if (u.changes.createTable)      node->change = DiffChangeKind::Create;
    else if (u.changes.dropTable)   node->change = DiffChangeKind::Drop;
    else if (hasStruct && hasData)  node->change = DiffChangeKind::Both;
    else if (hasStruct)             node->change = DiffChangeKind::Structure;
    else if (hasData)               node->change = DiffChangeKind::Data;
    else                            node->change = DiffChangeKind::Unchanged;

    wxString sm;
    if (hasStruct) sm += wxString::Format(L"%zu 条 DDL", u.ddl.size());
    if (hasData) {
        if (!sm.IsEmpty()) sm += L" · ";
        sm += wxString::Format(L"+%lld ~%lld -%lld", u.stat.inserts, u.stat.updates, u.stat.deletes);
    }
    node->summary = sm;

    // Structured detail (never parsed out of rendered `ddl` text — see file
    // header). dropTable carries no old-schema snapshot to enumerate from, so
    // a dropped table's node has no children; its summary line is enough.
    if (u.changes.createTable)
        AppendCreateChildren(*node, u.changes.createSchema);
    else if (!u.changes.dropTable)
        AppendChangeChildren(*node, u.changes);

    // Data half of drill-in (T7). Read from the STRUCTURED counters, never by
    // counting/parsing u.dml strings — same rule the structure half follows.
    if (opts.includeDataSummary)
        AppendDataSummary(*node, u.stat);

    // The concrete rows behind those counters. Appended AFTER the summary so the
    // aggregate reads first and the samples elaborate it, which is the order the
    // drill-in reads in.
    if (opts.includeSampleRows)
        AppendSampleRows(*node, u.rows, u.layout);

    // WHY this table's data was refused, attached to the table it is about.
    //
    // The 状态 column can only fit a short label ("主键排序不一致（已排除）"),
    // which is not self-explanatory for a refusal that exists to prevent silent
    // data corruption. The data layer already writes one sentence per table into
    // TableUnit::dataVerdictReason, so it is surfaced here as an amber Warning
    // child — the same node kind, and the same already-styled rendering path,
    // that plan-global warnings use. It is DISPLAY text: the decision was made
    // by dataVerdict, which the status column reads structurally.
    if (u.dataVerdict != db::sync::DataVerdict::Ok &&
        u.dataVerdict != db::sync::DataVerdict::NotRequested &&
        !u.dataVerdictReason.IsEmpty()) {
        auto w = std::make_unique<DiffNode>();
        w->kind  = DiffNodeKind::Warning;
        w->label = u.dataVerdictReason;
        node->children.push_back(std::move(w));
    }

    return node;
}

} // namespace

std::unique_ptr<DiffNode> MakeValueDiffNode(const ValueDiffCell& cell)
{
    auto n = std::make_unique<DiffNode>();
    n->kind    = DiffNodeKind::ValueDiff;
    n->label   = cell.column;
    n->before  = cell.before;
    n->after   = cell.after;
    n->differs = cell.differs;
    // Secondary line reads as a compact 目标 -> 源 so a consumer that only
    // renders label/summary (the tree) still conveys both sides.
    n->summary = cell.before + L"  →  " + cell.after;
    return n;
}

std::unique_ptr<DiffNode> MakeRowNode(DiffOp op, const wxString& rowLabel,
                                      const std::vector<ValueDiffCell>& cells,
                                      const wxString& rowKey)
{
    auto n = std::make_unique<DiffNode>();
    n->kind   = DiffNodeKind::Row;
    n->op     = op;
    n->label  = rowLabel;
    n->rowKey = rowKey;

    int differing = 0;
    for (const ValueDiffCell& c : cells) {
        if (c.differs) ++differing;
        n->children.push_back(MakeValueDiffNode(c));
    }
    n->summary = wxString::Format(L"%d/%zu 列不同", differing, cells.size());
    return n;
}

void AppendDataSummary(DiffNode& table, const db::sync::RowStat& stat)
{
    auto add = [&table](DiffOp op, const wchar_t* fmt, long long n, const wchar_t* verb) {
        if (n <= 0) return;
        auto node = std::make_unique<DiffNode>();
        node->kind    = DiffNodeKind::Row;
        node->op      = op;
        node->label   = wxString::Format(fmt, n);
        node->summary = verb;
        table.children.push_back(std::move(node));
    };
    add(DiffOp::Add,    L"新增 %lld 行", stat.inserts, L"INSERT");
    add(DiffOp::Modify, L"修改 %lld 行", stat.updates, L"UPDATE");
    add(DiffOp::Drop,   L"删除 %lld 行", stat.deletes, L"DELETE");
}

namespace {

// One typed cell as display text. NOT a SQL literal — db::sync::RenderLiteral
// is that, and it quotes/escapes/hex-frames for a dialect, which is exactly
// wrong in a comparison grid where the reader wants the VALUE. NULL is spelled
// distinctly so it can never be confused with an empty string, which is a
// different value and, in a primary key, a different row.
wxString CellText(const db::Cell& c)
{
    switch (c.kind) {
    case db::CellKind::Null:    return L"NULL";
    case db::CellKind::Binary:  return L"0x" + c.text;
    case db::CellKind::Numeric:
    case db::CellKind::Text:    break;
    }
    return c.text;
}

// "id = 42" / "tenant = 7, id = 42". Display only, never SQL — and never a row
// POSITION: a row's index in a sample is not an identity (see the file header).
wxString RowLabel(const std::vector<wxString>& pkColumns, const std::vector<db::Cell>& key)
{
    wxString s;
    const size_t n = pkColumns.size() < key.size() ? pkColumns.size() : key.size();
    for (size_t i = 0; i < n; ++i) {
        if (i) s += L", ";
        s += pkColumns[i] + L" = " + CellText(key[i]);
    }
    return s.IsEmpty() ? wxString(L"?") : s;
}

DiffOp ToOp(db::sync::RowChange::Op op)
{
    switch (op) {
    case db::sync::RowChange::Op::Insert: return DiffOp::Add;
    case db::sync::RowChange::Op::Update: return DiffOp::Modify;
    case db::sync::RowChange::Op::Delete: return DiffOp::Drop;
    }
    return DiffOp::Add;
}

} // namespace

void AppendSampleRows(DiffNode& table, const db::sync::RowChangeSet& rows,
                      const db::sync::RowLayout& layout)
{
    // WHY THE PER-ROW CHECKBOXES ARE ABOUT TO BE MISSING.
    //
    // Row-level selection is offered only for a fully materialized diff: past
    // RowChangeSet::kSampleCap the rows are counted but not kept, so a
    // checkbox next to the 500 that survived would imply the user had picked
    // from all of them. SyncSelection::SetRowExcluded refuses a truncated
    // table for exactly that reason, and the grid renders no checkbox there.
    //
    // "No checkbox" is indistinguishable from "the app is broken" unless
    // something SAYS why, so the reason is emitted as a node — the same amber
    // Warning kind, and the same already-styled rendering path, the per-table
    // data verdicts use — and it is emitted FIRST so it is read before the
    // sample it qualifies. The 状态 column's 行数超限（需手动对比） is the
    // short form of this same fact; this is the sentence behind it.
    if (rows.Truncated()) {
        auto w = std::make_unique<DiffNode>();
        w->kind  = DiffNodeKind::Warning;
        // CONSEQUENCE FIRST, cause second. This label lands in the grid's 表名
        // column, which is ~190dip wide and will clip a long sentence, so the
        // half a reader must not miss ("you cannot check rows here") leads and
        // the justification follows. The full text is one string rather than
        // two so it stays a single translatable unit; `summary` repeats the
        // cause in the neighbouring column for the same reason.
        w->label   = tr(L"本表只能整表勾选，无法逐行勾选");
        w->summary = wxString::Format(
            tr(L"差异行数超过 %zu 行明细上限，以下仅为样本"),
            db::sync::RowChangeSet::kSampleCap);
        table.children.push_back(std::move(w));
    }

    for (const db::sync::RowChange& r : rows.Sample()) {
        const wxString label = RowLabel(layout.pkColumns, r.Key());

        std::vector<ValueDiffCell> cells;
        switch (r.Operation()) {

        // A DELETE carries only its key (RowChange::Values() is empty for one),
        // which is not a shortcoming: the row is being removed, so the only
        // thing to show is WHICH row. The source side is genuinely absent.
        case db::sync::RowChange::Op::Delete:
            for (size_t i = 0; i < layout.pkColumns.size() && i < r.Key().size(); ++i)
                cells.push_back({ layout.pkColumns[i], CellText(r.Key()[i]),
                                  AbsentSide(), true });
            break;

        // An INSERT's target side is genuinely absent — the row does not exist
        // there yet — so this is a complete, honest two-sided view.
        case db::sync::RowChange::Op::Insert:
            for (size_t i = 0; i < layout.columns.size() && i < r.Values().size(); ++i)
                cells.push_back({ layout.columns[i], AbsentSide(),
                                  CellText(r.Values()[i]), true });
            break;

        // An UPDATE is a TRUE two-sided comparison, and for a data change it is
        // THE comparison the user asked for: before vs after. Both sides are
        // real values — PriorValues() is the row as it exists in the target
        // right now, index-aligned with Values() because both are in
        // `layout.columns` order (RowChange.h states that alignment; nothing
        // here re-derives it).
        //
        // Prior cells are rendered EXACTLY AS READ. They are not pushed through
        // a value conversion, deliberately: they came off the target stream, so
        // they are already target-native, and bridging a value that was never on
        // the source side is the error db::sync's tgtKeyBridges comment exists
        // to prevent. They are display-only and structurally outside the
        // emission path (RenderRowDml and the executor read Values()/Key()).
        //
        // The per-index bound keeps UnavailableSide() REACHABLE rather than
        // treating prior values as guaranteed: only sampled rows retain them
        // (RowChangeBuilder::KeepPriorValues defaults off so a 500k-row diff
        // does not materialize 500k target rows), so a RowChange built outside
        // RowChangeSet::Accept has none, and that is a fact to state, not to
        // paper over.
        case db::sync::RowChange::Op::Update: {
            const std::vector<db::Cell>& before = r.PriorValues();
            for (size_t i = 0; i < layout.columns.size() && i < r.Values().size(); ++i) {
                const bool  known  = i < before.size();
                const wxString lhs = known ? CellText(before[i]) : UnavailableSide();
                const wxString rhs = CellText(r.Values()[i]);
                // Mark ONLY the columns that actually disagree. With both sides
                // real, a blanket `true` would light up every column of a row in
                // which one field changed, which is precisely the noise the
                // per-cell highlight exists to cut. When the target side is
                // unknown the cell is marked differing: "we cannot show you this
                // side" must not render as "these two agree".
                cells.push_back({ layout.columns[i], lhs, rhs, !known || lhs != rhs });
            }
            break;
        }
        }

        table.children.push_back(MakeRowNode(ToOp(r.Operation()), label, cells, r.RowKey()));
    }
}

// ===========================================================================
// Functions & stored procedures — compare and display only (ADR-013)
// ===========================================================================

namespace {

// Which side of a routine pair to show, as display text. Three distinct
// outcomes, and collapsing any two of them would be a lie:
//   * the side has no such routine            -> AbsentSide()
//   * the side has it but we may not read it  -> UnreadableSide()
//   * we have the text                        -> the text
wxString RoutineSide(bool has, const db::sync::RoutineDef& def, const wxString& value)
{
    if (!has) return AbsentSide();
    if (!def.bodyReadable) return UnreadableSide();
    return value;
}

// One meta/body row of a routine pair, as the same ValueDiff leaf a data row
// uses: `before` is the TARGET side, `after` the SOURCE side (the convention
// every kind in this file follows).
void AppendRoutineLeaf(DiffNode& routine, const wxString& label,
                       const wxString& before, const wxString& after, bool differs)
{
    ValueDiffCell cell;
    cell.column  = label;
    cell.before  = before;
    cell.after   = after;
    cell.differs = differs;
    routine.children.push_back(MakeValueDiffNode(cell));
}

DiffOp RoutineOp(db::sync::RoutineVerdict v)
{
    switch (v) {
    case db::sync::RoutineVerdict::SourceOnly: return DiffOp::Add;    // 源端有，目标端要新增
    case db::sync::RoutineVerdict::TargetOnly: return DiffOp::Drop;   // 目标端多出来的
    default:                                   return DiffOp::Modify;
    }
}

std::unique_ptr<DiffNode> MakeRoutineNode(const db::sync::RoutinePair& p)
{
    auto n = std::make_unique<DiffNode>();
    n->kind    = DiffNodeKind::Routine;
    n->op      = RoutineOp(p.verdict);
    n->verdict = p.verdict;
    n->label   = p.Either().Signature();
    n->status  = db::sync::RoutineVerdictLabel(p.verdict);
    n->summary = p.reason;

    // `differs` is the HIGHLIGHT authorization, and it is granted for exactly
    // one verdict. BodyDiffers is issued only for a same-engine pair, so a
    // tinted body cell always means "these two are written in the same language
    // and their text is not the same". A cross-engine pair is NotComparable by
    // construction (RoutineDiff.h) and gets no highlight at all — colouring two
    // bodies in different procedural languages would be asserting a difference
    // the data layer explicitly refuses to claim.
    const bool bodyDiffers = (p.verdict == db::sync::RoutineVerdict::BodyDiffers);

    // 签名 first: for SignatureDiffers it IS the finding, and for a one-sided
    // routine it is the identity of the thing that is missing.
    const wxString srcSig = p.hasSource ? p.source.Signature() : AbsentSide();
    const wxString tgtSig = p.hasTarget ? p.target.Signature() : AbsentSide();
    AppendRoutineLeaf(*n, tr(L"签名"), tgtSig, srcSig, srcSig != tgtSig);

    // 语言 is what makes a cross-engine pair legible: seeing "plpgsql" against
    // "SQL" is the reason the tool declines to judge the bodies, stated as data
    // rather than as an apology.
    const wxString srcLang = p.hasSource ? p.source.language : AbsentSide();
    const wxString tgtLang = p.hasTarget ? p.target.language : AbsentSide();
    if (!p.source.language.IsEmpty() || !p.target.language.IsEmpty())
        AppendRoutineLeaf(*n, tr(L"语言"), tgtLang, srcLang, srcLang != tgtLang);

    AppendRoutineLeaf(*n, RoutineBodyRowLabel(),
                      RoutineSide(p.hasTarget, p.target, p.target.body),
                      RoutineSide(p.hasSource, p.source, p.source.body),
                      bodyDiffers);
    return n;
}

wxString RoutineCategoryLabel(db::sync::RoutineKind k)
{
    return k == db::sync::RoutineKind::Function ? tr(L"函数 Functions")
                                                : tr(L"存储过程 Procedures");
}

// "源端不可见（权限不足）" — which SIDE could not be read, not just that
// something could not be. A user whose target account is the restricted one
// needs to know which credentials to fix.
wxString SideStatus(const wxString& side, db::sync::RoutineReadStatus s)
{
    if (s == db::sync::RoutineReadStatus::Ok) return wxString();
    return side + db::sync::RoutineReadStatusLabel(s);
}

wxString JoinNonEmpty(const wxString& a, const wxString& b, const wxString& sep)
{
    if (a.IsEmpty()) return b;
    if (b.IsEmpty()) return a;
    return a + sep + b;
}

} // namespace

std::unique_ptr<DiffNode> MakeRoutineCategoryNode(db::sync::RoutineKind kind,
                                                  const db::sync::RoutineDiffSet& routines)
{
    auto cat = std::make_unique<DiffNode>();
    cat->kind     = DiffNodeKind::Category;
    cat->category = (kind == db::sync::RoutineKind::Function) ? DiffCategory::Functions
                                                              : DiffCategory::Procedures;
    cat->label    = RoutineCategoryLabel(kind);
    // Never checkable, never identified. Left at their defaults deliberately:
    // there is no line here that could be edited to "just make it selectable".
    // See the header — a category with no id and no children carrying one is
    // unreachable from ui::SyncSelection by construction.

    // DEGRADED: label the category and stop. No children, ever — see the header.
    if (!routines.Comparable()) {
        cat->status  = JoinNonEmpty(SideStatus(tr(L"源端"), routines.sourceStatus),
                                    SideStatus(tr(L"目标端"), routines.targetStatus),
                                    L" · ");
        cat->summary = JoinNonEmpty(routines.sourceStatusDetail, routines.targetStatusDetail,
                                    L" · ");
        if (cat->summary.IsEmpty())
            cat->summary = tr(L"未能读取两侧的例程目录，因此不做任何对比结论。");
        return cat;
    }

    const auto all  = routines.ByKind(kind);
    const auto diff = routines.DifferencesByKind(kind);
    for (const db::sync::RoutinePair* p : diff)
        cat->children.push_back(MakeRoutineNode(*p));

    // The counts are what makes an EMPTY category unambiguous: "0 differences
    // out of 12 compared" and "we never looked" must not look the same on
    // screen.
    cat->summary = wxString::Format(tr(L"共 %zu 项 · 差异 %zu 项"), all.size(), diff.size());
    if (!routines.sameEngine)
        cat->summary += tr(L" · 跨引擎：仅并排展示，不判断内容同异");
    return cat;
}

namespace {

DiffTree BuildTree(const db::sync::SyncPlan& plan, const db::sync::RoutineDiffSet* routines,
                   const DiffTreeOptions& opts)
{
    DiffTree tree;

    auto tablesCat = std::make_unique<DiffNode>();
    tablesCat->kind = DiffNodeKind::Category;
    tablesCat->category = DiffCategory::Tables;
    tablesCat->label = L"表 Tables";
    for (const auto& u : plan.units)
        tablesCat->children.push_back(BuildTableNode(u, opts));
    tree.roots.push_back(std::move(tablesCat));

    // Functions and procedures, between the executable tables and the warnings.
    // They are two categories rather than one list because the user asked for
    // 「不同的函数，存储过程」 — two named things.
    if (routines) {
        tree.roots.push_back(MakeRoutineCategoryNode(db::sync::RoutineKind::Function, *routines));
        tree.roots.push_back(MakeRoutineCategoryNode(db::sync::RoutineKind::Procedure, *routines));
    }

    // Bug #2 fix: a cross-engine "table missing on target, whole-table
    // auto-CREATE not supported" Finding (and any other skipped/gated item)
    // produces a SyncPlan::warnings entry but NO TableUnit — there is no DDL
    // to plan, so nothing above would ever surface it. Fold it into the tree
    // as its own top-level, non-checkable group instead of a side panel, so
    // it can never be silently dropped again.
    if (!plan.warnings.empty()) {
        auto warnGroup = std::make_unique<DiffNode>();
        warnGroup->kind = DiffNodeKind::Warning;
        warnGroup->label = wxString::Format(L"提示 Warnings (%zu)", plan.warnings.size());
        for (const auto& w : plan.warnings) {
            auto leaf = std::make_unique<DiffNode>();
            leaf->kind = DiffNodeKind::Warning;
            leaf->label = w;
            warnGroup->children.push_back(std::move(leaf));
        }
        tree.roots.push_back(std::move(warnGroup));
    }

    return tree;
}

} // namespace

DiffTree BuildDiffTree(const db::sync::SyncPlan& plan, const DiffTreeOptions& opts)
{
    return BuildTree(plan, nullptr, opts);
}

DiffTree BuildDiffTree(const db::sync::SyncPlan& plan, const db::sync::RoutineDiffSet& routines,
                       const DiffTreeOptions& opts)
{
    return BuildTree(plan, &routines, opts);
}

} // namespace ui
