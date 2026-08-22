// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DataTransfer.h — the request payload the data browser hands to the export
// dialog. ResultGridPanel packages the current view (columns, current-page rows,
// selection snapshot, and the db-qualified whole-table query parts) into one of
// these; MainFrame turns it into an ExportDialog against the live connection.
// Keeping it a plain struct means the grid never holds an IConnection.
#pragma once

#include <wx/string.h>
#include <vector>

#include "db/DbDriver.h"   // db::QueryResult, db::Dialect

namespace ui {

struct ExportRequest {
    // Column names (display order) — shared by all three scopes.
    std::vector<wxString> columns;

    // Scope: 当前页 — the rows currently materialised in the grid (in memory).
    db::QueryResult currentPage;

    // Scope: 选中记录 — a snapshot of the selected rows (empty → option disabled).
    std::vector<std::vector<wxString>> selectedRows;

    // Scope: 整表全部 — parts of the paged SELECT so the dialog can stream the whole
    // table chunk-by-chunk (never all rows in memory). `browse` is false for ad-hoc
    // editor results where no single-table target exists (整表 then unavailable).
    wxString    table;
    wxString    db;
    wxString    where;      // WHERE body (no keyword), "" = none
    wxString    orderBy;    // ORDER BY body (no keyword), "" = none
    db::Dialect dialect = db::Dialect::MySQL;
    bool        browse   = false;
};

} // namespace ui
