#include "ui/ResultGridPanel.h"

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <algorithm>

#include "ui/CellViewerPanel.h"
#include "ui/DataGrid.h"
#include "ui/FilterPanel.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/NullCellEditor.h"
#include "ui/ResultGridModel.h"
#include "ui/ResultGridSql.h"
#include "ui/SortPanel.h"
#include "ui/TableInfoPanel.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
// ---------------------------------------------------------------------------
ResultGridPanel::ResultGridPanel(wxWindow* parent, const Features& feat)
    : wxPanel(parent), feat_(feat)
{
    SetBackgroundColour(theme::kWhite);
    auto* v = new wxBoxSizer(wxVERTICAL);

    // ---- results: [ grid/messages notebook | table-info panel ] ----
    resultsSplit_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize,
                                         wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxBORDER_NONE);
    resultsSplit_->SetMinimumPaneSize(0);
    // The data browser (resultTabs=false) shows only the grid — no 结果/消息
    // notebook — so the grid parents straight into the splitter's left pane.
    wxWindow* gridParent = resultsSplit_;
    if (feat_.resultTabs) {
        results_ = new wxNotebook(resultsSplit_, wxID_ANY);
        results_->SetFont(Ui(9));
        gridParent = results_;
    }

    activeGrid_ = new DataGrid(gridParent, wxID_ANY);
    grid_ = activeGrid_;
    grid_->CreateGrid(0, 0);
    // Row labels (row numbers) stay visible so the user can click the left row
    // label to select a whole row (copy-row / paste-into-new-row workflow). Cell
    // selection + the cell viewer keep working (default wxGridSelectCells mode).
    grid_->SetRowLabelSize(46);
    grid_->SetRowLabelAlignment(wxALIGN_CENTRE, wxALIGN_CENTRE);
    grid_->EnableEditing(false);
    grid_->SetLabelBackgroundColour(theme::kGridHeaderBg);
    grid_->SetLabelTextColour(theme::kTextSecondary);
    grid_->SetLabelFont(Ui(9));
    grid_->SetColLabelAlignment(wxALIGN_LEFT, wxALIGN_CENTRE);   // Navicat-style left headers
    grid_->SetDefaultCellFont(Mono(9));
    grid_->SetDefaultCellTextColour(theme::kTextBody);
    grid_->SetDefaultCellBackgroundColour(theme::kWhite);   // uniform white data area
    grid_->SetBackgroundColour(theme::kWhite);              // incl. space beyond the cells
    grid_->SetGridLineColour(theme::kBorderGrid);
    grid_->SetDefaultRowSize(24);                                 // compact rows
    grid_->SetColLabelSize(28);
    grid_->SetSelectionBackground(wxColour(0xD6, 0xE6, 0xFB));    // soft blue selection
    grid_->SetSelectionForeground(theme::kTextStrong);
    grid_->SetCellHighlightPenWidth(1);                           // thin current-cell outline
    // Header click sorts / header right-click hides a column — only when the sort
    // feature is on; cell right-click (copy/paste/NULL/delete) is always available.
    if (feat_.sortHide) {
        grid_->Bind(wxEVT_GRID_LABEL_LEFT_CLICK,  &ResultGridPanel::OnGridLabelLeftClick,  this);
        grid_->Bind(wxEVT_GRID_LABEL_RIGHT_CLICK, &ResultGridPanel::OnGridLabelRightClick, this);
    }
    grid_->Bind(wxEVT_GRID_CELL_RIGHT_CLICK,  &ResultGridPanel::OnGridCellRightClick,  this);
    grid_->Bind(wxEVT_GRID_SELECT_CELL,       &ResultGridPanel::OnGridSelectCell,      this);
    // NULL cells edit as an empty box (no "NULL" text to delete); an empty commit
    // restores NULL. See NullCellEditor.h. Applies grid-wide (editor + browser).
    grid_->SetDefaultEditor(new NullCellEditor());
    // An inline cell edit is a pending change → refresh Save + re-grey NULL cells.
    grid_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& e) {
        e.Skip(); RecolourNull(e.GetRow(), e.GetCol()); UpdateEditButtons();
    });
    // The moment the user starts editing a cell, show 保存(green) + 取消(red) as
    // actionable — don't wait for the edit to commit (design §1: 输入即显现).
    grid_->Bind(wxEVT_GRID_EDITOR_SHOWN, [this](wxGridEvent& e) {
        e.Skip();
        if (!spec_.editable) return;
        editRow_ = e.GetRow();                      // amber wash on the editing row (§4b)
        SetRowWash(editRow_, true);
        if (saveData_) {
            saveData_->Enable(true);
            saveData_->SetBitmap(icons::Stroke(icons::Glyph::Commit, 16, theme::kGreen, 1.8));
            saveData_->SetToolTip(wxString());
        }
        if (cancelBtn_) {
            cancelBtn_->Enable(true);
            cancelBtn_->SetBitmap(icons::Stroke(icons::Glyph::Close, 16, theme::kDotRed, 1.8));
        }
    });
    // When a cell edit commits, if focus has left the grid (to a non-取消 area) and
    // edits are pending, auto-save without a confirm dialog (design §2). Moving to
    // another cell / pressing Enter keeps focus in the grid → stays pending. 取消
    // gets focus first, so FindFocus()==cancelBtn_ suppresses the auto-save.
    grid_->Bind(wxEVT_GRID_EDITOR_HIDDEN, [this](wxGridEvent& e) {
        e.Skip();
        if (editRow_ >= 0) { SetRowWash(editRow_, false); editRow_ = -1; }  // clear wash
        UpdateEditButtons();
        CallAfter([this]() { AutoSaveOnLeave(); });
    });
    grid_->SetTabBehaviour(wxGrid::Tab_Wrap);   // Tab wraps to the next row's first cell
    // P0 grid shortcuts (copy/paste/delete/add/refresh/…). Bind on the grid's cell
    // window (the actual keyboard-focus target), falling back to the grid itself so
    // the shortcut fires wherever focus lands.
    grid_->Bind(wxEVT_KEY_DOWN, &ResultGridPanel::OnGridKeyDown, this);
    if (wxWindow* gw = grid_->GetGridWindow())
        gw->Bind(wxEVT_KEY_DOWN, &ResultGridPanel::OnGridKeyDown, this);
    // Panel-level Ctrl+S → save, so it fires even when focus is on the toolbar or an
    // open cell editor. The message loop checks this before the grid key handler, so
    // it's the single Ctrl+S path (no double-save). Lands any open edit first.
    {
        static constexpr int kIdSave = wxID_HIGHEST + 200;
        wxAcceleratorEntry acc(wxACCEL_CTRL, static_cast<int>('S'), kIdSave);
        SetAcceleratorTable(wxAcceleratorTable(1, &acc));
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (grid_ && grid_->IsCellEditControlShown()) {
                grid_->SaveEditControlValue();
                grid_->DisableCellEditControl();
            }
            wxCommandEvent e; OnSaveData(e);
        }, kIdSave);
    }

    // In no-tab mode messages_ has no 消息 page to live on; keep it as an off-screen
    // text buffer (parented to the panel, hidden) so ShowResult/ShowError can still
    // record the last outcome, while errors are surfaced via the inline errorBar_.
    messages_ = new wxTextCtrl(feat_.resultTabs ? static_cast<wxWindow*>(results_)
                                                : static_cast<wxWindow*>(this),
                               wxID_ANY, L"", wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
    messages_->SetFont(Mono(9));
    if (!feat_.resultTabs) messages_->Hide();

    // ---- chrome bands, in top-to-bottom layout order. Each band is assembled
    // in ResultGridPanelChrome.cpp so this ctor stays a readable skeleton. ----
    if (feat_.filter || feat_.sortHide || feat_.importCsv || feat_.exportCsv)
        BuildTopToolbar(v);

    // Filter + sort panels live above the grid in a resizable area with a
    // draggable divider (ResultGridPanelChrome.cpp).
    if (feat_.filter || feat_.sortHide) BuildFilterSortArea(v);

    // With tabs, the grid + messages (+ optional 表单) are notebook pages; without
    // tabs (data browser) the grid is the splitter's left pane directly.
    if (feat_.resultTabs) {
        results_->AddPage(grid_, tr(L"结果"), true);
        results_->AddPage(messages_, tr(L"消息"), false);

        // ---- 表单 page: the current record rendered as a vertical label/value list ----
        if (feat_.formView) {
            auto* form = new wxScrolledWindow(results_, wxID_ANY);
            form->SetBackgroundColour(theme::kWhite);
            form->SetScrollRate(8, 12);
            formView_ = form;
            results_->AddPage(formView_, tr(L"表单"), false);
            formPageIdx_ = static_cast<int>(results_->GetPageCount()) - 1;
        }
    }

    // Inline error banner — the no-tab data browser has no 消息 page, so a failed
    // query surfaces here (above the grid) instead of vanishing. Hidden until an
    // error lands; cleared on the next successful ShowResult.
    if (!feat_.resultTabs) {
        errorBar_ = new wxStaticText(this, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        errorBar_->SetBackgroundColour(theme::kDiffDelBg);   // AA danger token pair
        errorBar_->SetForegroundColour(theme::kDiffDelFg);
        errorBar_->SetFont(Mono(9));
        errorBar_->Hide();
        v->Add(errorBar_, 0, wxEXPAND);
    }

    // The splitter's left pane is the notebook (tabs) or the bare grid (browser).
    resultsPane_ = feat_.resultTabs ? static_cast<wxWindow*>(results_)
                                    : static_cast<wxWindow*>(grid_);
    if (feat_.infoPanel) {
        infoPanel_ = new TableInfoPanel(resultsSplit_);
        resultsSplit_->SplitVertically(resultsPane_, infoPanel_, -240);
        resultsSplit_->SetSashGravity(1.0);               // grid grows, info panel fixed
        resultsSplit_->SetMinSize(wxSize(-1, 260));
        resultsSplit_->Unsplit(infoPanel_);               // hidden until a single-table result loads
    } else {
        resultsSplit_->Initialize(resultsPane_);
        resultsSplit_->SetMinSize(wxSize(-1, 260));
    }
    v->Add(resultsSplit_, 1, wxEXPAND);

    // ---- bottom cell viewer: the selected cell rendered in the chosen type,
    // under a collapsible chevron header. Hidden until 文本▾ summons it. ----
    if (feat_.cellViewer) BuildCellViewerChrome(v);

    // ---- bottom toolbar: left = data/txn ops, right = pager ----
    BuildBottomBar(v);

    // The rows/db/SQL readout moved to MainFrame's global bottom status bar (shown
    // only for data-browser tabs, design §5). status_/recordLabel_ are kept as
    // hidden sinks so the existing status-update calls remain harmless no-ops.
    status_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    status_->Hide();
    recordLabel_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    recordLabel_->Hide();

    // Bottom export bar — the query window removes the top toolbar, so 导出 lives
    // at the very bottom of the results instead.
    if (feat_.exportBottom) BuildBottomExportBar(v);

    SetSizer(v);
    UpdateEditButtons();
}

// ---------------------------------------------------------------------------
void ResultGridPanel::SetRunning(bool on)
{
    if (on) {
        status_->SetLabel(tr(L"● 正在执行查询…"));
        status_->SetForegroundColour(theme::kPrimary);
    }
    if (stopBtn_) stopBtn_->Enable(on);               // 停止 lights up only while running
    for (wxButton* b : { pageFirst_, pagePrev_, pageNext_, pageLast_ })
        if (b) b->Enable(!on ? b->IsEnabled() : false);  // freeze pager during a run
    if (!on) UpdatePager();                            // restore correct pager state
    status_->GetParent()->Layout();
}

// Empty the grid and drop the edit/diff state — used to clear stale results before a
// new query runs (so the previous result isn't shown while the next one executes).
void ResultGridPanel::Clear()
{
    grid_->BeginBatch();
    if (grid_->GetNumberRows() > 0) grid_->DeleteRows(0, grid_->GetNumberRows());
    if (grid_->GetNumberCols() > 0) grid_->DeleteCols(0, grid_->GetNumberCols());
    grid_->EndBatch();
    columns_.clear();
    origRows_.clear();
    rowOrig_.clear();
    deleted_.clear();
    if (cellViewer_) cellViewer_->Clear();
}

void ResultGridPanel::ShowResult(const db::QueryResult& r, const wxString& target,
                                 const EditableSpec& spec)
{
    // snapshot the result for diffing on save
    spec_ = spec;
    // Cap how many rows we materialize into the grid: copying every row and
    // filling wxGrid cell-by-cell on the UI thread freezes the app for huge
    // result sets. Editable (single-table) results are always paginated well
    // under this cap, so it only ever trims oversized free-form SELECTs —
    // signalled in the message box and status line below.
    constexpr size_t kMaxDisplayRows = 5000;
    const size_t totalRows = r.rows.size();
    const size_t shownRows = std::min(totalRows, kMaxDisplayRows);
    const bool   truncated = shownRows < totalRows;

    origRows_.assign(r.rows.begin(), r.rows.begin() + shownRows);
    columns_ = r.columns;
    rowOrig_.clear();
    deleted_.clear();
    // Browse mode drives a server-side ORDER BY that must survive page turns, so
    // only an editor (client-side sort) result clears the keys here.
    if (!browseMode_) sortKeys_.clear();

    grid_->BeginBatch();
    if (grid_->GetNumberRows() > 0) grid_->DeleteRows(0, grid_->GetNumberRows());
    if (grid_->GetNumberCols() > 0) grid_->DeleteCols(0, grid_->GetNumberCols());

    if (r.isSelect) {
        grid_->AppendCols(static_cast<int>(r.columns.size()));
        for (size_t c = 0; c < r.columns.size(); ++c)
            grid_->SetColLabelValue(static_cast<int>(c), r.columns[c]);
        grid_->AppendRows(static_cast<int>(shownRows));
        // Greyed NULLs on a plain white sheet — rows stay uniformly white
        // (zebra striping removed per user preference).
        const wxColour nullFg = theme::kTextGhost;      // #A6ADB8 muted null
        for (size_t row = 0; row < shownRows; ++row) {
            for (size_t c = 0; c < r.rows[row].size(); ++c) {
                const int rr = static_cast<int>(row), cc = static_cast<int>(c);
                const wxString& val = r.rows[row][c];
                grid_->SetCellValue(rr, cc, val);
                if (val == L"NULL") grid_->SetCellTextColour(rr, cc, nullFg);
            }
            rowOrig_.push_back(static_cast<int>(row));
        }
        grid_->AutoSizeColumns(false);
        // Cap absurdly wide columns (long JSON/blobs) so the grid stays readable.
        for (int c = 0; c < grid_->GetNumberCols(); ++c)
            if (grid_->GetColSize(c) > 360) grid_->SetColSize(c, 360);
    }
    grid_->EnableEditing(spec_.editable);
    // Calendar editor for pure `date` cols ONLY — never datetime/timestamp/time
    // (date-only editor drops the time part on save). ISO format keeps read/write YYYY-MM-DD.
    if (spec_.editable && static_cast<int>(spec_.colTypes.size()) == grid_->GetNumberCols()) {
        for (int c = 0; c < grid_->GetNumberCols(); ++c) {
            wxString t = spec_.colTypes[c].Lower(); t.Trim(true).Trim(false);
            if (t != L"date") continue;
            auto* attr = new wxGridCellAttr;
            attr->SetEditor(new wxGridCellDateEditor(L"%Y-%m-%d"));
            grid_->SetColAttr(c, attr);
        }
    }
    grid_->EndBatch();

    if (filterPanel_) filterPanel_->SetColumns(columns_, spec_.dialect);
    if (sortPanel_)   sortPanel_->SetColumns(columns_, spec_.dialect);
    if (filterBtn_)   filterBtn_->Enable(!spec_.table.IsEmpty());  // filter needs a base table
    if (cellViewer_)  cellViewer_->Clear();
    if (recordLabel_) recordLabel_->SetLabel(wxEmptyString);       // no row selected yet
    if (results_ && formView_ && formPageIdx_ >= 0 &&
        results_->GetSelection() == formPageIdx_)
        RefreshFormView();                                          // reflect the new columns/rows

    // ---- pagination context ----
    pageDialect_ = spec_.dialect;
    if (paginating_) {
        paginating_ = false;      // page/filter/sort reload — keep curPage_/where
    } else if (!browseMode_) {
        // a fresh editor run: adopt this table as the paging base (if single-table).
        // Browse-mode page/where/db state is owned by the pager (SetSourceTable),
        // never reset by a re-run, so a refresh can't drop the db qualifier.
        pageTable_ = spec_.editable ? spec_.table : wxString();
        pageDb_.clear();
        pageWhere_.clear();
        curPage_ = 0;
        RequestCount();
    }
    RefreshInfoPanel();           // guarded: only (re)fetches when the table changes
    if (browseMode_) RelabelSortHeaders();   // restore ▲/▼ the re-query's labels dropped
    UpdatePager();

    messages_->SetForegroundColour(theme::kGreen);
    messages_->SetValue(wxString::Format(
        L"✓ %s · %llu rows · %ld ms",
        r.isSelect ? L"SELECT" : L"OK", r.affected, r.elapsedMs));
    if (truncated)
        messages_->AppendText(wxString::Format(
            tr(L"\n⚠ 结果较大：表格仅显示前 %llu 行（共 %llu 行）。请加 LIMIT 或导出查看全部。"),
            static_cast<unsigned long long>(shownRows),
            static_cast<unsigned long long>(totalRows)));
    // Why this result is read-only, when we know and it isn't obvious. Set only
    // for a REFUSAL (see EditableSpec::notEditableReason) — never for a plain
    // join/aggregate, which needs no note.
    if (!spec_.notEditableReason.IsEmpty())
        messages_->AppendText(L"\n" + spec_.notEditableReason);

    status_->SetLabel(truncated
        ? wxString::Format(tr(L"● %llu rows（仅显示前 %llu）   %ld ms   |   %s"),
                           r.affected, static_cast<unsigned long long>(shownRows),
                           r.elapsedMs, target)
        : wxString::Format(L"● %llu rows   %ld ms   |   %s",
                           r.affected, r.elapsedMs, target));
    status_->SetForegroundColour(theme::kGreen);
    if (results_) results_->SetSelection(0);          // show the grid page (tab mode)
    if (errorBar_ && errorBar_->IsShown()) {          // clear any prior inline error
        errorBar_->Hide();
        Layout();
    }
    UpdateEditButtons();
    status_->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Data editing
// ---------------------------------------------------------------------------
void ResultGridPanel::UpdateEditButtons()
{
    const bool on = spec_.editable;
    if (addRow_)   addRow_->Enable(on);
    if (delRow_)   delRow_->Enable(on);
    const bool dirty = on && HasUnsavedEdits();
    if (saveData_) {
        // Save is clickable AND green only when there are actual pending changes;
        // grey + disabled when there's nothing to save.
        saveData_->Enable(dirty);
        saveData_->SetBitmap(icons::Stroke(icons::Glyph::Commit, 16,
                             dirty ? theme::kGreen : theme::kTextGhost, 1.8));
        saveData_->SetToolTip(!on  ? tr(L"仅单表查询且有主键时可编辑数据")
                            : dirty ? wxString()
                                    : tr(L"没有待保存的修改"));
    }
    if (cancelBtn_) {
        cancelBtn_->Enable(dirty);
        cancelBtn_->SetBitmap(icons::Stroke(icons::Glyph::Close, 16,
                              dirty ? theme::kDotRed : theme::kTextGhost, 1.8));
        cancelBtn_->SetToolTip(dirty ? tr(L"取消(放弃未保存的修改)")
                                     : tr(L"没有待放弃的修改"));
    }

    // Top-toolbar gating: 导入 needs an editable single-table result; 排序/导出
    // work for any non-empty result. (开始事务/筛选 are gated elsewhere.)
    const bool hasResult = !columns_.empty();
    if (importBtn_) {
        importBtn_->Enable(on);
        importBtn_->SetToolTip(on ? wxString()
                                  : tr(L"仅可编辑的单表结果支持从 CSV 导入行"));
    }
    if (sortBtn_)   sortBtn_->Enable(hasResult);
    if (exportBtn_) exportBtn_->Enable(hasResult);
}

void ResultGridPanel::OnAddRow(wxCommandEvent&)
{
    const int r = grid_->GetNumberRows();
    grid_->AppendRows(1);
    rowOrig_.push_back(-1);                 // new row → INSERT on 保存
    grid_->SetGridCursor(r, 0);
    grid_->MakeCellVisible(r, 0);
    grid_->SelectRow(r);                    // highlight the whole new row
    // Return keyboard focus to the grid so Ctrl+V lands here (the 增加行 button
    // had grabbed it) — this is what makes "增加行 → Ctrl+V paste" work. Re-assert
    // via CallAfter so the button event draining can't steal it back.
    grid_->SetFocus();
    CallAfter([this]() { if (grid_) grid_->SetFocus(); });
    UpdateEditButtons();   // new (empty) row: Save stays off until it gets content
}

void ResultGridPanel::OnDeleteRow(wxCommandEvent&)
{
    const int r = grid_->GetGridCursorRow();
    if (r < 0 || r >= grid_->GetNumberRows()) return;
    if (r < static_cast<int>(rowOrig_.size()) && rowOrig_[r] >= 0)
        deleted_.push_back(origRows_[rowOrig_[r]]);   // remember for DELETE
    grid_->DeleteRows(r, 1);
    if (r < static_cast<int>(rowOrig_.size()))
        rowOrig_.erase(rowOrig_.begin() + r);
    UpdateEditButtons();   // a delete is a pending change
}

void ResultGridPanel::OnSaveData(wxCommandEvent&)
{
    const gridsql::EditDml d = BuildEditDml();
    if (!d.dirty) return;        // Save is disabled when clean; nothing to apply
    // The PK gate refused: there are pending edits but no safe way to key them.
    // Say so — doing nothing quietly would read to the user as a successful save.
    if (d.gate != gridsql::PkGate::Ok) { ReportPkGateRefusal(d); return; }
    const wxString& dml = d.sql;
    if (dml.IsEmpty()) return;
    // 保存 = apply the pending edits to the DB. It does NOT commit or close a manual
    // transaction (design §4): with a transaction open, the DML runs inside it (made
    // permanent by 提交事务, undone by 回滚事务); with no transaction it applies and
    // autocommits. No confirm dialog — 取消 is the safety net (design §2).
    if (inTxn_ && applyInTxn_) applyInTxn_(dml);
    else if (applyDml_)        applyDml_(dml);
}

// Apply pending edits silently once the user leaves the grid after editing. Skips
// when focus stayed inside the grid (moved to another cell / pressed Enter) or when
// it landed on 取消 (which discards instead).
void ResultGridPanel::AutoSaveOnLeave()
{
    const gridsql::EditDml d = BuildEditDml();
    if (!spec_.editable || !d.dirty) return;
    // Gate refused: leave the edits pending rather than popping the explanation
    // on every focus change. 保存 stays lit and explains itself when clicked, and
    // ConfirmDiscardPendingEdits still guards a page turn — nothing is lost.
    if (d.gate != gridsql::PkGate::Ok) return;
    wxWindow* f = wxWindow::FindFocus();
    if (f == cancelBtn_) return;
    for (wxWindow* w = f; w; w = w->GetParent())
        if (w == grid_) return;                  // still inside the grid → keep pending
    wxCommandEvent e; OnSaveData(e);             // left the grid → auto-save
}

// 取消, or after a txn commit / rollback: drop pending edits and re-fetch the current
// page so the grid reflects the true server (or in-txn) state.
void ResultGridPanel::DiscardPendingReload()
{
    if (grid_->IsCellEditControlShown()) grid_->DisableCellEditControl();
    deleted_.clear();
    if (browseMode_ && !pageTable_.IsEmpty()) {
        LoadPage();                              // ShowResult resets rowOrig_/deleted_/edits
    } else {
        wxCommandEvent e(wxEVT_BUTTON, ID_RUN_QUERY);
        e.SetEventObject(this);
        ProcessWindowEvent(e);                   // editor: re-run the current query
    }
}

// Swap the toolbar's transaction slot: idle shows 开始事务; an open transaction
// shows 回滚事务 + 提交事务 in its place. Reflow so the width change settles.
void ResultGridPanel::UpdateTxnButtons()
{
    if (!txBeginBtn_) return;                   // toolbar not built (feature off)
    txBeginBtn_->Show(!inTxn_);
    if (txRollbackBtn_) txRollbackBtn_->Show(inTxn_);
    if (txCommitBtn_)   txCommitBtn_->Show(inTxn_);
    if (txBeginBtn_->GetParent()) txBeginBtn_->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Data browser: sort / hide-column / cell context menu
// ---------------------------------------------------------------------------
void ResultGridPanel::SetCellText(int row, int col, const wxString& v)
{
    if (row < 0 || col < 0 || row >= grid_->GetNumberRows() ||
        col >= grid_->GetNumberCols())
        return;
    grid_->SetCellValue(row, col, v);   // diffed against origRows_ on 保存修改
    RecolourNull(row, col);             // grey (Null) marker / normal for a value
}

void ResultGridPanel::DeleteGridRow(int r)
{
    if (r < 0 || r >= grid_->GetNumberRows()) return;
    if (r < static_cast<int>(rowOrig_.size()) && rowOrig_[r] >= 0)
        deleted_.push_back(origRows_[rowOrig_[r]]);   // remember for DELETE
    grid_->DeleteRows(r, 1);
    if (r < static_cast<int>(rowOrig_.size()))
        rowOrig_.erase(rowOrig_.begin() + r);
    UpdateEditButtons();   // a delete is a pending change
}

// SortByColumns lives in ResultGridPanelOps.cpp (client-side multi-key reorder).

// A sorted column shows ▲/▼; with more than one key it also carries the 1-based
// priority (e.g. `created_at ▼1  severity ▲2`). wxGrid labels are plain text.
void ResultGridPanel::RelabelSortHeaders()
{
    const std::vector<wxString> labels =
        gridmodel::SortHeaderLabels(columns_, sortKeys_);
    for (int c = 0; c < grid_->GetNumberCols() && c < static_cast<int>(labels.size()); ++c)
        grid_->SetColLabelValue(c, labels[c]);
}

void ResultGridPanel::OnGridLabelLeftClick(wxGridEvent& ev)
{
    const int col = ev.GetCol();
    if (col < 0) { ev.Skip(); return; }

    // Plain click = sort by ONLY this column (three-state); Shift-click = add /
    // cycle this column as the next key (design S-C). Both edit one sortKeys_;
    // the key algebra itself is pure (gridmodel::, unit-tested).
    if (ev.ShiftDown())
        ToggleSortKey(col);
    else
        SetSortKeysAndApply(gridmodel::NextKeysOnHeaderClick(sortKeys_, col));
    // no Skip → suppress the default full-column selection
}

void ResultGridPanel::OnGridLabelRightClick(wxGridEvent& ev)
{
    const int col = ev.GetCol();
    if (col < 0) {                       // a row-label (gutter) click → whole-row menu
        const int row = ev.GetRow();
        if (row >= 0) ShowRowLabelMenu(row);
        else ev.Skip();
        return;
    }
    wxMenu m;
    wxMenuItem* a = m.Append(wxID_ANY, tr(L"升序排序"));
    m.Bind(wxEVT_MENU, [this, col](wxCommandEvent&) { ApplySort(col, true); }, a->GetId());
    wxMenuItem* d = m.Append(wxID_ANY, tr(L"降序排序"));
    m.Bind(wxEVT_MENU, [this, col](wxCommandEvent&) { ApplySort(col, false); }, d->GetId());
    m.AppendSeparator();
    wxMenuItem* h = m.Append(wxID_ANY, tr(L"隐藏此列"));
    m.Bind(wxEVT_MENU, [this, col](wxCommandEvent&) { grid_->HideCol(col); }, h->GetId());
    wxMenuItem* s = m.Append(wxID_ANY, tr(L"显示所有列"));
    m.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        for (int c = 0; c < grid_->GetNumberCols(); ++c) grid_->ShowCol(c);
    }, s->GetId());
    grid_->PopupMenu(&m);
}

// OnGridCellRightClick lives in ResultGridPanelOps.cpp (the full cell menu).

void ResultGridPanel::OnGridSelectCell(wxGridEvent& ev)
{
    const int r = ev.GetRow(), c = ev.GetCol();
    activeRow_ = r;                                     // gutter highlight (§4a)
    if (activeGrid_) activeGrid_->SetActiveRow(r);
    if (cellViewer_ && cellViewerVisible_ && r >= 0 && c >= 0) {
        const wxString v = grid_->GetCellValue(r, c);
        cellViewer_->ShowValue(v);
        UpdateCellViewerSummary(v);                     // keep the collapsed summary live
    }
    UpdateRecordStatus(r);                              // current-row / total (numeric)
    // keep the 表单 page in sync while it is the visible one (the grid cursor
    // hasn't moved yet during this event, so feed it the incoming row).
    if (results_ && formView_ && formPageIdx_ >= 0 &&
        results_->GetSelection() == formPageIdx_)
        RefreshFormView(r);
    ev.Skip();   // let the grid handle the selection normally
}

// ---------------------------------------------------------------------------
// 表单 view — the selected record as a vertical column:value list.
// ---------------------------------------------------------------------------
void ResultGridPanel::RefreshFormView(int rowOverride)
{
    if (!formView_) return;
    const int r = (rowOverride >= 0) ? rowOverride : grid_->GetGridCursorRow();
    const bool hasRow = (r >= 0 && r < grid_->GetNumberRows() && !columns_.empty());

    formView_->DestroyChildren();
    auto* col = new wxBoxSizer(wxVERTICAL);
    if (!hasRow) {
        auto* hint = new wxStaticText(formView_, wxID_ANY, tr(L"（请选择一行）"));
        hint->SetFont(Ui(9));
        hint->SetForegroundColour(theme::kTextGhost);
        col->Add(hint, 0, wxALL, 16);
    } else {
        auto* fg = new wxFlexGridSizer(2, 8, 16);
        fg->AddGrowableCol(1, 1);
        for (size_t c = 0; c < columns_.size(); ++c) {
            auto* l = new wxStaticText(formView_, wxID_ANY, columns_[c]);
            l->SetFont(Ui(9));
            l->SetForegroundColour(theme::kTextSecondary);
            const wxString val = grid_->GetCellValue(r, static_cast<int>(c));
            const bool isNull = (val == L"NULL");
            auto* vl = new wxStaticText(formView_, wxID_ANY, isNull ? tr(L"(Null)") : val);
            vl->SetFont(Mono(9));
            vl->SetForegroundColour(isNull ? theme::kTextGhost : theme::kText);
            fg->Add(l, 0, wxALIGN_TOP);
            fg->Add(vl, 1, wxEXPAND);
        }
        col->Add(fg, 1, wxEXPAND | wxALL, 16);
    }
    formView_->SetSizer(col, /*deleteOld*/ true);
    formView_->FitInside();
    formView_->Layout();
}

void ResultGridPanel::ShowFormView()
{
    if (!results_ || formPageIdx_ < 0) return;
    RefreshFormView(grid_->GetGridCursorRow());
    results_->SetSelection(formPageIdx_);
}

void ResultGridPanel::UpdateRecordStatus(int row)
{
    if (!recordLabel_) return;
    const int total = grid_->GetNumberRows();
    if (row < 0 || total == 0)
        recordLabel_->SetLabel(wxEmptyString);
    else
        recordLabel_->SetLabel(wxString::Format(L"%d / %d", row + 1, total));  // numeric only
    recordLabel_->GetParent()->Layout();
}

// ToggleFilterPanel / ToggleSortPanel live in ResultGridPanelChrome.cpp
// (mutually-exclusive panel-area switching).

// Apply the filter's WHERE: reset to page 0, recount (for the new predicate), and
// reload. Needs a single-table result — the 筛选 button is disabled otherwise.
void ResultGridPanel::ApplyFilter(const wxString& where)
{
    if (spec_.table.IsEmpty()) {
        wxMessageBox(tr(L"筛选仅适用于单表结果。"), tr(L"筛选"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!ConfirmDiscardPendingEdits()) return;    // guard: no silent edit loss
    pageTable_ = spec_.table;                     // pageDb_ (browse qualifier) preserved
    pageWhere_ = where;
    curPage_   = 0;
    RequestCount();
    LoadPage();
}

// ---------------------------------------------------------------------------
// Pagination
// ---------------------------------------------------------------------------
void ResultGridPanel::GoToPage(int page)
{
    if (pageTable_.IsEmpty()) return;
    if (!ConfirmDiscardPendingEdits()) return;    // guard: no silent edit loss
    curPage_ = gridmodel::ClampPage(page, totalRows_, pageSize_);
    LoadPage();
}

// Build the paged query (carrying the current filter WHERE) and stage it for the
// container to re-run — paginating_ tells ShowResult to preserve the page context.
void ResultGridPanel::LoadPage()
{
    if (pageTable_.IsEmpty()) return;
    // Statement text (incl. the server-side multi-column ORDER BY in sortKeys_
    // priority order) is built by the pure generator — see ResultGridSql.h.
    const wxString sql = gridsql::BuildPageSelect(pageDialect_, pageDb_, pageTable_,
                                                  pageWhere_, columns_, sortKeys_,
                                                  pageSize_, curPage_);
    lastSql_ = sql;                 // surfaced in the global status bar (§5)
    paginating_ = true;
    if (stageQuery_) stageQuery_(sql);
    wxCommandEvent e(wxEVT_BUTTON, ID_RUN_QUERY);
    e.SetEventObject(this);
    ProcessWindowEvent(e);
}

void ResultGridPanel::RequestCount()
{
    totalRows_ = -1;
    if (countRows_ && !pageTable_.IsEmpty())
        countRows_(pageTable_, pageWhere_, [this](long long n) {
            totalRows_ = n; UpdatePager();
        });
    UpdatePager();
}

void ResultGridPanel::UpdatePager()
{
    // Label + every button's enable-state derive from one pure computation, so
    // the indicator and the buttons can never disagree (gridmodel::, unit-tested).
    const gridmodel::PagerState st =
        gridmodel::ComputePagerState(!pageTable_.IsEmpty(), totalRows_, curPage_, pageSize_);
    if (pageLabel_) pageLabel_->SetLabel(st.label);   // numeric-only: "page / pages"
    if (pageFirst_) pageFirst_->Enable(st.first);
    if (pagePrev_)  pagePrev_->Enable(st.prev);
    if (pageNext_)  pageNext_->Enable(st.next);
    if (pageLast_)  pageLast_->Enable(st.last);
    if (pageSizeChoice_) pageSizeChoice_->Enable(st.sizeChoice);
    if (pageLabel_) pageLabel_->GetParent()->Layout();
    if (statusChanged_) statusChanged_();   // mirror rows/db/SQL to the global bar (§5)
}

void ResultGridPanel::ShowError(const wxString& err)
{
    // messages_ always keeps the full text (it is the 消息 page in tab mode, or an
    // off-screen buffer in the data browser). Feedback must never go silent, so:
    //   tab mode → switch to the 消息 page; no-tab mode → reveal the inline banner.
    messages_->SetForegroundColour(theme::kDotRed);
    messages_->SetValue(L"✗ " + err);
    status_->SetForegroundColour(theme::kDotRed);
    if (results_) {
        status_->SetLabel(tr(L"● 查询失败(详见消息页)"));
        results_->SetSelection(1);
    } else {
        status_->SetLabel(tr(L"● 查询失败"));
        if (errorBar_) {
            errorBar_->SetLabel(L"✗ " + err);
            errorBar_->Show();
            Layout();
        }
    }
    status_->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Right-hand table-info panel: show it only for single-table results and fetch
// its data via the handler (population lives in TableInfoPanel).
// ---------------------------------------------------------------------------
void ResultGridPanel::RefreshInfoPanel()
{
    if (!resultsSplit_ || !infoPanel_) return;
    const bool show = !spec_.table.IsEmpty();
    if (show && !resultsSplit_->IsSplit())
        resultsSplit_->SplitVertically(resultsPane_, infoPanel_, -240);
    else if (!show && resultsSplit_->IsSplit())
        resultsSplit_->Unsplit(infoPanel_);
    if (!show) { infoShownTable_.clear(); return; }
    if (spec_.table == infoShownTable_) return;   // same table — don't refetch on page turns
    infoShownTable_ = spec_.table;
    infoPanel_->SetTitle(spec_.table);
    infoPanel_->Reset();
    if (tableDetail_)
        tableDetail_(spec_.table, [this](const db::TableDetail& d) { infoPanel_->Populate(d); });
}

} // namespace ui
