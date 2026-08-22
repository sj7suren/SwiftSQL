// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncEngine.h — orchestration for the cross-database sync suite
// (see docs/design/cross-db-sync.md §4.6). Pure orchestration: the structural
// diff lives in SchemaDiff, the data diff in DataSync, DDL rendering in the
// drivers. This class only sequences them, topo-sorts by FK, and drives the
// target transaction. No diff logic belongs here (≤1000-line charter).
#pragma once

#include "db/DbDriver.h"
#include "db/DataSync.h"      // RowStat / RowChangeSet / RowLayout / DataDiffSpec
#include "db/DataSyncExec.h"  // TableDataSpec / DataExecResult (T10 — the apply half)
#include "db/SchemaDelta.h"   // SchemaDelta/Finding (A4 — Compare()'s pure result type)
#include <atomic>
#include <functional>
#include <vector>

namespace db::sync {

// What to synchronize. `tables` empty = every table in the source; otherwise the
// intersection of that list with the source/target tables. Drop-class actions
// (DROP TABLE / DROP COLUMN / DELETE) stay off unless explicitly requested.
struct SyncScope {
    bool                  structure         = true;
    bool                  data              = false;
    bool                  dropMissingTables = false;   // target-only tables
    // SCHEMA-QUALIFIED. A bare name here selected every same-named table in every
    // schema at once on PostgreSQL -- and, because the table universe below was
    // deduplicated by bare name, collapsed them into a single unit.
    std::vector<QualifiedName> tables;
};

// Why a table's DATA was or wasn't planned. Structured because the UI has a
// 状态 column with exactly these cases in its vocabulary (就绪 / 无主键（已排除）
// / …), and the only alternative to a verdict enum is for the UI to recover the
// case by grepping SyncPlan::warnings — string-sniffing a human-readable,
// translatable sentence to make an execution decision. That was correctly
// refused, so the signal is carried properly instead.
enum class DataVerdict {
    NotRequested,    // data sync not in scope, or the table is target-only
    Ok,              // diffed successfully; see TableUnit::rows for the outcome
    NoPrimaryKey,    // 无主键（已排除）
    // PK order cannot be guaranteed on both engines, as judged from the SCHEMA
    // TYPE by CheckDataDiffOrdering()/ClassifyKeyOrder() before any row is read
    // (collation and byte-order hazards). This is the ONLY producer of this
    // verdict.
    UnorderableKey,
    TargetMissing,   // target lacks the table and structure sync won't create it
    // A column/value the gate refuses to convert. Note this ALSO covers key
    // columns refused by KeyOrderSafe() in BuildRowLayout — a tz-ambiguous
    // temporal PK (MySQL TIMESTAMP / PG timestamptz) lands HERE, not in
    // UnorderableKey, because it is discovered at value-bridge time rather than
    // by the schema-type gate. Both refuse the table with a specific reason in
    // dataVerdictReason; do not treat UnorderableKey as the complete set of
    // ordering refusals. See SyncCellCompare.h for the full path.
    Blocked,
};

// A dry-run-friendly, ordered plan. preamble/postamble wrap the whole run
// (e.g. MySQL SET FOREIGN_KEY_CHECKS=0/1). warnings surface skipped/gated items.
struct SyncPlan {
    std::vector<wxString> preamble, postamble, warnings;
    struct TableUnit {
        // THE UNIT'S IDENTITY: the SOURCE side's schema-qualified name.
        //
        // Why the source side and not the pair: this unit already carries the
        // pair context in `dataSpec` (both TableSchemas, each with its own
        // QualifiedName), so duplicating the target name here would create a
        // second place for the two to disagree -- the exact failure being fixed.
        // The target's identity has exactly one spelling, dataSpec.TargetTable().
        //
        // This is also the string ui::TableKey wraps, so the plan, the grid, the
        // selection model and the execution spec all name a table identically.
        // On MySQL it is byte-identical to the bare name used before; on
        // PostgreSQL it gains the `schema.` prefix that keeps public.orders and
        // archive.orders apart.
        QualifiedName         table;
        std::vector<wxString> ddl;

        // ---- DEPRECATED (T11): PREVIEW TEXT ONLY, NEVER AN EXECUTION LIST ----
        // Rendered from `rows.Sample()`, so it holds at most
        // RowChangeSet::kSampleCap (500) statements no matter how many rows
        // differ. That is the entire point — the old field materialized one
        // wxString per changed row, so a 500k-row diff built 500k strings before
        // the user had even seen the result.
        //
        // Executing this vector would therefore apply a SAMPLE and report
        // success, which is why SyncEngine::Execute no longer reads it at all:
        // data is applied by streaming the diff a second time (DataSyncExec).
        // Kept only so the SQL-preview pane keeps compiling; it disappears with
        // the UI swap.
        std::vector<wxString> dml;
        bool                  dmlTruncated = false;   // sample < actual change count

        // Mirrors rows.Stat() (inserts/updates/deletes). `unchanged` is not
        // retained per table. Kept for existing consumers.
        RowStat               stat;

        // Structured form of `ddl` (A4) — the same actionable, verdict-gated
        // column/index/FK changes ddl was rendered from, before dialect text
        // rendering. Lets a consumer (e.g. the UI) inspect what changed
        // without re-parsing rendered SQL strings. Empty when ddl is empty.
        SchemaChangeSet       changes;

        // ---- the data half, structured (T11) ----
        // Counts + capped sample + audit trail + the derived Executable() flag.
        // This is what ui::MakeTableDiff consumes.
        RowChangeSet          rows;
        // The per-column conversion plan and the target-side names, so a
        // consumer can render a sample row to SQL without rebuilding bridges.
        RowLayout             layout;
        // Everything needed to RE-derive this table's diff at execute time.
        DataDiffSpec          dataSpec;

        DataVerdict           dataVerdict = DataVerdict::NotRequested;
        wxString              dataVerdictReason;   // display text for the verdict

        bool HasData() const { return !rows.Empty(); }
    };
    std::vector<TableUnit> units;

    bool Empty() const
    {
        for (const auto& u : units)
            if (!u.ddl.empty() || u.HasData()) return false;
        return preamble.empty() && postamble.empty();
    }
};

// The data half's authorization for one Execute() call: which categories of
// which tables the user actually approved. Built by the caller from
// ui::ExecutionSpec; see DataSyncExec.h for why deletes need a token.
//
// Lookup is by table NAME — identity, never position.
struct DataExecPlan {
    std::vector<TableDataSpec> tables;

    const TableDataSpec* Find(const QualifiedName& table) const
    {
        for (const auto& t : tables) if (t.Table() == table.Key()) return &t;
        return nullptr;
    }
    bool Empty() const { return tables.empty(); }
};

class SyncEngine {
public:
    // src/tgt outlive the engine; they are two independent IConnection instances
    // (possibly different dialects). srcDb/tgtDb are the databases to compare.
    SyncEngine(IConnection& src, IConnection& tgt, wxString srcDb, wxString tgtDb);

    using BuildProgress =
        std::function<void(const wxString& phase, int done, int total)>;
    using RunProgress =
        std::function<void(int done, int total, const wxString& sql)>;

    // Generate the plan (no side effects on the target — safe dry-run). Returns
    // false with err only on an introspection/diff failure or cancellation.
    // Internally: Compare() + Plan() per table (A4) for structure, the
    // pre-existing DataSync streaming loop for data, then FK topo-sort +
    // dialect preamble/postamble — unchanged external contract/signature so
    // the UI (a different track this round) needs no changes.
    bool BuildPlan(const SyncScope& scope, SyncPlan& out, wxString& err,
                   const std::atomic<bool>& stop, const BuildProgress& progress);

    // ---- A4: BuildPlan's structure half, split into a pure comparison step
    // and a rendering step, each usable standalone (unit tests, or a future
    // caller that wants to inspect the delta before deciding whether to
    // render it). Table-scoped — BuildPlan calls one Compare()+Plan() pair per
    // table in the sync scope. ----

    // Pure comparison: wraps DiffSchema (A4) with this engine's src_/tgt_
    // dialects, so cross-dialect column pairs go through each side's own
    // DialectProfile::Interpret (A3). No I/O at all — `srcSchema`/`tgtSchema`
    // are already-fetched TableSchema value objects (an empty one on either
    // side follows DiffSchema's createTable/dropTable convention; the table
    // name comes from whichever side is non-empty, same as DiffSchema).
    SchemaDelta Compare(const TableSchema& srcSchema, const TableSchema& tgtSchema) const;

    // Render an already-computed SchemaDelta into this target's DDL statements
    // (tgt_.RenderSchemaChange). Not a free function — dialect-specific
    // rendering needs the live target driver. `delta.Empty()` renders nothing
    // (ddl left empty, not an error).
    bool Plan(const SchemaDelta& delta, std::vector<wxString>& ddl, wxString& err);

    // Apply the plan to the target.
    //
    // STRUCTURE runs from plan.units[].ddl, in FK topological order, exactly as
    // before. DATA does NOT run from plan.units[].dml — that vector is a capped
    // preview (see TableUnit) and executing it would apply at most 500 rows per
    // table while reporting success. Instead each table's diff is STREAMED a
    // second time into the target (DataSyncExec), honoring `data`'s per-(table,
    // category) authorization at the point of detection.
    //
    // useTransaction wraps the DDL phase in BEGIN/COMMIT on rollback-capable
    // dialects; MySQL DDL auto-commits and is reported honestly as
    // non-rollbackable. The DATA phase is always per-table transactional
    // regardless of this flag — a single transaction spanning a multi-million
    // row sync exhausts PG WAL / MySQL undo (see DataSyncExec.h).
    //
    // `dataResult` is filled even on failure and is the authoritative account of
    // which tables committed.
    bool Execute(const SyncPlan& plan, const DataExecPlan& data,
                 bool useTransaction, DataExecResult& dataResult, wxString& err,
                 const std::atomic<bool>& stop, const RunProgress& progress);

    // Structure-only overload, kept so existing callers keep compiling during
    // the UI swap.
    //
    // It REFUSES, loudly, any plan that carries data changes. That is not
    // fussiness: this signature has no way to express which data categories the
    // user approved, so the only alternatives would be to guess (and run
    // DELETEs nobody checked) or to execute the capped `dml` preview (and apply
    // 500 of 500k rows, reporting success). Failing closed is the only honest
    // third option. Callers with data must use the overload above.
    bool Execute(const SyncPlan& plan, bool useTransaction, wxString& err,
                 const std::atomic<bool>& stop, const RunProgress& progress);

private:
    // Point each connection at the database this engine was constructed for
    // (MySQL/SQL Server `USE`, PostgreSQL reconnect, no-op elsewhere). Called at
    // the top of BuildPlan and, load-bearingly, at the top of Execute: the DDL
    // this engine emits is NOT database-qualified on either MySQL or PostgreSQL,
    // so an unbound session writes into the wrong database. The .cpp comment
    // carries the full account of the P0 that proved it.
    bool BindSessions(wxString& err);

    // The whole DDL+data run. Split out of Execute() for ONE structural reason:
    // Execute() owns a scope guard that replays plan.postamble on tgt_, and this
    // helper holds every early return (cancel, statement failure, COMMIT
    // failure, the data phase's own result). Because the guard lives in the
    // CALLER's scope, no return added here — now or later — can skip the
    // restore. The postamble used to be a trailing statement list that nothing
    // executed at all; making it un-skippable is the fix, not merely executing
    // it once. Do NOT inline this back into Execute().
    bool ExecuteRun(const SyncPlan& plan, const DataExecPlan& data,
                    bool useTransaction, DataExecResult& dataResult, wxString& err,
                    const std::atomic<bool>& stop, const RunProgress& progress);

    // BuildPlan's data half for ONE table: decides the DataVerdict, runs the
    // compare pass when the verdict allows it, and fills unit.rows / layout /
    // dataSpec / stat / dml-preview. Sets `err` only for a genuine
    // introspection or I/O failure — a per-table refusal is a verdict, not an
    // error, and must not abort the whole compare.
    void PlanTableData(const TableSchema& srcSchema, const TableSchema& tgtSchema,
                       bool tgtHas, bool tableWillExist, SyncPlan::TableUnit& unit,
                       std::vector<wxString>& warnings, wxString& err,
                       const std::atomic<bool>& stop);

    // Target-dialect quoted/qualified table name, matching what the DML uses.
    // Delegates to db::QualifiedTableSql -- this used to be a fourth private
    // reimplementation of the same rule (merge, applier, SQL preview being the
    // others), which is how the preview and the executor could have disagreed
    // about which table a statement hit.
    wxString QualifiedTarget(const QualifiedName& table) const;

    IConnection& src_;
    IConnection& tgt_;
    wxString     srcDb_;
    wxString     tgtDb_;
};

} // namespace db::sync
