// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCompareRow.h — the pure, widget-free half of the Navicat-style compare grid
// (T8 of ADR-015): how one grid ROW is classified (类型 / 状态) and how a
// db::sync::SyncPlan becomes the ui::CompareResult that SyncSelection adopts.
//
// WHY THIS IS ITS OWN HEADER
// SyncCompareGrid.cpp pulls in <wx/dataview.h> and can therefore never be
// compiled into a headless test. The two decisions that are actually worth
// testing — "what kind of change is this row?" and "may its data half run?" —
// have nothing to do with wxDataViewCtrl, so they live here, header-only, and
// are covered by tests/SyncCompareRowTests.cpp. The widget only formats what
// these functions return.
//
// ---------------------------------------------------------------------------
// STATUS IS DERIVED FROM STRUCTURED FLAGS, NEVER FROM TEXT
// ---------------------------------------------------------------------------
// ClassifyCompareStatus() reads TableDiff's booleans (truncated /
// dataExecutable / structureExecutable). It deliberately does NOT look at
// TableDiff::findings, which SyncSelection.h marks DISPLAY ONLY — deciding
// whether something may execute by grepping a human-readable reason string is
// exactly the string-sniffing both SyncDiffModel.h and RowChange.h forbid.
//
// ---------------------------------------------------------------------------
// GAP CLOSED (T11 — this file's half of the data-layer rewire)
// ---------------------------------------------------------------------------
// The PRD's 状态 vocabulary includes 无主键（已排除）, which used to be
// unreachable: SyncPlan::TableUnit carried no primary-key verdict and the "no
// PK" case surfaced only as free text in SyncPlan::warnings, which this file
// may not parse. TableUnit now carries db::sync::DataVerdict, and units with
// zero changes now reach the plan, so the verdict is mapped structurally into
// ui::DataStatus and every refusal case renders its own label — including
// UnorderableKey, which is a distinct correctness refusal (a PK whose byte
// ordering cannot be guaranteed on both engines would silently desync the
// sorted merge) and is deliberately NOT folded into a generic error.
#pragma once

#include "db/SyncEngine.h"
#include "ui/I18n.h"
#include "ui/SyncDiffModel.h"
#include "ui/SyncSelection.h"
#include "ui/SyncSelectionAdapter.h"

namespace ui {

// ---------------------------------------------------------------------------
// 类型 column
// ---------------------------------------------------------------------------

enum class CompareRowType {
    StructureOnly,      // 仅结构
    DataOnly,           // 仅数据
    StructureAndData,   // 结构+数据
    NewTable,           // 新建表   (source-only)
    ExtraTable,         // 目标多余表 (target-only, drop opted in)
    Unchanged,          // nothing to do; never expected in a populated grid
};

// Maps the tree model's per-table classification onto the grid's vocabulary.
// Takes DiffChangeKind rather than a DiffNode so the mapping is trivially
// exhaustive and testable.
inline CompareRowType ClassifyCompareRow(DiffChangeKind change)
{
    switch (change) {
    case DiffChangeKind::Create:    return CompareRowType::NewTable;
    case DiffChangeKind::Drop:      return CompareRowType::ExtraTable;
    case DiffChangeKind::Structure: return CompareRowType::StructureOnly;
    case DiffChangeKind::Data:      return CompareRowType::DataOnly;
    case DiffChangeKind::Both:      return CompareRowType::StructureAndData;
    case DiffChangeKind::Unchanged: return CompareRowType::Unchanged;
    }
    return CompareRowType::Unchanged;
}

inline wxString CompareRowTypeLabel(CompareRowType t)
{
    switch (t) {
    case CompareRowType::StructureOnly:    return tr(L"仅结构");
    case CompareRowType::DataOnly:         return tr(L"仅数据");
    case CompareRowType::StructureAndData: return tr(L"结构+数据");
    case CompareRowType::NewTable:         return tr(L"新建表");
    case CompareRowType::ExtraTable:       return tr(L"目标多余表");
    case CompareRowType::Unchanged:        return tr(L"无变更");
    }
    return wxString();
}

// ---------------------------------------------------------------------------
// 状态 column
// ---------------------------------------------------------------------------

enum class CompareRowStatus {
    Ready,                    // 就绪
    NoPrimaryKey,             // 无主键（已排除）
    UnorderableKey,           // 主键排序不可跨引擎保证（已排除）
    TargetMissingTable,       // 目标缺少此表（未创建，已排除数据）
    RowLimitExceeded,         // 行数超限（需手动对比）
    HasUnconvertibleValues,   // 含不可转换值
};

// Order matters, and every branch below reads a STRUCTURED flag:
//
//  1. Blocked outranks everything: "含不可转换值" is the fact that changes what
//     the user MAY DO. It is tested via the executable booleans rather than via
//     DataStatus::Blocked because structure can be blocked independently of
//     data, and because dataExecutable is derived on the data-layer side
//     (RowChangeSet::Executable(), `!blocked_` with no path back to true).
//  2. Then the per-table data REFUSALS. These are mutually exclusive with
//     truncation by construction — a refused table was never diffed, so it has
//     no rows to cap — but they are tested first anyway so that adding a future
//     verdict cannot silently start reading as 就绪.
//  3. Then truncation, which only changes what the user may HAND-PICK.
inline CompareRowStatus ClassifyCompareStatus(const TableDiff& diff)
{
    if (!diff.dataExecutable || !diff.structureExecutable)
        return CompareRowStatus::HasUnconvertibleValues;

    switch (diff.dataStatus) {
    case DataStatus::NoPrimaryKey:   return CompareRowStatus::NoPrimaryKey;
    case DataStatus::UnorderableKey: return CompareRowStatus::UnorderableKey;
    case DataStatus::TargetMissing:  return CompareRowStatus::TargetMissingTable;
    case DataStatus::Blocked:        return CompareRowStatus::HasUnconvertibleValues;
    case DataStatus::NotRequested:
    case DataStatus::Ok:             break;
    }

    if (diff.truncated)
        return CompareRowStatus::RowLimitExceeded;
    return CompareRowStatus::Ready;
}

inline wxString CompareRowStatusLabel(CompareRowStatus s)
{
    switch (s) {
    case CompareRowStatus::Ready:                  return tr(L"就绪");
    case CompareRowStatus::NoPrimaryKey:           return tr(L"无主键（已排除）");
    case CompareRowStatus::UnorderableKey:         return tr(L"主键排序不一致（已排除）");
    case CompareRowStatus::TargetMissingTable:     return tr(L"目标缺少此表（已排除数据）");
    case CompareRowStatus::RowLimitExceeded:       return tr(L"行数超限（需手动对比）");
    case CompareRowStatus::HasUnconvertibleValues: return tr(L"含不可转换值");
    }
    return wxString();
}

// NOTE on where the LONGER explanation lives. The 状态 label has to fit a grid
// column, and a bare 「主键排序不一致（已排除）」 is not self-explanatory. The
// full sentence is not duplicated here as a second, parallel vocabulary: the
// data layer already produces one per table in
// SyncPlan::TableUnit::dataVerdictReason, and SyncDiffModel surfaces THAT as an
// amber Warning child of the table row, through the rendering path the grid
// already has. Two sources of explanation would eventually disagree.

// True when the row's state should read as a problem (drives the amber/red
// foreground in the grid). Kept next to the enum so the widget never
// re-derives "is this bad?" with its own private list of cases.
inline bool CompareRowStatusIsWarning(CompareRowStatus s)
{
    return s != CompareRowStatus::Ready;
}

// ---------------------------------------------------------------------------
// SyncPlan -> CompareResult
// ---------------------------------------------------------------------------

// Pure mapping of the data layer's verdict onto the UI's. Exhaustive by switch
// so a new db::sync::DataVerdict is a compiler warning here, not a table that
// silently reads 就绪.
inline DataStatus ToDataStatus(db::sync::DataVerdict v)
{
    switch (v) {
    case db::sync::DataVerdict::NotRequested:   return DataStatus::NotRequested;
    case db::sync::DataVerdict::Ok:             return DataStatus::Ok;
    case db::sync::DataVerdict::NoPrimaryKey:   return DataStatus::NoPrimaryKey;
    case db::sync::DataVerdict::UnorderableKey: return DataStatus::UnorderableKey;
    case db::sync::DataVerdict::TargetMissing:  return DataStatus::TargetMissing;
    case db::sync::DataVerdict::Blocked:        return DataStatus::Blocked;
    }
    return DataStatus::NotRequested;
}

// Build the selection model's input from a compared plan, one ui::MakeTableDiff
// per unit.
//
// THIS IS NOT A SECOND ADAPTER ANY MORE. It used to be: SyncEngine::BuildPlan
// did not emit a db::sync::RowChangeSet, so this function synthesized a
// TableDiff with `truncated = false` and `dataExecutable = true` HARDCODED.
// Both were demonstrably wrong once the data layer landed, and wrong in the
// dangerous direction:
//
//   * dataExecutable = true offered the user execution of a table the data
//     layer had structurally REFUSED (RowChangeSet::Executable() is false when
//     any value cannot cross into the target engine — a MySQL 0000-00-00
//     zero-date bound for PostgreSQL, say);
//   * truncated = false offered per-row hand-picking on a diff that only
//     materialized its first 500 rows, i.e. on rows that are not all present.
//
// Everything now comes from the structured carrier. `structureExecutable` stays
// true by construction and not by assumption: SchemaDelta::AddColumnChange is
// the only door into a SchemaChangeSet and it refuses !MayAutoAlter verdicts,
// so a change that reached TableUnit::changes is already gated.
inline CompareResult MakeCompareResultFromPlan(const db::sync::SyncPlan& plan)
{
    CompareResult out;
    out.tables.reserve(plan.units.size());
    for (const db::sync::SyncPlan::TableUnit& u : plan.units) {
        // Key(), not Display(): this string becomes the ui::TableKey that the
        // selection map, the drill-in tree and the execution spec all agree on.
        // Round-tripping it back to a QualifiedName is QualifiedName::Parse.
        TableDiff d = MakeTableDiff(u.table.Key(), u.rows,
                                    /*hasStructure*/ !u.ddl.empty(),
                                    /*structureExecutable*/ true);
        d.dataStatus = ToDataStatus(u.dataVerdict);

        // The verdict's own display text is per-TABLE (unlike SyncPlan::warnings,
        // which is plan-global and is surfaced whole as its own grid branch), so
        // it can be attributed here without parsing anything. Display only — the
        // decision was already made by the verdict above.
        if (!u.dataVerdictReason.IsEmpty() && d.dataStatus != DataStatus::Ok &&
            d.dataStatus != DataStatus::NotRequested)
            d.findings.push_back(u.dataVerdictReason);

        out.tables.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Row-level exclusions, counted per category
// ---------------------------------------------------------------------------
//
// WHY THIS LIVES HERE AND NOT IN SyncSelection
// SyncSelection holds exclusions as a bare std::set<RowKey>: it knows a row was
// unchecked, and deliberately not whether that row was an insert, an update or
// a delete. Teaching it the category would mean giving the pure check-state
// model a second, parallel copy of the diff — the thing it is built to avoid.
//
// The DiffTree already carries both facts on the same node (DiffNode::rowKey
// and DiffNode::op), so the join happens here, over the two things that already
// exist, and stays a pure function testable without a widget.
//
// EXACTNESS. Subtracting these from a table's counters is exact, not an
// estimate, and that follows from the truncation rule rather than from luck:
// exclusions can only exist for a NON-truncated table (SyncSelection::
// SetRowExcluded refuses otherwise), and a non-truncated RowChangeSet holds
// EVERY counted row in its sample. So each excluded row is one row that was
// counted, exactly once, in exactly one category.
struct ExcludedRowCounts {
    long long inserts = 0;
    long long updates = 0;
    long long deletes = 0;

    long long Total() const { return inserts + updates + deletes; }
};

// One table's exclusions. `table` must be a Table-kind DiffNode; SUMMARY row
// children (「修改 5 行」, no rowKey) are skipped by the empty-key test, so a
// counter can never be mistaken for a row.
inline ExcludedRowCounts CountExcludedRows(const DiffNode& table, const SyncSelection& sel)
{
    ExcludedRowCounts out;
    if (table.kind != DiffNodeKind::Table || table.id.IsEmpty()) return out;

    const TableKey key(table.id);

    // Early-out on the overwhelmingly common case (nothing excluded), so the
    // grid can call this from a cell repaint without walking a 500-row sample
    // per visible table. It is an optimization only: the loop below over an
    // empty exclusion set is already a no-op.
    if (sel.ExcludedRows(key).empty()) return out;

    for (const auto& child : table.children) {
        if (child->kind != DiffNodeKind::Row || child->rowKey.IsEmpty()) continue;
        // RowKey(child->rowKey) wraps the string the data layer produced and
        // this node carried; nothing re-encodes anything (see DiffNode::rowKey).
        if (!sel.IsRowExcluded(key, RowKey(child->rowKey))) continue;
        switch (child->op) {
        case DiffOp::Add:    ++out.inserts; break;
        case DiffOp::Modify: ++out.updates; break;
        case DiffOp::Drop:   ++out.deletes; break;
        }
    }
    return out;
}

// Whole-tree total, for the page footer and the destructive-confirmation count.
inline ExcludedRowCounts CountExcludedRows(const DiffTree& tree, const SyncSelection& sel)
{
    ExcludedRowCounts out;
    for (const auto& root : tree.roots) {
        if (root->kind != DiffNodeKind::Category) continue;
        for (const auto& t : root->children) {
            const ExcludedRowCounts c = CountExcludedRows(*t, sel);
            out.inserts += c.inserts;
            out.updates += c.updates;
            out.deletes += c.deletes;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ExecutionSpec -> what actually runs (two halves, both structural)
// ---------------------------------------------------------------------------
//
// WHAT USED TO BE HERE, AND WHY IT IS GONE
// A function called FilterPlanBySpec used to reduce the plan by classifying
// each rendered statement by its LEADING SQL VERB (INSERT/UPDATE/DELETE/…),
// because SyncPlan::TableUnit then carried one flat, untagged `dml` vector and
// there was no other way to keep a DELETE the user had unchecked from running.
// It failed closed on an unrecognized verb, which made it defensible, but it
// was never more than a stopgap: it re-derived a category that the producer
// already knew and had thrown away.
//
// The data layer now filters per (table, category) AT THE POINT OF DETECTION
// inside the merge (see DataSyncExec.h). A category that is off never reaches
// the RowChangeSink, so no RowChangeBuilder, no RowChange, no rendered
// statement and no bound batch row is ever constructed for it. There is
// nothing downstream to accidentally execute and nothing left to classify, so
// the verb classifier is deleted rather than kept "just in case" — keeping a
// second, weaker gate alongside a structural one only creates the possibility
// of the two disagreeing.
//
// Two builders replace it, because execution now has two authorizations:
//   FilterStructureBySpec() -> the DDL half, still a SyncPlan.
//   BuildDataExecPlan()     -> the DATA half, a db::sync::DataExecPlan.
// Both look tables up by TableExecSpec::Table() — identity, never position
// (see SyncSelection.h's Round 1 bug note).

// The DDL half. Tables absent from the spec are dropped whole; a table that is
// present keeps its `ddl` only when the spec includes Structure.
//
// The DATA fields (`rows`, `layout`, `dataSpec`, `dataVerdict`) are copied
// through UNFILTERED and on every unit the spec names, even one with no DDL:
// SyncEngine::Execute re-derives each table's diff from `dataSpec`, so a unit
// dropped here is a table that cannot be data-synced at all. Filtering data is
// the DataExecPlan's job, and doing it in two places is how the two get to
// disagree.
inline db::sync::SyncPlan FilterStructureBySpec(const db::sync::SyncPlan& plan,
                                                const ExecutionSpec&      spec)
{
    db::sync::SyncPlan out;
    out.preamble  = plan.preamble;
    out.postamble = plan.postamble;
    out.warnings  = plan.warnings;

    for (const db::sync::SyncPlan::TableUnit& u : plan.units) {
        const TableExecSpec* ts = spec.Find(TableKey(u.table.Key()));
        if (!ts) continue;

        db::sync::SyncPlan::TableUnit keep = u;
        keep.dml.clear();                  // preview text, never an execution list
        if (!ts->Includes(ChangeCategory::Structure)) keep.ddl.clear();

        const bool anyData = ts->Includes(ChangeCategory::Inserts) ||
                             ts->Includes(ChangeCategory::Updates) ||
                             ts->Includes(ChangeCategory::Deletes);
        if (keep.ddl.empty() && !anyData) continue;

        out.units.push_back(std::move(keep));
    }
    return out;
}

// The DATA half: the db-layer mirror of the user's per-(table, category)
// choices, which SyncEngine::Execute consults per table by NAME.
//
// THE DELETE PATH, END TO END. `TableExecSpec::Deletes()` can only be true if
// SyncSelection::Build() called TableExecSpec::EnableDeletes(diff, gate) with a
// ui::DeleteGate, and the only producer of one is
// SyncSelection::AcquireDeleteGate(), which returns nullopt while the delete
// master switch is off. So `ts.Deletes()` IS the proof the switch was on — and
// it is exactly that proof which is converted here into the db layer's
// counterpart token via AuthorizeDeletes(). Passing `ts.Deletes()` (rather than
// a separately-carried bool) is deliberate: the token can then only ever be
// minted from the same fact that authorized it, and AuthorizeDeletes returns
// nullopt for the false case, so the optional is dereferenced only inside the
// branch where it is guaranteed engaged. This function is the single place the
// two capability tokens meet.
inline db::sync::DataExecPlan BuildDataExecPlan(const ExecutionSpec& spec)
{
    db::sync::DataExecPlan out;
    for (const TableExecSpec& ts : spec.Tables()) {
        db::sync::TableDataSpec t(ts.Table().Value());

        if (ts.Includes(ChangeCategory::Inserts)) t.EnableInserts();
        if (ts.Includes(ChangeCategory::Updates)) t.EnableUpdates();
        if (ts.Deletes()) {
            if (auto auth = db::sync::AuthorizeDeletes(true)) t.EnableDeletes(*auth);
        }

        // Row-level exclusions travel as the SAME string the data layer's
        // db::sync::EncodeRowKey() produces, so no translation step exists to
        // get wrong: SyncCompareGrid writes db::sync::RowChange::RowKey() into
        // DiffNode::rowKey verbatim, hands that to SyncSelection::
        // SetRowExcluded as a ui::RowKey, and it arrives here unchanged. The
        // chain is a copy from end to end — the string is encoded ONCE, by the
        // producer, and re-derived nowhere.
        for (const RowKey& k : ts.ExcludedRows()) t.ExcludeRow(k.Value());

        if (t.AnyWrite()) out.tables.push_back(std::move(t));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sample rendering (SQL preview)
// ---------------------------------------------------------------------------

// Is this sample row's category one the user checked? Reads RowChange::Op —
// the category the PRODUCER tagged — instead of re-deriving it from text.
inline bool RowAllowedBy(const TableExecSpec& ts, db::sync::RowChange::Op op)
{
    switch (op) {
    case db::sync::RowChange::Op::Insert: return ts.Includes(ChangeCategory::Inserts);
    case db::sync::RowChange::Op::Update: return ts.Includes(ChangeCategory::Updates);
    case db::sync::RowChange::Op::Delete: return ts.Includes(ChangeCategory::Deletes);
    }
    return false;
}

// Everything db::sync::RenderRowDml needs, rebuilt from the unit's own layout
// and dataSpec. Returns false when the table has no usable layout (no data was
// diffed), in which case there are no sample rows to render either.
//
// The qualification rule mirrors SyncEngine::QualifiedTarget: MySQL alone gets
// a db prefix. The dialect comes off a ColumnBridge (`toDialect`), which is the
// target dialect the bridges were built for — not a second guess at it.
inline bool MakeRowRenderSpec(const db::sync::SyncPlan::TableUnit& u,
                              db::sync::RowRenderSpec&             out)
{
    if (u.layout.Empty()) return false;

    const db::Dialect d = u.layout.valueBridges.front().toDialect;

    // THIRD copy of "how do we spell the target table", now removed. This one
    // rendered the SQL PREVIEW, so a disagreement with the executor meant the
    // user was shown a statement against one table while another was written.
    // db::QualifiedTableSql + DataDiffSpec::TargetTable() are the single source
    // the merge, the applier and this preview all read.
    out.qualifiedTable =
        db::QualifiedTableSql(u.dataSpec.tgtDb, u.dataSpec.TargetTable(), d);
    out.columns   = u.layout.columns;
    out.pkColumns = u.layout.pkColumns;
    out.dialect   = d;
    return true;
}

} // namespace ui
