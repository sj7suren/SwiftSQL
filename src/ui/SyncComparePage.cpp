// SyncComparePage.cpp — see header. Pure composition: splitter, toolbar,
// grid, detail tabs, and the wiring that keeps them all reading one
// SyncSelection.
#include "ui/SyncComparePage.h"

#include <wx/checkbox.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include "db/SyncEngine.h"
#include "ui/I18n.h"
#include "ui/SyncCompareDetail.h"
#include "ui/SyncCompareGrid.h"
#include "ui/SyncCompareRow.h"
#include "ui/SyncSqlPreviewPane.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

SyncComparePage::SyncComparePage(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    BuildToolbar(root);
    root->Add(new wxStaticLine(this), 0, wxEXPAND);

    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxSP_NOBORDER);
    splitter_->SetMinimumPaneSize(FromDIP(120));

    grid_ = new SyncCompareGrid(splitter_);
    grid_->OnSelectionChanged = [this]() { OnGridSelectionChanged(); };
    grid_->OnFocusChanged     = [this]() { OnGridFocusChanged(); };

    tabs_ = new wxNotebook(splitter_, wxID_ANY);
    tabs_->SetFont(Ui(9));
    sql_       = new SyncSqlPreviewPane(tabs_);
    structure_ = new SyncStructureCompareView(tabs_);
    data_      = new SyncDataCompareView(tabs_);
    routine_   = new SyncRoutineCompareView(tabs_);
    tabs_->AddPage(sql_,       tr(L"SQL 预览"), /*select*/ true);
    tabs_->AddPage(structure_, tr(L"结构对比"));
    tabs_->AddPage(data_,      tr(L"数据对比"));
    // Its own tab rather than a fourth column of 结构对比: a routine's two sides
    // are paragraphs of code, and it is not selectable, so mixing it into the
    // table-oriented views would put non-actionable content inside the panes
    // whose every other row IS actionable.
    tabs_->AddPage(routine_,   tr(L"函数/存储过程"));
    routineTab_ = static_cast<int>(tabs_->GetPageCount()) - 1;

    // Grid on top, detail below — the layout the requirement spells out
    // (「选中后对应下面应该显示 SQL 语句」). 55/45 keeps both usable; the sash
    // lets the user favour either.
    splitter_->SplitHorizontally(grid_, tabs_, 0);
    splitter_->SetSashGravity(0.55);
    root->Add(splitter_, 1, wxEXPAND);

    SetSizer(root);
}

void SyncComparePage::BuildToolbar(wxBoxSizer* root)
{
    auto* bar = new wxBoxSizer(wxHORIZONTAL);

    // The delete master switch. Off by default and separate from any per-table
    // control, because SyncSelection makes it the gate that decides whether a
    // DeleteGate can be minted at all — no switch, no deletes, regardless of
    // what any checkbox says.
    cbAllowDeletes_ = new wxCheckBox(this, wxID_ANY, tr(L"允许删除目标中多余的行"));
    cbAllowDeletes_->SetForegroundColour(theme::kDiffDelFg);
    cbAllowDeletes_->SetToolTip(
        tr(L"关闭时，「删除」列不可勾选，且执行计划中不会包含任何 DELETE 语句。"));
    cbAllowDeletes_->Bind(wxEVT_CHECKBOX, &SyncComparePage::OnAllowDeletes, this);
    bar->Add(cbAllowDeletes_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    bar->AddStretchSpacer(1);

    summary_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    summary_->SetFont(Ui(9));
    summary_->SetForegroundColour(theme::kTextSecondary);
    bar->Add(summary_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    root->Add(bar, 0, wxEXPAND | wxTOP | wxBOTTOM, 8);
}

void SyncComparePage::SetPlan(const db::sync::SyncPlan* plan,
                              const db::sync::RoutineDiffSet* routines)
{
    // The tree build and the selection seeding live in the session (the model
    // half), so they are reachable without a window — see SyncCompareSession.h.
    session_.SetPlan(plan, routines);
    if (!session_.HasPlan()) return;

    session_.SetAllowDeletes(cbAllowDeletes_->IsChecked());

    grid_->SetSource(&session_.Tree(), &session_.Selection());
    sql_->SetSource(session_.Plan(), &session_.Selection());
    OnGridFocusChanged();
    RefreshSummary();
    sql_->RefreshNow();
}

void SyncComparePage::RefreshFromSession()
{
    // The scripted hook mutates the session directly (it is the model the
    // dialog builds its plan from). The widgets are pure readers of that model,
    // so re-reading them is the whole of "catching up" — there is no widget
    // state to reconcile, because none of them owns any.
    if (cbAllowDeletes_) cbAllowDeletes_->SetValue(session_.AllowDeletes());
    if (grid_) grid_->RefreshFromSelection();
    RefreshSummary();
    if (sql_) sql_->RefreshNow();
}

void SyncComparePage::OnAllowDeletes(wxCommandEvent&)
{
    session_.SetAllowDeletes(cbAllowDeletes_->IsChecked());
    // The switch changes which cells are activatable AND which statements the
    // spec may contain, so both halves refresh.
    grid_->RefreshFromSelection();
    OnGridSelectionChanged();
}

void SyncComparePage::OnGridSelectionChanged()
{
    sql_->ScheduleRefresh();   // debounced: a select-all is one render, not N
    RefreshSummary();
    if (OnSelectionChanged) OnSelectionChanged();
}

void SyncComparePage::OnGridFocusChanged()
{
    const wxString id = grid_->FocusedTableId();
    const DiffNode* table = FindTableNode(id);
    const DiffNode* focus = grid_->FocusedNode();

    structure_->ShowTable(table);
    data_->ShowNode(table, focus);

    // A routine and a table are mutually exclusive focuses (FocusedRoutine()
    // stops at a Table ancestor), so this never fights the two views above.
    const DiffNode* routine = grid_->FocusedRoutine();
    routine_->ShowRoutine(routine);
    // Switch TO the routine tab when a routine is picked, and never switch
    // away: the other three panes have nothing to say about a routine, so
    // leaving the user on 结构对比 reading 「在上方选择一个表」 would look like
    // the click did nothing. Returning them to a table is their own gesture.
    if (routine && routineTab_ >= 0 && tabs_->GetSelection() != routineTab_)
        tabs_->SetSelection(routineTab_);
}

const DiffNode* SyncComparePage::FindTableNode(const wxString& id) const
{
    if (id.IsEmpty()) return nullptr;
    // By stable id, never by position — the same contract the grid and the
    // selection model hold to.
    for (const auto& root : session_.Tree().roots) {
        if (root->kind != DiffNodeKind::Category) continue;
        for (const auto& t : root->children)
            if (t->kind == DiffNodeKind::Table && t->id == id) return t.get();
    }
    return nullptr;
}

void SyncComparePage::RefreshSummary()
{
    const SelectionSummary  s  = session_.Summary();
    const ExcludedRowCounts ex = session_.ExcludedRows();

    // Summarize() is computed FROM Build(), so these counters can never claim
    // deletes the master switch would strip. Row exclusions are subtracted on
    // top, and the subtraction is exact rather than approximate: a row can only
    // be excluded on a NON-truncated table, whose sample holds every counted
    // row, so each excluded row is one row that was counted exactly once (see
    // ui::CountExcludedRows). Clamped at zero anyway — a footer that reads
    // 「插入 -1」 would be a worse bug than the one it exposed.
    auto net = [](long long total, long long excluded) {
        return total > excluded ? total - excluded : 0;
    };
    wxString label = wxString::Format(
        tr(L"已选 %zu 张表 · 结构 %zu · 插入 %lld · 更新 %lld · 删除 %lld"),
        s.tables, s.structureTables, net(s.inserts, ex.inserts),
        net(s.updates, ex.updates), net(s.deletes, ex.deletes));
    if (ex.Total() > 0)
        label += wxString::Format(tr(L" · 已逐行排除 %lld 行"), ex.Total());

    summary_->SetLabel(label);
    Layout();
}

// The three execution-facing answers are the session's, verbatim. This page
// deliberately keeps NO logic of its own here: a second implementation is how
// the widget and the plan get to disagree, and it is also how a headless test
// of the session could stay green while the running dialog did something else.
long long SyncComparePage::EffectiveDeleteRows() const
{
    return session_.EffectiveDeleteRows();
}

bool SyncComparePage::HasAnythingSelected() const
{
    return session_.HasAnythingSelected();
}

db::sync::SyncPlan SyncComparePage::BuildExecutionPlan() const
{
    return session_.BuildExecutionPlan();
}

db::sync::DataExecPlan SyncComparePage::BuildDataExecPlan() const
{
    return session_.BuildDataExecPlan();
}

} // namespace ui
