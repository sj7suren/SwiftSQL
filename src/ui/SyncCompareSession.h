// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCompareSession.h — the widget-free MODEL half of the data-sync compare
// screen: the borrowed SyncPlan, the DiffTree built from it, the SyncSelection
// the user's gestures mutate, and the two execution artefacts those three
// produce.
//
// ---------------------------------------------------------------------------
// WHY THIS WAS LIFTED OUT OF SyncComparePage
// ---------------------------------------------------------------------------
// SyncComparePage is a wxPanel. Everything it owns therefore requires a wxApp,
// a parent window and a window station to exist at all, which means the ONE
// question that matters about that class — 「given the state the user left the
// dialog in, what will actually execute?」 — could only ever be answered by a
// human clicking. The wizard -> ExecutionSpec -> DataExecPlan path was, as a
// direct consequence, the last layer of the sync feature verified by argument
// rather than by execution, and it sits immediately upstream of the only
// feature in this program that WRITES to a user's database.
//
// The split is along the same seam as SyncCompareRow.h / SyncCompareGrid.cpp:
// the decisions live in a headless type, the widget only renders and forwards.
// SyncComparePage now owns ONE of these and delegates; it holds no check state
// of its own, so there is no second copy that could disagree with the one the
// execution artefacts are built from. That is load-bearing for the test: a
// scripted run drives THIS object, and it is the same object the running
// dialog builds its plan from — not a re-implementation that could stay green
// while the dialog rotted.
//
// This type deliberately has no notion of a checkbox, a grid row or a
// selection highlight. It cannot tell you what the user SEES. See
// SyncSpecScript.h for the exact boundary of what a scripted run establishes.
#pragma once

#include "ui/SyncCompareRow.h"   // ExcludedRowCounts, Build*BySpec, SyncPlan
#include "ui/SyncDiffModel.h"
#include "ui/SyncSelection.h"

namespace db::sync { struct RoutineDiffSet; }

namespace ui {

class SyncCompareSession {
public:
    // Adopt a freshly compared plan. `plan` is BORROWED and must outlive the
    // session (the wizard owns it). `routines` is copied out of, never held.
    //
    // Reset, not Rebind: a new plan is a new compare, and carrying check state
    // across two different compares would misrepresent what the user approved.
    // The selection is seeded from the PLAN ALONE — there is no overload that
    // takes `routines`, which is the structural reason a compare-only routine
    // can never acquire check state and therefore can never reach a spec.
    void SetPlan(const db::sync::SyncPlan*      plan,
                 const db::sync::RoutineDiffSet* routines = nullptr);

    bool                      HasPlan() const { return plan_ != nullptr; }
    const db::sync::SyncPlan* Plan() const { return plan_; }

    const DiffTree&      Tree() const { return tree_; }
    SyncSelection&       Selection() { return selection_; }
    const SyncSelection& Selection() const { return selection_; }

    // The delete master switch, mirrored onto the selection model — the one
    // gate that decides whether a ui::DeleteGate can be minted at all.
    void SetAllowDeletes(bool on) { selection_.SetAllowDeletes(on); }
    bool AllowDeletes() const { return selection_.AllowDeletes(); }

    // ---- the execution artefacts ------------------------------------------
    //
    // Both are derived from Build(), so they cannot describe different
    // selections, and neither consults a widget.

    bool                   HasAnythingSelected() const;
    db::sync::SyncPlan     BuildExecutionPlan() const;
    db::sync::DataExecPlan BuildDataExecPlan() const;

    // Delete rows surviving BOTH gates the user operated: the category
    // checkbox (already zero unless the master switch is on, because it derives
    // from Build()) minus the delete rows individually unchecked.
    long long EffectiveDeleteRows() const;

    SelectionSummary  Summary() const { return selection_.Summarize(); }
    ExcludedRowCounts ExcludedRows() const { return CountExcludedRows(tree_, selection_); }

private:
    const db::sync::SyncPlan* plan_ = nullptr;   // borrowed
    DiffTree                  tree_;
    SyncSelection             selection_;
};

} // namespace ui
