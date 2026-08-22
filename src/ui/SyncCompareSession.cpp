// SyncCompareSession.cpp — see header. Pure model composition; no widget.
#include "ui/SyncCompareSession.h"

#include "db/RoutineDiff.h"
#include "db/SyncEngine.h"

namespace ui {

void SyncCompareSession::SetPlan(const db::sync::SyncPlan*       plan,
                                 const db::sync::RoutineDiffSet* routines)
{
    plan_ = plan;
    if (!plan_) return;

    // Data-summary children ON: expanding a data-diff table must show its
    // 新增/修改/删除 rows, or drill-in dead-ends for exactly the tables the
    // data half added support for.
    DiffTreeOptions opts;
    opts.includeDataSummary = true;
    tree_ = routines ? BuildDiffTree(*plan_, *routines, opts)
                     : BuildDiffTree(*plan_, opts);

    // Seeded from the plan only — see the header's note on why `routines` is
    // structurally not an input here.
    selection_.Reset(MakeCompareResultFromPlan(*plan_));
}

bool SyncCompareSession::HasAnythingSelected() const
{
    return !selection_.Build().Empty();
}

db::sync::SyncPlan SyncCompareSession::BuildExecutionPlan() const
{
    if (!plan_) return db::sync::SyncPlan{};
    return FilterStructureBySpec(*plan_, selection_.Build());
}

db::sync::DataExecPlan SyncCompareSession::BuildDataExecPlan() const
{
    if (!plan_) return db::sync::DataExecPlan{};
    return ui::BuildDataExecPlan(selection_.Build());
}

long long SyncCompareSession::EffectiveDeleteRows() const
{
    // A count, not a bool, precisely so "the user armed deletes but then
    // excluded every one of them" reads as zero instead of prompting for a
    // typed database name to authorize nothing.
    const long long armed    = selection_.Summarize().deletes;
    const long long excluded = CountExcludedRows(tree_, selection_).deletes;
    return armed > excluded ? armed - excluded : 0;
}

} // namespace ui
