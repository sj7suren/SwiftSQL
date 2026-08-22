// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SchemaModel.h — normalized, dialect-neutral schema value objects for the
// cross-database synchronization suite (see docs/design/cross-db-sync.md).
//
// These are pure data: no wx UI, no vendor headers, no IConnection dependency
// (DbDriver.h includes THIS, not the other way round). The diff engine
// (db::sync::DiffSchema) consumes TableSchema pairs and produces a
// SchemaChangeSet; drivers render that change set into dialect SQL.
#pragma once

#include "db/QualifiedName.h"

#include <wx/string.h>
#include <vector>

namespace db {

// Cross-dialect column category. Comparison keys on this + length/scale so that
// "varchar" (MySQL) and "character varying" (PG) are seen as equal. Unmapped
// raw types fall to Other — the diff engine then degrades to a rawType compare
// (exact within one engine) rather than emitting a wrong ALTER.
enum class ColKind {
    Integer, Decimal, Float, Boolean, Char, Varchar, Text,
    Binary, Blob, Date, Time, Timestamp, Json, Enum, Uuid,
    Other
};

struct NormColumn {
    wxString  name;
    ColKind   kind = ColKind::Other;
    wxString  rawType;                 // engine-native ("varchar(80)") — true value within one engine
    long long length = -1;             // char length or numeric precision; -1 = N/A
    int       scale = -1;              // numeric scale; -1 = N/A
    bool      notNull = false;
    bool      hasDefault = false;
    bool      autoIncrement = false;
    wxString  defaultExpr;             // default value/expression text ("" when none)
    int       ordinal = 0;
};

struct NormIndex {
    wxString              name;
    std::vector<wxString> columns;
    bool                  unique = false;
    bool                  primary = false;
};

struct NormForeignKey {
    wxString              name;
    std::vector<wxString> columns;
    wxString              refTable;
    std::vector<wxString> refColumns;
    wxString              onDelete;
    wxString              onUpdate;
};

// A whole table's normalized structure, assembled by IConnection::GetTableSchema.
struct TableSchema {
    // SCHEMA-QUALIFIED (see QualifiedName.h). This is what carries a PostgreSQL
    // table's namespace out of introspection and into the plan, the selection
    // model and the executor, so `public.orders` and `archive.orders` stay two
    // tables all the way through. On MySQL/SQLite the schema half is empty and
    // this behaves exactly like the bare wxString it replaced.
    //
    // It is also what makes an EMPTY TableSchema mean only one thing again. The
    // PostgreSQL reader used to hard-code `table_schema='public'`, so a table in
    // any other schema came back with zero columns AND a true return — colliding
    // with DiffSchema's "empty schema == table does not exist" convention. A
    // reader that is told which schema to look in cannot produce that collision.
    QualifiedName               name;
    std::vector<NormColumn>     columns;
    std::vector<NormIndex>      indexes;
    std::vector<NormForeignKey> foreignKeys;
    std::vector<wxString>       primaryKey;   // ordered; empty = no PK
    wxString                    engine;
    wxString                    charset;
    wxString                    collation;
    wxString                    comment;

    const NormColumn* FindColumn(const wxString& col) const
    {
        for (const auto& c : columns) if (c.name == col) return &c;
        return nullptr;
    }
};

// ---- abstract change set (dialect-neutral; DiffSchema produces, driver renders) ----
struct ColumnChange {
    enum class Op { Add, Modify, Drop };
    Op         op = Op::Add;
    NormColumn column;        // desired column (for Add/Modify); name only matters for Drop
    wxString   afterColumn;   // optional positional hint ("" = append)

    // The TARGET's definition of this column as it exists right now, when there
    // is one. Purely informational — no renderer reads it, and it is not part of
    // what gets executed.
    //
    // Why it exists (ADR-015 T11): a Modify used to carry only `column`, the
    // DESIRED definition, so a side-by-side "源定义 / 目标定义" view had nothing
    // to put in the target cell and rendered a placeholder. The comparison
    // already holds the target column in hand at the moment it decides a Modify
    // is needed (SchemaDiff.cpp), so snapshotting it there costs one copy per
    // changed column and removes the need for any consumer to re-introspect the
    // target — or, worse, to guess.
    //
    // Populated for Modify (the target's current column) and Drop (the column
    // being dropped IS the target's). Never for Add: the target has no such
    // column, and `hasCurrent == false` says exactly that rather than handing
    // back a default-constructed NormColumn that could be mistaken for one.
    bool       hasCurrent = false;
    NormColumn current;
};

struct IndexChange {
    enum class Op { Add, Drop };
    Op        op = Op::Add;
    NormIndex index;
};

struct FkChange {
    enum class Op { Add, Drop };
    Op             op = Op::Add;
    NormForeignKey fk;
};

struct SchemaChangeSet {
    wxString                  table;
    bool                      createTable = false;   // target missing the table entirely
    TableSchema               createSchema;          // full source schema when createTable
    std::vector<ColumnChange> columns;
    std::vector<IndexChange>  indexes;
    std::vector<FkChange>     foreignKeys;
    bool                      dropTable = false;      // source missing the table (opt-in)

    bool Empty() const
    {
        return !createTable && !dropTable && columns.empty() &&
               indexes.empty() && foreignKeys.empty();
    }
};

} // namespace db
