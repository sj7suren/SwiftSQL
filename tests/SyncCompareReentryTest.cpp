// SyncCompareReentryTest.cpp — regression test for the P0 compare→display crash.
//
// THE BUG: ui::SyncCompareGrid populates its wxDataViewCtrl by calling
// wxDataViewModel::ItemChanged() (from RefreshFromSelection / ItemsRefreshed) to
// repaint each top-level row. On the generic (MSW) wxDataViewCtrl that call
// SYNCHRONOUSLY emits wxEVT_DATAVIEW_ITEM_VALUE_CHANGED — the same event a user's
// checkbox edit produces — which the grid handles by calling RefreshFromSelection
// again, which fires ItemChanged again: unbounded recursion that stack-overflows
// the instant the compare grid is first populated. It could not be caught by any
// headless model test because it lives entirely in the widget's event loop; it
// was reproduced live and fixed with a re-entrancy guard in SyncCompareGrid.
//
// THIS TEST reproduces the exact trigger with a canned plan (the recursion is
// data-independent — any non-empty plan populates a root and starts the loop):
// it constructs the REAL ui::SyncComparePage and calls SetPlan(). Before the fix
// SetPlan never returns (it stack-overflows and the process dies); after the fix
// it returns and this test exits 0. No window is shown and no server is touched —
// the model notification that starts the loop fires regardless of visibility, so
// a wxFrame that is never Show()n is enough.
//
// It needs a wxApp (a window station for the wxFrame/wxDataViewCtrl), so it is a
// wxIMPLEMENT_APP_CONSOLE program rather than a plain main(). A 5s safety timer
// guarantees the process exits even if a future regression reintroduces a
// non-crashing hang.
#include "db/SyncEngine.h"
#include "ui/SyncComparePage.h"

#include <wx/app.h>
#include <wx/frame.h>
#include <wx/timer.h>

#include <cstdio>

using namespace db;
using namespace db::sync;

namespace {

int g_checks = 0, g_fails = 0;
void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    std::printf(cond ? "  ok   %s\n" : "  FAIL %s\n", name);
    if (!cond) ++g_fails;
}

NormColumn Col(const wxString& name, const wxString& raw, bool notNull = false)
{
    NormColumn c; c.name = name; c.rawType = raw; c.notNull = notNull; return c;
}

// A minimal plan that still populates the grid: one source-only CREATE table
// (which gives the grid a top-level row plus column children) and one plain
// structural change. Enough for ItemsRefreshed to emit at least one ItemChanged
// and start the loop.
SyncPlan BuildPlan()
{
    SyncPlan plan;
    {
        SyncPlan::TableUnit u;
        u.table = L"t_new";
        u.ddl.push_back(L"CREATE TABLE t_new (id INT, v VARCHAR(30))");
        u.changes.table = L"t_new";
        u.changes.createTable = true;
        u.changes.createSchema.name = L"t_new";
        u.changes.createSchema.columns.push_back(Col(L"id", L"int", true));
        u.changes.createSchema.columns.push_back(Col(L"v", L"varchar(30)"));
        plan.units.push_back(std::move(u));
    }
    {
        SyncPlan::TableUnit u;
        u.table = L"orders";
        u.ddl.push_back(L"ALTER TABLE orders ADD COLUMN status VARCHAR(20)");
        u.changes.table = L"orders";
        ColumnChange add; add.op = ColumnChange::Op::Add; add.column = Col(L"status", L"varchar(20)");
        u.changes.columns.push_back(add);
        plan.units.push_back(std::move(u));
    }
    return plan;
}

class App : public wxApp {
public:
    bool OnInit() override
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf("== SyncCompareReentryTest ==\n");

        plan_ = BuildPlan();

        // Never Show()n: the recursion is triggered by the model notification in
        // SetPlan, not by painting.
        frame_ = new wxFrame(nullptr, wxID_ANY, "reentry");
        page_  = new ui::SyncComparePage(frame_);

        // Before the fix this call never returns (stack overflow). Reaching the
        // line after it IS the assertion.
        page_->SetPlan(&plan_, nullptr);
        ExpectTrue("SetPlan returned without runaway recursion", true);

        // A second SetPlan mirrors the wizard's re-compare (back → compare again),
        // which reassigns the borrowed plan and re-populates the same grid.
        page_->SetPlan(&plan_, nullptr);
        ExpectTrue("second SetPlan (re-compare) returned", true);

        frame_->Destroy();

        safety_.SetOwner(this);
        Bind(wxEVT_TIMER, &App::OnDone, this);
        safety_.Start(200, wxTIMER_ONE_SHOT);
        return true;
    }

    int OnExit() override
    {
        std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
        return g_fails ? 1 : 0;
    }

private:
    void OnDone(wxTimerEvent&) { ExitMainLoop(); }

    SyncPlan plan_;
    wxFrame* frame_ = nullptr;
    ui::SyncComparePage* page_ = nullptr;
    wxTimer safety_;
};

} // namespace

wxIMPLEMENT_APP_CONSOLE(App);
