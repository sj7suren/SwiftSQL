// SchemaDiff.h — pure, dialect-aware structural diff for the cross-database
// synchronization suite (see docs/design/cross-db-sync.md §4.3–§4.4).
//
// DiffSchema is a *pure function*: it takes a source and target TableSchema
// (plus which dialect each side is) and produces an abstract SchemaChangeSet
// (Add/Modify/Drop of columns, indexes, FKs) plus a Finding per column
// comparison that needed one (A4). It never touches a connection, so it is
// fully unit-testable. Drivers turn the change set into dialect SQL via
// IConnection::RenderSchemaChange — this file has zero knowledge of ALTER
// text.
//
// A4 note: DiffSchema now takes the source/target Dialect directly (not a
// bare `sameDialect` bool) because cross-dialect column comparison needs real
// per-engine semantics — DialectProfile::Interpret/Render (A3) — not just "are
// these the same engine or not". sameDialect is derived internally as
// srcDialect == tgtDialect; every same-dialect code path is byte-for-byte
// unchanged from before this file depended on Dialect.
#pragma once

#include "db/SchemaModel.h"
#include "db/SchemaDelta.h"   // Finding (A1/A4)
#include "db/DbDriver.h"      // Dialect (precedented: PgSql.h/MySqlSql.h already
                              // include this just for Dialect/QuoteIdent)
#include <wx/string.h>
#include <vector>

namespace db::sync {

struct SchemaDiffResult {
    SchemaChangeSet      changes;
    // Was `std::vector<wxString> warnings` (free text). Now a typed Finding per
    // notable column/table-level observation (A4) — see DiffSchema's .cpp for
    // exactly when a Finding is pushed. `Finding.column` is "" for a
    // table-level note (options mismatch, cross-engine createTable skip) that
    // isn't about any one column.
    std::vector<Finding> warnings;
};

// Diff `target` towards `source` (single direction src → tgt), given which
// dialect each side actually is.
//
// Table-presence convention (the caller passes an empty TableSchema — no
// columns — for a side that lacks the table):
//   source has cols, target empty  → same-dialect: createTable (whole source
//                                     schema). Cross-dialect: NOT auto-created
//                                     (translating every column/index/FK of a
//                                     whole table correctly is out of scope for
//                                     A4's column-level convergence) — a
//                                     Finding is recorded instead.
//   source empty,   target has cols → dropTable  (opt-in; caller decides to keep)
//   both have cols                  → column / index / FK diff
//
// Column comparison, same dialect (srcDialect == tgtDialect): compares the
// engine-native rawType exactly, unchanged from before A4.
//
// Column comparison, cross dialect: each side's NormColumn is Interpret()ed by
// its OWN DialectProfile into a CanonicalType (A1/A3), then CompareCanonical
// classifies the pairing. TypeVerdict::Lossy/Unmappable can NEVER produce a
// ColumnChange — SchemaChangeSet's `columns` field only ever receives what
// SchemaDelta::AddColumnChange would have allowed (A4 wires this same
// verdict-gate at the SchemaDelta layer too; DiffSchema itself already refuses
// to construct the ColumnChange for a non-auto-alterable verdict). A skipped
// or acted-on cross-dialect column produces a Finding.
SchemaDiffResult DiffSchema(const TableSchema& source, const TableSchema& target,
                            Dialect srcDialect, Dialect tgtDialect);

} // namespace db::sync
