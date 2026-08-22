// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ResultGridModel.h — the PURE, widget-free half of the result grid.
//
// Everything here is a free function over plain values (wxString / std::vector).
// No wxWindow, no wxGrid, no theme, no icons: ResultGridPanel reads cells out of
// its wxGrid, hands the resulting matrix to these functions, and writes the
// answer back. That separation exists for one reason — this is the layer where
// the interesting mistakes live (off-by-one page math, a sort comparator that
// loses the row→original mapping, a paste that mis-splits TSV), and it is the
// only way to exercise them headlessly. See tests/ResultGridModelTests.cpp.
//
// The SQL-emitting half lives next door in ResultGridSql.{h,cpp}; this file
// deliberately knows nothing about dialects or statements.
#pragma once

#include <wx/string.h>
#include <vector>

namespace ui {

// One ORDER BY key: a result-column index plus direction. Owned here (rather
// than nested in ResultGridPanel) so both pure modules and the widget agree on
// a single type; ResultGridPanel::SortKey is an alias for it.
struct SortKey {
    int  col;
    bool asc;
};

namespace gridmodel {

using CellRow  = std::vector<wxString>;
using CellGrid = std::vector<CellRow>;

// ---------------------------------------------------------------------------
// Sort-key algebra — the three gestures that mutate the ORDER BY key list.
// All are pure reducers: (current keys, column) → next keys. The widget then
// applies the result through one funnel (SetSortKeysAndApply).
// ---------------------------------------------------------------------------

// Plain header left-click: three-state replace-single.
//   [col asc] → [col desc] → [] → [col asc]
// Any other current state (empty, a different single key, a multi-key list) is
// replaced outright by a single ascending key on `col`.
std::vector<SortKey> NextKeysOnHeaderClick(const std::vector<SortKey>& keys, int col);

// Shift-click / 「＋ 加入多列排序」: append `col` as the next key, or flip it
// asc→desc if already present, or drop it when it was already desc. Key order
// (= ORDER BY priority) of the other columns is preserved.
std::vector<SortKey> NextKeysOnToggle(const std::vector<SortKey>& keys, int col);

// Column header text: the base name plus `  ▲`/`  ▼`, carrying the 1-based
// priority digit when more than one key is active (`created_at ▼1  severity ▲2`).
// Returns one label per column, in column order.
std::vector<wxString> SortHeaderLabels(const std::vector<wxString>& columns,
                                       const std::vector<SortKey>& keys);

// ---------------------------------------------------------------------------
// Client-side multi-key row reorder.
// ---------------------------------------------------------------------------

// A grid row plus its index into the ORIGINAL result rows (-1 = a new, unsaved
// row). `orig` must travel with the cells through the sort, otherwise the
// edit/diff mapping silently retargets and a save writes to the wrong record.
struct SortableRow {
    CellRow cells;
    int     orig = -1;
};

// Stable multi-key sort. The comparator walks `keys` in priority order and the
// first non-equal key decides; a key whose column is out of range is skipped.
// Per cell and per key: numeric comparison when BOTH sides parse as a number,
// otherwise case-insensitive text. No-op for an empty key list or <2 rows.
void SortRows(std::vector<SortableRow>& rows, const std::vector<SortKey>& keys);

// ---------------------------------------------------------------------------
// Clipboard text.
// ---------------------------------------------------------------------------

// Rectangular cell block → TSV (tab between cells, newline between rows, no
// trailing newline).
wxString JoinTsv(const CellGrid& rows);

// The parsed clipboard for a paste. A single line with no tab is a scalar and
// goes into one cell; anything else is a row-wise fill starting at column 0.
struct ClipboardPaste {
    bool     empty  = true;    // nothing usable on the clipboard
    bool     scalar = false;   // single value → single-cell paste
    wxString value;            // valid when `scalar`
    CellGrid rows;             // valid when !scalar && !empty
};

// Strips CR, splits on LF, drops trailing blank lines, then classifies. Fields
// are split on tab with no escape processing (matching the copy side).
ClipboardPaste ParseClipboardPaste(const wxString& text);

// ---------------------------------------------------------------------------
// Pager arithmetic.
// ---------------------------------------------------------------------------

// Index of the last page. A not-yet-known (<0) or empty (0) row count pins the
// answer at page 0.
int MaxPageIndex(long long totalRows, int pageSize);

// Clamp a requested page index into [0, MaxPageIndex]. An unknown row count
// (totalRows < 0) only clamps the lower bound — paging forward blind is allowed.
int ClampPage(int page, long long totalRows, int pageSize);

// The enable-state + label of the whole pager, derived in one place so the
// buttons and the indicator can never disagree.
struct PagerState {
    wxString label;             // "—" / "3" (count unknown) / "3 / 12"
    bool     first = false;
    bool     prev  = false;
    bool     next  = false;
    bool     last  = false;
    bool     sizeChoice = false;
};
PagerState ComputePagerState(bool hasTable, long long totalRows,
                             int curPage, int pageSize);

// ---------------------------------------------------------------------------
// Misc presentation helpers.
// ---------------------------------------------------------------------------

// One-line summary shown in the cell viewer's collapsed header: whitespace
// flattened to spaces, elided past 60 characters.
wxString SummarizeCellValue(const wxString& value);

} // namespace gridmodel
} // namespace ui
