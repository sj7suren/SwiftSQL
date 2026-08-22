// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ResultGridModel.cpp — implementation of the pure result-grid model.
// Widget-free by construction: wxString + std::vector only (see the header).
#include "ui/ResultGridModel.h"

#include <wx/arrstr.h>
#include <algorithm>

namespace ui::gridmodel {

// ---------------------------------------------------------------------------
// Sort-key algebra
// ---------------------------------------------------------------------------
std::vector<SortKey> NextKeysOnHeaderClick(const std::vector<SortKey>& keys, int col)
{
    std::vector<SortKey> next;
    if (keys.size() == 1 && keys[0].col == col) {
        if (keys[0].asc) next.push_back({ col, false });   // asc → desc
        // desc → off (empty)
    } else {
        next.push_back({ col, true });                     // replace with single asc
    }
    return next;
}

std::vector<SortKey> NextKeysOnToggle(const std::vector<SortKey>& keys, int col)
{
    std::vector<SortKey> next = keys;
    bool found = false;
    for (size_t i = 0; i < next.size(); ++i)
        if (next[i].col == col) {
            found = true;
            if (next[i].asc) next[i].asc = false;          // asc → desc
            else next.erase(next.begin() + i);             // desc → remove
            break;
        }
    if (!found) next.push_back({ col, true });             // append asc
    return next;
}

std::vector<wxString> SortHeaderLabels(const std::vector<wxString>& columns,
                                       const std::vector<SortKey>& keys)
{
    const bool multi = keys.size() > 1;
    std::vector<wxString> labels;
    labels.reserve(columns.size());
    for (size_t c = 0; c < columns.size(); ++c) {
        wxString suffix;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i].col == static_cast<int>(c)) {
                suffix = wxString(L"  ") + (keys[i].asc ? L"▲" : L"▼");
                if (multi) suffix += wxString::Format(L"%zu", i + 1);
                break;
            }
        labels.push_back(columns[c] + suffix);
    }
    return labels;
}

// ---------------------------------------------------------------------------
// Client-side multi-key row reorder
// ---------------------------------------------------------------------------
void SortRows(std::vector<SortableRow>& rows, const std::vector<SortKey>& keys)
{
    if (keys.empty() || rows.size() <= 1) return;

    std::stable_sort(rows.begin(), rows.end(),
                     [&](const SortableRow& a, const SortableRow& b) {
        for (const SortKey& k : keys) {
            if (k.col < 0 || k.col >= static_cast<int>(a.cells.size()) ||
                k.col >= static_cast<int>(b.cells.size()))
                continue;
            const wxString& x = a.cells[k.col];
            const wxString& y = b.cells[k.col];
            double dx, dy; int c;
            if (x.ToDouble(&dx) && y.ToDouble(&dy)) c = (dx < dy) ? -1 : (dx > dy ? 1 : 0);
            else c = x.CmpNoCase(y);
            if (c != 0) return k.asc ? c < 0 : c > 0;   // first non-equal key decides
        }
        return false;
    });
}

// ---------------------------------------------------------------------------
// Clipboard text
// ---------------------------------------------------------------------------
wxString JoinTsv(const CellGrid& rows)
{
    wxString text;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) text += L"\n";
        for (size_t c = 0; c < rows[r].size(); ++c) {
            if (c) text += L"\t";
            text += rows[r][c];
        }
    }
    return text;
}

ClipboardPaste ParseClipboardPaste(const wxString& text)
{
    ClipboardPaste out;
    if (text.IsEmpty()) return out;

    wxString body = text;
    body.Replace(L"\r", wxEmptyString);

    wxArrayString lines = wxSplit(body, L'\n', L'\0');    // '\0' escape = no escaping
    while (!lines.IsEmpty() && lines.Last().IsEmpty())    // drop trailing blank lines
        lines.RemoveAt(lines.GetCount() - 1);
    if (lines.IsEmpty()) return out;

    out.empty = false;
    if (lines.GetCount() == 1 && lines[0].Find(L'\t') == wxNOT_FOUND) {
        out.scalar = true;
        out.value  = lines[0];
        return out;
    }
    for (size_t i = 0; i < lines.GetCount(); ++i) {
        const wxArrayString fields = wxSplit(lines[i], L'\t', L'\0');
        CellRow row;
        row.reserve(fields.GetCount());
        for (size_t f = 0; f < fields.GetCount(); ++f) row.push_back(fields[f]);
        out.rows.push_back(std::move(row));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Pager arithmetic
// ---------------------------------------------------------------------------
int MaxPageIndex(long long totalRows, int pageSize)
{
    if (totalRows <= 0 || pageSize <= 0) return 0;
    return static_cast<int>((totalRows - 1) / pageSize);
}

int ClampPage(int page, long long totalRows, int pageSize)
{
    const int maxPage = MaxPageIndex(totalRows, pageSize);
    if (totalRows >= 0 && page > maxPage) page = maxPage;
    if (page < 0) page = 0;
    return page;
}

PagerState ComputePagerState(bool hasTable, long long totalRows,
                             int curPage, int pageSize)
{
    const int maxPage = MaxPageIndex(totalRows, pageSize);
    PagerState s;
    if (!hasTable)            s.label = L"—";
    else if (totalRows < 0)   s.label = wxString::Format(L"%d", curPage + 1);
    else                      s.label = wxString::Format(L"%d / %d", curPage + 1, maxPage + 1);

    s.first      = hasTable && curPage > 0;
    s.prev       = hasTable && curPage > 0;
    s.next       = hasTable && (totalRows < 0 || curPage < maxPage);
    s.last       = hasTable && totalRows >= 0 && curPage < maxPage;
    s.sizeChoice = hasTable;
    return s;
}

// ---------------------------------------------------------------------------
// Misc presentation helpers
// ---------------------------------------------------------------------------
wxString SummarizeCellValue(const wxString& value)
{
    wxString s = value;
    s.Replace(L"\n", L" ");
    s.Replace(L"\r", L" ");
    s.Replace(L"\t", L" ");
    if (s.length() > 60) s = s.Left(60) + L"…";
    return s;
}

} // namespace ui::gridmodel
