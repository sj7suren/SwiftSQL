// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// FilterPanel.h — a Navicat-style column filter: a stack of segmented condition
// rows, each `☑ 启用 | [列 ▾] | [运算符 ▾] | ⟨值⟩ | and/or | + () ×`. The value is
// the only true inline field; column and operator open pickers (ColumnPicker /
// wxMenu). Each row carries its own AND/OR connective to the NEXT row, so the
// panel emits a WHERE clause with explicit parentheses around consecutive AND
// runs (what you see == what is sent). Purely presentational — it knows the
// column names + dialect and emits the clause through onApply.
#pragma once

#include <wx/panel.h>
#include <wx/string.h>

#include <functional>
#include <vector>

#include "db/DbDriver.h"   // db::Dialect, db::QuoteIdent

class wxScrolledWindow;
class wxCheckBox;
class wxTextCtrl;
class wxButton;
class wxStaticText;

namespace ui {

class ColumnPicker;
class InlineDropdown;   // borderless "text + ▾ + underline" clickable segment
class UnderlineText;    // borderless value input with a bottom underline

class FilterPanel : public wxPanel {
public:
    // onApply is called with the built WHERE clause (no "WHERE" keyword, "" = no
    // filter) when the user clicks 应用 or presses Enter in a value field; fired
    // with "" when they click 清除.
    FilterPanel(wxWindow* parent, std::function<void(const wxString& where)> onApply);
    ~FilterPanel();

    // Fresh table: remember the columns + dialect and prune any now-invalid picks.
    void SetColumns(const std::vector<wxString>& cols, db::Dialect dialect);

private:
    struct Row {
        wxWindow*       host    = nullptr;
        wxCheckBox*     enable  = nullptr; // ☑ include this row
        InlineDropdown* colField = nullptr;// 列 ▾ (borderless) → ColumnPicker
        InlineDropdown* opField  = nullptr;// 运算符 ▾ (borderless) → wxMenu
        UnderlineText*  valHost  = nullptr;// borderless value wrapper (owns `val`)
        wxTextCtrl*     val     = nullptr;  // inline value (inside valHost)
        wxStaticText*   warn    = nullptr;  // '!' incomplete marker
        wxButton*     connBtn = nullptr;   // and/or → relation to the NEXT row
        wxButton*     addBtn  = nullptr;   // + insert below
        wxButton*     groupBtn = nullptr;  // () grouping — greyed (P2)
        wxButton*     remove  = nullptr;   // × delete
        int           col     = -1;        // chosen column index (-1 = unset)
        wxString      op      = L"=";      // operator key
        bool          connAnd = true;      // AND (true) / OR (false) to next row
        bool          enabled = true;      // ☑ state
    };

    Row*     AddRow(int afterIndex = -1);       // append, or insert after an index
    void     RemoveRow(Row* r);
    void     ResetToOneRow();
    void     OpenPicker(Row* r);
    void     ShowOpMenu(Row* r);
    void     RefreshColBtn(Row* r) const;
    void     RefreshOpBtn(Row* r) const;
    void     UpdateRowState(Row* r);            // value enable/hint + incomplete/disabled look
    void     UpdateConnVisibility();            // hide the last row's connective
    void     Relayout();
    int      IndexOf(const Row* r) const;
    wxString RowCond(const Row* r) const;       // "" if disabled / incomplete
    wxString AssembleWhere() const;             // clause with explicit AND-run parens
    void     UpdatePreview();
    void     Apply();

    std::function<void(const wxString&)> onApply_;
    std::vector<wxString> columns_;
    db::Dialect           dialect_ = db::Dialect::MySQL;

    wxScrolledWindow* rowsHost_ = nullptr;
    wxStaticText*     preview_  = nullptr;      // read-only live SQL preview
    ColumnPicker*     picker_   = nullptr;
    std::vector<Row*> rows_;
};

} // namespace ui
