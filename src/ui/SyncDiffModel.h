// SyncDiffModel.h — pure, non-wx tree model for the sync wizard's diff-review
// step (T6 of the cross-engine sync feature). Builds a categorized DiffNode
// tree from a db::sync::SyncPlan: Category -> Table -> Column/Index/Constraint.
//
// "Non-wx" here follows SchemaModel.h's convention: wxString is used (the
// project's baseline string type, not a UI dependency), but nothing here
// touches a wxWindow/wxTreeCtrl/wxColour/etc. That keeps this file testable
// headless (see tests/SyncDiffModelTests.cpp) and reusable by any future
// consumer that isn't wxWidgets at all.
//
// Column/index/FK differences are read from SyncPlan::TableUnit::changes
// (SchemaChangeSet) — NOT parsed out of the rendered `ddl` strings. That
// structured field exists specifically so consumers like this one never have
// to string-sniff SQL text to know what changed (see SyncEngine.h).
//
// Stable identity (Bug #1 fix): every Table-kind DiffNode's `id` is set to
// SyncPlan::TableUnit::table — the ONE key a consumer may use to map a tree
// node back to its plan unit. Category/Column/Index/Constraint/Warning nodes
// carry no id; they are structural or display-only and are never independently
// selectable for execution. A tree row's POSITION (its index among siblings,
// or a flattened row number) must never be used for that purpose — see
// SyncWizardDialog.cpp's header comment for the bug this replaces.
#pragma once

#include <wx/string.h>
#include <memory>
#include <vector>
#include "db/RoutineDiff.h"
#include "db/SyncEngine.h"
#include "ui/I18n.h"

namespace ui {

// ---------------------------------------------------------------------------
// Placeholder tokens for a side that carries no value
// ---------------------------------------------------------------------------
// These live here, next to the producer, rather than in the view: the view
// greys a cell out when it holds one of them, so "what counts as 'no value
// here'" has to be ONE list. Two copies would drift and a placeholder would
// start rendering as if it were data.
//
// AbsentSide     — this side genuinely has no such object/row. An INSERT's
//                  target side, a DROP's source side. A true, final fact.
// UnavailableSide— the side EXISTS but its value did not reach the UI. Not the
//                  same claim, and deliberately worded so it can never be
//                  misread as "the value is blank".
//
//                  This is now a FALLBACK, not the normal path for any case.
//                  db::sync::RowChange carries PriorValues() — the target's
//                  own cells for an Update — so an UPDATE renders both real
//                  sides. It stays reachable because prior values are retained
//                  only for rows that entered the <=500-row sample (see
//                  RowChangeBuilder::KeepPriorValues, whose default-off is the
//                  memory contract that keeps a 500k-row diff from
//                  materializing 500k target rows). A RowChange built outside
//                  RowChangeSet::Accept legitimately has none, and inventing a
//                  target value for it would be worse than saying so.
// UnreadableSide — the value EXISTS on that side and the server refused to show
//                  it to this account. Distinct from both of the above on
//                  purpose: MySQL lists a routine to anyone but returns a NULL
//                  ROUTINE_DEFINITION without privileges (see
//                  db::sync::RoutineDef::bodyReadable), so "we were not allowed
//                  to read this" must never render as an empty body — two
//                  hidden bodies would otherwise look identical.
inline wxString AbsentSide()      { return tr(L"（不存在）"); }
inline wxString UnavailableSide() { return tr(L"（未随比对下发）"); }
inline wxString UnreadableSide()  { return tr(L"（无权查看定义）"); }

inline bool IsPlaceholderSide(const wxString& s)
{
    return s == AbsentSide() || s == UnavailableSide() || s == UnreadableSide();
}

// What changed about a table as a whole. Mirrors the vocabulary the old flat
// list used (New/Drop/Structure/Data/Both) so the visual language carries
// over unchanged.
enum class DiffChangeKind {
    Create,       // table exists in source only -> CREATE TABLE
    Drop,         // table exists in target only, drop-missing opted in -> DROP TABLE
    Structure,    // column/index/FK changes only
    Data,         // row-level DML only, no structural change
    Both,         // structure AND data
    Unchanged,    // present for completeness; nothing to actually do
};

// Add/Modify/Drop for a single Column/Index/Constraint/Row leaf.
enum class DiffOp { Add, Modify, Drop };

// Row      — a DATA difference under a Table node. Two flavours, distinguished
//            only by whether it has ValueDiff children:
//              * a SUMMARY row ("新增 12 行") produced by AppendDataSummary()
//                from the plan's counters — this is what the compare grid's
//                drill-in shows for a data-diff table today, because
//                SyncPlan::TableUnit carries counters (RowStat) and rendered
//                DML strings but NOT per-row before/after values;
//              * a CONCRETE row ("id = 42") produced by MakeRowNode(), whose
//                ValueDiff children are the per-column before/after pairs the
//                数据对比 tab renders side by side.
// ValueDiff — a leaf holding ONE column's两侧 values. `before` is the TARGET's
//            current value/definition, `after` is the SOURCE's. Never a
//            statement string: this is structured data, so the detail views
//            never string-sniff rendered SQL (same rule as the Column/Index
//            leaves — see the file header).
// Routine   — ONE function or stored procedure pair, under the Functions or
//             Procedures category. INFORMATIONAL ONLY, and that is a structural
//             property rather than a styling choice: a Routine node carries no
//             `id` (so it can never be turned into a ui::TableKey), is never
//             `checkable`, and nothing derived from it is read by
//             SyncSelection / ExecutionSpec / DataExecPlan — those are built
//             from db::sync::SyncPlan, which by ADR-013 never contains a
//             routine. Its two sides are its ValueDiff children (签名 / 语言 /
//             定义), the same shape a data row uses, so the side-by-side views
//             render it through the path they already have.
enum class DiffNodeKind { Category, Table, Column, Index, Constraint, Warning, Row, ValueDiff,
                          Routine };

// Object-type categories a diff can belong to. Tables is the executable one;
// Functions/Procedures are compare-and-display only (ADR-013).
enum class DiffCategory { Tables, Functions, Procedures };

// One column's two sides, as handed to MakeRowNode(). Kept a plain value type
// so building these needs neither a db type nor a wx widget — the 数据对比
// view consumes exactly this shape.
struct ValueDiffCell {
    wxString column;
    wxString before;              // 目标当前值 / 目标定义
    wxString after;               // 源值 / 源定义
    bool     differs = false;     // drives the cell highlight; caller decides
};

// One row in the categorized diff tree.
struct DiffNode {
    DiffNodeKind kind = DiffNodeKind::Table;

    // Meaningful only for Column/Index/Constraint/Row leaves.
    DiffOp op = DiffOp::Add;

    wxString label;     // primary display text (table name / column name / warning text)
    wxString summary;   // secondary text (DDL/DML counts, type description, row stats)

    // The two sides being compared. Convention, uniform across every kind that
    // sets them: `before` is the TARGET's current value/definition, `after` is
    // the SOURCE's. Either may be a placeholder token (see AbsentSide /
    // UnavailableSide above).
    //
    // Set on:
    //   ValueDiff — one column's two values within a concrete data row.
    //   Column    — the column's two DEFINITIONS. `before` comes from
    //               db::ColumnChange::current, which exists only when
    //               `hasCurrent` (a Modify against a target column we read);
    //               an Add has no target side and a Drop no source side.
    // Empty on every other kind.
    wxString before;
    wxString after;
    bool     differs = false;

    // STABLE identifier — see file header. Non-empty ONLY for Table nodes;
    // equals db::sync::SyncPlan::TableUnit::table.
    wxString id;

    // STABLE ROW identifier, and the second half of the identity contract.
    // Non-empty ONLY for a CONCRETE Row node built from a real
    // db::sync::RowChange, where it is that row's RowChange::RowKey() COPIED
    // VERBATIM — i.e. db::sync::EncodeRowKey() over the raw, pre-bridge key
    // cells, produced once by the data layer and carried, never re-derived.
    //
    // Re-encoding RowChange::Key() here instead would look equivalent and be
    // wrong: Key() holds POST-conversion cells, so for a coerced cross-engine
    // key (MySQL tinyint(1) 1 -> PostgreSQL TRUE) the re-encoded string does
    // not match the one db::sync::TableDataSpec::ExcludeRow is matched against,
    // and a row the user unchecked would sync anyway — a silent miss, in the
    // dangerous direction. Empty on SUMMARY Row nodes (「修改 5 行」 is a
    // counter, not a row) and on every other kind, and a row with no key is
    // never offered a per-row checkbox.
    wxString rowKey;

    // Category-node-only. Which of the three object-type categories this
    // header row is. Read by the grid to decide whether the category is
    // FLATTENED AWAY (Tables — its table children become the grid's top-level
    // rows) or kept as a visible group header (Functions / Procedures, whose
    // children are informational and want a heading that says so).
    DiffCategory category = DiffCategory::Tables;

    // The 状态 column's text for the kinds that have no TableDiff behind them:
    //   Category — the routine read status ("不可见（权限不足）"), or empty when
    //              the category was read normally;
    //   Routine  — db::sync::RoutineVerdictLabel(verdict).
    // Empty on every other kind, which keeps the table rows' 状态 column
    // derived from structured flags exactly as before (SyncCompareRow.h).
    wxString status;

    // Routine-node-only, and deliberately the data layer's own enum rather than
    // a second UI vocabulary parsed back out of `status`. The detail pane has to
    // present cross-engine NotComparable differently from same-engine
    // BodyDiffers, and deciding that by string-matching a translated label is
    // exactly the string-sniffing this model forbids everywhere else.
    db::sync::RoutineVerdict verdict = db::sync::RoutineVerdict::Identical;

    // Table-node-only classification/flags.
    DiffChangeKind change = DiffChangeKind::Unchanged;
    bool destructive = false;   // this table's plan carries a DROP/DELETE/TRUNCATE statement
    bool checkable   = false;   // only Table nodes are independently selectable
    bool checked     = true;    // current UI check state; seeded from !destructive

    // Column/Index/Constraint/Row children (Table nodes only) — the field-level
    // detail the diff-review "点开表看哪里不一致" requirement needs. A Row node
    // in turn owns its ValueDiff leaves.
    std::vector<std::unique_ptr<DiffNode>> children;
};

// The whole tree handed to the UI. `roots` holds, in order: one Category node
// per populated category ("表" always; "函数" and "存储过程" when a routine
// comparison was requested — see MakeRoutineCategoryNode), then — if the plan carries any
// — one Warning group node whose children are one leaf per
// SyncPlan::warnings entry (Bug #2: cross-engine "table missing, can't
// auto-CREATE" findings that produce a warning but no TableUnit must still
// surface, never silently vanish behind a flat plan_.Empty() check).
struct DiffTree {
    std::vector<std::unique_ptr<DiffNode>> roots;
};

// ---------------------------------------------------------------------------
// Row / ValueDiff builders (T7)
// ---------------------------------------------------------------------------

// One ValueDiff leaf. `cell.column` becomes the node label.
std::unique_ptr<DiffNode> MakeValueDiffNode(const ValueDiffCell& cell);

// One CONCRETE data row plus its per-column before/after leaves. `rowLabel` is
// a human-readable row identity ("id = 42") — display only, never SQL, and
// never a row POSITION (see the file header's identity contract; a row's index
// in a sample is not an identity).
//
// `rowKey` is the MACHINE identity that per-row exclusion is keyed on, and must
// be db::sync::RowChange::RowKey() verbatim — see DiffNode::rowKey. Defaulted
// empty for the callers that build a display-only row (tests, summary shapes);
// a node with an empty rowKey is never offered a checkbox, so the default is
// fail-closed rather than fail-silent.
std::unique_ptr<DiffNode> MakeRowNode(DiffOp op, const wxString& rowLabel,
                                      const std::vector<ValueDiffCell>& cells,
                                      const wxString& rowKey = wxString());

// Append 新增N / 修改N / 删除N summary Row children to a Table node, skipping
// zero counters. This is the data half of drill-in: expanding a data-diff
// table in the compare grid shows these, exactly as expanding a
// structure-diff table shows its Column/Index/Constraint leaves.
void AppendDataSummary(DiffNode& table, const db::sync::RowStat& stat);

// Append CONCRETE Row children built from a table's capped sample of real
// db::sync::RowChange objects, each with its per-column ValueDiff leaves. This
// is what gives the 数据对比 tab actual per-row values instead of counters.
//
// Bounded by construction: RowChangeSet never holds more than kSampleCap (500)
// rows, so this cannot become the memory bug the structured carrier exists to
// fix. `rows` and `layout` are the unit's own — the layout supplies the column
// and primary-key NAMES the sample's positional cells are matched against, so
// nothing here pairs a value with a name by guessing.
void AppendSampleRows(DiffNode& table, const db::sync::RowChangeSet& rows,
                      const db::sync::RowLayout& layout);

// ---------------------------------------------------------------------------
// Tree construction
// ---------------------------------------------------------------------------

struct DiffTreeOptions {
    // Emit AppendDataSummary() children for tables with row changes. ON by
    // default because a data-diff table with no children is a dead end in the
    // UI. Set false to get the structure-only tree (the shape Round 1's tree
    // widget produced, and the shape the pre-T7 tests assert on).
    bool includeDataSummary = true;

    // Also emit one CONCRETE Row child per row in each unit's capped sample
    // (AppendSampleRows), which is what the 数据对比 tab renders as real
    // before/after values. ON by default: with it off, drilling into a data
    // difference reaches counters and stops there. Bounded at
    // db::sync::RowChangeSet::kSampleCap (500) rows per table by the carrier
    // itself, not by anything this model does.
    bool includeSampleRows = true;
};

// Build the tree from a freshly-compared plan. Pure function, no I/O.
DiffTree BuildDiffTree(const db::sync::SyncPlan& plan, const DiffTreeOptions& opts = {});

// ---------------------------------------------------------------------------
// Functions & stored procedures (ADR-013) — COMPARE AND DISPLAY ONLY
// ---------------------------------------------------------------------------
//
// The label a Routine node's 定义 child carries. Exposed so the detail pane can
// find the body row structurally (it renders that one row tall and wrapped)
// instead of guessing at "the last child".
inline wxString RoutineBodyRowLabel() { return tr(L"定义"); }

// Build the 函数 / 存储过程 category node for ONE kind, ready to be pushed into
// DiffTree::roots.
//
// WHAT THIS DOES NOT DO, and why the omission is the design:
//   * it never sets `id`, so no routine node can be converted into a
//     ui::TableKey — ui::StableId's only constructor takes the node's id, and a
//     routine has none to give;
//   * it never sets `checkable`, so the grid renders no checkbox;
//   * it copies db::sync::RoutineDef::body as DISPLAY TEXT into a ValueDiff
//     leaf. That string is the routine's inner source, not a CREATE statement,
//     and this file has no way to reach an executor even if it were.
//
// DEGRADATION. When `routines.Comparable()` is false the category is returned
// with its status label and ZERO children — never a partial list. A
// one-sided routine list read off a side we could not see is a false claim, not
// an incomplete one.
//
// Only DIFFERENCES become children (RoutineDiffSet::DifferencesByKind); the
// identical ones are accounted for in the category's `summary` counts so that
// "nothing listed here" is never ambiguous between "all the same" and
// "we didn't look".
std::unique_ptr<DiffNode> MakeRoutineCategoryNode(db::sync::RoutineKind kind,
                                                  const db::sync::RoutineDiffSet& routines);

// Plan tree + the two routine categories, in reading order: 表, 函数, 存储过程,
// then any warnings. Overload rather than a defaulted parameter so the existing
// two-argument BuildDiffTree(plan, opts) calls stay unambiguous.
DiffTree BuildDiffTree(const db::sync::SyncPlan& plan,
                       const db::sync::RoutineDiffSet& routines,
                       const DiffTreeOptions& opts = {});

} // namespace ui
