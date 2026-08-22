// SyncSelectionAdapter.h — the ONE seam between the data layer's
// db::sync::RowChangeSet (src/db/RowChange.h, ADR-015 T3) and the compare
// screen's pure check-state model, ui::SyncSelection (ui/SyncSelection.h, T6).
//
// WHY THIS IS A SEPARATE FILE
// ui/SyncSelection.{h,cpp} deliberately depends on nothing but wxString. That
// is not fastidiousness: tests/SyncSelectionTests.cpp links only
// swiftsql::core, so if anyone ever pulls a db (or wx widget) type into the
// selection model, that test target stops linking. The fitness function only
// works while the model stays clean, so the data-layer knowledge lives here
// instead — header-only, so it costs the ui library nothing.
//
// Everything this file does is field mapping. It contains no policy: what may
// execute is decided by db::sync::RowChangeSet::Executable() (derived, never
// stored — see RowChange.h's "structural gate") and by the delete master
// switch inside SyncSelection. Nothing here re-derives either.
#pragma once

#include "db/RowChange.h"
#include "ui/SyncSelection.h"

namespace ui {

// Build the selection model's view of ONE table.
//
// `table` is the stable table name — the same string SyncDiffModel.h uses for
// DiffNode::id (== db::sync::SyncPlan::TableUnit::table), so a check made in
// the compare grid and a node clicked in the drill-in tree resolve to the same
// table with no translation step. RowChangeSet itself carries no table name
// (it is a per-table value held by its owner), which is why the caller supplies
// it here.
//
// The structure half comes from a different producer than the data half (the
// SchemaDelta track vs RowChangeSet), so it is passed in separately and is
// blocked independently.
inline TableDiff MakeTableDiff(const wxString&               table,
                               const db::sync::RowChangeSet& rows,
                               bool                          hasStructure        = false,
                               bool                          structureExecutable = true)
{
    const db::sync::RowChangeStat& stat = rows.Stat();

    TableDiff d;
    d.key                 = TableKey(table);
    d.hasStructure        = hasStructure;
    d.structureExecutable = structureExecutable;
    d.inserts             = stat.inserts;
    d.updates             = stat.updates;
    d.deletes             = stat.deletes;

    // Row-level selection is offered only for a fully materialized diff. The
    // data layer already made that call; do not second-guess it by comparing
    // Sample().size() against kSampleCap.
    d.truncated = rows.Truncated();

    // DERIVED on the data-layer side (`!blocked_`, with no path back to true).
    // Mirrored, never recomputed: a blocked set can only ever arrive here as
    // false, and TableDiff::Executable() then excludes every data category of
    // this table from the execution spec.
    d.dataExecutable = rows.Executable();

    // Display-only audit trail for the grid's "why is this greyed out?" text.
    d.findings.reserve(rows.Findings().size());
    for (const db::sync::Finding& f : rows.Findings()) {
        wxString line = f.column.IsEmpty() ? f.table : (f.table + L"." + f.column);
        if (!f.reason.IsEmpty()) line += L": " + f.reason;
        d.findings.push_back(line);
    }

    return d;
}

} // namespace ui
