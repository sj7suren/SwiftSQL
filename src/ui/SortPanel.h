// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SortPanel.h — a collapsible multi-column sort editor for the data browser.
//
// Mirrors FilterPanel's flat pattern (top bar + scrollable row stack + 应用/清除 +
// read-only preview) using the shared FlatControls. Each row picks a column (via
// the searchable ColumnPicker), an ASC/DESC direction (a flat text toggle), and a
// priority that can be moved up/down. Row order = ORDER BY priority. onApply emits
// the ordered (columnIndex, asc) key list.
#pragma once

#include <wx/panel.h>
#include <wx/string.h>

#include <functional>
#include <utility>
#include <vector>

#include "db/DbDriver.h"   // db::Dialect, db::QuoteIdent

class wxScrolledWindow;
class wxStaticText;

namespace ui {

class ColumnPicker;
class InlineDropdown;   // borderless "列名 ▾" segment (shared FlatControls)
class FlatButton;       // borderless text/icon button (shared FlatControls)

class SortPanel : public wxPanel {
public:
    // onApply is called with the ordered sort keys (columnIndex, asc) when the
    // user clicks 应用; fired with an empty list when they click 清除.
    SortPanel(wxWindow* parent,
              std::function<void(const std::vector<std::pair<int, bool>>&)> onApply);
    ~SortPanel();

    // Rebuild the column universe (fresh table). Prunes rows whose column no
    // longer exists and refreshes the preview.
    void SetColumns(const std::vector<wxString>& cols, db::Dialect dialect);

    // Replace the panel's rows to mirror an external sortKeys_ change (header /
    // cell-menu gestures write the same list). Does not fire onApply.
    void SetKeys(const std::vector<std::pair<int, bool>>& keys);

private:
    struct Row {
        wxWindow*       host    = nullptr;
        wxStaticText*   badge   = nullptr;  // #N priority
        InlineDropdown* colField = nullptr; // 列名 ▾ (borderless) → ColumnPicker
        FlatButton*     dirBtn  = nullptr;  // ASC / DESC toggle (text, not arrows)
        FlatButton*     upBtn   = nullptr;  // move up
        FlatButton*     downBtn = nullptr;  // move down
        FlatButton*     remove  = nullptr;  // × delete
        int             col     = -1;       // chosen column index (-1 = unset)
        bool            asc     = true;
    };

    Row*     AddSortRow(int col = -1, bool asc = true);
    void     RemoveSortRow(Row* r);
    void     MoveRow(Row* r, bool up);
    void     OpenPicker(Row* r);
    void     SetRowDir(Row* r, bool asc);   // ASC=true; updates the toggle text
    void     RefreshColButton(Row* r) const;
    void     RenumberBadges();
    void     Relayout();
    std::vector<std::pair<int, bool>> BuildKeys() const;
    void     UpdatePreview();

    std::function<void(const std::vector<std::pair<int, bool>>&)> onApply_;
    std::vector<wxString> columns_;
    db::Dialect           dialect_ = db::Dialect::MySQL;

    wxScrolledWindow* rowsHost_ = nullptr;
    wxStaticText*     preview_  = nullptr;
    ColumnPicker*     picker_   = nullptr;
    std::vector<Row*> rows_;
};

} // namespace ui
