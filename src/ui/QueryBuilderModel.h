// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// QueryBuilderModel.h — the pure data + SQL-generation core of the visual query
// builder (QueryBuilderPanel). Deliberately UI-free (no wxWindow, only wxRect for
// canvas geometry) so BuildSql() is a single, side-effect-free function that can be
// unit-tested in isolation. The panel owns one QueryBuilderModel and re-renders the
// SQL preview after every model mutation.
//
// Design notes:
//  * Tables carry a STABLE integer id (not a vector index): joins reference tables
//    by id, so removing a box never invalidates a join's endpoints or silently
//    re-points it at a different table.
//  * Every table gets an auto alias (t1, t2, …) assigned at add time and never
//    reused, so self-joins (same table added twice) and qualified column refs
//    (alias.col) are always unambiguous.
//  * FROM = the first table in `tables`; the rest are chained on with JOINs. A table
//    with no join edge to the already-emitted set is CROSS JOIN'd, so the output is
//    ALWAYS a syntactically valid statement regardless of how the graph is wired.
#pragma once

#include <wx/gdicmn.h>   // wxRect
#include <wx/string.h>
#include <vector>
#include "db/DbDriver.h"  // db::Dialect, db::QuoteIdent

namespace ui {

// One column inside a table box.
struct QbColumn {
    wxString name;
    wxString type;               // declared type, shown faint on the right
    bool     selected = false;   // checkbox → included in the SELECT list
    bool     isPk = false;       // PK marker (amber dot)
};

// A table box placed on the canvas.
struct QbTable {
    int                   id = 0;     // stable unique id (joins reference this)
    wxString              name;       // real table name
    wxString              alias;      // auto alias (t1, t2, …); stable for its lifetime
    wxRect                rect;       // position + size in canvas (logical) coords
    std::vector<QbColumn> columns;
};

enum class QbJoinType { Inner, Left, Right };

// A join edge between two table boxes' columns.
struct QbJoin {
    int        leftId = 0;   // table id of the first-picked column
    wxString   leftCol;
    int        rightId = 0;  // table id of the second-picked column
    wxString   rightCol;
    QbJoinType type = QbJoinType::Inner;
};

class QueryBuilderModel {
public:
    std::vector<QbTable> tables;
    std::vector<QbJoin>  joins;
    db::Dialect          dialect = db::Dialect::MySQL;
    wxString             database;          // current db (for MySQL qualification)
    bool                 qualifyDb = false; // prefix FROM/JOIN tables with `db`.

    // ---- mutation helpers (keep joins consistent) ----
    // Add a table box; returns the new table's stable id. `alias` is auto-assigned.
    int  AddTable(const wxString& name, const std::vector<QbColumn>& cols,
                  const wxRect& rect);
    // Remove a table box by id; also drops every join touching it.
    void RemoveTable(int id);
    // Add a join edge (INNER by default). No-op if it would connect a table to
    // itself on the same column, or if an identical edge already exists.
    void AddJoin(int leftId, const wxString& leftCol,
                 int rightId, const wxString& rightCol,
                 QbJoinType type = QbJoinType::Inner);
    void RemoveJoin(size_t index);           // by position in `joins`
    QbTable*       TableById(int id);
    const QbTable* TableById(int id) const;

    // ---- SQL generation (pure) ----
    // Assemble a SELECT statement from the current model. Empty model → "".
    // No trailing ';' (the editor / user decides). Every identifier is quoted for
    // `dialect`; column refs are alias-qualified; unchecked-everything → SELECT *.
    wxString BuildSql() const;

private:
    int nextId_ = 1;      // next stable table id
    int nextAlias_ = 1;   // next alias number (t<n>)
};

// Free helper (exposed for testing): the JOIN keyword for a type.
inline wxString QbJoinKeyword(QbJoinType t)
{
    switch (t) {
        case QbJoinType::Left:  return L"LEFT JOIN";
        case QbJoinType::Right: return L"RIGHT JOIN";
        case QbJoinType::Inner:
        default:                return L"INNER JOIN";
    }
}

} // namespace ui
