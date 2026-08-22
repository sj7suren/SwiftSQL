// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncComparePage.h — the single-screen data-sync compare page (T8 of ADR-015).
// This is the page the user's requirement describes end to end:
//
//   「第一步是对比表结构差异或者是数据差异，以界面表格显示出来，让用户去勾选哪些
//     可以同步还是所有同步，选中后对应下面应该显示 SQL 语句是什么，界面上要有明确
//     的对比」
//
// Layout — one screen, a wxSplitterWindow:
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ ☑全选   □ 允许删除目标多余的行        3 表 · +12 ~5 -0    │  toolbar
//   ├──────────────────────────────────────────────────────────┤
//   │ ☑ │ 表名 │ 类型 │ 结构差异 │ 插入 │ 更新 │ 删除 │ 状态     │  SyncCompareGrid
//   │   ▸ orders   结构+数据   2 项   12    5    0    就绪       │  (expandable)
//   ├──────────────────────────────────────────────────────────┤
//   │ [ SQL 预览 ] [ 结构对比 ] [ 数据对比 ]                     │  detail tabs
//   │ ...                                                       │
//   └──────────────────────────────────────────────────────────┘
//
// It replaces TWO wizard steps (the old diff-review tree page and the separate
// SQL-preview page), which is what makes the SQL visible 「下面」 at the same
// time as the checkboxes instead of one screen later.
//
// ---------------------------------------------------------------------------
// OWNERSHIP — why the page owns ONE model object and the widgets borrow it
// ---------------------------------------------------------------------------
// The page owns a ui::SyncCompareSession, which holds the DiffTree (node
// storage), the SyncSelection (check state) and the borrowed SyncPlan. The
// grid, the preview and the detail views all take borrowed pointers into it.
// That is deliberate: there is exactly one SyncSelection instance per compare,
// so every widget that reads a check bit reads the same bit, and the execution
// spec is built from that same object. No widget can hold a divergent copy
// because none of them holds a copy at all.
//
// The models were LIFTED OUT of this class rather than merely grouped: a
// wxPanel cannot be constructed without a wxApp and a parent window, so while
// the check state lived here, "what will this dialog state execute?" was
// answerable only by a human clicking. It is now answerable headlessly against
// the very object this page delegates to — see ui/SyncSpecScript.h, and read
// its note on what that does and does not establish (it is NOT evidence that
// this screen renders).
#pragma once

#include <wx/panel.h>
#include <functional>
#include <wx/string.h>

#include "ui/SyncCompareSession.h"
#include "ui/SyncDiffModel.h"
#include "ui/SyncSelection.h"

class wxBoxSizer;
class wxSplitterWindow;
class wxNotebook;
class wxCheckBox;
class wxStaticText;
class wxCommandEvent;

namespace db::sync { struct SyncPlan; struct DataExecPlan; struct RoutineDiffSet; }

namespace ui {

class SyncCompareGrid;
class SyncSqlPreviewPane;
class SyncStructureCompareView;
class SyncDataCompareView;
class SyncRoutineCompareView;

class SyncComparePage : public wxPanel {
public:
    explicit SyncComparePage(wxWindow* parent);

    // Adopt a freshly compared plan: rebuilds the diff tree, seeds the
    // selection, and repoints every child widget. The plan is BORROWED and must
    // outlive the page (the wizard owns it).
    //
    // `routines` is the compare-only functions & stored procedures result
    // (ADR-013), or nullptr when the user turned that option off. Unlike the
    // plan it is NOT borrowed past this call: BuildDiffTree copies every string
    // it needs into the tree's own nodes, so no RoutinePair pointer outlives
    // the caller's value (RoutineDiffSet::ByKind returns non-owning pointers
    // into the set — see RoutineDiff.h). It is also never handed to
    // SyncSelection: the selection is seeded from the SyncPlan alone, which is
    // what keeps routines structurally out of the execution spec.
    void SetPlan(const db::sync::SyncPlan* plan,
                 const db::sync::RoutineDiffSet* routines = nullptr);

    // True when anything at all would execute — the wizard's 执行 gate. Derived
    // from the ExecutionSpec, never from a widget, so the button state and the
    // run agree by construction.
    bool HasAnythingSelected() const;

    // The two halves handed to SyncRunnerDialog, both built from the SAME
    // SyncSelection::Build() call so they cannot describe different selections.
    //
    // BuildExecutionPlan() carries the DDL and, per table, everything
    // SyncEngine::Execute needs to RE-derive the data diff at execute time.
    // BuildDataExecPlan() carries the authorization: which categories of which
    // tables may write. Unchecked categories, non-executable categories and —
    // unless the delete master switch is on — every delete are structurally
    // absent from it; nothing downstream re-checks by inspecting SQL text.
    db::sync::SyncPlan      BuildExecutionPlan() const;
    db::sync::DataExecPlan  BuildDataExecPlan() const;

    // The model half, for the scripted verification hook (SWIFTSQL_SYNCSPEC in
    // SyncWizardDialog.cpp). Exposed rather than duplicated so a scripted run
    // drives the SAME object 执行 builds its plan from; a parallel copy could
    // stay green while this page rotted.
    SyncCompareSession&       Session() { return session_; }
    const SyncCompareSession& Session() const { return session_; }

    // Re-read every widget from the session after it was mutated out of band
    // (the scripted hook is the only such caller).
    void RefreshFromSession();

    // How many rows this selection would actually DELETE: the armed delete
    // count (already zero unless the master switch is on, because it derives
    // from the same Build()) minus the delete rows the user individually
    // unchecked. Handed to SyncRunnerDialog so its destructive-operation
    // confirmation is about a real number rather than a category flag — a user
    // who armed deletes and then excluded every one of them is not about to
    // destroy anything, and should not be asked to type a database name to
    // authorize it.
    long long EffectiveDeleteRows() const;

    // Fired whenever the selection changes, so the wizard can re-evaluate its
    // 执行 button.
    std::function<void()> OnSelectionChanged;

private:
    void BuildToolbar(wxBoxSizer* root);
    void OnAllowDeletes(wxCommandEvent&);
    void OnGridSelectionChanged();
    void OnGridFocusChanged();
    void RefreshSummary();
    const DiffNode* FindTableNode(const wxString& id) const;

    // The owned model — the single source of truth for the whole page.
    SyncCompareSession session_;

    wxSplitterWindow*         splitter_ = nullptr;
    SyncCompareGrid*          grid_     = nullptr;
    wxNotebook*               tabs_     = nullptr;
    SyncSqlPreviewPane*       sql_      = nullptr;
    SyncStructureCompareView* structure_ = nullptr;
    SyncDataCompareView*      data_      = nullptr;
    SyncRoutineCompareView*   routine_   = nullptr;
    int                       routineTab_ = -1;   // notebook index, -1 == not added

    wxCheckBox*   cbAllowDeletes_ = nullptr;
    wxStaticText* summary_ = nullptr;
};

} // namespace ui
