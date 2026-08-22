// SchemaDelta.h — sync-specific comparison result value objects for the
// cross-database synchronization suite (see docs/design/cross-db-sync.md).
// Consumes SchemaModel.h (SchemaChangeSet et al.) and SyncTypeMap.h
// (TypeVerdict); this is the "sync orchestration" layer DialectProfile's hard
// rule forbids depending on — the dependency direction is one-way (SchemaDelta.h
// -> SyncTypeMap.h / SchemaModel.h, never the reverse).
#pragma once

#include "db/SchemaModel.h"
#include "db/SyncTypeMap.h"
#include <wx/string.h>
#include <vector>

namespace db::sync {

// One column-level verdict, whether or not it became a ColumnChange. Every
// column DiffSchema/Compare looks at produces exactly one Finding — this is
// what SyncPlan::warnings used to carry as free text (see SchemaDiff.h).
struct Finding {
    wxString    table;
    wxString    column;
    TypeVerdict verdict = TypeVerdict::Unmappable;
    wxString    reason;
};

// Pure structural comparison result for ONE table pair (Compare()'s per-table
// unit; SyncEngine aggregates one of these per table in scope). Verdict-gated
// BY CONSTRUCTION, not by convention: AddColumnChange() is the only way to
// populate the column-change list, and it structurally refuses any verdict
// !MayAutoAlter() — there is no public mutator for the underlying vector, so a
// Lossy/Unmappable column literally cannot become a ColumnChange here; it can
// only ever surface as a Finding. This is the mechanism the task brief asked
// for ("structurally incapable of producing a ColumnChange... not just usually
// skipped by convention").
class SchemaDelta {
public:
    wxString    table;
    bool        createTable = false;   // target missing the table entirely
    TableSchema createSchema;          // full source schema when createTable
    bool        dropTable = false;     // source missing the table (opt-in)

    std::vector<IndexChange> indexes;
    std::vector<FkChange>    foreignKeys;
    // Every column this comparison looked at, whichever way it went — including
    // the ones that landed in `columns_` AND the ones that didn't (Lossy/
    // Unmappable, or a same-dialect column that simply didn't differ, need not
    // be a Finding — Compare() only records one when a verdict actually gated
    // something out or the caller wants an audit trail; see SchemaDiff.cpp/A4).
    std::vector<Finding>     findings;
    // Table-option mismatches (engine/charset/collation/comment) — never a
    // structural rewrite, always a note.
    std::vector<wxString>    optionWarnings;

    // The only entry point that can populate the actionable column-change list.
    // Returns false (nothing added) when `verdict` isn't Identical/Equivalent —
    // the caller should record a Finding for that case instead (see
    // SchemaDiff.cpp's TryAddColumnChange usage for the standard pattern).
    bool AddColumnChange(TypeVerdict verdict, ColumnChange cc)
    {
        if (!MayAutoAlter(verdict)) return false;
        columns_.push_back(std::move(cc));
        return true;
    }

    const std::vector<ColumnChange>& Columns() const { return columns_; }

    bool Empty() const
    {
        return !createTable && !dropTable && columns_.empty() &&
               indexes.empty() && foreignKeys.empty();
    }

    // Convert to the driver-facing SchemaChangeSet (RenderSchemaChange's input).
    // Only ever built from the already-gated `columns_` — a Lossy/Unmappable
    // column cannot reach this either, transitively.
    SchemaChangeSet ToChangeSet() const
    {
        SchemaChangeSet ch;
        ch.table        = table;
        ch.createTable  = createTable;
        ch.createSchema = createSchema;
        ch.columns      = columns_;
        ch.indexes      = indexes;
        ch.foreignKeys  = foreignKeys;
        ch.dropTable    = dropTable;
        return ch;
    }

private:
    std::vector<ColumnChange> columns_;
};

} // namespace db::sync
