// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// RowChange.h — the carrier for per-row DATA changes (ADR-015 T3).
//
// Replaces SyncPlan::TableUnit's single `dml` wxString, which materializes every
// generated statement: a 500k-row diff builds 500k wxStrings before anything is
// executed or even shown. A RowChangeSet instead keeps (a) counters, which is all
// the review grid actually needs, and (b) a CAPPED sample of real rows for
// drill-in and SQL preview. Statement text is rendered on demand, only over the
// sample. (The SyncEngine rewire that consumes this is a separate task.)
//
// ---------------------------------------------------------------------------
// THE STRUCTURAL GATE — the entire point of this header
// ---------------------------------------------------------------------------
// Round 1 made an unsafe schema verdict structurally incapable of producing a
// ColumnChange (SchemaDelta::AddColumnChange is the only door, and it refuses
// !MayAutoAlter). This header holds the same bar for values, via four
// reinforcing mechanisms:
//
//   1. RowChange has a PRIVATE default constructor and PRIVATE cell storage with
//      const-only accessors. There is no public way to construct one, and no
//      mutator of any kind. `RowChange r;` does not compile outside the builder.
//   2. The sole friend is RowChangeBuilder, whose Build() returns
//      std::optional<RowChange> and yields nullopt whenever ANY cell of the row
//      came back !MayEmit. An Unrepresentable cell therefore fails the WHOLE ROW
//      — a partially converted row is worse than an absent one.
//   3. EMITTABLE cells enter a builder ONLY through AddCell/AddKeyCell, which
//      call ConvertCell themselves. A caller cannot hand over a pre-converted
//      cell and thereby bypass the verdict; the verdict is computed inside the
//      gate, not supplied to it. (RowChange::PriorValues() is the one set of
//      cells that does NOT go through ConvertCell — precisely because it is
//      never emitted. See its comment; it is unreachable from RenderRowDml and
//      from the executor, which read Values()/Key() only.)
//   4. RowChangeSet::Executable() is DERIVED (`!blocked_`), not stored. There is
//      no `executable` field to flip — not by the builder, not by anyone. It
//      becomes false the instant a blocked row or blocking Finding is recorded
//      and cannot be turned back on.
//
// Consequence: "someone later writes code that accidentally executes an
// unconvertible row" is not a code-review question. To do it they would have to
// obtain a RowChange for that row, and the only expression that yields one is an
// optional that is empty in exactly that case.
#pragma once

#include "db/SchemaDelta.h"    // db::sync::Finding
#include "db/SyncTypes.h"      // Cell
#include "db/SyncValueMap.h"   // ValueVerdict / MayEmit / ColumnBridge / ConvertCell

#include <wx/string.h>
#include <cstddef>
#include <optional>
#include <vector>

namespace db { enum class Dialect; }   // opaque; see SyncValueMap.h

namespace db::sync {

// The review grid's numbers.
//
// NOTE (contradiction with the ADR sketch, deliberately resolved here): the
// sketch called this `RowStat`, but db::sync::RowStat ALREADY exists in
// DataSync.h with a different shape (it carries a fourth `unchanged` counter).
// Redefining that name in the same namespace would be an ODR violation, and
// including DataSync.h here would both drag DbDriver.h into the UI's include
// path and couple this header to a file being refactored in parallel. Hence the
// distinct name. `blocked` is new and has no counterpart in the sketch: rows
// that were detected as differences but refused by the gate must be visible as a
// number, or they would silently vanish from the review UI.
struct RowChangeStat {
    long long inserts = 0;
    long long updates = 0;
    long long deletes = 0;
    long long blocked = 0;   // detected, but not emittable — see findings

    // Rows that matched on both sides and compared EQUAL.
    //
    // Not a counter the execution path uses — nothing is done to these rows —
    // but the one number that tells a CLEAN compare apart from a compare that
    // read nothing. "0 differences" out of 100,000 unchanged rows and
    // "0 differences" because the scan returned no rows at all are the same
    // display today, and they mean opposite things: the first is a healthy
    // result, the second is a silent failure (wrong database, empty source,
    // a filter that excluded everything). The merge has always counted this —
    // DiffRowChanges filled a local RowStat and threw it away — so the value
    // was measured and then discarded before any caller could see it.
    //
    // NOT included in Total(), deliberately: Total() is "how many rows this
    // change set will act on", and an unchanged row is acted on by nothing.
    long long unchanged = 0;

    long long Total() const { return inserts + updates + deletes; }
};

// One emittable row change. Constructible ONLY by RowChangeBuilder (mechanism 1
// + 2 above). Copy/move are public and harmless: copying a valid RowChange
// yields a valid RowChange, and there is no mutator to degrade one afterwards.
class RowChange {
public:
    enum class Op { Insert, Update, Delete };

    Op Operation() const { return op_; }

    // Converted, target-dialect-ready cells for every bridged column, in bridge
    // order. Empty for Delete (a delete carries only its key).
    const std::vector<Cell>& Values() const { return values_; }

    // Converted primary-key cells, in the table's primaryKey order. Used to
    // build the WHERE clause of Update/Delete.
    const std::vector<Cell>& Key() const { return key_; }

    // ---- DISPLAY-ONLY: the target's values BEFORE this change --------------
    //
    // For an Update, the row that is currently in the target, cell-for-cell
    // aligned with Values() (both are in the layout's `columns` order), so a
    // consumer renders column i as PriorValues()[i] -> Values()[i]. Empty for
    // Insert (there is nothing there yet) and for Delete (Values() is empty
    // too; the key IS the row). Empty as well for any row the producer chose
    // not to materialize — see RowChangeBuilder::KeepPriorValues.
    //
    // THESE CELLS ARE NOT CONVERTED, DELIBERATELY, AND MUST NEVER BE EMITTED:
    //   * They were read off the TARGET stream, so they are already in the
    //     target's own dialect. Pushing them through a source->target bridge
    //     would convert a value that was never on the source side — the exact
    //     error DataSync.h's tgtKeyBridges comment exists to prevent.
    //   * They are never re-emitted: RenderRowDml and the executor read
    //     Values()/Key() only. A conversion verdict on them would therefore be
    //     a verdict about nothing.
    //   * Worse, running them through the gate would let a DISPLAY concern
    //     block EXECUTION: a target-side value that happens to be
    //     Unrepresentable under the src->tgt bridge would fail the whole row,
    //     refusing an update that is perfectly safe to apply.
    // The safety story is unchanged because these cells are structurally out of
    // the emission path, not because they passed a check.
    const std::vector<Cell>& PriorValues() const { return prior_; }
    bool HasPriorValues() const { return !prior_.empty(); }

    // The row's canonical identity: EncodeRowKey() over the RAW, pre-bridge key
    // cells, i.e. byte-for-byte the string RowChangeSink receives and the string
    // ui::RowKey carries. Retained rather than re-derived from Key(), because
    // Key() holds POST-conversion cells: re-encoding it reproduces the original
    // only while every key bridge is passthrough. For a coerced cross-engine key
    // (MySQL tinyint(1) -> PG boolean rewrites 1 into TRUE) it silently would
    // not, and a row-level exclusion keyed off it would miss — letting a row the
    // user unchecked sync anyway. One encoding, produced once, carried.
    const wxString& RowKey() const { return rowKey_; }

    // The row's aggregate verdict: Exact when every cell crossed untouched,
    // Coerced when at least one was rewritten. Never Unrepresentable — such a
    // row cannot become a RowChange at all.
    ValueVerdict Verdict() const { return verdict_; }

private:
    RowChange() = default;                 // <- the gate: no public construction
    friend class RowChangeBuilder;

    Op                op_ = Op::Insert;
    std::vector<Cell> values_;
    std::vector<Cell> key_;
    std::vector<Cell> prior_;              // display only; never emitted
    wxString          rowKey_;
    ValueVerdict      verdict_ = ValueVerdict::Exact;
};

// The only factory for RowChange. Single-use: Build() consumes the builder.
//
// Usage (the standard pattern, mirroring SchemaDiff.cpp's TryAddColumnChange):
//     RowChangeBuilder b(RowChange::Op::Insert, table, rowKeyText);
//     for (const auto& br : bridges) b.AddCell(srcRow[br.srcIdx], br);
//     set.Accept(std::move(b));          // records the row OR its Finding
class RowChangeBuilder {
public:
    // `rowKey` is the row's identity. It is used for two things and NEVER in
    // SQL: it prefixes this row's Finding text ("行 id=42：…"), and it becomes
    // RowChange::RowKey(). Producers that care about row-level exclusion must
    // pass exactly what they hand the RowChangeSink — DataSync.cpp passes the
    // one EncodeRowKey() result to both, which is what makes the encoding
    // single-sourced instead of re-derivable-and-sometimes-wrong.
    RowChangeBuilder(RowChange::Op op, wxString table, wxString rowKey)
        : table_(std::move(table)), rowKeyText_(std::move(rowKey))
    {
        row_.op_ = op;
    }

    // Convert `in` through `bridge` and append it to the row's value list.
    // Returns false when the cell (or an earlier one) was !MayEmit — the row is
    // then permanently blocked and Build() will yield nullopt. Callers may keep
    // calling; subsequent calls short-circuit.
    bool AddCell(const Cell& in, const ColumnBridge& bridge);

    // Same, for a primary-key cell. A blocked key blocks the row identically —
    // an UPDATE/DELETE with a mis-converted key is the most dangerous statement
    // this system could possibly emit.
    bool AddKeyCell(const Cell& in, const ColumnBridge& bridge);

    // Point the builder at the target row this Update replaces, for
    // RowChange::PriorValues(). NON-OWNING and NOT COPIED here: `targetRow`
    // must merely outlive Build(), which is trivially true at the one call site
    // (DataSync.cpp's merge callback, where the row is a MergeRows local). Not
    // copying is the point — see KeepPriorValues.
    void AttachPriorValues(const std::vector<Cell>* targetRow) { prior_ = targetRow; }

    // Whether Build() actually MATERIALIZES the attached prior row. Default
    // FALSE, and that default is the memory contract: the execute pass streams
    // every changed row and never displays anything, so it must not pay a
    // vector copy per row. The only opt-in is RowChangeSet::Accept, which turns
    // it on exactly for the rows that will enter the <=500-row sample. A 500k
    // row diff therefore materializes prior values for 500 rows, not 500k.
    void KeepPriorValues(bool on) { keepPrior_ = on; }

    bool Blocked() const { return blocked_; }

    // Findings accumulated while converting (both the blocking Unrepresentable
    // ones and, when the caller asked for an audit trail, the Coerced notes).
    const std::vector<Finding>& Findings() const { return findings_; }

    // Opt in to recording a Finding for merely-Coerced cells too. Off by default:
    // one Finding per coerced cell over a large table is itself a memory bug.
    void RecordCoercions(bool on) { recordCoercions_ = on; }

    // The ONLY expression in the codebase that yields a RowChange. Empty exactly
    // when Blocked(). Consumes the builder's row.
    std::optional<RowChange> Build();

private:
    bool Convert(const Cell& in, const ColumnBridge& bridge, std::vector<Cell>& into);

    RowChange            row_;
    wxString             table_;
    wxString             rowKeyText_;
    std::vector<Finding> findings_;
    const std::vector<Cell>* prior_ = nullptr;   // non-owning; see AttachPriorValues
    bool                 blocked_ = false;
    bool                 built_ = false;
    bool                 recordCoercions_ = false;
    bool                 keepPrior_ = false;
};

// Per-table data-change result: counters + a capped sample + the audit trail.
//
// Every mutator maintains the executability invariant; there is no raw access to
// the members and no `executable` flag to set (mechanism 4 above).
class RowChangeSet {
public:
    // Sample cap. The sample exists for drill-in and SQL preview ONLY — it is
    // never the source of truth for how many rows changed (Stat() is), and the
    // execution path streams rows rather than replaying the sample.
    static constexpr size_t kSampleCap = 500;

    // The only way a row enters the sample. Consumes the builder:
    //   * builder blocked  -> stat.blocked++, its Findings are absorbed, the set
    //                         becomes permanently non-executable, returns false.
    //   * otherwise        -> counters bump, row appended while under the cap
    //                         (`truncated` set once the cap is hit), returns true.
    //
    // Also the single place RowChangeBuilder::KeepPriorValues is turned on, and
    // it is turned on ONLY for a row that is about to enter the sample. Rows
    // past the cap are counted and dropped exactly as before, carrying nothing.
    bool Accept(RowChangeBuilder&& builder);

    // Record a Finding that did not come from a row (e.g. BuildBridges' column
    // level verdicts). A !MayEmit verdict blocks the whole set, exactly as a
    // blocked row does.
    void AddFinding(Finding f, ValueVerdict verdict);

    // Same, for the schema track's TypeVerdict ladder (BuildBridges emits those).
    void AddFinding(Finding f);

    // Record the merge's unchanged-row count. Separate from Accept() because an
    // unchanged row never becomes a RowChangeBuilder — the merge classifies it
    // and moves on — so there is no builder for Accept() to count. The scan
    // knows the number; this is the only door it has into the set.
    //
    // Cannot affect executability: it touches one display counter and nothing
    // else, so the invariant Accept()/AddFinding() maintain is untouched.
    void SetUnchanged(long long n) { stat_.unchanged = n; }

    const RowChangeStat&        Stat() const { return stat_; }
    const std::vector<RowChange>& Sample() const { return sample_; }
    const std::vector<Finding>& Findings() const { return findings_; }
    bool Truncated() const { return truncated_; }

    // Derived, never stored. False once anything blocking has been recorded, and
    // there is no path back to true.
    bool Executable() const { return !blocked_; }

    bool Empty() const { return stat_.Total() == 0 && stat_.blocked == 0; }

private:
    RowChangeStat          stat_;
    std::vector<RowChange> sample_;
    std::vector<Finding>   findings_;
    bool                   truncated_ = false;
    bool                   blocked_ = false;
};

// ---------------------------------------------------------------------------
// Rendering (bounded by construction: only ever applied to the <=500 sample)
// ---------------------------------------------------------------------------

// Names needed to turn a RowChange back into SQL. `columns` must be in the same
// order as the ColumnBridge vector the row was built from; `pkColumns` in the
// same order as the key cells.
struct RowRenderSpec {
    wxString              qualifiedTable;   // already quoted+qualified by the caller
    std::vector<wxString> columns;
    std::vector<wxString> pkColumns;
    Dialect               dialect{};
};

// One INSERT/UPDATE/DELETE statement (no trailing semicolon), or an empty string
// if the row and the spec disagree on arity (defensive; never expected).
wxString RenderRowDml(const RowChange& row, const RowRenderSpec& spec);

} // namespace db::sync
