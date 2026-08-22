// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ResultGridPanelChrome.cpp — third translation unit for ResultGridPanel: the
// pure widget-assembly half. Every band of chrome the panel stacks vertically is
// built here (top toolbar, the 文本▾ split button, the resizable filter/sort
// area, the collapsible cell-viewer header, the bottom ops+pager bar, and the
// query-window bottom export bar), leaving ResultGridPanel.cpp's constructor a
// readable skeleton: configure the grid, then call these in layout order.
//
// This is the widget-level seam. The logic seams live in ResultGridSql.{h,cpp}
// and ResultGridModel.{h,cpp}, which are wx-widget-free and unit-tested; nothing
// in THIS file is testable headlessly, which is exactly why it is isolated —
// keeping untestable construction code out of the files that hold behaviour.
#include "ui/ResultGridPanel.h"

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/notebook.h>
#include <wx/dcbuffer.h>
#include <algorithm>

#include "ui/CellViewerPanel.h"
#include "ui/FilterPanel.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/SortPanel.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

// Width of the ▾ hit region on the right of the 文本▾ split button. File-scope
// rather than function-local so the paint/click lambdas can name it without an
// explicit capture (MSVC /std:c++17 refuses the implicit one).
static constexpr int kArrowW = 20;

// Bubble a MainFrame command ID up the window chain (run / stop / txn verbs the
// panel itself does not own — the container holds the connection).
void ResultGridPanel::PostCommand(int id)
{
    wxCommandEvent e(wxEVT_BUTTON, id);
    e.SetEventObject(this);
    ProcessWindowEvent(e);
}

// ---------------------------------------------------------------------------
// Top toolbar (Navicat order): 筛选 · 排序 · 文本▾ · 事务 · 导入 · 导出
// ---------------------------------------------------------------------------
void ResultGridPanel::BuildTopToolbar(wxSizer* v)
{
    auto* gtb = new wxPanel(this);
    gtb->SetBackgroundColour(theme::kMenuBg);
    auto* gh = new wxBoxSizer(wxHORIZONTAL);
    auto gtbBtn = [&](icons::Glyph g, const wxString& label, std::function<void()> fn,
                      const wxColour& col = theme::kTextSecondary) {
        auto* b = new wxButton(gtb, wxID_ANY, label, wxDefaultPosition,
                               wxSize(-1, 28), wxBORDER_NONE);
        b->SetBitmap(icons::Stroke(g, 20, col, 1.9));  // larger, clearer (col = semantic hue)
        b->SetBitmapMargins(1, 0);                     // tight icon↔label (~4px visual)
        b->SetBackgroundColour(theme::kMenuBg);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        // hover feedback: background lifts to the chrome hover tint, restores on leave
        b->Bind(wxEVT_ENTER_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kChromeBtnHover); b->Refresh(); e.Skip();
        });
        b->Bind(wxEVT_LEAVE_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kMenuBg); b->Refresh(); e.Skip();
        });
        gh->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        return b;
    };
    gh->AddSpacer(8);
    if (feat_.filter)
        filterBtn_ = gtbBtn(icons::Glyph::Filter, tr(L"筛选"), [this] { ToggleFilterPanel(); });
    if (feat_.sortHide)
        sortBtn_   = gtbBtn(icons::Glyph::Sort, tr(L"排序"), [this] { ToggleSortPanel(); });
    if (feat_.cellViewer) BuildViewTypeButton(gtb, gh);
    // Stateful transaction slot: idle → [开始事务]; while open → [回滚事务][提交事务].
    // The three buttons occupy the same position; UpdateTxnButtons() shows one set.
    txBeginBtn_ = gtbBtn(icons::Glyph::TxDatabase, tr(L"开始事务"),
        [this] { PostCommand(ID_TX_BEGIN); inTxn_ = true; UpdateTxnButtons(); });
    // 回滚事务: ROLLBACK undoes every save applied since 开始事务 → reload shows
    // the pre-transaction data; reset the edit buttons; restore [开始事务].
    txRollbackBtn_ = gtbBtn(icons::Glyph::TxRollback, tr(L"回滚事务"),
        [this] { PostCommand(ID_TX_ROLLBACK); inTxn_ = false; UpdateTxnButtons();
                 DiscardPendingReload(); }, theme::kDotRed);
    // 提交事务: COMMIT persists everything → reload shows the committed data.
    txCommitBtn_ = gtbBtn(icons::Glyph::TxCommit, tr(L"提交事务"),
        [this] { PostCommand(ID_TX_COMMIT); inTxn_ = false; UpdateTxnButtons();
                 DiscardPendingReload(); }, theme::kGreen);
    if (feat_.importCsv)
        importBtn_ = gtbBtn(icons::Glyph::Import, tr(L"导入"), [this] { StartImport(); });
    if (feat_.exportCsv)
        exportBtn_ = gtbBtn(icons::Glyph::Export, tr(L"导出"), [this] { StartExport(); });
    gh->AddStretchSpacer();
    gtb->SetSizer(gh);
    gtb->SetMinSize(wxSize(-1, 34));   // room for the larger 20px glyphs
    v->Add(gtb, 0, wxEXPAND);
    UpdateTxnButtons();                // initial: show 开始事务, hide 回滚/提交
}

// 文本▾ — a split button: the body (icon + type label) toggles the bottom cell
// viewer; the right ▾ region (kArrowW px) pops the render-type menu.
void ResultGridPanel::BuildViewTypeButton(wxWindow* bar, wxSizer* h)
{
    auto* vt = new wxPanel(bar, wxID_ANY, wxDefaultPosition, wxSize(88, 28));
    vt->SetBackgroundStyle(wxBG_STYLE_PAINT);
    vt->SetBackgroundColour(theme::kMenuBg);
    vt->Bind(wxEVT_PAINT, [this, vt](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(vt);
        const wxSize sz = vt->GetClientSize();
        dc.SetBrush(wxBrush(viewTypeHover_ ? theme::kChromeBtnHover : theme::kMenuBg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        const int sepX = sz.x - kArrowW;
        dc.DrawBitmap(icons::Stroke(icons::Glyph::Eye, 20, theme::kTextSecondary, 1.9),
                      3, (sz.y - 20) / 2, true);
        static const wchar_t* const kN[] = { L"文本", L"HEX", L"JSON",
                                             L"XML", L"WEB", L"IMAGE" };
        const int ti = cellViewer_ ? cellViewer_->GetType() : 0;
        const wxString lbl = tr(kN[(ti >= 0 && ti < 6) ? ti : 0]);
        dc.SetFont(Ui(9));
        dc.SetTextForeground(theme::kTextSecondary);
        wxCoord tw = 0, th = 0; dc.GetTextExtent(lbl, &tw, &th);
        dc.DrawText(lbl, 26, (sz.y - th) / 2);
        dc.SetPen(wxPen(theme::kBorder, 1));                    // 1px split divider
        dc.DrawLine(sepX, 6, sepX, sz.y - 6);
        dc.DrawBitmap(icons::Stroke(icons::Glyph::ChevronDown, 12,
                                    theme::kTextSecondary, 1.8),
                      sepX + (kArrowW - 12) / 2, (sz.y - 12) / 2, true);
    });
    vt->Bind(wxEVT_LEFT_UP, [this, vt](wxMouseEvent& e) {
        const wxSize sz = vt->GetClientSize();
        if (e.GetX() >= sz.x - kArrowW) ShowViewTypeMenu();   // ▾ region → menu
        else                            ToggleViewTypeBody(); // body → show/hide
    });
    vt->Bind(wxEVT_ENTER_WINDOW, [this, vt](wxMouseEvent& e) {
        viewTypeHover_ = true;  vt->Refresh(); e.Skip();
    });
    vt->Bind(wxEVT_LEAVE_WINDOW, [this, vt](wxMouseEvent& e) {
        viewTypeHover_ = false; vt->Refresh(); e.Skip();
    });
    viewTypeBtn_ = vt;
    h->Add(vt, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
}

// ---------------------------------------------------------------------------
// Bottom cell viewer — the selected cell's full content in the chosen render type
// (文本/HEX/JSON/XML/WEB/IMAGE), wrapped in a collapsible chevron header (unified
// pattern, design §4). The collapsed state shows a one-line summary and survives
// page turns.
// ---------------------------------------------------------------------------
void ResultGridPanel::BuildCellViewerChrome(wxSizer* v)
{
    cvHeader_ = new wxPanel(this);
    cvHeader_->SetBackgroundColour(theme::kSidebarBg);
    auto* hh = new wxBoxSizer(wxHORIZONTAL);
    cvChevron_ = new wxButton(cvHeader_, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(22, 22), wxBORDER_NONE);
    cvChevron_->SetBitmap(icons::Stroke(icons::Glyph::ChevronDown, 13,
                                        theme::kTextSecondary, 1.8));
    cvChevron_->SetBackgroundColour(theme::kSidebarBg);
    cvChevron_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleCellViewer(); });
    auto* cvLbl = new wxStaticText(cvHeader_, wxID_ANY, tr(L"查看器"));
    cvLbl->SetFont(Ui(9));
    cvLbl->SetForegroundColour(theme::kTextSecondary);
    cvSummary_ = new wxStaticText(cvHeader_, wxID_ANY, wxEmptyString);
    cvSummary_->SetFont(Mono(8));
    cvSummary_->SetForegroundColour(theme::kTextMuted);
    cvSummary_->Hide();                                   // shown only when collapsed
    hh->Add(cvChevron_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    hh->Add(cvLbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    hh->Add(cvSummary_, 1, wxALIGN_CENTER_VERTICAL);
    cvHeader_->SetSizer(hh);
    cvHeader_->SetMinSize(wxSize(-1, 24));
    v->Add(cvHeader_, 0, wxEXPAND);

    // The render-type selector now lives on the top toolbar (文本▾), so the panel
    // is built without its internal dropdown and stays hidden until the user
    // summons it via 文本▾ / the cell-menu 显示▶.
    cellViewer_ = new CellViewerPanel(this, /*showSelector*/ false);
    cellViewer_->SetMinSize(wxSize(-1, 130));
    v->Add(cellViewer_, 0, wxEXPAND);
    cvHeader_->Hide();
    cellViewer_->Hide();
}

// ---------------------------------------------------------------------------
// Bottom toolbar: left = data/txn ops, right = pager (+ grid/form toggle).
// Icon-only by design: no Chinese text labels; each control is a vector glyph
// with a tr() tooltip. Semantic colour only for commit=green and
// rollback/stop/delete=red; everything else single-tone.
// ---------------------------------------------------------------------------
void ResultGridPanel::BuildBottomBar(wxSizer* v)
{
    auto* editBar = new wxPanel(this);
    editBar->SetBackgroundColour(theme::kMenuBg);
    auto* eh = new wxBoxSizer(wxHORIZONTAL);

    auto iconBtn = [&](icons::Glyph g, const wxString& tip, const wxColour& col,
                       std::function<void()> fn) {
        auto* b = new wxButton(editBar, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(30, 26), wxBORDER_NONE);
        b->SetBitmap(icons::Stroke(g, 16, col, 1.8));
        b->SetToolTip(tip);
        b->SetBackgroundColour(theme::kMenuBg);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        // Hover feedback (same as the top toolbar): lift the background on enter,
        // restore on leave. Applies to every bottom-bar icon button uniformly.
        b->Bind(wxEVT_ENTER_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kChromeBtnHover); b->Refresh(); e.Skip();
        });
        b->Bind(wxEVT_LEAVE_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kMenuBg); b->Refresh(); e.Skip();
        });
        eh->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
        return b;
    };

    eh->AddSpacer(8);
    if (feat_.editable) {
        addRow_ = iconBtn(icons::Glyph::Plus, tr(L"增加行"), theme::kTextSecondary,
                          [this] { wxCommandEvent e; OnAddRow(e); });
        delRow_ = iconBtn(icons::Glyph::Minus, tr(L"删除行"), theme::kDotRed,
                          [this] { wxCommandEvent e; OnDeleteRow(e); });
        saveData_ = iconBtn(icons::Glyph::Commit, tr(L"保存"), theme::kTextSecondary,
                            [this] { wxCommandEvent e; OnSaveData(e); });
        cancelBtn_ = iconBtn(icons::Glyph::Close, tr(L"取消(放弃未保存的修改)"),
                             theme::kTextSecondary, [this] { DiscardPendingReload(); });
        auto* s = new wxStaticText(editBar, wxID_ANY, L"│");
        s->SetForegroundColour(theme::kBorder);
        eh->Add(s, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    }

    // Transaction controls live in the toolbar's stateful slot (§3); the bottom
    // bar keeps only 刷新 · 停止 here.
    iconBtn(icons::Glyph::Refresh, tr(L"刷新"), theme::kTextSecondary,
            [this] { RefreshCurrent(); });
    stopBtn_ = iconBtn(icons::Glyph::Stop, tr(L"停止查询"), theme::kDotRed,
                       [this] { PostCommand(ID_STOP_QUERY); });
    stopBtn_->Enable(false);   // only while a query runs

    eh->AddStretchSpacer();

    // ---- pager (right) — icon buttons + a numeric-only page indicator ----
    if (feat_.pager) {
        pageFirst_ = iconBtn(icons::Glyph::PageFirst, tr(L"首页"), theme::kTextSecondary,
                             [this] { GoToPage(0); });
        pagePrev_  = iconBtn(icons::Glyph::PagePrev, tr(L"上一页"), theme::kTextSecondary,
                             [this] { GoToPage(curPage_ - 1); });
        pageLabel_ = new wxStaticText(editBar, wxID_ANY, L"—");
        pageLabel_->SetFont(Mono(8));
        pageLabel_->SetForegroundColour(theme::kTextSecondary);
        eh->Add(pageLabel_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
        pageNext_ = iconBtn(icons::Glyph::PageNext, tr(L"下一页"), theme::kTextSecondary,
                            [this] { GoToPage(curPage_ + 1); });
        pageLast_ = iconBtn(icons::Glyph::PageLast, tr(L"末页"), theme::kTextSecondary,
                            [this] { GoToPage(1 << 29); });
        pageSizeChoice_ = new wxChoice(editBar, wxID_ANY);
        for (const wchar_t* s : { L"100", L"200", L"500", L"1000" }) pageSizeChoice_->Append(s);
        pageSizeChoice_->SetSelection(3);          // default 1000 rows/page (design §6)
        pageSizeChoice_->SetToolTip(tr(L"每页行数"));
        pageSizeChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (!ConfirmDiscardPendingEdits()) {   // guard: don't drop unsaved edits
                UpdatePager(); return;              // (choice already changed; leave it)
            }
            long val = 1000; pageSizeChoice_->GetStringSelection().ToLong(&val);
            pageSize_ = static_cast<int>(val); curPage_ = 0; LoadPage();
        });
        eh->Add(pageSizeChoice_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
        eh->AddSpacer(12);
    }

    // ---- grid / form view toggle (bottom-right, per the Navicat reference) ----
    if (feat_.formView) {
        auto viewBtn = [&](icons::Glyph g, const wxString& tip, std::function<void()> fn) {
            auto* b = new wxButton(editBar, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxSize(30, 26), wxBORDER_NONE);
            b->SetBitmap(icons::Stroke(g, 16, theme::kTextSecondary, 1.8));
            b->SetToolTip(tip);
            b->SetBackgroundColour(theme::kMenuBg);
            b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
            eh->Add(b, 0, wxALIGN_CENTER_VERTICAL);
            return b;
        };
        gridViewBtn_ = viewBtn(icons::Glyph::Table, tr(L"网格视图"),
                               [this] { if (results_) results_->SetSelection(0); });
        formViewBtn_ = viewBtn(icons::Glyph::ViewLayers, tr(L"表单视图"),
                               [this] { ShowFormView(); });
        eh->AddSpacer(8);
    }

    editBar->SetSizer(eh);
    editBar->SetMinSize(wxSize(-1, 34));
    v->Add(editBar, 0, wxEXPAND);
}

// The query window removes the top toolbar, so 导出 lives at the very bottom of
// the results instead.
void ResultGridPanel::BuildBottomExportBar(wxSizer* v)
{
    auto* bb = new wxPanel(this);
    bb->SetBackgroundColour(theme::kSidebarBg);
    auto* bh = new wxBoxSizer(wxHORIZONTAL);
    bh->AddStretchSpacer();
    auto* ex = new wxButton(bb, wxID_ANY, tr(L"导出"), wxDefaultPosition,
                            wxDefaultSize, wxBORDER_NONE);
    ex->SetBitmap(icons::Stroke(icons::Glyph::Export, 16, theme::kTextSecondary, 1.8));
    ex->SetBitmapMargins(2, 0);
    ex->SetBackgroundColour(theme::kSidebarBg);
    ex->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartExport(); });
    bh->Add(ex, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    bb->SetSizer(bh);
    v->Add(bb, 0, wxEXPAND);
}

// ---------------------------------------------------------------------------
// Resizable filter/sort area: a container panel holding the filter + sort panels,
// with a draggable grip below it so the user can grow/shrink the whole panel area
// vs. the grid. The grip + area are shown only while a panel is open, so a
// collapsed panel leaves the grid full-height (no leftover sash). Kept out of the
// existing grid|info splitter (resultsSplit_) so the two never fight.
// ---------------------------------------------------------------------------
void ResultGridPanel::BuildFilterSortArea(wxSizer* v)
{
    panelArea_ = new wxPanel(this);
    panelArea_->SetBackgroundColour(theme::kMenuBg);
    auto* pas = new wxBoxSizer(wxVERTICAL);
    panelArea_->SetSizer(pas);

    // Toggling / Esc-hiding a panel re-evaluates the area's visibility uniformly.
    auto onShow = [this](wxShowEvent& e) {
        e.Skip(); CallAfter([this] { UpdatePanelAreaVisibility(); });
    };
    if (feat_.filter) {
        filterPanel_ = new FilterPanel(panelArea_, [this](const wxString& w) { ApplyFilter(w); });
        filterPanel_->Hide();
        filterPanel_->Bind(wxEVT_SHOW, onShow);
        pas->Add(filterPanel_, 1, wxEXPAND);
    }
    if (feat_.sortHide) {
        sortPanel_ = new SortPanel(panelArea_,
            [this](const std::vector<std::pair<int, bool>>& keys) { ApplySortKeys(keys); });
        sortPanel_->Hide();
        sortPanel_->Bind(wxEVT_SHOW, onShow);
        pas->Add(sortPanel_, 1, wxEXPAND);
    }
    panelArea_->SetMinSize(wxSize(-1, panelAreaHeight_));
    panelArea_->Hide();
    v->Add(panelArea_, 0, wxEXPAND);

    // ---- draggable divider (grip) ----
    panelGrip_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 6));
    panelGrip_->SetBackgroundColour(theme::kBorder);
    panelGrip_->SetCursor(wxCursor(wxCURSOR_SIZENS));
    panelGrip_->Hide();
    panelGrip_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        panelGrip_->CaptureMouse();
        dragStartY_ = wxGetMousePosition().y;
        dragStartH_ = panelAreaHeight_;
        e.Skip();
    });
    panelGrip_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (panelGrip_->HasCapture()) {
            int h = dragStartH_ + (wxGetMousePosition().y - dragStartY_);
            h = std::max(120, std::min(h, 460));         // keep a sane min/max
            if (h != panelAreaHeight_) {
                panelAreaHeight_ = h;
                panelArea_->SetMinSize(wxSize(-1, h));
                Layout();
            }
        }
        e.Skip();
    });
    panelGrip_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        if (panelGrip_->HasCapture()) panelGrip_->ReleaseMouse();
        e.Skip();
    });
    panelGrip_->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
    v->Add(panelGrip_, 0, wxEXPAND);
}

// Filter and Sort are mutually exclusive: the panel area hosts at most one at a
// time, so neither can be occluded by the other. Hiding a panel only Hide()s it —
// its condition rows / sort keys survive untouched and reappear on the next Show().
// Only the panel's own 「清除」 button (or the tab closing) clears its state.
void ResultGridPanel::ToggleFilterPanel()
{
    if (!filterPanel_) return;
    if (filterPanel_->IsShown()) {
        filterPanel_->Hide();
    } else {
        filterPanel_->Show();
        if (sortPanel_) sortPanel_->Hide();
    }
    UpdatePanelAreaVisibility();
}

void ResultGridPanel::ToggleSortPanel()
{
    if (!sortPanel_) return;
    if (sortPanel_->IsShown()) {
        sortPanel_->Hide();
    } else {
        sortPanel_->Show();
        if (filterPanel_) filterPanel_->Hide();
    }
    UpdatePanelAreaVisibility();
}

void ResultGridPanel::UpdatePanelAreaVisibility()
{
    if (!panelArea_) return;
    const bool any = (filterPanel_ && filterPanel_->IsShown()) ||
                     (sortPanel_ && sortPanel_->IsShown());
    if (panelArea_->IsShown() != any) panelArea_->Show(any);
    if (panelGrip_ && panelGrip_->IsShown() != any) panelGrip_->Show(any);
    Layout();
}

} // namespace ui
