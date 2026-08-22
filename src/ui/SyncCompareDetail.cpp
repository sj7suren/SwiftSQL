// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCompareDetail.cpp — see header.
#include "ui/SyncCompareDetail.h"

#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <vector>

#include "ui/I18n.h"
#include "ui/SyncDiffModel.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

bool IsStructureLeaf(const DiffNode& n)
{
    return n.kind == DiffNodeKind::Column || n.kind == DiffNodeKind::Index ||
           n.kind == DiffNodeKind::Constraint;
}

wxString KindWord(DiffNodeKind k)
{
    switch (k) {
    case DiffNodeKind::Column:     return tr(L"列");
    case DiffNodeKind::Index:      return tr(L"索引");
    case DiffNodeKind::Constraint: return tr(L"外键");
    default:                       return wxString();
    }
}

} // namespace

// ===========================================================================
// Shared chrome
// ===========================================================================

SyncSideBySideView::SyncSideBySideView(wxWindow* parent, const wxString& leftTitle,
                                       const wxString& rightTitle)
    : wxPanel(parent, wxID_ANY)
{
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Above the grid, so a qualification is read BEFORE the thing it qualifies.
    note_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    note_->SetFont(Ui(9));
    note_->SetForegroundColour(theme::kTextSecondary);
    note_->Hide();
    s->Add(note_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    grid_ = new wxGrid(this, wxID_ANY);
    grid_->CreateGrid(0, 3);
    grid_->EnableEditing(false);
    grid_->DisableDragRowSize();
    grid_->SetRowLabelSize(0);
    grid_->SetColLabelSize(FromDIP(24));
    grid_->SetDefaultCellFont(Ui(9));
    grid_->SetLabelFont(Ui(9, /*bold*/ true));
    grid_->SetGridLineColour(theme::kBorderGrid);
    grid_->SetDefaultCellBackgroundColour(theme::kEditorBg);
    grid_->SetDefaultCellTextColour(theme::kTextBody);
    grid_->SetColLabelValue(0, tr(L"对象"));
    grid_->SetColLabelValue(1, leftTitle);
    grid_->SetColLabelValue(2, rightTitle);
    grid_->SetColSize(0, FromDIP(180));
    grid_->SetColSize(1, FromDIP(280));
    grid_->SetColSize(2, FromDIP(280));
    s->Add(grid_, 1, wxEXPAND);

    empty_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                              wxALIGN_CENTRE_HORIZONTAL);
    empty_->SetFont(Ui(9));
    empty_->SetForegroundColour(theme::kTextMuted);
    s->Add(empty_, 1, wxEXPAND | wxALL, 24);

    SetSizer(s);
    ShowEmpty(tr(L"在上方选择一个表以查看对比。"));
}

void SyncSideBySideView::BeginRows(int rows)
{
    if (grid_->GetNumberRows() > 0) grid_->DeleteRows(0, grid_->GetNumberRows());
    if (rows > 0) grid_->AppendRows(rows);
}

void SyncSideBySideView::SetRow(int row, const wxString& label, const wxString& left,
                                const wxString& right, bool differs)
{
    grid_->SetCellValue(row, 0, label);
    grid_->SetCellValue(row, 1, left);
    grid_->SetCellValue(row, 2, right);

    if (!differs) return;

    // Only the two VALUE cells are tinted, never the label cell: the point of
    // the highlight is "these two disagree", and colouring the name too would
    // dilute it.
    //
    // Both sides get the SAME "modified" token rather than green-left /
    // red-right. Both views are now oriented 目标|源 (current state first, the
    // way a user reads "what it is now vs what it will become"), so the palette
    // would still not be safe to make directional: a green "source" column would
    // read as "good" on a DROP, where the source having nothing is the point.
    // "Differs" is the one claim that is true in every case. A side holding a
    // placeholder — absent, or a value that never reached the UI — is greyed
    // with the unsupported token so it reads as "there is nothing here", not as
    // a value that happens to look like that text.
    const bool leftAbsent  = IsPlaceholderSide(left);
    const bool rightAbsent = IsPlaceholderSide(right);
    grid_->SetCellBackgroundColour(row, 1, leftAbsent ? theme::kDiffUnsupBg : theme::kDiffModBg);
    grid_->SetCellTextColour(row, 1, leftAbsent ? theme::kTextMuted : theme::kDiffModFg);
    grid_->SetCellBackgroundColour(row, 2, rightAbsent ? theme::kDiffUnsupBg : theme::kDiffModBg);
    grid_->SetCellTextColour(row, 2, rightAbsent ? theme::kTextMuted : theme::kDiffModFg);
}

void SyncSideBySideView::SetNote(const wxString& text)
{
    if (!note_) return;
    note_->SetLabel(text);
    note_->Wrap(FromDIP(760));
    note_->Show(!text.IsEmpty());
    Layout();
}

void SyncSideBySideView::MakeRowTall(int row, int heightDip)
{
    grid_->SetRowSize(row, FromDIP(heightDip));
    for (int col = 0; col < 3; ++col) {
        grid_->SetCellRenderer(row, col, new wxGridCellAutoWrapStringRenderer);
        grid_->SetCellAlignment(row, col, wxALIGN_LEFT, wxALIGN_TOP);
    }
}

void SyncSideBySideView::ShowEmpty(const wxString& message)
{
    empty_->SetLabel(message);
    empty_->Wrap(FromDIP(520));
    // The note qualifies the GRID; with no grid on screen it would be a
    // statement about nothing.
    if (note_) note_->Hide();
    grid_->Hide();
    empty_->Show();
    Layout();
}

void SyncSideBySideView::ShowGrid()
{
    empty_->Hide();
    grid_->Show();
    Layout();
}

// ===========================================================================
// 结构对比
// ===========================================================================

// Columns are 目标 | 源, matching 数据对比. They used to be 源 | 目标 here and
// 目标 | 源 there, which meant the left column changed meaning when the user
// switched tabs — a reversal nothing on screen announced. One convention now:
// LEFT is always what the target has NOW, RIGHT is always what the source
// would make it.
SyncStructureCompareView::SyncStructureCompareView(wxWindow* parent)
    : SyncSideBySideView(parent, tr(L"目标当前定义"), tr(L"源定义"))
{
}

void SyncStructureCompareView::ShowTable(const DiffNode* table)
{
    if (!table) { ShowEmpty(tr(L"在上方选择一个表以查看结构对比。")); return; }

    std::vector<const DiffNode*> leaves;
    for (const auto& c : table->children)
        if (IsStructureLeaf(*c)) leaves.push_back(c.get());

    if (leaves.empty()) {
        ShowEmpty(wxString::Format(tr(L"表 %s 没有结构差异。"), table->label));
        return;
    }

    BeginRows(static_cast<int>(leaves.size()));
    for (size_t i = 0; i < leaves.size(); ++i) {
        const DiffNode& n = *leaves[i];
        const wxString label = KindWord(n.kind) + L"  " + n.label;

        // A TRUE side-by-side. This used to render the source definition
        // against the placeholder 「（目标当前定义未随比对计划下发）」, because
        // db::ColumnChange carried only the DESIRED column and the target's
        // current definition never reached the UI. ColumnChange now carries
        // hasCurrent/current, so SyncDiffModel fills both sides of the leaf and
        // this view just displays them — including, for the rarer kinds below,
        // the honest placeholder when a side really has nothing.
        //
        // Index and FK leaves are still one-sided: db::IndexChange / db::FkChange
        // are Add-or-Drop only (there is no Modify — a changed index is emitted
        // as a Drop plus an Add), so "the other side" is genuinely absent rather
        // than missing, and the fallback below states exactly that.
        wxString before = n.before, after = n.after;
        if (before.IsEmpty() && after.IsEmpty()) {
            before = (n.op == DiffOp::Add)  ? AbsentSide() : n.summary;
            after  = (n.op == DiffOp::Drop) ? AbsentSide() : n.summary;
        }
        SetRow(static_cast<int>(i), label, before, after, /*differs*/ true);
    }
    ShowGrid();
}

// ===========================================================================
// 数据对比
// ===========================================================================

SyncDataCompareView::SyncDataCompareView(wxWindow* parent)
    : SyncSideBySideView(parent, tr(L"目标当前值"), tr(L"源值"))
{
}

void SyncDataCompareView::ShowNode(const DiffNode* table, const DiffNode* focus)
{
    if (!table) { ShowEmpty(tr(L"在上方选择一个表以查看数据对比。")); return; }

    // Collect the Row nodes in scope: the focused one if the user drilled into
    // a specific row, otherwise every data row of the table.
    std::vector<const DiffNode*> rows;
    if (focus && focus->kind == DiffNodeKind::Row) {
        rows.push_back(focus);
    } else {
        for (const auto& c : table->children)
            if (c->kind == DiffNodeKind::Row) rows.push_back(c.get());
    }

    if (rows.empty()) {
        ShowEmpty(wxString::Format(tr(L"表 %s 没有数据差异。"), table->label));
        return;
    }

    // Rendering budget. A table can carry up to 500 sampled rows and a wide
    // table 50+ columns, so the whole-table view is bounded rather than allowed
    // to build a 25,000-row wxGrid the user will never scroll — and the trim is
    // ANNOUNCED, because a silently short list reads as "that's all there is".
    // Drilling into a single row (`focus`) is never trimmed: that view is one
    // row by definition.
    constexpr int kMaxCells = 600;

    // Count the value leaves actually available. A Row node with no ValueDiff
    // children is a COUNTER (「新增 12 行」), not a materialized row.
    int cells = 0;
    for (const DiffNode* r : rows) cells += static_cast<int>(r->children.size());
    const int shown = cells > kMaxCells ? kMaxCells : cells;

    if (cells == 0) {
        // Reached when the table's Row children are all COUNTER rows with no
        // materialized sample behind them — a table whose data diff was refused
        // (no primary key, unorderable key, blocked value), or a plan built with
        // DiffTreeOptions::includeSampleRows off. Not the normal path any more:
        // SyncEngine now fills TableUnit::rows with a capped sample of real
        // db::sync::RowChange objects and SyncDiffModel turns each into a Row +
        // ValueDiff leaves, which is what fills the grid below.
        wxString msg;
        for (const DiffNode* r : rows) msg += r->label + L"    ";
        ShowEmpty(msg + L"\n\n" +
                  tr(L"此表没有可逐行展示的差异样本（未做数据比对，或该表已被排除）。"));
        return;
    }

    // One extra row for the "trimmed" notice, so it lives in the grid the user
    // is scrolling rather than in a caption they have already scrolled past.
    const bool trimmed = shown < cells;
    BeginRows(shown + (trimmed ? 1 : 0));

    int row = 0;
    for (const DiffNode* r : rows) {
        for (const auto& leaf : r->children) {
            if (leaf->kind != DiffNodeKind::ValueDiff) continue;
            if (row >= shown) break;
            // Label carries the row identity so several rows can share the grid
            // without the reader losing track of which row a cell belongs to.
            SetRow(row++, r->label + L" · " + leaf->label,
                   leaf->before, leaf->after, leaf->differs);
        }
        if (row >= shown) break;
    }
    if (trimmed) {
        SetRow(row, tr(L"…"),
               wxString::Format(tr(L"仅显示前 %d 项，共 %d 项"), shown, cells),
               tr(L"展开单行可查看该行全部字段"), /*differs*/ false);
    }
    ShowGrid();
}

// ===========================================================================
// 函数 / 存储过程
// ===========================================================================

namespace {

// The plain-language sentence above the two bodies. Written to be read by
// someone who does not know what "PL/pgSQL" is: it says what we did, what we
// declined to say, and — in every branch, without exception — that nothing on
// this pane will be executed.
//
// Driven by the data layer's verdict, never by matching the label string.
wxString RoutineNote(const DiffNode& n)
{
    const wxString tail =
        tr(L"函数与存储过程仅作对比展示，不会随本次同步执行；如需变更，请人工改写。");

    switch (n.verdict) {
    case db::sync::RoutineVerdict::NotComparable:
        return tr(L"两侧使用的过程语言不同（例如 PL/pgSQL 与 MySQL 存储过程语法），"
                  L"它们是两种各自独立的编程语言。因此这里不判断两段代码是否等价，"
                  L"也不做高亮，只把两侧定义原样并排列出，由你自行比较。") +
               L" " + tail;
    case db::sync::RoutineVerdict::BodyDiffers:
        return tr(L"两侧为同一种数据库，定义文本存在差异（已高亮）。") + L" " + tail;
    case db::sync::RoutineVerdict::SignatureDiffers:
        return tr(L"两侧同名，但声明（参数或返回类型）不同。") + L" " + tail;
    case db::sync::RoutineVerdict::SourceOnly:
        return tr(L"该例程只存在于源端，目标端没有同名对象。") + L" " + tail;
    case db::sync::RoutineVerdict::TargetOnly:
        return tr(L"该例程只存在于目标端，源端没有同名对象。") + L" " + tail;
    case db::sync::RoutineVerdict::Identical:
        break;
    }
    return tail;
}

} // namespace

SyncRoutineCompareView::SyncRoutineCompareView(wxWindow* parent)
    : SyncSideBySideView(parent, tr(L"目标端定义"), tr(L"源端定义"))
{
    // A body is a paragraph, not a value: take width off the label column and
    // give it to the two sides.
    grid_->SetColSize(0, FromDIP(110));
    grid_->SetColSize(1, FromDIP(340));
    grid_->SetColSize(2, FromDIP(340));
}

void SyncRoutineCompareView::ShowRoutine(const DiffNode* routine)
{
    if (!routine || routine->kind != DiffNodeKind::Routine) {
        ShowEmpty(tr(L"在上方的「函数」或「存储过程」分组中选择一项，以并排查看两侧定义。"));
        return;
    }

    // The 签名 / 语言 / 定义 leaves the model built. Same ValueDiff shape a data
    // row uses, so this loop is the same loop 数据对比 runs.
    std::vector<const DiffNode*> rows;
    for (const auto& c : routine->children)
        if (c->kind == DiffNodeKind::ValueDiff) rows.push_back(c.get());

    if (rows.empty()) {
        ShowEmpty(wxString::Format(tr(L"%s 没有可展示的定义。"), routine->label));
        return;
    }

    SetNote(RoutineNote(*routine));

    BeginRows(static_cast<int>(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        const DiffNode& leaf = *rows[i];
        const int row = static_cast<int>(i);
        // `differs` came from the model and is true for the BODY row only when
        // the verdict was BodyDiffers — i.e. same engine. A cross-engine pair
        // renders both bodies unhighlighted, which is the whole point.
        SetRow(row, leaf.label, leaf.before, leaf.after, leaf.differs);
        if (leaf.label == RoutineBodyRowLabel()) MakeRowTall(row, 220);
    }
    ShowGrid();
}

} // namespace ui
