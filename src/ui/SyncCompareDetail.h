// SyncCompareDetail.h — the two side-by-side comparison views that sit in the
// bottom half of the compare page (T8 of ADR-015), next to the SQL preview:
//
//   SyncStructureCompareView  「结构对比」 — 对象 | 源定义 | 目标定义, one row per
//                             changed column/index/foreign key, differing rows
//                             tinted with the design system's diff tokens.
//   SyncDataCompareView       「数据对比」 — 列 | 目标当前值 | 源值 for the rows of a
//                             selected table, with the differing CELLS (not just
//                             the row) tinted.
//
// This is the user's 「界面上要有明确的对比」: the two sides are adjacent columns
// in one grid, so the comparison is spatial rather than something the reader has
// to reconstruct from SQL text.
//
// ---------------------------------------------------------------------------
// WHY wxGrid AND NOT wxListCtrl
// ---------------------------------------------------------------------------
// The requirement is per-CELL colouring (only the columns whose values differ
// light up). wxListCtrl in report mode carries attributes per ROW on MSW, so it
// can highlight a changed row but cannot point at the changed field. wxGrid has
// SetCellBackgroundColour, which is exactly the granularity asked for.
//
// ---------------------------------------------------------------------------
// SPLIT OUT OF SyncComparePage.cpp ON PURPOSE
// ---------------------------------------------------------------------------
// The project charter caps a file at 1000 lines and this round's brief tightens
// that to ~700 for these widgets. The page is a layout host (splitter +
// notebook + wiring); these are renderers. Keeping them apart holds both files
// well inside the cap and keeps the page readable as pure composition.
#pragma once

#include <wx/panel.h>

class wxGrid;
class wxStaticText;

namespace ui {

struct DiffNode;

// Shared chrome: a wxGrid configured read-only with the project's fonts/tokens,
// plus an empty-state label that replaces it when there is nothing to show.
// Both views need identical setup; neither needs to know how the other fills it.
class SyncSideBySideView : public wxPanel {
public:
    SyncSideBySideView(wxWindow* parent, const wxString& leftTitle, const wxString& rightTitle);

protected:
    void BeginRows(int rows);
    void SetRow(int row, const wxString& label, const wxString& left, const wxString& right,
                bool differs);
    void ShowEmpty(const wxString& message);
    void ShowGrid();

    // A sentence ABOVE the grid, for the case where the grid's content needs
    // qualifying before it is read rather than after. Empty text hides it.
    // Added for the cross-engine routine case: two procedural bodies shown side
    // by side are meaningless — actively misleading, even — without the
    // statement that the tool is not claiming they are equivalent or different.
    void SetNote(const wxString& text);

    // Turn one row into a multi-line block: word-wrapped, top-aligned, and
    // tall. Routine bodies are the only thing here that is a paragraph rather
    // than a value.
    void MakeRowTall(int row, int heightDip);

    wxGrid*       grid_ = nullptr;
    wxStaticText* empty_ = nullptr;
    wxStaticText* note_ = nullptr;
};

// 「结构对比」. `table` is the focused Table DiffNode (nullptr => empty state).
class SyncStructureCompareView : public SyncSideBySideView {
public:
    explicit SyncStructureCompareView(wxWindow* parent);
    void ShowTable(const DiffNode* table);
};

// 「数据对比」. Renders the ValueDiff leaves of the focused table's Row nodes.
// `focus` may be the table node or one of its Row children — passing the child
// narrows the view to that row, which is what clicking 「修改 N 行」 does.
class SyncDataCompareView : public SyncSideBySideView {
public:
    explicit SyncDataCompareView(wxWindow* parent);
    void ShowNode(const DiffNode* table, const DiffNode* focus);
};

// 「函数/存储过程」 — ONE routine's two sides: 签名 / 语言 / 定义, read-only,
// with the body rendered as a wrapped block rather than a value.
//
// THE TWO CASES THIS VIEW EXISTS TO KEEP APART
//   BodyDiffers   (same engine)  — the two bodies are written in the SAME
//                                  language and their text is not the same.
//                                  That is a real finding, so the body row is
//                                  highlighted like any other difference.
//   NotComparable (cross engine) — the two bodies are written in DIFFERENT
//                                  procedural languages. "They differ" would be
//                                  a tautology and "they match" would be
//                                  unfounded, so both bodies are shown PLAIN,
//                                  with no highlight, under a note that says in
//                                  ordinary words why no verdict is offered and
//                                  that nothing here will be auto-translated or
//                                  executed.
// The distinction is driven by DiffNode::verdict — the data layer's own enum —
// never by matching the translated label text.
class SyncRoutineCompareView : public SyncSideBySideView {
public:
    explicit SyncRoutineCompareView(wxWindow* parent);
    void ShowRoutine(const DiffNode* routine);
};

} // namespace ui
