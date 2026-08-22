// SyncCompareGrid.h — the Navicat-style compare grid (T8 of ADR-015): the top
// half of the single-screen 数据同步 compare page.
//
// ---------------------------------------------------------------------------
// WHY wxDataViewCtrl AND NOT wxTreeCtrl / wxListCtrl
// ---------------------------------------------------------------------------
// Round 1 shipped a wxTreeCtrl (ui::SyncDiffPanel, removed by this change) so
// the user could expand a table and see WHICH field differs — the
// 「点开表可以显示对比哪里不一致」 requirement.
// The PRD for this round additionally demands per-table COLUMNS (类型 / 结构差异
// / 插入 / 更新 / 删除 / 状态) and per-category checkboxes. A wxTreeCtrl has
// expandability but no columns; a wxListCtrl has columns but no expandability.
// wxDataViewCtrl is the only one of the three with both, which is what lets the
// grid gain columns WITHOUT regressing drill-in.
//
// Expanding a row shows that table's ui::DiffNode children, unchanged from
// Round 1's tree:
//   * a structure diff  -> Column / Index / Constraint leaves with their
//                          Add/Modify/Drop op and rendered definition;
//   * a data diff       -> the 新增N / 修改N / 删除N Row summaries added in T7;
//   * a create          -> every column/index/FK of the new table, as Adds.
//
// ---------------------------------------------------------------------------
// THE WIDGET IS NEVER THE SOURCE OF TRUTH
// ---------------------------------------------------------------------------
// Not one check bit is stored here. wxDataViewModel::GetValue() reads the
// answer out of ui::SyncSelection on every repaint, and SetValue() writes
// straight back into it. There is no cache to fall out of sync, no "apply"
// step, and no way for a repaint to disagree with what Build() will execute.
//
// Identity: every item is a ui::DiffNode*, and a table node's check is keyed by
// ui::TableKey(node->id) — the stable table name. Positions are never used;
// ui::StableId deletes its integral constructors, so a row index physically
// cannot become a key (see SyncSelection.h's file header for the Round 1 bug
// that made this a type-system matter rather than a review comment).
#pragma once

#include <wx/panel.h>
#include <functional>
#include <map>
#include <vector>
#include <wx/string.h>

class wxDataViewCtrl;
class wxDataViewEvent;

namespace ui {

struct DiffTree;
struct DiffNode;
class SyncSelection;
class SyncCompareGridModel;

class SyncCompareGrid : public wxPanel {
public:
    explicit SyncCompareGrid(wxWindow* parent);
    ~SyncCompareGrid() override;

    // Adopt a freshly-built tree + the selection model that owns its check
    // state. BOTH are borrowed, not owned, and must outlive this widget: the
    // page above owns them precisely so the grid cannot be tempted to keep a
    // private copy of either.
    void SetSource(const DiffTree* tree, SyncSelection* selection);

    // Re-read every cell from SyncSelection. Call after anything mutates the
    // selection from outside the grid (the delete master switch, a select-all
    // driven by the page, a Rebind() after re-compare).
    void RefreshFromSelection();

    // The stable id of the table row the user is focused on — the table whose
    // structure/data the detail tabs below should render. Empty when the focus
    // is on a warning branch or nothing at all. Resolving through the node's
    // Table ancestor means clicking a CHILD (a column leaf, a 新增N row) keeps
    // the detail pane on that child's table instead of blanking it.
    wxString FocusedTableId() const;

    // The Routine node the user is focused on (the node itself or the routine
    // above a focused 签名/语言/定义 leaf), or nullptr when the focus is
    // anywhere else. Resolved through the parent chain for the same reason
    // FocusedTableId() is: position is never an identity.
    const DiffNode* FocusedRoutine() const;

    // The DiffNode the user is focused on, or nullptr. Lets the detail tabs
    // render a specific child (e.g. the 修改N summary) rather than only ever
    // the whole table.
    const DiffNode* FocusedNode() const;

    // Fired after any check change, so the page can refresh the SQL preview and
    // the footer counters.
    std::function<void()> OnSelectionChanged;

    // Fired when the focused row changes, so the page can repoint the detail
    // tabs.
    std::function<void()> OnFocusChanged;

private:
    void BuildColumns();
    void OnActivated(wxDataViewEvent&);
    void OnSelectionEvent(wxDataViewEvent&);
    void OnHeaderClick(wxDataViewEvent&);
    void OnValueChanged(wxDataViewEvent&);
    void ExpandTopLevel();

    wxDataViewCtrl*       view_  = nullptr;
    SyncCompareGridModel* model_ = nullptr;   // ref-counted by wxDataViewCtrl
    const DiffTree*       tree_  = nullptr;   // borrowed
    SyncSelection*        sel_   = nullptr;   // borrowed

    // Re-entrancy guard. wxDataViewModel::ItemChanged() (fired from
    // RefreshFromSelection/NotifyRow to REPAINT rows) synchronously emits
    // wxEVT_DATAVIEW_ITEM_VALUE_CHANGED — the SAME event a user's checkbox edit
    // produces — so without this flag OnValueChanged would call
    // RefreshFromSelection, which fires ItemChanged again, in an unbounded
    // recursion that stack-overflows the very first time the grid is populated
    // (the compare→display crash). True while WE are emitting model changes, so
    // OnValueChanged can tell our own repaint notifications from a real edit.
    bool refreshing_ = false;
};

} // namespace ui
