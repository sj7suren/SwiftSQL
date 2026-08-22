// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DataSync.h — PK-ordered streaming data diff for the cross-database sync suite
// (see docs/design/cross-db-sync.md §4.5).
//
// Both sides are streamed ordered by primary key; a lock-step merge classifies
// each row as INSERT (src only) / DELETE (tgt only) / UPDATE (same PK, non-PK
// columns differ) / unchanged. Memory stays flat: rows are pulled through a
// bounded queue, never buffering a whole table. DML is rendered with
// RenderLiteral + QuoteIdent against the *target* dialect.
//
// ---- ordering: the one silent failure mode in this feature (ADR-015 T2) ----
// A sorted merge is correct ONLY if both servers return rows in the SAME total
// order, and in the same order the merge itself compares keys with (CmpCell,
// code-point). They do not agree by default: MySQL's stock utf8mb4_general_ci is
// case- and accent-insensitive, PostgreSQL's default collation is neither. A
// desynced merge does not fail — it emits INSERTs for rows that already exist
// and DELETEs for rows that were never missing, reports success, and corrupts
// the target. Every other failure here is loud; this one is not. Three defenses,
// all mandatory, none sufficient alone:
//   1. StreamOptions::binaryOrder forces byte ordering at the query level, so
//      both servers agree with each other AND with CmpCell (see SyncTypes.h).
//   2. CheckDataDiffOrdering() REFUSES a table whose PK cannot be byte-ordered
//      on both sides, with a Finding — never a silent attempt.
//   3. DiffAndEmitData() asserts, per row, that neither cursor's key ever moves
//      backwards, and aborts the table if one does — the net for whatever 1-2
//      missed, turning silent corruption into a loud error.
#pragma once

#include "db/DbDriver.h"
#include "db/RowChange.h"     // RowChange / RowChangeSet — the structured sink (T9)
#include "db/SchemaDelta.h"   // Finding — how a refused table is reported
#include "db/SyncCellCompare.h" // CompareRules — dialect-aware cell comparison
#include <atomic>
#include <functional>

namespace db::sync {

struct RowStat { long long inserts = 0, updates = 0, deletes = 0, unchanged = 0; };

// Fired from inside the merge loop (see DataSyncOptions::progress). rowsScanned
// counts rows pulled from BOTH cursors; rowsEstimated is whatever the caller
// supplied (-1 = unknown, render an indeterminate gauge).
using ProgressFn = std::function<void(const wxString& table,
                                      long long rowsScanned,
                                      long long rowsEstimated)>;

struct DataSyncOptions {
    bool      insert        = true;
    bool      update        = true;
    bool      deleteMissing = false;   // safety red line: off by default
    long long batchRows     = 1000;    // bounds the in-flight row queue
    // Intra-table progress. Firing only once per table leaves a single 10M-row
    // table showing a frozen gauge for minutes; this fires every ~4096 scanned
    // rows instead. Empty = no progress reporting (the historical behavior).
    ProgressFn progress;
    long long  rowsEstimated = -1;     // passed straight through to progress
};

using StmtSink = std::function<void(const wxString&)>;

// Defense 2. Decide whether `schema`'s primary key can be ordered identically by
// both engines, and hence whether this table may be data-diffed at all.
//
// Collation only affects *text* keys. Integer / decimal / date-time / binary /
// UUID keys order deterministically and identically everywhere, and are ~95% of
// real primary keys — those are always allowed, no dialect support needed. A
// text key is allowed only when BOTH dialects can be forced to byte order
// (MySQL / PostgreSQL via StreamOptions::binaryOrder, SQLite natively — its
// default BINARY collation already IS byte order). Anywhere else, and for keys
// whose type we cannot classify at all, the table is refused: a Finding is
// appended to `out` and the function returns false. Refusing is the whole point
// — the alternative is attempting it and corrupting data.
//
// Also fills `binaryOrderCols` with the PK columns that need an explicit
// byte-order COLLATE in the ORDER BY (i.e. the text ones), which is what
// StreamOptions::binaryOrderCols wants.
bool CheckDataDiffOrdering(const TableSchema& schema, Dialect srcDialect,
                           Dialect tgtDialect, std::vector<Finding>& out,
                           std::vector<wxString>& binaryOrderCols);

// Diff `schema`'s data src → tgt and emit INSERT/UPDATE/DELETE statements.
//
// ---- STATUS: no production caller, and that is deliberate ------------------
// Nothing under src/ calls this. The shipping data path is DiffRowChanges()
// (compare) + ExecuteDataSync() (apply), both driven from SyncEngine. This
// function is retained as a SUPPORTED TEXT-RENDERING ENTRY POINT, not as dead
// code awaiting deletion, for two reasons:
//
//   1. It is the only entry point that renders the merge's decisions as SQL
//      TEXT. The structured path deliberately never builds a statement string
//      (the executor sends parameterized batches), so SQL preview and any
//      "show me the statements" diagnostic have nothing else to call.
//   2. It is the vehicle DataSyncOrderingTests.cpp drives the three ordering
//      defenses through — the suite guarding this project's worst defect, a
//      desynced sorted merge that emitted 10 spurious INSERTs and 10 DELETEs
//      from 11 identical rows. Those defenses live in the shared merge core
//      that BOTH paths use, so exercising them here exercises them for
//      DiffRowChanges too. Deleting this would delete that coverage.
//
// Consequence for maintainers: it shares the merge core with DiffRowChanges,
// so a change to the merge is still covered here — but its own STATEMENT TEXT
// is pinned by tests and shipped to nobody. Do not add behaviour here expecting
// the product to pick it up; add it to the structured path.
//
// Requires a primary key (schema.primaryKey non-empty) — returns false with err
// otherwise (the caller gates no-PK tables). stat counts *detected* differences
// regardless of which options are enabled; emit() fires only for enabled ops.
//
// ---- CONTRACT ASYMMETRY vs. DiffRowChanges (they are NOT interchangeable) --
// These two neighbours disagree on what a missing primary key MEANS, and a
// caller that assumes they behave alike will mis-handle exactly the table that
// most needs handling:
//
//   * HERE, a no-PK table is a HARD ERROR: returns false, `err` set. Right for
//     a text-emitting call — there is no statement to render, so there is
//     nothing to hand back.
//   * In DiffRowChanges(), a no-PK table is a STRUCTURAL VERDICT: returns
//     true, `err` EMPTY, and the refusal is recorded on the result set
//     (Executable() == false, Findings() non-empty). Right for the compare
//     screen — the table must still REACH the plan to be displayable, and a
//     table dropped on the floor cannot be shown to the user at all.
//
// So `false` here and `true` there describe the SAME table. Checking only the
// bool after DiffRowChanges() will treat a refused table as a successful empty
// diff; check Executable() as well.
//
// Runs CheckDataDiffOrdering() itself and returns false if the table is refused
// (defense 2), streams both sides with binaryOrder set (defense 1), and aborts
// with an error the moment either cursor's key moves backwards (defense 3).
//
// stop is honored between rows AND inside the row cursors' blocking waits, so a
// cancel is acknowledged promptly rather than after the current server response
// finally arrives; the caller's join is therefore bounded.
bool DiffAndEmitData(IConnection& src, IConnection& tgt,
                     const wxString& srcDb, const wxString& tgtDb,
                     const TableSchema& schema, const DataSyncOptions& opt,
                     const StmtSink& emit, RowStat& stat, wxString& err,
                     const std::atomic<bool>& stop);

// Target table doesn't exist yet (it will be CREATE'd by the structure phase):
// every source row is an INSERT. Streams the source once; no target read, no PK
// needed. Used by the orchestrator for freshly-created tables.
bool EmitInsertAll(IConnection& src, IConnection& tgt,
                   const wxString& srcDb, const wxString& tgtDb,
                   const TableSchema& schema, const DataSyncOptions& opt,
                   const StmtSink& emit, RowStat& stat, wxString& err,
                   const std::atomic<bool>& stop);

// ===========================================================================
//  T9 — the STRUCTURED sink. The two functions above render statement TEXT and
//  are kept only for the SQL-preview/live-test paths that genuinely want text;
//  everything that decides or executes now goes through the types below.
//
//  Why this exists: a statement sink forces the producer to materialize one
//  wxString per changed row. A 500k-row diff therefore built 500k strings
//  before the user had even looked at the result. It also erased the change's
//  CATEGORY (a consumer had to recover "is this a DELETE?" by sniffing the
//  leading verb) and bypassed the value-conversion gate entirely, because
//  RenderLiteral will happily render a value that cannot legally cross into the
//  target engine. Emitting RowChange instead fixes all three: the row is built
//  through RowChangeBuilder (so the gate runs at the point of production, not
//  at the point of use), it carries its Op, and the caller decides what — if
//  anything — to retain.
// ===========================================================================

// Everything needed to (re-)run one table's data diff. Deliberately a VALUE:
// the compare pass hands one of these to the execute pass, which replays the
// diff against live connections rather than replaying a materialized statement
// list. See DataSyncExec.h for the honest consequence of re-reading.
struct DataDiffSpec {
    wxString    srcDb, tgtDb;
    TableSchema srcSchema;             // source side; supplies PK + column order
    TableSchema tgtSchema;             // target side; supplies the bridge targets
    // Target table does not exist yet (the structure phase will create it):
    // every source row is an INSERT and the target is never read. `tgtSchema`
    // should then be the schema the target WILL have.
    bool        targetMissing = false;

    // -----------------------------------------------------------------------
    // THE TWO SIDES, NAMED ONCE
    // -----------------------------------------------------------------------
    // These two accessors are the ONLY sanctioned way to learn which table a
    // side refers to, and that is a correctness mechanism rather than a style
    // preference.
    //
    // What went wrong before: the merge (DataSync.cpp's MergeParams) carried ONE
    // `table` field and used it to open BOTH cursors, so it read the target
    // using the SOURCE's name — while the applier (DataSyncExec.cpp's
    // TableApplier) wrote to the TARGET's name. Same-named tables are the only
    // configuration in which those two agree, and SyncEngine only ever builds
    // specs that way, so the disagreement was invisible. A caller that paired
    // `orders` with `orders_v2` would have READ orders and WRITTEN orders_v2:
    // a silent wrong-table execution, and the write half of it.
    //
    // The structural guarantee is not "we carry two names" — it is that there is
    // exactly ONE expression for the target's identity, and the target READ and
    // the target WRITE both evaluate it. They cannot drift apart, because there
    // is no second place for either to disagree with. MergeParams now carries
    // srcTable/tgtTable filled from these, and TableApplier renders from
    // TargetTable(); neither reaches into a schema's `name` directly.
    //
    // Authority: the source side's name governs every read from `src`, the
    // target side's every read from AND write to `tgt`.
    const QualifiedName& SourceTable() const { return srcSchema.name; }

    // The target's identity. Falls back to the source's name ONLY when the
    // target schema has no name of its own, which is the targetMissing case —
    // the table is about to be CREATEd and the creator names it after the
    // source. That fallback is why this is a function and not a field: it is one
    // decision, made once, seen identically by the reader and the writer, rather
    // than a rule two files had each implemented for themselves.
    const QualifiedName& TargetTable() const
    {
        return tgtSchema.name.IsEmpty() ? srcSchema.name : tgtSchema.name;
    }
};

// The per-table conversion plan, computed ONCE (BuildBridges is per column
// pair, never per cell) plus the target-side names needed to render or bind a
// RowChange. Both sides are streamed with exactly `columns`, in this order, so
// a bridge's srcIdx indexes both rows identically and the merge can compare
// them positionally.
struct RowLayout {
    std::vector<ColumnBridge> valueBridges;   // one per synced column, row order
    std::vector<ColumnBridge> keyBridges;     // one per PK column, PK order
    std::vector<ColumnBridge> tgtKeyBridges;  // ditto, but IDENTITY — see below
    std::vector<wxString>     columns;        // column names, valueBridges order
    std::vector<wxString>     pkColumns;      // PK column names, keyBridges order
    std::vector<size_t>       pkIdx;          // PK positions within `columns`

    // How each column's cells must be canonicalized BEFORE the merge compares
    // them — indexed like `columns`, derived once from valueBridges. Without
    // this the merge compared raw cells, so a MySQL tinyint(1) 1 and a
    // PostgreSQL boolean 't' reported a phantom UPDATE (and, as a key, failed to
    // match at all and produced DELETE + INSERT for an unchanged row). Every
    // rule is Raw for same-engine sync, and `allRaw` then lets the merge take
    // the historical path with no per-cell cost. See SyncCellCompare.h.
    CompareRules              compare;

    bool Empty() const { return valueBridges.empty(); }
};

// Build the layout for one table pair. Returns false — with blocking Findings
// appended to `findings` — when the table's data cannot be synced at all (no
// usable column overlap, a PK column absent from the target, or a column pair
// whose types cannot carry values).
//
// The tgtKeyBridges subtlety, which is a correctness matter and not a detail:
// a DELETE's key comes off the TARGET stream, so it is already target-native.
// Pushing it through a source->target bridge would convert a value that was
// never on the source side — e.g. re-encoding an already-PG boolean — and the
// resulting WHERE clause would match the wrong row or no row. tgtKeyBridges are
// therefore identity bridges: they still go through ConvertCell (so the gate is
// uniform and nothing routes around RowChangeBuilder) but on its passthrough
// fast path.
bool BuildRowLayout(const DataDiffSpec& spec, Dialect srcDialect,
                    Dialect tgtDialect, RowLayout& out,
                    std::vector<Finding>& findings);

// Fired once per DETECTED difference whose category is enabled in
// DataSyncOptions. Return false to abort the table (the caller's own error text
// wins; `err` gets a generic abort message otherwise).
//
// It hands over the BUILDER, not a finished RowChange, and that is deliberate:
// RowChangeBuilder::Build() is the only expression in the tree that yields a
// RowChange, and RowChangeSet::Accept(builder&&) is the only door into a set.
// Handing over a built row would mean this file had already called Build() and
// would then need a second, parallel way to report a row the gate refused —
// i.e. a second door. Instead the producer fills the builder (so every cell
// goes through ConvertCell inside the gate) and the CONSUMER decides what a
// blocked row means for it: the compare pass counts it and latches the set
// non-executable, the execute pass aborts.
//
// `rowKey` is EncodeRowKey() over this row's RAW (pre-bridge) primary key — the
// same identity the UI's ui::RowKey carries, so per-row exclusions can be
// applied by the executor without any re-encoding. The identical string is also
// handed to the builder and surfaces as RowChange::RowKey(), so a consumer that
// keeps rows (the compare pass's sample) and a consumer that streams them (the
// execute pass) are comparing the same encoding — NOT one derived from
// RowChange::Key(), whose cells are post-conversion.
using RowChangeSink =
    std::function<bool(RowChangeBuilder&& builder, const wxString& rowKey)>;

// The shared streaming core of BOTH passes. Runs the same sorted merge (and the
// same three ordering defenses) as DiffAndEmitData, but hands the caller
// structured rows and retains nothing itself — memory is flat regardless of how
// many rows differ.
//
// `stat` counts every DETECTED difference, including categories switched off in
// `opt`; `sink` fires only for enabled categories. That split is what lets the
// compare screen show a delete count while the delete master switch is off.
bool StreamRowChanges(IConnection& src, IConnection& tgt,
                      const DataDiffSpec& spec, const RowLayout& layout,
                      const DataSyncOptions& opt, const RowChangeSink& sink,
                      RowStat& stat, wxString& err,
                      const std::atomic<bool>& stop);

// PASS 1 (compare). Full scan for exact counts; materializes at most
// RowChangeSet::kSampleCap (500) rows into out.Sample(), setting out.Truncated()
// once the cap is passed. Builds the layout itself and folds its Findings into
// `out`, so a table blocked at the column level arrives as a non-Executable set
// rather than as a thrown-away error.
//
// Sampled Update rows additionally carry RowChange::PriorValues() — the target's
// CURRENT cells, aligned with layout.columns exactly as Values() is — so a
// drill-in view can render a real before/after per column instead of "not sent".
// Rows past the cap carry nothing; see RowChangeBuilder::KeepPriorValues.
//
// Call this with insert/update/deleteMissing ALL enabled: the user's per-
// category choice is applied in pass 2, and a compare that suppressed deletes
// would show the user a delete count of zero for a table that has them.
//
// A refused table (no primary key, or a column-level block) comes back as
// `true` with `err` EMPTY and out.Executable() == false — NOT as an error, and
// NOT the way DiffAndEmitData reports the same table (which returns false with
// err set; see the asymmetry note there). The bool answers "did the engine
// work", Executable() answers "may this be applied". Test both.
bool DiffRowChanges(IConnection& src, IConnection& tgt,
                    const DataDiffSpec& spec, const DataSyncOptions& opt,
                    RowLayout& layout, RowChangeSet& out, wxString& err,
                    const std::atomic<bool>& stop);

// The canonical text encoding of a row's primary key — the identity the UI's
// ui::RowKey is expected to carry, so a row excluded in the grid and a row
// streamed by pass 2 resolve to the same string with no translation step.
//
// Format: `col=value` joined by '\x1F' (unit separator), in PK order, with a
// NULL rendered as the literal "\0" marker rather than the empty string (a NULL
// PK and an empty-string PK are different rows). Not SQL, never parsed back.
wxString EncodeRowKey(const std::vector<wxString>& pkColumns,
                      const std::vector<Cell>& keyCells);

} // namespace db::sync
