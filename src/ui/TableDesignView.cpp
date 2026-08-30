// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/TableDesignView.h"

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/popupwin.h>
#include <wx/listbox.h>
#include <wx/checklst.h>
#include <wx/clipbrd.h>
#include <wx/stc/stc.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "db/SqlKeywords.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/FlatControls.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/DesignGridCells.h"

namespace ui {

// Ui()/Mono() font helpers come from ui/UiFonts.h (pulled in via FlatControls.h);
// the file-local duplicates were removed to avoid an ambiguous-overload clash.

namespace {

void StyleGrid(wxGrid* g)
{
    g->HideRowLabels();
    g->EnableEditing(false);
    g->SetSelectionMode(wxGrid::wxGridSelectRows);
    g->SetLabelBackgroundColour(theme::kGridHeaderBg);
    g->SetLabelTextColour(theme::kTextMuted);
    g->SetLabelFont(Ui(8.5, true));
    g->SetDefaultCellFont(Mono(9.5));
    g->SetDefaultCellTextColour(theme::kTextBody);
    g->SetGridLineColour(theme::kBorderGrid);
    g->SetDefaultCellBackgroundColour(theme::kWhite);   // uniform white data area
    g->SetBackgroundColour(theme::kWhite);              // incl. space beyond the cells
    g->SetDefaultRowSize(30);
    g->SetColLabelSize(34);
}

// The 8 design-view tabs. Order is fixed and mirrors the wxSimplebook page order
// and the TabId enum in the header. The 触发器 tab is hidden per-dialect (see Load →
// SupportsTriggers). ⑥SQL预览 = incremental ALTER; ⑦TABLE DDL = full CREATE.
constexpr int kTabCount = 8;
const wchar_t* kTabNames[kTabCount] = {
    L"字段", L"索引", L"外键", L"触发器", L"选项", L"注释", L"DDL 预览", L"TABLE DDL"
};

// Field-form columns: 序号 + the 8 field attributes. Column indices here index the
// FlexGridSizer's columns (序号 is a real column now, not a grid row-label gutter).
enum FormCol {
    F_Num = 0, F_Name, F_Type, F_Length, F_Scale, F_Key, F_NotNull, F_Virtual, F_Comment, F_Count
};
const wchar_t* kHeaderCols[F_Count] = {
    L"序号", L"字段名", L"类型", L"长度", L"精度", L"键", L"非空", L"虚拟", L"注释"
};
const int kColMin[F_Count] = { 42, 150, 140, 66, 66, 46, 46, 46, 200 };

// 字段名 / 注释 read left-aligned (free text); every other column stays centred.
inline bool ColLeft(int col) { return col == F_Name || col == F_Comment; }

// Split an introspected length ("10,2" → 10 / 2; "80" → 80 / "").
void SplitLength(const wxString& raw, wxString& len, wxString& scale)
{
    const int comma = raw.Find(L',');
    if (comma == wxNOT_FOUND) { len = raw; scale.clear(); }
    else { len = raw.Left(comma); scale = raw.Mid(comma + 1); }
}

// Style a read-only wxStyledTextCtrl as a SQL code pane (⑥SQL 预览 / ⑦TABLE DDL):
// SQL lexer + theme syntax colours, no margins/wrap, read-only. The keyword *lists*
// are dialect-specific and applied later (ApplyCodeKeywords) once the profile exists.
void ConfigureCodePane(wxStyledTextCtrl* stc)
{
    stc->SetLexer(wxSTC_LEX_SQL);
    stc->StyleSetFont(wxSTC_STYLE_DEFAULT, Mono(10));
    stc->StyleSetForeground(wxSTC_STYLE_DEFAULT, theme::kSynDefault);
    stc->StyleClearAll();
    stc->StyleSetForeground(wxSTC_SQL_WORD,        theme::kSynKeyword);
    stc->StyleSetForeground(wxSTC_SQL_WORD2,       theme::kSynFunction);
    stc->StyleSetForeground(wxSTC_SQL_STRING,      theme::kSynString);
    stc->StyleSetForeground(wxSTC_SQL_CHARACTER,   theme::kSynString);
    stc->StyleSetForeground(wxSTC_SQL_COMMENT,     theme::kSynComment);
    stc->StyleSetForeground(wxSTC_SQL_COMMENTLINE, theme::kSynComment);
    stc->StyleSetForeground(wxSTC_SQL_COMMENTLINEDOC, theme::kSynComment);
    stc->StyleSetForeground(wxSTC_SQL_NUMBER,      theme::kSynNumber);
    stc->StyleSetForeground(wxSTC_SQL_OPERATOR,    theme::kSynOperator);
    stc->StyleSetForeground(wxSTC_SQL_IDENTIFIER,  theme::kSynDefault);
    stc->SetMarginWidth(0, 0);
    stc->SetMarginWidth(1, 0);
    stc->SetMarginWidth(2, 0);
    // Wrap long DDL lines instead of scrolling horizontally (reported layout issue);
    // a small indent on continuation lines keeps wrapped statements readable.
    stc->SetWrapMode(wxSTC_WRAP_WORD);
    stc->SetWrapStartIndent(4);
    stc->SetCaretLineVisible(false);
    stc->SetReadOnly(true);
}

} // namespace

// ===========================================================================
TableDesignView::TableDesignView(wxWindow* parent)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kWhite);
    colW_.assign(kColMin, kColMin + F_Count);   // live per-column widths (drag-resizable)
    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- header ----
    auto* header = new wxPanel(this);
    header->SetBackgroundColour(theme::kMenuBg);
    auto* hs = new wxBoxSizer(wxHORIZONTAL);
    // Context toolbar: a host panel whose flat icon+label buttons are rebuilt per
    // active tab (RebuildToolbar). Left-aligned; the table title sits on the right.
    toolbar_ = new wxPanel(header);
    toolbar_->SetBackgroundColour(theme::kMenuBg);
    toolbarSizer_ = new wxBoxSizer(wxHORIZONTAL);
    toolbar_->SetSizer(toolbarSizer_);
    hs->Add(toolbar_, 0, wxALIGN_CENTER_VERTICAL);

    hs->AddStretchSpacer();

    // Table title (icon + title + subtitle) — moved to the RIGHT.
    auto* icon = new wxStaticBitmap(header, wxID_ANY,
                                    icons::Stroke(icons::Glyph::Table, 18, theme::kPrimary, 1.9));
    hs->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    title_ = new wxStaticText(header, wxID_ANY, tr(L"表设计"));
    title_->SetFont(Mono(13, true));
    title_->SetForegroundColour(theme::kText);
    hs->Add(title_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    subtitle_ = new wxStaticText(header, wxID_ANY, tr(L"选择左侧表以查看结构"));
    subtitle_->SetFont(Ui(9));
    subtitle_->SetForegroundColour(theme::kTextFaint);
    hs->Add(subtitle_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 12);

    header->SetSizer(hs);
    header->SetMinSize(wxSize(-1, 38));          // tighter toolbar band
    root->Add(header, 0, wxEXPAND);

    // ---- tab row ----
    auto* tabBar = new wxPanel(this);
    tabBar->SetBackgroundColour(theme::kTabStripBg);
    auto* ts = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < kTabCount; ++i) {
        const int idx = i;
        auto* tab = new TabCell(tabBar, tr(kTabNames[i]), [this, idx] { SelectTab(idx); });
        tabs_[i] = tab;
        ts->Add(tab, 0, wxEXPAND);               // tabs fill the (compact) strip height
    }
    static_cast<TabCell*>(tabs_[0])->SetSelected(true);   // 字段 is the default page
    tabBar->SetSizer(ts);
    tabBar->SetMinSize(wxSize(-1, 28));           // compact tab strip
    root->Add(tabBar, 0, wxEXPAND);

    // ---- pages ----
    book_ = new wxSimplebook(this, wxID_ANY);

    // 字段 page = always-live field form (left) + dialect attribute panel (right).
    auto* fieldsPage = new wxPanel(book_);
    fieldsPage->SetBackgroundColour(theme::kWhite);
    auto* fps = new wxBoxSizer(wxHORIZONTAL);

    formScroll_ = new wxScrolledWindow(fieldsPage, wxID_ANY);
    // Grid lines via the "gap shows through" trick: each cell control FILLS its cell
    // (white bg) and the 1px sizer gaps between the (opaque) controls reveal what the
    // parent painted behind them as crisp table lines. The scroller itself is WHITE;
    // a paint handler tints only the rectangle the field rows occupy with the grid-line
    // colour, so the empty space below/right of the rows stays white. Vertical centring:
    // controls use their NATURAL height (no forced height) so filling doesn't stretch
    // them tall enough to top-align their text.
    formScroll_->SetBackgroundColour(theme::kWhite);
    formScroll_->SetScrollRate(12, 12);
    formSizer_ = new wxFlexGridSizer(F_Count, /*vgap*/1, /*hgap*/1);
    formScroll_->SetSizer(formSizer_);
    formScroll_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(formScroll_);
        formScroll_->DoPrepareDC(dc);              // scroll-offset aware
        const wxSize rows = formSizer_->GetMinSize();
        dc.SetBrush(wxBrush(theme::kBorderGrid));
        dc.SetPen(*wxTRANSPARENT_PEN);
        // +1: the cells cover [0, rows) exactly, so the extra tinted pixel peeks out
        // past the last column/row as the table's right + bottom outer border.
        dc.DrawRectangle(0, 0, rows.x + 1, rows.y + 1);
    });
    fps->Add(formScroll_, 1, wxEXPAND | wxALL, 6);

    // 1px separator keeps the (now white) attribute panel visually apart from the form.
    auto* attrSep = new wxPanel(fieldsPage, wxID_ANY);
    attrSep->SetBackgroundColour(theme::kBorder);
    attrSep->SetMinSize(wxSize(1, -1));
    fps->Add(attrSep, 0, wxEXPAND);

    attrScroll_ = new wxScrolledWindow(fieldsPage, wxID_ANY);
    attrScroll_->SetBackgroundColour(theme::kWhite);
    attrScroll_->SetScrollRate(0, 12);
    attrScroll_->SetMinSize(wxSize(268, -1));
    attrSizer_ = new wxBoxSizer(wxVERTICAL);
    attrScroll_->SetSizer(attrSizer_);
    fps->Add(attrScroll_, 0, wxEXPAND);

    fieldsPage->SetSizer(fps);
    BuildAttrPanel(-1);   // seed the panel with its "select a field" hint

    // 索引 tab: always-live grid (mirrors the field form). Header is row 0 of the
    // flexgrid; a paint handler tints only the occupied rect so the 1px gaps read as
    // grid lines (same trick as the field form).
    BuildIndexGrid();

    // 外键 tab: always-live grid (same machinery as the 索引 tab).
    BuildFkGrid();

    // ⑦TABLE DDL (full CREATE) + ⑥SQL 预览 (incremental ALTER): read-only syntax-
    // coloured SQL panes (wxStyledTextCtrl, SQL lexer; keywords fed per-dialect in Load).
    auto makeCodePane = [&]() {
        auto* t = new wxStyledTextCtrl(book_, wxID_ANY);
        ConfigureCodePane(t);
        return t;
    };
    sqlPreview_ = makeCodePane();
    ddl_        = makeCodePane();

    // 触发器 tab: master/detail — always-live 序号/名称/时机/事件 list over a multiline
    // body editor bound to the current row (TableDesignView_Trigger.cpp).
    BuildTriggerGrid();

    // 选项 tab: whole-table form driven by profile_->TableOptionSpecs()
    // (TableDesignView_Options.cpp). Built empty here; rebuilt+filled per-dialect on Load.
    BuildOptionsForm();

    // 注释 tab: a single multi-line textarea for the table COMMENT. Save routes
    // through ExecuteEdit (ALTER … COMMENT / COMMENT ON TABLE per dialect).
    commentPage_ = new wxPanel(book_);
    commentPage_->SetBackgroundColour(theme::kWhite);
    {
        auto* cs = new wxBoxSizer(wxVERTICAL);
        auto* cl = new wxStaticText(commentPage_, wxID_ANY, tr(L"表注释"));
        cl->SetFont(Ui(9));
        cl->SetForegroundColour(theme::kTextSecondary);
        cs->Add(cl, 0, wxLEFT | wxTOP, 10);
        // wxTE_RICH2: the rich-edit control shows the vertical scrollbar only when the
        // text overflows (the plain multiline EDIT shows it always, even when empty).
        commentEdit_ = new wxTextCtrl(commentPage_, wxID_ANY, L"",
                                      wxDefaultPosition, wxDefaultSize,
                                      wxTE_MULTILINE | wxTE_RICH2);
        commentEdit_->SetFont(Ui(10));
        cs->Add(commentEdit_, 1, wxEXPAND | wxALL, 10);
        commentPage_->SetSizer(cs);
    }

    // Page order MUST match the TabId enum / kTabNames.
    book_->AddPage(fieldsPage,    tr(kTabNames[Tab_Fields]));
    book_->AddPage(indexScroll_,  tr(kTabNames[Tab_Indexes]));
    book_->AddPage(fkScroll_,     tr(kTabNames[Tab_ForeignKeys]));
    book_->AddPage(triggersPage_, tr(kTabNames[Tab_Triggers]));
    book_->AddPage(optionsPage_,  tr(kTabNames[Tab_Options]));
    book_->AddPage(commentPage_,  tr(kTabNames[Tab_Comment]));
    book_->AddPage(sqlPreview_,   tr(kTabNames[Tab_SqlPreview]));
    book_->AddPage(ddl_,          tr(kTabNames[Tab_TableDdl]));
    root->Add(book_, 1, wxEXPAND);

    SetSizer(root);
    Bind(wxEVT_CHAR_HOOK, &TableDesignView::OnViewKey, this);   // Ctrl+S / Ctrl+C / Ctrl+V
    UpdateEditability();
}

// ---------------------------------------------------------------------------
void TableDesignView::SelectTab(int index)
{
    // Switching inner tabs must NOT prompt — the unsaved-changes prompt belongs to the
    // design tab's CLOSE flow only (HasUnsavedChanges/ConfirmClose cover all tabs).
    for (int i = 0; i < kTabCount; ++i)
        static_cast<TabCell*>(tabs_[i])->SetSelected(i == index);
    book_->SetSelection(index);
    RebuildToolbar(index);   // top menu follows the active tab (M2.2)
    if (index == Tab_SqlPreview || index == Tab_TableDdl)
        RefreshGeneratedTabs();   // ⑥⑦ regenerate from the current edit state (M4a)
}

void TableDesignView::UpdateEditability()
{
    // Editability is applied inside RebuildToolbar (per-button needEditable), so a
    // refresh just rebuilds the current tab's toolbar.
    RebuildToolbar(book_ ? book_->GetSelection() : Tab_Fields);
}

// ---- Context toolbar, 字段-tab operations, attribute panel, edit-model & save/DDL
// ---- generation are implemented in TableDesignView_Tabs.cpp (≤1000-line charter). ----

// ---- 索引 / 外键 / 触发器 / 选项 tab implementations live in their own TUs
// ---- (TableDesignView_Index.cpp / _Fk.cpp / _Trigger.cpp / _Options.cpp) per the
// ---- ≤1000-line charter. ----

void TableDesignView::ShowEmpty(const wxString& msg)
{
    title_->SetLabel(tr(L"表设计"));
    subtitle_->SetLabel(msg);
    formScroll_->Freeze();
    formSizer_->Clear(/*delete_windows=*/true);
    rows_.clear();
    formScroll_->FitInside();
    formScroll_->Thaw();
    if (indexSizer_) { indexSizer_->Clear(/*delete_windows=*/true); idxRows_.clear();
                       curIdxRow_ = -1; BuildIndexHeaderRow();
                       indexScroll_->FitInside(); }
    if (fkSizer_)    { fkSizer_->Clear(/*delete_windows=*/true); fkRows_.clear();
                       curFkRow_ = -1; BuildFkHeaderRow();
                       fkScroll_->FitInside(); }
    ResetTriggerGrid();
    BuildOptionsForm();
    if (ddl_) SetCodePane(ddl_, L"");
    if (sqlPreview_) SetCodePane(sqlPreview_, L"");
    origCols_.clear();
    attrRow_ = -1;
    BuildAttrPanel(-1);
    UpdateEditability();
    Layout();
}

// Which existing columns moved (for a dialect that can reorder). A column moved iff
// its predecessor in the NEW order differs from its predecessor in the LOADED order;
// its AFTER target is the CURRENT name of that new predecessor ("" = FIRST). Only
// existing (non-added, non-dropped) columns participate.
void TableDesignView::ComputeColumnReorder(std::map<wxString, wxString>& reorder) const
{
    reorder.clear();
    if (!profile_ || !profile_->SupportsColumnReorder()) return;

    std::vector<wxString> newOrder;              // existing columns, current order
    std::map<wxString, wxString> curName;        // origName → current name
    std::set<wxString> existing;
    for (const auto& up : rows_) {
        const FieldRow* fr = up.get();
        if (fr->origName.IsEmpty()) continue;    // freshly added → not a reorder
        newOrder.push_back(fr->origName);
        existing.insert(fr->origName);
        wxString n = fr->name ? fr->name->GetValue() : fr->origName;
        curName[fr->origName] = n.Trim().Trim(false);
    }
    std::vector<wxString> oldOrder;              // loaded order, dropped columns skipped
    for (const auto& c : origCols_)
        if (existing.count(c.name)) oldOrder.push_back(c.name);

    std::map<wxString, wxString> oldPred;
    for (size_t i = 0; i < oldOrder.size(); ++i)
        oldPred[oldOrder[i]] = (i == 0) ? wxString() : oldOrder[i - 1];

    for (size_t i = 0; i < newOrder.size(); ++i) {
        const wxString newPred = (i == 0) ? wxString() : newOrder[i - 1];
        if (newPred != oldPred[newOrder[i]])
            reorder[newOrder[i]] = newPred.IsEmpty() ? wxString() : curName[newPred];
    }
}

// Feed this dialect's reserved words (set 0 → wxSTC_SQL_WORD) and functions (set 1 →
// wxSTC_SQL_WORD2) to both code panes. Lower-cased; the SQL lexer matches case-
// insensitively, so upper-case DDL keywords still highlight.
void TableDesignView::ApplyCodeKeywords()
{
    if (!profile_) return;
    const db::Dialect d = profile_->GetDialect();
    auto joinLower = [](const std::vector<wxString>& v) {
        wxString s;
        for (const wxString& k : v) s += k.Lower() + L" ";
        return s;
    };
    const wxString words = joinLower(db::Keywords(d));
    const wxString funcs = joinLower(db::Functions(d));
    for (wxStyledTextCtrl* p : { sqlPreview_, ddl_ }) {
        if (!p) continue;
        p->SetKeyWords(0, words);
        p->SetKeyWords(1, funcs);
        p->Colourise(0, -1);
    }
}

// Set a read-only STC pane's text (toggle read-only around the write) and re-colour.
void TableDesignView::SetCodePane(wxStyledTextCtrl* pane, const wxString& text)
{
    if (!pane) return;
    pane->SetReadOnly(false);
    pane->SetText(text);
    pane->SetReadOnly(true);
    pane->Colourise(0, -1);
}

// ---- profile-driven helpers -----------------------------------------------
const db::TypeDescriptor* TableDesignView::FindType(const wxString& name) const
{
    if (!profile_) return nullptr;
    for (const auto& t : profile_->Types())
        if (t.name.IsSameAs(name, /*caseSensitive=*/false)) return &t;
    return nullptr;
}

int TableDesignView::RowIndexOf(const FieldRow* fr) const
{
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].get() == fr) return static_cast<int>(i);
    return -1;
}

// Length/scale cells accept input only when the (resolved) type takes them. We use
// SetEditable (not Enable) so a non-applicable cell keeps the normal white table
// look — just not typeable — instead of the disabled grey. While the user is
// mid-typing a not-yet-valid type name (FindType == null) we keep both editable and
// DON'T clear their text — clearing would wipe a length entered before the type name
// finished resolving.
void TableDesignView::ApplyRowTypeEditability(int row)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    FieldRow* fr = rows_[row].get();
    const db::TypeDescriptor* td = FindType(fr->type->GetValue());
    if (!td) {
        fr->length->SetEditable(true);
        fr->scale->SetEditable(true);
        return;
    }
    fr->length->SetEditable(td->takesLength);
    fr->scale->SetEditable(td->takesScale);
    if (!td->takesLength) fr->length->ChangeValue(wxString());
    if (!td->takesScale)  fr->scale->ChangeValue(wxString());
}

// ---- field form ------------------------------------------------------------
// All content (header + cells) is centred both axes per the design ask; owner-drawn
// cells (header/键) centre in their paint, text cells via wxTE_CENTRE + the single-line
// control's native vertical centring.
void TableDesignView::BuildHeaderRow()
{
    headerCells_.assign(F_Count, nullptr);
    for (int i = 0; i < F_Count; ++i) {
        auto* h = new HeaderCell(formScroll_, tr(kHeaderCols[i]), /*centred*/!ColLeft(i), i, colW_[i],
                                 [this](int col, int w) { ResizeColumn(col, w); });
        headerCells_[i] = h;
        formSizer_->Add(h, 0, wxEXPAND);   // fill so the 1px gaps read as grid lines
    }
}

// Header-drag resize: set column `col` to `newWidth` (clamped) by pushing that width
// onto every cell in the column (header + each row) as its min size, then relayout.
void TableDesignView::ResizeColumn(int col, int newWidth)
{
    if (col < 0 || col >= F_Count) return;
    const int w = std::max(38, newWidth);           // floor so a column can't vanish
    if (w == colW_[col]) return;
    colW_[col] = w;

    formScroll_->Freeze();
    if (col < static_cast<int>(headerCells_.size()) && headerCells_[col])
        headerCells_[col]->SetMinSize(wxSize(w, kHeaderH));
    for (auto& up : rows_) {
        wxWindow* cell = RowCells(up.get())[col];   // checkbox cols → the filler panel
        if (cell) cell->SetMinSize(wxSize(w, kRowH));
    }
    formSizer_->Layout();
    formScroll_->FitInside();
    formScroll_->Refresh();                          // repaint the grid-line backdrop
    formScroll_->Thaw();
}

// Create one row's always-live controls, append them to the form sizer (column
// order), wire focus + type-change handlers, and register the row. Returns it.
TableDesignView::FieldRow* TableDesignView::AddRowControls()
{
    auto owned = std::make_unique<FieldRow>();
    FieldRow* p = owned.get();
    wxWindow* host = formScroll_;

    auto focusCb = [this, p](wxFocusEvent& e) { OnRowFocus(p); e.Skip(); };

    // Cells FILL their cell (wxEXPAND) with a white background so the 1px sizer gaps
    // read as grid lines; a single-line wxTextCtrl centres its text vertically natively,
    // and wxTE_CENTRE centres it horizontally → all content is centred both axes.
    auto makeText = [&](int col, const wxColour& fg) {
        const long align = ColLeft(col) ? wxTE_LEFT : wxTE_CENTRE;
        auto* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxBORDER_NONE | align);
        tc->SetMinSize(wxSize(colW_[col], kRowH));
        tc->SetBackgroundColour(theme::kWhite);
        tc->SetForegroundColour(fg);
        tc->SetFont(Mono(9.5));
        tc->Bind(wxEVT_SET_FOCUS, focusCb);
        return tc;
    };
    // 序号: painted NumCell — left-click selects the row (Ctrl/Shift for multi),
    // right-click opens the field context menu. No caret (not focusable).
    auto* numCell = new NumCell(host,
        [this, p](wxMouseEvent& e) {
            const int r = RowIndexOf(p);
            if (r >= 0) SelectRow(r, e.ControlDown(), e.ShiftDown());
        },
        [this, p](wxMouseEvent&) {
            const int r = RowIndexOf(p);
            if (r < 0) return;
            if (selRows_.find(r) == selRows_.end()) SelectRow(r, false, false);
            ShowFieldMenu(r);
        });
    numCell->SetMinSize(wxSize(colW_[F_Num], kRowH));
    numCell->SetNumber(static_cast<int>(rows_.size()) + 1);
    p->num = numCell;
    formSizer_->Add(p->num, 0, wxEXPAND);

    p->name = makeText(F_Name, theme::kTextBody);
    // Leaving the name field auto-fills an empty type from the name (id/…→int, date→datetime).
    p->name->Bind(wxEVT_KILL_FOCUS, [this, p](wxFocusEvent& e) { InferTypeFromName(p); e.Skip(); });
    formSizer_->Add(p->name, 0, wxEXPAND);

    // 类型: a flat, borderless TypeCell (looks like a plain white table cell) that owns
    // a custom filter dropdown — type to narrow, ↓/↑ to move, Enter/click to echo the
    // pick back into the cell. onChanged → the same length/scale + panel refresh path.
    auto* typeCell = new TypeCell(host, [this, p] { OnRowTypeChanged(p); });
    typeCell->SetChoices(typeChoices_);
    typeCell->SetMinSize(wxSize(colW_[F_Type], kRowH));
    typeCell->SetBackgroundColour(theme::kWhite);
    typeCell->SetForegroundColour(theme::kSynKeyword);
    typeCell->Bind(wxEVT_SET_FOCUS, focusCb);
    p->type = typeCell;
    formSizer_->Add(p->type, 0, wxEXPAND);

    p->length = makeText(F_Length, theme::kTextBody);
    formSizer_->Add(p->length, 0, wxEXPAND);

    p->scale = makeText(F_Scale, theme::kTextBody);
    formSizer_->Add(p->scale, 0, wxEXPAND);

    // 键: a painted, non-focusable BadgeCell → no text caret; a click selects the row
    // and toggles this column's membership in the (composite) primary key.
    auto* keyCell = new BadgeCell(host);
    keyCell->SetMinSize(wxSize(colW_[F_Key], kRowH));   // honour the (resizable) width
    keyCell->Bind(wxEVT_LEFT_DOWN, [this, p](wxMouseEvent& e) {
        SetCurrentRow(RowIndexOf(p));   // click also binds the row (no focus event fires)
        OnKeyClick(p);
        e.Skip();
    });
    p->key = keyCell;
    formSizer_->Add(p->key, 0, wxEXPAND);

    // Checkboxes (非空/虚拟): the small box is centred both axes inside a white filler
    // panel that fills the cell — the panel gives the opaque fill for the grid-line
    // gaps while the box sits centred (a bare wxEXPAND checkbox would left-align).
    auto makeCheck = [&](int col) {
        auto* cell = new wxPanel(host);
        cell->SetBackgroundColour(theme::kWhite);
        cell->SetMinSize(wxSize(colW_[col], kRowH));
        auto* cb = new wxCheckBox(cell, wxID_ANY, wxEmptyString);
        cb->SetBackgroundColour(theme::kWhite);
        cb->Bind(wxEVT_SET_FOCUS, focusCb);
        auto* cs = new wxBoxSizer(wxHORIZONTAL);
        cs->AddStretchSpacer();
        cs->Add(cb, 0, wxALIGN_CENTER_VERTICAL);
        cs->AddStretchSpacer();
        cell->SetSizer(cs);
        formSizer_->Add(cell, 0, wxEXPAND);
        return cb;
    };
    p->notNull = makeCheck(F_NotNull);
    p->virt    = makeCheck(F_Virtual);

    p->comment = makeText(F_Comment, theme::kTextMuted);
    formSizer_->Add(p->comment, 0, wxEXPAND);

    rows_.push_back(std::move(owned));
    return p;
}

// Click a key cell to toggle this row's membership in the (composite) primary key.
// Click order = PK column order (PK1, PK2, …); clicking a PK cell again removes it and
// the rest renumber. FK/UQ badges (introspected, read-only) don't participate.
void TableDesignView::OnKeyClick(FieldRow* fr)
{
    if (!fr) return;
    if (fr->baseKey == L"FK" || fr->baseKey == L"UQ") return;   // read-only badge
    auto it = std::find(pkOrder_.begin(), pkOrder_.end(), fr);
    if (it != pkOrder_.end()) pkOrder_.erase(it);               // was PK → cancel
    else                      pkOrder_.push_back(fr);           // → append as next PK
    UpdateKeyBadges();
}

// Repaint every key cell: "PKn" (amber) for primary-key columns in click order, else
// the read-only FK/UQ badge (or blank).
void TableDesignView::UpdateKeyBadges()
{
    for (auto& up : rows_) {
        FieldRow* fr = up.get();
        int pkIdx = -1;
        for (size_t i = 0; i < pkOrder_.size(); ++i)
            if (pkOrder_[i] == fr) { pkIdx = static_cast<int>(i); break; }
        auto* badge = static_cast<BadgeCell*>(fr->key);
        if (pkIdx >= 0)
            badge->SetBadge(wxString::Format(L"PK%d", pkIdx + 1), theme::kDotAmber);
        else if (fr->baseKey == L"FK") badge->SetBadge(L"FK", theme::kPrimary);
        else if (fr->baseKey == L"UQ") badge->SetBadge(L"UQ", theme::kSynTable);
        else                           badge->SetBadge(fr->baseKey, theme::kTextMuted);
    }
}

void TableDesignView::FillRowFromColumn(FieldRow* fr, const db::ColumnInfo& c)
{
    wxString len, splitScale; SplitLength(c.length, len, splitScale);
    const wxString scale = !c.scale.IsEmpty() ? c.scale : splitScale;
    fr->name->ChangeValue(c.name);
    fr->type->ChangeValue(c.type);
    fr->length->ChangeValue(len);
    fr->scale->ChangeValue(scale);
    fr->notNull->SetValue(c.notNull);
    // 虚拟: checked = VIRTUAL (has expression + not stored).
    fr->virt->SetValue(!c.generatedExpr.IsEmpty() && !c.generatedStored);
    fr->comment->ChangeValue(c.comment);

    // Key: PK columns feed the composite-PK order; FK/UQ stay as read-only badges.
    // (UpdateKeyBadges paints all key cells after the whole form is built.)
    if (c.key == L"PK") { fr->baseKey.clear(); pkOrder_.push_back(fr); }
    else                  fr->baseKey = c.key;   // "FK" / "UQ" / ""

    fr->origName = c.name;

    // Panel-side state: default value + the introspected dialect extras (only
    // non-empty values seeded; empty ⇒ control shows blank).
    fr->defaultVal = c.defaultVal;
    fr->attrVals.clear();
    auto seed = [&](const wxString& id, const wxString& v) {
        if (!v.IsEmpty()) fr->attrVals.emplace_back(id, v);
    };
    seed(L"charset", c.charset);
    seed(L"collation", c.collation);
    if (c.unsignedFlag)  fr->attrVals.emplace_back(L"unsigned", L"1");
    seed(L"keyLength", c.keyLength);
    if (c.autoIncrement) fr->attrVals.emplace_back(L"autoIncrement", L"1");
    seed(L"generated", c.generatedExpr);
}

void TableDesignView::RenumberRows()
{
    for (size_t i = 0; i < rows_.size(); ++i)
        static_cast<NumCell*>(rows_[i]->num)->SetNumber(static_cast<int>(i) + 1);
}

// Apply the current-row + selection look to one row: NumCell state (bold-blue number
// when current) + a whole-row tint when selected.
void TableDesignView::RepaintRow(int row)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    FieldRow* fr = rows_[row].get();
    const bool current  = (row == attrRow_);
    const bool selected = selRows_.count(row) > 0;
    static_cast<NumCell*>(fr->num)->SetState(current, selected);

    const wxColour bg = selected ? theme::kRowHeaderActiveBg : theme::kWhite;
    for (wxWindow* w : RowCells(fr)) {
        if (!w || w == fr->num) continue;             // NumCell painted above
        if (auto* badge = dynamic_cast<BadgeCell*>(w)) { badge->SetCellBg(bg); continue; }
        w->SetBackgroundColour(bg);
        for (wxWindow* ch : w->GetChildren())         // e.g. the checkbox inside its panel
            ch->SetBackgroundColour(bg);
        w->Refresh();
    }
}

void TableDesignView::RepaintSelection()
{
    if (formScroll_) formScroll_->Freeze();
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) RepaintRow(i);
    if (formScroll_) formScroll_->Thaw();
}

// ---- 序号 selection + clipboard --------------------------------------------
// 序号 left-click: plain = single-select; Ctrl = toggle; Shift = range from anchor.
void TableDesignView::SelectRow(int row, bool ctrl, bool shift)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (shift && selAnchor_ >= 0) {
        selRows_.clear();
        const int a = std::min(selAnchor_, row), b = std::max(selAnchor_, row);
        for (int i = a; i <= b; ++i) selRows_.insert(i);
    } else if (ctrl) {
        if (selRows_.count(row)) selRows_.erase(row); else selRows_.insert(row);
        selAnchor_ = row;
    } else {
        selRows_.clear(); selRows_.insert(row); selAnchor_ = row;
    }
    SetCurrentRow(row);
    RepaintSelection();
}

// ---- 字段-tab context-menu / clipboard / type-inference operations live in
// ---- TableDesignView_Tabs.cpp. ----

// Make `row` the current row: flush the previously-bound panel, (re)bind the
// attribute panel to this row (reuse in place when the structure matches), and move
// the current-row number highlight. Optionally focus the name field.
void TableDesignView::SetCurrentRow(int row, bool focusName)
{
    if (row == attrRow_) {
        if (focusName && row >= 0 && row < static_cast<int>(rows_.size()))
            rows_[row]->name->SetFocus();
        return;
    }
    FlushAttrPanel();
    const int prev = attrRow_;
    const bool valid = row >= 0 && row < static_cast<int>(rows_.size())
                       && profile_ && IsEditable();
    const bool reusable = valid && prev >= 0 && AttrSignature(row) == panelSig_;
    if (!valid)          BuildAttrPanel(-1);
    else if (reusable)   ReloadAttrPanel(row);   // fast: same controls, just new values
    else                 BuildAttrPanel(row);    // structure differs → full rebuild
    // attrRow_ is now (valid ? row : -1)
    if (prev >= 0 && prev < static_cast<int>(rows_.size())) RepaintRow(prev);
    if (attrRow_ >= 0 && attrRow_ < static_cast<int>(rows_.size())) RepaintRow(attrRow_);
    if (focusName && attrRow_ >= 0)
        rows_[attrRow_]->name->SetFocus();
}

void TableDesignView::OnRowFocus(FieldRow* fr)
{
    if (populating_) return;
    const int r = RowIndexOf(fr);
    if (r >= 0) SetCurrentRow(r, /*focusName=*/false);
}

void TableDesignView::OnRowTypeChanged(FieldRow* fr)
{
    if (populating_) return;
    const int r = RowIndexOf(fr);
    if (r < 0) return;
    // Typing in the type cell binds this row as current (no-op if focus already did).
    if (r != attrRow_) SetCurrentRow(r);
    ApplyRowTypeEditability(r);
    if (r == attrRow_) {
        if (fieldLbl_)
            fieldLbl_->SetLabel(fr->name->GetValue() + L"  ·  " + fr->type->GetValue());
        // The applicable-attribute set can change with the type's ColKind; rebuild
        // only when it actually changes (cheap signature compare), not per keystroke.
        if (AttrSignature(r) != panelSig_) {
            FlushAttrPanel();
            BuildAttrPanel(r);
        }
    }
}


// ---------------------------------------------------------------------------
void TableDesignView::PopulateFields()
{
    populating_ = true;
    formScroll_->Freeze();
    formSizer_->Clear(/*delete_windows=*/true);
    rows_.clear();
    pkOrder_.clear();
    selRows_.clear(); selAnchor_ = -1;   // don't carry a stale selection across tables

    typeChoices_.Clear();
    if (profile_) for (const auto& t : profile_->Types()) typeChoices_.Add(t.name);

    BuildHeaderRow();
    for (size_t i = 0; i < origCols_.size(); ++i) {
        FieldRow* fr = AddRowControls();
        FillRowFromColumn(fr, origCols_[i]);   // also feeds pkOrder_ for PK columns
        ApplyRowTypeEditability(static_cast<int>(i));
        // Capture the load-time baseline via the exact save-path model builder, so an
        // untouched row diffs equal (no spurious Modify on every open).
        fr->baseline = RowToModel(static_cast<int>(i));
        fr->hasBaseline = true;
    }
    UpdateKeyBadges();   // paint PKn / FK / UQ once the whole form + pkOrder_ exist

    formSizer_->Layout();
    formScroll_->FitInside();
    formScroll_->Thaw();
    attrRow_ = -1;
    BuildAttrPanel(-1);
    populating_ = false;
}

// ---------------------------------------------------------------------------
void TableDesignView::Load(std::shared_ptr<db::IConnection> conn, const wxString& database,
                           const wxString& table, db::DbType type)
{
    conn_ = conn; db_ = database; table_ = table; dbType_ = type;
    profile_ = &db::GetDialectProfile(type);

    // 触发器 tab is offered only where the design view renders trigger DDL (MySQL/
    // SQLite); PG/SQL Server/Oracle hide it (profile SupportsTriggers()=false).
    const bool showTrig = profile_->SupportsTriggers();
    if (tabs_[Tab_Triggers]->IsShown() != showTrig) {
        tabs_[Tab_Triggers]->Show(showTrig);
        tabs_[Tab_Triggers]->GetParent()->Layout();
        if (!showTrig && book_->GetSelection() == Tab_Triggers)
            SelectTab(Tab_Fields);
    }

    if (!conn || !conn->IsConnected() || table.IsEmpty()) {
        ShowEmpty(table.IsEmpty() ? tr(L"双击左侧表名以查看其结构") : tr(L"未连接"));
        return;
    }

    wxString err;
    origCols_.clear();
    if (!conn->GetColumns(database, table, origCols_, err)) {
        ShowEmpty(tr(L"读取结构失败:") + err);
        return;
    }

    // Candidate charset/collation values for the panel combos — reuse the exact data
    // source the New-Database dialog uses. Best-effort: empty caps just means the
    // combos degrade to free-text entry (e.g. SQLite has none).
    createCaps_ = db::DbCreateCaps{};
    wxString capsErr;
    conn->GetCreateDatabaseCaps(createCaps_, capsErr);

    title_->SetLabel(table);
    subtitle_->SetLabel(wxString::Format(L"public schema · %zu columns", origCols_.size()));

    PopulateFields();

    // 注释 / 选项 baselines from the live schema (best-effort) — used both to seed the
    // fields and as the dirty-tracking baseline (prompt on tab switch).
    {
        db::TableSchema ts; wxString e2;
        const bool ok = conn->GetTableSchema(database, table, ts, e2);
        origComment_ = ok ? ts.comment : wxString();
        origOptions_ = db::TableOptions{};
        if (ok) { origOptions_.engine = ts.engine; origOptions_.charset = ts.charset;
                  origOptions_.collation = ts.collation; }
        if (commentEdit_) commentEdit_->ChangeValue(origComment_);
    }

    // ---- 索引 ----
    std::vector<db::IndexInfo> idx;
    conn->GetIndexes(database, table, idx, err);
    PopulateIndexes(idx);

    // ---- 外键 ----
    std::vector<db::ForeignKey> fk;
    conn->GetForeignKeys(database, table, fk, err);
    PopulateForeignKeys(fk);

    // ---- 触发器 (add-only, not introspected) / 选项 (dialect form) ----
    ResetTriggerGrid();
    BuildOptionsForm();   // rebuild + fill current values per the resolved dialect

    // ---- DDL ----
    ApplyCodeKeywords();   // feed this dialect's keyword/function lists to the STC panes
    wxString ddl;
    if (conn->GetCreateDdl(database, table, ddl, err))
        SetCodePane(ddl_, ddl);
    else
        SetCodePane(ddl_, tr(L"-- 获取 DDL 失败:") + err);

    UpdateEditability();
    SelectTab(0);
    Layout();
}

// ---------------------------------------------------------------------------
std::vector<wxWindow*> TableDesignView::RowCells(FieldRow* fr) const
{
    // Column order F_Num..F_Comment. The 非空/虚拟 sizer items are the checkboxes'
    // filler panels (their parent), not the raw checkboxes.
    return { fr->num, fr->name, fr->type, fr->length, fr->scale, fr->key,
             fr->notNull->GetParent(), fr->virt->GetParent(), fr->comment };
}

// Swap rows_[a] and rows_[a+1] both in the sizer layout and the vector, keeping each
// FieldRow object (its controls, PK membership, diff baseline) intact — only the
// visual/logical order changes. Header occupies grid-row 0, so data row r's cells sit
// at sizer indices [F_Count*(r+1) .. +F_Count).
void TableDesignView::SwapAdjacentRows(int a)
{
    if (a < 0 || a + 1 >= static_cast<int>(rows_.size())) return;
    const std::vector<wxWindow*> lower = RowCells(rows_[a + 1].get());
    const int base = F_Count * (a + 1);         // where row a currently starts
    for (wxWindow* w : lower) formSizer_->Detach(w);   // pull the lower row out…
    int idx = base;
    for (wxWindow* w : lower)                    // …and re-insert it above row a
        formSizer_->Insert(idx++, w, 0, wxEXPAND);
    std::swap(rows_[a], rows_[a + 1]);
}

// Shared tail for 增加/插入: seed a fresh row's defaults, place it, focus its name.
void TableDesignView::OnAddField()   { InsertFieldAt(-1); }                      // append
void TableDesignView::OnInsertField(){ InsertFieldAt(attrRow_ >= 0 ? attrRow_ : -1); }

// Seed a fresh field row (varchar/255) and place it: at<0 (or no current row) appends
// at the end; otherwise it is bubbled up to sit ABOVE index `at`. Focuses its name.
void TableDesignView::InsertFieldAt(int at)
{
    if (!IsEditable()) return;   // base: conn+table (unchanged); NewTableView: conn only
    if (attrRow_ >= 0) FlushAttrPanel();   // fold pending panel edits before we reindex
    attrRow_ = -1;

    selRows_.clear(); selAnchor_ = -1;     // structural change → drop the selection
    formScroll_->Freeze();
    FieldRow* fr = AddRowControls();       // always appended at the end first
    fr->origName.clear();                  // blank row — user fills it (no defaults)
    fr->hasBaseline = false;

    const int last = static_cast<int>(rows_.size()) - 1;
    int finalIdx = last;
    if (at >= 0 && at < last) {             // insert above `at`: bubble the new row up
        for (int i = last; i > at; --i) SwapAdjacentRows(i - 1);
        finalIdx = at;
    }
    RenumberRows();
    ApplyRowTypeEditability(finalIdx);
    formSizer_->Layout();
    formScroll_->FitInside();
    if (finalIdx == last) formScroll_->Scroll(-1, 100000);   // appended → reveal bottom
    SetCurrentRow(finalIdx, /*focusName=*/true);
    rows_[finalIdx]->name->SetInsertionPointEnd();
    RepaintSelection();   // clear any prior selection tint (selection was reset above)
    formScroll_->Thaw();
}

void TableDesignView::OnDeleteField()
{
    const int r = attrRow_;
    if (r < 0 || r >= static_cast<int>(rows_.size())) return;

    selRows_.clear(); selAnchor_ = -1;   // structural change → drop the selection
    formScroll_->Freeze();
    FieldRow* fr = rows_[r].get();
    if (auto it = std::find(pkOrder_.begin(), pkOrder_.end(), fr); it != pkOrder_.end())
        pkOrder_.erase(it);              // drop from the composite PK, then renumber below
    for (wxWindow* w : RowCells(fr)) {   // detach + destroy the row's sizer cells
        if (!w) continue;
        formSizer_->Detach(w);
        w->Hide();
        w->Destroy();                    // destroying a checkbox filler panel takes its box
    }
    rows_.erase(rows_.begin() + r);
    RenumberRows();
    UpdateKeyBadges();                   // PKn renumber after a PK column is removed
    attrRow_ = -1;                       // dropped the bound row
    formSizer_->Layout();
    formScroll_->FitInside();

    if (rows_.empty())
        BuildAttrPanel(-1);
    else
        SetCurrentRow(std::min(r, static_cast<int>(rows_.size()) - 1), /*focusName=*/true);
    RepaintSelection();                  // clear any prior selection tint
    formScroll_->Thaw();                 // one repaint for the whole delete
}

// Toggle the current field's membership in the composite primary key.
void TableDesignView::OnToggleKeyOfCurrent()
{
    if (attrRow_ < 0 || attrRow_ >= static_cast<int>(rows_.size())) return;
    OnKeyClick(rows_[attrRow_].get());   // append/remove in pkOrder_ + repaint badges
}

// 上移/下移 the current field by one position (delta = -1 / +1).
void TableDesignView::OnMoveField(int delta)
{
    if (!IsEditable()) return;   // base: conn+table (unchanged); NewTableView: conn only
    const int cur = attrRow_;
    if (cur < 0) return;
    const int tgt = cur + delta;
    if (tgt < 0 || tgt >= static_cast<int>(rows_.size())) return;

    FlushAttrPanel();      // fold pending panel edits into the (pre-move) current row…
    attrRow_ = -1;         // …then detach the binding so the swap can't misroute a flush
    selRows_.clear(); selAnchor_ = -1;
    formScroll_->Freeze();
    SwapAdjacentRows(std::min(cur, tgt));
    RenumberRows();
    formSizer_->Layout();
    formScroll_->FitInside();
    SetCurrentRow(tgt, /*focusName=*/false);   // follow the moved row to its new index
    RepaintSelection();                        // clear any prior selection tint
    formScroll_->Thaw();
}

// ---- OnCloseTab, the attribute panel, the edit-model, save/DDL generation, 注释 save
// ---- and dirty-tracking are implemented in TableDesignView_Tabs.cpp. ----

} // namespace ui
