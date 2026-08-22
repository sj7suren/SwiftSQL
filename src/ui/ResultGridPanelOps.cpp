// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ResultGridPanelOps.cpp — second translation unit for ResultGridPanel: the
// grid's OPERATIONS — keyboard routing, selection, the context menus, the
// clipboard, import/export packaging, and the sort/filter application funnel.
//
// It is deliberately thin on logic: the statements and the row/key/page
// arithmetic all come from the two pure modules (ui::gridsql / ui::gridmodel,
// see ResultGridSql.h and ResultGridModel.h, both headlessly unit-tested). What
// stays here is the wx-side wiring — read cells out of the wxGrid, call the pure
// function, write the answer back or push it at the clipboard/menu/handler.
// Widget CONSTRUCTION lives in the third TU, ResultGridPanelChrome.cpp.
#include "ui/ResultGridPanel.h"

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/clipbrd.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <algorithm>

#include "ui/CellViewerPanel.h"
#include "ui/FilterPanel.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/ResultGridModel.h"
#include "ui/ResultGridSql.h"
#include "ui/SortPanel.h"
#include "ui/Theme.h"

namespace ui {

namespace {
// Above this many rows, the synchronous client-side sort (read back → stable_sort →
// write back, all on the UI thread) would freeze the grid. Past it we require an
// explicit confirmation instead of a silent multi-second hang; below it the sort
// stays instant. Browse-mode results never reach the client path — they already
// re-query with a server-side ORDER BY that also survives page turns.
constexpr int kClientSortRowLimit = 2000;
} // namespace

// ---------------------------------------------------------------------------
// Widget → pure-value bridge. Everything downstream of these four helpers works
// on plain vectors/strings, which is what makes the SQL + model layers testable.
// ---------------------------------------------------------------------------
gridsql::DmlContext ResultGridPanel::MakeDmlContext() const
{
    gridsql::DmlContext ctx;
    ctx.table     = spec_.table;
    ctx.db        = pageDb_;          // browse qualifier ("" outside browse mode)
    ctx.dialect   = spec_.dialect;
    ctx.columns   = columns_;
    ctx.pkColumns = spec_.pkColumns;
    return ctx;
}

gridsql::CellGrid ResultGridPanel::SnapshotGrid() const
{
    const int nr = grid_->GetNumberRows(), nc = grid_->GetNumberCols();
    gridsql::CellGrid out;
    out.reserve(nr);
    for (int r = 0; r < nr; ++r) {
        gridsql::CellRow row(nc);
        for (int c = 0; c < nc; ++c) row[c] = grid_->GetCellValue(r, c);
        out.push_back(std::move(row));
    }
    return out;
}

gridsql::CellGrid ResultGridPanel::SnapshotRows(const std::vector<int>& rows) const
{
    const int nc = grid_->GetNumberCols();
    gridsql::CellGrid out;
    out.reserve(rows.size());
    for (int r : rows) {
        gridsql::CellRow row(nc);
        for (int c = 0; c < nc; ++c) row[c] = grid_->GetCellValue(r, c);
        out.push_back(std::move(row));
    }
    return out;
}

// The rows the 复制为 actions act on: the current selection block's row span,
// else just the cursor row. (Distinct from SelectedFullRows(), which requires a
// selection spanning every column.)
std::vector<int> ResultGridPanel::SelectedOrCursorRows() const
{
    std::vector<int> rows;
    const auto tl = grid_->GetSelectionBlockTopLeft();
    const auto br = grid_->GetSelectionBlockBottomRight();
    if (!tl.IsEmpty() && !br.IsEmpty())
        for (int r = tl[0].GetRow(); r <= br[0].GetRow(); ++r) rows.push_back(r);
    else { const int r = grid_->GetGridCursorRow(); if (r >= 0) rows.push_back(r); }
    return rows;
}

// Scripted counterpart of an inline cell edit + 保存. It applies THE SAME
// refusals the interactive path applies, in the same order, and says which one
// fired.
//
// THE STATED INVARIANT: a scripted edit never writes a cell into a grid whose
// result is not bound to an editable table. Until now that was merely true by
// accident — the cell was written unconditionally and the write was caught
// downstream, because an unbound spec carries no PK columns and the PkGate then
// yielded empty SQL. That made a refusal indistinguishable from "nothing to do",
// and left the correctness of the SCRIPTED path resting on a property of the
// PRIMARY-KEY path, so a refactor of the latter could silently arm the former.
// EditableSpec::editable is now checked HERE, first, before the grid is touched
// at all, and the refusal is reported rather than swallowed.
gridsql::ScriptedEditResult ResultGridPanel::ScriptedEdit(const wxString& col,
                                                          const wxString& value)
{
    using S = gridsql::ScriptedEditStatus;
    gridsql::ScriptedEditResult out;

    // 1. The bind gate (ui::qsrc). Refused ⇒ nothing is written, and the reason
    //    the user would have seen under the grid is what the hook reports.
    if (!spec_.editable) {
        out.status = S::NotEditable;
        out.detail = spec_.notEditableReason;
        return out;
    }

    // 2. Address the cell. A missing column or an empty result is a bad script,
    //    not a refused write, and reads differently for exactly that reason.
    int target = -1;
    for (size_t c = 0; c < columns_.size(); ++c)
        if (columns_[c] == col) { target = static_cast<int>(c); break; }
    if (target < 0)               { out.status = S::NoSuchColumn; out.detail = col; return out; }
    if (grid_->GetNumberRows() < 1) { out.status = S::NoRows; return out; }

    grid_->SetCellValue(0, target, value);

    // 3. The PK gate. Same verdict the 保存 button surfaces via
    //    ReportPkGateRefusal — here it is flattened into `detail`.
    const gridsql::EditDml d = BuildEditDml();
    if (d.gate != gridsql::PkGate::Ok) {
        out.status = S::PkRefused;
        for (const wxString& c : d.missingPk) {       // empty ⇔ no PK at all
            if (!out.detail.IsEmpty()) out.detail += L",";
            out.detail += c;
        }
        return out;
    }
    // 4. Both gates passed: an empty diff can now ONLY mean the cell already
    //    held `value`, so it is reported as such and not as a failure.
    if (!d.dirty || d.sql.IsEmpty()) { out.status = S::NoChange; return out; }
    out.sql = d.sql;
    return out;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------
// P0 grid shortcuts (design-product). Ctrl+S is handled by a panel-level
// accelerator (set up in the ctor) so it also fires from the toolbar / editor, so
// it is intentionally NOT re-handled here.
//
// IRON RULE: while a cell editor is open, every key goes to the text box —
// Ctrl+C/V/X/A, Home/End/arrows/Delete/Backspace/Esc/Enter — so inline editing is
// never hijacked. (Ctrl+S still lands the edit + saves, via the accelerator, which
// the message loop checks before this key handler ever runs.)
void ResultGridPanel::OnGridKeyDown(wxKeyEvent& ev)
{
    if (grid_->IsCellEditControlShown()) { ev.Skip(); return; }

    const int kc = ev.GetKeyCode();
    const bool ctrl = ev.ControlDown(), alt = ev.AltDown(), shift = ev.ShiftDown();

    if (ctrl && !alt) {
        if (kc == 'A') { grid_->SelectAll(); return; }
        if (kc == 'C') { if (shift) CopyRowsAsSql(); else CopyCells(); return; }  // Ctrl+Shift+C = as SQL
        if (kc == 'V' && !shift) {
            PasteIntoCell(grid_->GetGridCursorRow(), grid_->GetGridCursorCol());
            return;
        }
        if (kc == WXK_DELETE) { DeleteSelectedRecordsWithConfirm(); return; }     // Ctrl+Delete = delete record
        // P1 (later): Ctrl+PageUp/Down paginate, Ctrl+0 set NULL,
        //   Ctrl+Shift+F filter, Ctrl+Shift+Enter commit txn, Ctrl+Shift+E export.
    }
    if (alt && kc == WXK_INSERT) { wxCommandEvent e; OnAddRow(e); return; }       // Alt+Insert = add row
    if (!ctrl && !alt && kc == WXK_DELETE) { OnGridDeleteKey(); return; }         // Delete = del row / NULL
    if (kc == WXK_F5) { RefreshCurrent(); return; }                              // F5 = refresh

    ev.Skip();   // navigation (arrows/Tab/Home/End/Ctrl+Home·End/PageUp·Down) → wxGrid
}

// Delete key with no modifier: a whole-row selection deletes those records; a cell
// / block selection sets the selected cells to NULL (reversible — no confirm).
void ResultGridPanel::OnGridDeleteKey()
{
    if (!spec_.editable) return;
    if (!SelectedFullRows().empty()) { DeleteSelectedRecordsWithConfirm(); return; }

    const auto tl = grid_->GetSelectionBlockTopLeft();
    const auto br = grid_->GetSelectionBlockBottomRight();
    if (!tl.IsEmpty() && !br.IsEmpty()) {
        for (int r = tl[0].GetRow(); r <= br[0].GetRow(); ++r)
            for (int c = tl[0].GetCol(); c <= br[0].GetCol(); ++c)
                SetCellText(r, c, L"NULL");
    } else {
        const int r = grid_->GetGridCursorRow(), c = grid_->GetGridCursorCol();
        if (r >= 0 && c >= 0) SetCellText(r, c, L"NULL");
    }
    UpdateEditButtons();
}

// Delete the whole-row selection (or the cursor row) with a light confirmation.
// Exception: when every target row is a brand-new unsaved row (rowOrig_ == -1) the
// delete is lossless → no prompt. Deletes bottom-up so indices stay valid.
void ResultGridPanel::DeleteSelectedRecordsWithConfirm()
{
    if (!spec_.editable) return;
    std::vector<int> rows = SelectedFullRows();
    if (rows.empty()) {
        const int r = grid_->GetGridCursorRow();
        if (r >= 0) rows.push_back(r);
    }
    if (rows.empty()) return;

    bool allNew = true;
    for (int r : rows)
        if (r < static_cast<int>(rowOrig_.size()) && rowOrig_[r] >= 0) { allNew = false; break; }

    if (!allNew &&
        wxMessageBox(wxString::Format(tr(L"删除选中的 %d 条记录？"), static_cast<int>(rows.size())),
                     tr(L"删除记录"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;

    std::sort(rows.begin(), rows.end());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) DeleteGridRow(*it);   // bottom-up
}

// Rows selected whole — via a left row-label click (which, in the default
// wxGridSelectCells mode, selects a block spanning every column) or via row
// selection mode. De-duplicated and sorted.
std::vector<int> ResultGridPanel::SelectedFullRows() const
{
    const int nc = grid_->GetNumberCols();
    std::vector<int> rows;
    for (int r : grid_->GetSelectedRows()) rows.push_back(r);          // row-select mode
    const auto tl = grid_->GetSelectionBlockTopLeft();
    const auto br = grid_->GetSelectionBlockBottomRight();
    for (size_t i = 0; i < tl.size() && i < br.size(); ++i)
        if (nc > 0 && tl[i].GetCol() == 0 && br[i].GetCol() == nc - 1)  // spans all columns
            for (int r = tl[i].GetRow(); r <= br[i].GetRow(); ++r) rows.push_back(r);
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

// Push text at the clipboard. Every 复制 action funnels through here.
static void PutOnClipboard(const wxString& text)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

// Copy as TSV. A whole-row selection copies every column of each selected row;
// otherwise the selection block, otherwise just the cursor cell.
void ResultGridPanel::CopyCells()
{
    gridsql::CellGrid block;
    const std::vector<int> fullRows = SelectedFullRows();
    if (!fullRows.empty()) {
        block = SnapshotRows(fullRows);
    } else {
        const auto tl = grid_->GetSelectionBlockTopLeft();
        const auto br = grid_->GetSelectionBlockBottomRight();
        if (!tl.IsEmpty() && !br.IsEmpty()) {
            const int r0 = tl[0].GetRow(), c0 = tl[0].GetCol();
            const int r1 = br[0].GetRow(), c1 = br[0].GetCol();
            for (int r = r0; r <= r1; ++r) {
                gridsql::CellRow row;
                for (int c = c0; c <= c1; ++c) row.push_back(grid_->GetCellValue(r, c));
                block.push_back(std::move(row));
            }
        } else {
            const int r = grid_->GetGridCursorRow(), c = grid_->GetGridCursorCol();
            if (r >= 0 && c >= 0) block.push_back({ grid_->GetCellValue(r, c) });
        }
    }
    PutOnClipboard(gridmodel::JoinTsv(block));
}

void ResultGridPanel::CopyRowsAsSql()
{
    if (columns_.empty()) return;
    const std::vector<int> rows = SelectedOrCursorRows();
    if (rows.empty()) return;
    PutOnClipboard(gridsql::BuildInsertStatements(MakeDmlContext(), SnapshotRows(rows)));
}

// Copy the selected rows as UPDATE (asUpdate) or DELETE statements. The WHERE clause is
// built from the primary key, so this needs a single-table result WITH a PK; otherwise
// a hint is shown (a keyless WHERE would be unsafe). UPDATE sets every non-PK column.
void ResultGridPanel::CopyRowsAsDml(bool asUpdate)
{
    if (columns_.empty() || spec_.pkColumns.empty()) {
        wxMessageBox(tr(L"需要有主键的单表结果才能生成 UPDATE / DELETE 语句。"),
                     tr(L"复制为"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const gridsql::DmlContext ctx = MakeDmlContext();
    // Every pk column must be present in the result — the same all-or-nothing
    // gate the 保存 path applies. Say which columns are missing rather than
    // putting nothing on the clipboard and leaving the user to guess.
    const std::vector<wxString> missing = gridsql::MissingPkColumns(ctx);
    if (!missing.empty()) {
        wxString cols;
        for (const wxString& c : missing) {
            if (!cols.IsEmpty()) cols += L", ";
            cols += c;
        }
        wxMessageBox(wxString::Format(
                         tr(L"当前结果集缺少主键列 %s，无法生成有主键条件的语句。\n\n"
                            L"请重新查询并包含该表的全部主键列。"), cols),
                     tr(L"复制为"), wxOK | wxICON_WARNING, this);
        return;
    }
    const std::vector<int> rows = SelectedOrCursorRows();
    if (rows.empty()) return;
    PutOnClipboard(gridsql::BuildRowDmlStatements(ctx, SnapshotRows(rows), asUpdate));
}

// Paste the clipboard. A single value (no tab) goes into the target cell; a TSV
// row (has tabs) fills the target row's cells from column 0 across the row —
// multiple TSV lines fill consecutive rows, appending new (INSERT) rows past the
// end. So 增加行 → 粘贴 turns a copied row into a new record ready to 保存.
void ResultGridPanel::PasteIntoCell(int row, int col)
{
    if (!spec_.editable) return;
    wxString text;
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject d; wxTheClipboard->GetData(d); text = d.GetText();
        }
        wxTheClipboard->Close();
    }
    // Splitting / classifying the clipboard text is pure (gridmodel::, unit-tested).
    const gridmodel::ClipboardPaste p = gridmodel::ParseClipboardPaste(text);
    if (p.empty) return;

    // Single scalar → single-cell paste (fallback / preserves old behaviour).
    if (p.scalar) {
        if (row >= 0 && col >= 0) SetCellText(row, col, p.value);
        return;
    }

    // Whole-row paste: fill from column 0; append INSERT rows past the grid end.
    const int nc = grid_->GetNumberCols();
    int target = (row >= 0) ? row : grid_->GetNumberRows();
    grid_->BeginBatch();
    for (const auto& fields : p.rows) {
        if (target >= grid_->GetNumberRows()) {
            grid_->AppendRows(1);
            rowOrig_.push_back(-1);                       // new row → INSERT on 保存
        }
        const int n = std::min<int>(nc, static_cast<int>(fields.size()));
        for (int c = 0; c < n; ++c) SetCellText(target, c, fields[c]);
        ++target;
    }
    grid_->EndBatch();
    if (target > 0) { grid_->MakeCellVisible(target - 1, 0); grid_->SetGridCursor(target - 1, 0); }
    UpdateEditButtons();
}

// The grid cell context menu (copy / copy-as / paste / set-null / sort / filter /
// display / remove-all / delete / save-as / refresh). Kept here so ResultGridPanel.cpp
// stays under the 1000-line charter.
void ResultGridPanel::OnGridCellRightClick(wxGridEvent& ev)
{
    const int r = ev.GetRow(), c = ev.GetCol();
    // Keep an existing whole-row selection so 复制 copies the row; only move the
    // cursor when nothing row-wide is selected.
    if (r >= 0 && c >= 0 && SelectedFullRows().empty()) grid_->SetGridCursor(r, c);
    const bool ed = spec_.editable;

    wxMenu m;
    auto add = [&](const wxString& label, std::function<void()> fn, bool enabled = true) {
        wxMenuItem* mi = m.Append(wxID_ANY, label);
        mi->Enable(enabled);
        if (enabled) m.Bind(wxEVT_MENU, [fn](wxCommandEvent&) { fn(); }, mi->GetId());
    };

    // ---- copy / paste (design §7) ----
    add(tr(L"复制"), [this] { CopyCells(); });
    {   // 复制为 ▶
        auto* sub = new wxMenu;
        wxMenuItem* s1 = sub->Append(wxID_ANY, tr(L"SQL (INSERT 语句)"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyRowsAsSql(); }, s1->GetId());
        wxMenuItem* su = sub->Append(wxID_ANY, tr(L"SQL (UPDATE 语句)"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyRowsAsDml(true); }, su->GetId());
        wxMenuItem* sd = sub->Append(wxID_ANY, tr(L"SQL (DELETE 语句)"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyRowsAsDml(false); }, sd->GetId());
        wxMenuItem* s2 = sub->Append(wxID_ANY, tr(L"纯文本值"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyCells(); }, s2->GetId());
        m.AppendSubMenu(sub, tr(L"复制为"));
    }
    add(tr(L"粘贴"), [this, r, c] { PasteIntoCell(r, c); }, ed);
    m.AppendSeparator();

    // ---- cell mutation (editable only) ----
    add(tr(L"设为空字符串"), [this, r, c] { SetCellText(r, c, wxString()); UpdateEditButtons(); }, ed);
    add(tr(L"设为 NULL"),   [this, r, c] { SetCellText(r, c, L"NULL"); UpdateEditButtons(); }, ed);
    m.AppendSeparator();

    const bool hasCol = (c >= 0 && c < static_cast<int>(columns_.size()));

    {   // 排序 ▶ (this column): replace-single asc/desc, or append to multi-sort
        auto* sub = new wxMenu;
        wxMenuItem* sa = sub->Append(wxID_ANY, tr(L"▲ 升序（仅此列）"));
        sub->Bind(wxEVT_MENU, [this, c](wxCommandEvent&) { ApplySort(c, true); }, sa->GetId());
        wxMenuItem* sd = sub->Append(wxID_ANY, tr(L"▼ 降序（仅此列）"));
        sub->Bind(wxEVT_MENU, [this, c](wxCommandEvent&) { ApplySort(c, false); }, sd->GetId());
        sub->AppendSeparator();
        wxMenuItem* sm = sub->Append(wxID_ANY, tr(L"＋ 加入多列排序"));
        sub->Bind(wxEVT_MENU, [this, c](wxCommandEvent&) { ToggleSortKey(c); }, sm->GetId());
        m.AppendSubMenu(sub, tr(L"排序"))->Enable(hasCol);
    }
    {   // 筛选 ▶ by this cell's value
        auto* sub = new wxMenu;
        wxMenuItem* fe = sub->Append(wxID_ANY, tr(L"按此格值筛选"));
        sub->Bind(wxEVT_MENU, [this, c](wxCommandEvent&) { FilterByCellValue(c, false); },
                  fe->GetId());
        wxMenuItem* fx = sub->Append(wxID_ANY, tr(L"排除此格值"));
        sub->Bind(wxEVT_MENU, [this, c](wxCommandEvent&) { FilterByCellValue(c, true); },
                  fx->GetId());
        m.AppendSubMenu(sub, tr(L"筛选"))->Enable(hasCol && !spec_.table.IsEmpty());
    }
    {   // 显示 ▶ — set the cell viewer's render type for this column
        auto* sub = new wxMenu;
        const wxString kinds[] = { tr(L"文本"), L"HEX", L"IMAGE", L"WEB", L"JSON", L"XML" };
        const int      idx[]   = { 0, 1, 5, 4, 2, 3 };   // map to CellViewerPanel type index
        for (size_t k = 0; k < 6; ++k) {
            wxMenuItem* mi = sub->Append(wxID_ANY, kinds[k]);
            const int ti = idx[k];
            sub->Bind(wxEVT_MENU, [this, ti](wxCommandEvent&) { SetCellViewerType(ti); },
                      mi->GetId());
        }
        m.AppendSubMenu(sub, tr(L"显示"))->Enable(cellViewer_ != nullptr);
    }
    add(tr(L"移除所有筛选与排序"), [this] { RemoveAllSortAndFilters(); },
        browseMode_ && !pageTable_.IsEmpty());
    m.AppendSeparator();

    add(tr(L"删除记录"), [this, r] { DeleteGridRow(r); }, ed);
    add(tr(L"将数据另存为…"), [this] { StartExport(); });
    add(tr(L"刷新"), [this] { RefreshCurrent(); });
    grid_->PopupMenu(&m);
}

// Whole-row right-click menu (from the left gutter): the cell menu's row-scoped
// actions. Selects the row first so copy / delete act on the right target; a
// multi-row selection applies to the whole batch.
void ResultGridPanel::ShowRowLabelMenu(int row)
{
    if (SelectedFullRows().empty()) grid_->SelectRow(row);
    grid_->SetGridCursor(row, 0);
    const bool ed = spec_.editable;

    wxMenu m;
    auto add = [&](const wxString& label, std::function<void()> fn, bool enabled = true) {
        wxMenuItem* mi = m.Append(wxID_ANY, label);
        mi->Enable(enabled);
        if (enabled) m.Bind(wxEVT_MENU, [fn](wxCommandEvent&) { fn(); }, mi->GetId());
    };
    add(tr(L"复制整行"), [this] { CopyCells(); });
    {   auto* sub = new wxMenu;
        wxMenuItem* s1 = sub->Append(wxID_ANY, tr(L"SQL (INSERT 语句)"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyRowsAsSql(); }, s1->GetId());
        wxMenuItem* su = sub->Append(wxID_ANY, tr(L"SQL (UPDATE 语句)"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyRowsAsDml(true); }, su->GetId());
        wxMenuItem* sd = sub->Append(wxID_ANY, tr(L"SQL (DELETE 语句)"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyRowsAsDml(false); }, sd->GetId());
        wxMenuItem* s2 = sub->Append(wxID_ANY, tr(L"纯文本值"));
        sub->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyCells(); }, s2->GetId());
        m.AppendSubMenu(sub, tr(L"复制为"));
    }
    add(tr(L"粘贴到新行"), [this] {
        wxCommandEvent e; OnAddRow(e);
        PasteIntoCell(grid_->GetGridCursorRow(), 0);
    }, ed);
    m.AppendSeparator();
    add(tr(L"删除记录"), [this] {
        const std::vector<int> rows = SelectedFullRows();   // delete bottom-up (stable indices)
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) DeleteGridRow(*it);
    }, ed);
    m.AppendSeparator();
    add(tr(L"刷新"), [this] { RefreshCurrent(); });
    grid_->PopupMenu(&m);
}

// Amber wash on the row being edited (§4b); restore the plain white row
// background when the edit ends.
void ResultGridPanel::SetRowWash(int row, bool on)
{
    if (row < 0 || row >= grid_->GetNumberRows()) return;
    const int nc = grid_->GetNumberCols();
    for (int c = 0; c < nc; ++c)
        grid_->SetCellBackgroundColour(row, c,
            on ? theme::kDiffModBg : theme::kWhite);
    grid_->Refresh();
}

// The rows / db / SQL triple surfaced in the global bottom status bar (§5).
void ResultGridPanel::GetStatusInfo(wxString& rows, wxString& db, wxString& sql) const
{
    const long long n = (totalRows_ >= 0) ? totalRows_
                                          : static_cast<long long>(grid_->GetNumberRows());
    rows = wxString::Format(L"%lld ", n) + tr(L"行");
    db   = pageDb_;
    sql  = lastSql_;
}

// Reorder the whole grid by the ordered key list, carrying rowOrig_ along so the
// edit/diff mapping (grid row → original row) survives the sort. Comparator walks
// keys in priority order; first non-equal key decides. Numeric when both cells
// parse as numbers, else case-insensitive text (per-cell, per key).
void ResultGridPanel::SortByColumns(const std::vector<SortKey>& keys)
{
    const int nr = grid_->GetNumberRows(), nc = grid_->GetNumberCols();
    if (keys.empty() || nr <= 1) return;

    // Read out → sort as plain values (gridmodel::SortRows, unit-tested, and the
    // place the rowOrig_ mapping is proven to travel with its cells) → write back.
    std::vector<gridmodel::SortableRow> rows(nr);
    for (int r = 0; r < nr; ++r) {
        rows[r].cells.resize(nc);
        for (int c = 0; c < nc; ++c) rows[r].cells[c] = grid_->GetCellValue(r, c);
        rows[r].orig = (r < static_cast<int>(rowOrig_.size())) ? rowOrig_[r] : -1;
    }
    gridmodel::SortRows(rows, keys);

    grid_->BeginBatch();
    for (int r = 0; r < nr; ++r) {
        for (int c = 0; c < nc; ++c) grid_->SetCellValue(r, c, rows[r].cells[c]);
        if (r < static_cast<int>(rowOrig_.size())) rowOrig_[r] = rows[r].orig;
    }
    grid_->EndBatch();
}

// Grey a cell that holds the NULL marker so a NULL reads as empty/de-emphasised;
// restore the normal body colour once it holds a real value. Called after every
// programmatic set (SetCellText: paste / 设为NULL / 设为空字符串) and after an
// inline edit commits (wxEVT_GRID_CELL_CHANGED).
void ResultGridPanel::RecolourNull(int row, int col)
{
    if (row < 0 || col < 0 || row >= grid_->GetNumberRows() ||
        col >= grid_->GetNumberCols())
        return;
    const bool isNull = (grid_->GetCellValue(row, col) == L"NULL");
    grid_->SetCellTextColour(row, col, isNull ? theme::kTextGhost : theme::kTextBody);
}

// ---------------------------------------------------------------------------
// Data transfer — package the current view for the export dialog, or trigger the
// import dialog. The file dialogs (ExportDialog / ImportDialog) run against the
// live connection, which the grid never holds; MainFrame wires both handlers.
// ---------------------------------------------------------------------------
ExportRequest ResultGridPanel::BuildExportRequest() const
{
    ExportRequest req;
    req.columns = columns_;
    req.dialect = pageDialect_;
    req.table   = browseMode_ ? pageTable_ : spec_.table;
    req.db      = pageDb_;
    req.where   = pageWhere_;
    req.browse  = browseMode_ && !pageTable_.IsEmpty();

    // ORDER BY body from the active sort keys — the SAME generator LoadPage's
    // server-side sort uses, so the exported order can't drift from the shown one.
    req.orderBy = gridsql::BuildOrderByBody(columns_, sortKeys_, pageDialect_);

    // 当前页 — the rows as currently materialised in the grid.
    req.currentPage.columns  = columns_;
    req.currentPage.isSelect = true;
    req.currentPage.rows     = SnapshotGrid();

    // 选中记录 — the full rows spanned by the current selection block (if any).
    const int nr = grid_->GetNumberRows();
    const auto tl = grid_->GetSelectionBlockTopLeft();
    const auto br = grid_->GetSelectionBlockBottomRight();
    if (!tl.IsEmpty() && !br.IsEmpty()) {
        std::vector<int> sel;
        for (int r = tl[0].GetRow(); r <= br[0].GetRow() && r < nr; ++r) sel.push_back(r);
        req.selectedRows = SnapshotRows(sel);
    }
    return req;
}

void ResultGridPanel::StartExport()
{
    if (columns_.empty()) {
        wxMessageBox(tr(L"没有可导出的结果。"), tr(L"导出"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (exportHandler_) exportHandler_(BuildExportRequest());
}

void ResultGridPanel::StartImport()
{
    if (!importHandler_) return;
    const wxString table = browseMode_ ? pageTable_ : spec_.table;
    importHandler_(pageDb_, table, pageDialect_);
}

// ---------------------------------------------------------------------------
// Diff the grid against the original result into INSERT/UPDATE/DELETE. "" when
// nothing changed. Requires an editable single-table result with a primary key.
// ---------------------------------------------------------------------------
gridsql::EditDml ResultGridPanel::BuildEditDml() const
{
    if (!spec_.editable) return gridsql::EditDml{};
    // The whole diff is pure: snapshot the grid, hand it plus the original rows /
    // row mapping / deleted rows to the generator. See ResultGridSql.h (including
    // its note on the composite-PK gate) and tests/ResultGridSqlTests.cpp.
    return gridsql::BuildEditDml(MakeDmlContext(), origRows_, rowOrig_,
                                 deleted_, SnapshotGrid());
}

wxString ResultGridPanel::BuildDml() const { return BuildEditDml().sql; }

// The refusal the composite-PK gate owes the user. Reached from 保存 (and from
// 复制为 UPDATE/DELETE), which are the only places the gate can bite. Silence
// here would read as "saved"; the point of the message is to name the missing
// columns and say what to do about it.
void ResultGridPanel::ReportPkGateRefusal(const gridsql::EditDml& d)
{
    if (d.gate == gridsql::PkGate::Ok) return;
    wxString msg;
    if (d.gate == gridsql::PkGate::NoPrimaryKey) {
        msg = tr(L"该表没有主键，数据浏览器无法确定要修改哪一行，因此不会生成任何语句。");
    } else {
        wxString cols;
        for (const wxString& c : d.missingPk) {
            if (!cols.IsEmpty()) cols += L", ";
            cols += c;
        }
        msg = wxString::Format(
            tr(L"当前结果集缺少主键列 %s，无法安全地定位要修改的行，因此不会生成任何语句。\n\n"
               L"请重新查询并包含该表的全部主键列后再编辑。"), cols);
    }
    wxMessageBox(msg, tr(L"无法保存修改"), wxOK | wxICON_WARNING, this);
}

// ---------------------------------------------------------------------------
// Data browser (打开表) hosting: bind a concrete table + db-qualified paging,
// the unsaved-edit guard, server-side sort, cell-value filter, cell-viewer
// render-type switch, guarded refresh, and the collapsible cell-viewer chrome.
// ---------------------------------------------------------------------------
wxString ResultGridPanel::QualifiedPageTable() const
{
    return gridsql::QualifyForPaging(pageDialect_, pageDb_, pageTable_);
}

void ResultGridPanel::SetSourceTable(const wxString& db, const wxString& table,
                                     db::Dialect dialect)
{
    pageDb_      = db;
    pageTable_   = table;
    pageDialect_ = dialect;
    pageWhere_.clear();
    sortKeys_.clear();
    curPage_    = 0;
    browseMode_ = true;      // page/sort/where owned by the pager, not reset on re-run
}

void ResultGridPanel::LoadFirstPage()
{
    if (pageTable_.IsEmpty()) return;
    RequestCount();
    LoadPage();
}

// .dirty, NOT !.sql.IsEmpty(): when the PK gate refuses there ARE pending edits
// but no statements, and answering "clean" there would grey out 保存 and let the
// unsaved-changes guard wave a page turn through, discarding them silently.
bool ResultGridPanel::HasUnsavedEdits() const { return BuildEditDml().dirty; }

// Unsaved-edit guard (design decision B / P0 red line). Default (and Esc) = 取消.
bool ResultGridPanel::ConfirmDiscardPendingEdits()
{
    if (!HasUnsavedEdits()) return true;
    wxMessageDialog dlg(this, tr(L"当前有未保存的修改。是否先保存,再继续?"),
                        tr(L"未保存的修改"),
                        wxYES_NO | wxCANCEL | wxICON_WARNING | wxCANCEL_DEFAULT);
    dlg.SetYesNoCancelLabels(tr(L"保存"), tr(L"放弃"), tr(L"取消"));
    const int r = dlg.ShowModal();
    if (r == wxID_CANCEL) return false;                 // stay put, lose nothing
    if (r == wxID_YES) { wxCommandEvent e; OnSaveData(e); }  // 保存 then proceed
    return true;                                          // wxID_NO → discard + proceed
}

// Guard, then commit the new sort key list and re-query (browse, server-side
// ORDER BY that survives page turns) or client-sort (editor). One source of
// truth: header gestures, the cell menu, and the SortPanel all funnel here and
// keep the panel mirror in sync. 取消 aborts and leaves sortKeys_ unchanged.
void ResultGridPanel::SetSortKeysAndApply(std::vector<SortKey> keys)
{
    if (!ConfirmDiscardPendingEdits()) return;   // abort: keep old sortKeys_

    const bool serverSide = browseMode_ && !pageTable_.IsEmpty();

    // Large-result guard. Browse mode routes to a server-side ORDER BY (below), but
    // an arbitrary editor result has no owned single-table SQL to re-issue, so its
    // only option is the synchronous client sort — which would freeze the UI on a
    // very large grid. Past the threshold, make the big sort an explicit choice
    // rather than a silent hang; small results keep their instant sort. Checked
    // before mutating sortKeys_ so a decline leaves the panel state untouched.
    if (!serverSide && grid_->GetNumberRows() > kClientSortRowLimit) {
        wxMessageDialog dlg(this,
            wxString::Format(tr(L"当前结果有 %d 行。此结果不是单表浏览，只能在界面线程"
                                L"一次性完成客户端排序，期间界面可能短暂无响应。是否继续？"),
                             grid_->GetNumberRows()),
            tr(L"排序较大的结果集"),
            wxYES_NO | wxICON_WARNING | wxNO_DEFAULT);
        dlg.SetYesNoLabels(tr(L"继续排序"), tr(L"取消"));
        if (dlg.ShowModal() != wxID_YES) return;   // leave sortKeys_ unchanged
    }

    sortKeys_ = std::move(keys);
    if (sortPanel_) {
        std::vector<std::pair<int, bool>> pairs;
        for (const SortKey& k : sortKeys_) pairs.emplace_back(k.col, k.asc);
        sortPanel_->SetKeys(pairs);              // keep the panel editor in sync
    }
    if (serverSide) {
        curPage_ = 0; LoadPage();                // server-side; Relabel in ShowResult
    } else {
        SortByColumns(sortKeys_);
        RelabelSortHeaders();
    }
}

// Replace the whole sort with a single key (plain header click / cell 仅此列 /
// header right-click 升序·降序).
void ResultGridPanel::ApplySort(int col, bool asc)
{
    if (col < 0 || col >= static_cast<int>(columns_.size())) return;
    SetSortKeysAndApply({ { col, asc } });
}

// Shift-click / ＋加入多列排序: append this column as the next key, flip
// asc→desc, or (already desc) remove it. Builds/edits the multi-key list.
void ResultGridPanel::ToggleSortKey(int col)
{
    if (col < 0 || col >= static_cast<int>(columns_.size())) return;
    SetSortKeysAndApply(gridmodel::NextKeysOnToggle(sortKeys_, col));
}

// From the SortPanel 应用: adopt its ordered (col, asc) key list.
void ResultGridPanel::ApplySortKeys(const std::vector<std::pair<int, bool>>& keys)
{
    std::vector<SortKey> sk;
    for (const auto& k : keys) sk.push_back({ k.first, k.second });
    SetSortKeysAndApply(std::move(sk));
}

void ResultGridPanel::RemoveAllSortAndFilters()
{
    if (!ConfirmDiscardPendingEdits()) return;
    sortKeys_.clear();
    if (sortPanel_) sortPanel_->SetKeys({});
    pageWhere_.clear();
    curPage_ = 0;
    if (browseMode_ && !pageTable_.IsEmpty()) { RequestCount(); LoadPage(); }
    else RelabelSortHeaders();
}

// Push a one-condition WHERE (col = / <> this cell) through the same server-side
// filter path as the panel. NULL maps to IS [NOT] NULL.
void ResultGridPanel::FilterByCellValue(int col, bool exclude)
{
    if (col < 0 || col >= static_cast<int>(columns_.size()) || spec_.table.IsEmpty()) return;
    const int r = grid_->GetGridCursorRow();
    const wxString val = (r >= 0) ? grid_->GetCellValue(r, col) : wxString();
    ApplyFilter(gridsql::BuildCellFilterClause(pageDialect_, columns_[col], val, exclude));
}

// 显示▶ from the cell menu routes to the same reveal-and-render path as 文本▾.
void ResultGridPanel::SetCellViewerType(int typeIndex) { ShowCellViewerAs(typeIndex); }

namespace {
const wchar_t* const kViewTypeNames[] = { L"文本", L"HEX", L"JSON", L"XML", L"WEB", L"IMAGE" };
}

// Reveal the (default-hidden) bottom viewer and render the current cell in the
// chosen type; keep the top-toolbar 文本▾ label in sync.
void ResultGridPanel::ShowCellViewerAs(int typeIndex)
{
    if (!cellViewer_) return;
    cellViewerVisible_   = true;
    cellViewerCollapsed_ = false;
    if (cvHeader_) cvHeader_->Show(true);
    cellViewer_->Show(true);
    cellViewer_->SetType(typeIndex);
    if (cvChevron_)
        cvChevron_->SetBitmap(icons::Stroke(icons::Glyph::ChevronDown, 13,
                                            theme::kTextSecondary, 1.8));
    if (cvSummary_) cvSummary_->Show(false);
    if (viewTypeBtn_) viewTypeBtn_->Refresh();   // repaint the split-button type label
    const int r = grid_->GetGridCursorRow(), c = grid_->GetGridCursorCol();
    if (r >= 0 && c >= 0) {
        const wxString v = grid_->GetCellValue(r, c);
        cellViewer_->ShowValue(v);
        UpdateCellViewerSummary(v);
    }
    Layout();
}

void ResultGridPanel::ShowViewTypeMenu()
{
    if (!cellViewer_) return;
    // Clicking 文本▾ pops the type menu immediately; the bottom viewer is revealed
    // and re-rendered only once a type is chosen (design refinement).
    wxMenu m;
    const int cur = cellViewer_->GetType();
    for (int i = 0; i < 6; ++i) {
        wxMenuItem* mi = m.AppendCheckItem(wxID_ANY, tr(kViewTypeNames[i]));
        mi->Check(i == cur);
        m.Bind(wxEVT_MENU, [this, i](wxCommandEvent&) { ShowCellViewerAs(i); }, mi->GetId());
    }
    // Drop below the button so it reads as a real dropdown.
    if (viewTypeBtn_)
        viewTypeBtn_->PopupMenu(&m, 0, viewTypeBtn_->GetSize().GetHeight());
    else
        PopupMenu(&m);
}

// Split-button body: show the (default-hidden) cell viewer, or hide it if shown.
// "Click once to appear, click again to disappear" — no menu.
void ResultGridPanel::ToggleViewTypeBody()
{
    if (!cellViewer_) return;
    if (cellViewerVisible_) {
        cellViewerVisible_ = false;
        if (cvHeader_) cvHeader_->Hide();
        cellViewer_->Hide();
        Layout();
    } else {
        ShowCellViewerAs(cellViewer_->GetType());   // reveal + render current type
    }
}

void ResultGridPanel::RefreshCurrent()
{
    if (!ConfirmDiscardPendingEdits()) return;
    if (browseMode_ && !pageTable_.IsEmpty()) { LoadPage(); return; }
    wxCommandEvent e(wxEVT_BUTTON, ID_RUN_QUERY);
    e.SetEventObject(this);
    ProcessWindowEvent(e);   // bubbles up to MainFrame's run handler
}

void ResultGridPanel::ToggleCellViewer()
{
    if (!cellViewer_) return;
    cellViewerCollapsed_ = !cellViewerCollapsed_;
    cellViewer_->Show(!cellViewerCollapsed_);
    if (cvChevron_)
        cvChevron_->SetBitmap(icons::Stroke(
            cellViewerCollapsed_ ? icons::Glyph::ChevronRight : icons::Glyph::ChevronDown,
            13, theme::kTextSecondary, 1.8));
    if (cvSummary_) cvSummary_->Show(cellViewerCollapsed_);
    Layout();
}

void ResultGridPanel::UpdateCellViewerSummary(const wxString& value)
{
    if (!cvSummary_) return;
    cvSummary_->SetLabel(gridmodel::SummarizeCellValue(value));
    if (cvHeader_) cvHeader_->Layout();
}

// BuildFilterSortArea / ToggleFilterPanel / ToggleSortPanel /
// UpdatePanelAreaVisibility live in ResultGridPanelChrome.cpp with the rest of
// the widget construction.

} // namespace ui
