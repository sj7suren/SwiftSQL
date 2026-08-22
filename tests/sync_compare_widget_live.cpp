// sync_compare_widget_live.cpp — env-gated LIVE GUI reproduction driver for the
// P0 compare->display crash. Unlike sync_compare_live (headless model only),
// this creates the REAL wxDataViewCtrl widget (ui::SyncComparePage) in a real
// wxFrame, calls SetPlan with a plan built from live MySQL, PAINTS it, drives a
// re-compare, and destroys it — exercising the exact widget layer the headless
// model test proved clean.
//
// It writes progress to %TEMP%/swiftsql_widget.txt (unbuffered) so a crash's
// last-reached step survives. Auto-exits via a safety timer — no human, and the
// launcher is expected to kill the PID if the timer is beaten by a crash.
//
// SAFETY: env-gated, self-skips, two throwaway swiftsql_p0w_* databases dropped
// on the way in and out. Env: SWIFTSQL_MYTEST_HOST/PORT/USER/PASS, _DB(boot).
#include "mysql_pg_live.h"

#include "db/MySqlRoutineRead.h"
#include "db/RoutineCompare.h"
#include "db/RoutineDiff.h"
#include "db/SyncEngine.h"

#include "ui/SyncComparePage.h"

#include <wx/app.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/timer.h>

#include <atomic>
#include <cstdio>
#include <memory>

using namespace db;
using namespace db::sync;
using namespace mplive;

namespace {
const std::atomic<bool> g_never{false};
std::FILE* g_log = nullptr;
void Log(const char* s) { if (g_log) { std::fprintf(g_log, "%s\n", s); std::fflush(g_log); } }

RoutineDiffSet CompareRoutinesLive(IConnection& src, IConnection& tgt,
                                   const wxString& srcDb, const wxString& tgtDb)
{
    std::vector<RoutineDef> sr, tr;
    wxString sD, tD, useErr;
    src.UseDatabase(srcDb, useErr);
    tgt.UseDatabase(tgtDb, useErr);
    const RoutineReadStatus ss = mysqlroutine::ReadRoutines(src, srcDb, sr, sD);
    const RoutineReadStatus ts = mysqlroutine::ReadRoutines(tgt, tgtDb, tr, tD);
    RoutineCompareOptions opt = MakeRoutineCompareOptions(src.GetDialect(), tgt.GetDialect());
    opt.enabled = true;
    RoutineDiffSet out = CompareRoutines(sr, tr, ss, ts, opt);
    out.sourceStatusDetail = sD; out.targetStatusDetail = tD;
    return out;
}

bool SetupSrc(IConnection& c)
{
    bool ok = true;
    ok &= Exec(c, L"CREATE TABLE t_rows (id INT NOT NULL PRIMARY KEY, name VARCHAR(50), n INT)", "src t_rows");
    ok &= Exec(c, L"INSERT INTO t_rows(id,name,n) VALUES (1,'same',10),(2,'src',20),(3,'srconly',30)", "src rows");
    ok &= Exec(c, L"CREATE TABLE t_struct (id INT NOT NULL PRIMARY KEY, a VARCHAR(20), b INT, KEY idx_b(b))", "src t_struct");
    ok &= Exec(c, L"INSERT INTO t_struct(id,a,b) VALUES (1,'x',1)", "src struct rows");
    ok &= Exec(c, L"CREATE TABLE t_new (id INT NOT NULL PRIMARY KEY, v VARCHAR(30))", "src t_new");
    ok &= Exec(c, L"INSERT INTO t_new(id,v) VALUES (1,'a'),(2,'b'),(3,'c')", "src new rows");
    ok &= Exec(c, L"CREATE FUNCTION f_add(x INT,y INT) RETURNS INT DETERMINISTIC RETURN x+y", "src fn");
    ok &= Exec(c, L"CREATE PROCEDURE p_t() BEGIN UPDATE t_rows SET n=n+1 WHERE id=1; END", "src proc");
    return ok;
}
bool SetupTgt(IConnection& c)
{
    bool ok = true;
    ok &= Exec(c, L"CREATE TABLE t_rows (id INT NOT NULL PRIMARY KEY, name VARCHAR(50), n INT)", "tgt t_rows");
    ok &= Exec(c, L"INSERT INTO t_rows(id,name,n) VALUES (1,'same',10),(2,'tgt',99),(4,'tgtonly',40)", "tgt rows");
    ok &= Exec(c, L"CREATE TABLE t_struct (id INT NOT NULL PRIMARY KEY, a VARCHAR(20))", "tgt t_struct");
    ok &= Exec(c, L"INSERT INTO t_struct(id,a) VALUES (1,'x')", "tgt struct rows");
    ok &= Exec(c, L"CREATE TABLE t_extra (id INT NOT NULL PRIMARY KEY, v VARCHAR(10))", "tgt t_extra");
    ok &= Exec(c, L"CREATE FUNCTION f_add(x INT,y INT) RETURNS INT DETERMINISTIC RETURN x-y", "tgt fn");
    return ok;
}

class App : public wxApp {
public:
    bool OnInit() override
    {
        const wxString logPath = wxFileName::GetTempDir() + wxFileName::GetPathSeparator() +
                                 L"swiftsql_widget.txt";
        g_log = std::fopen((const char*)logPath.utf8_str(), "w");
        Log("START");

        host_ = Env("SWIFTSQL_MYTEST_HOST"); user_ = Env("SWIFTSQL_MYTEST_USER");
        pass_ = Env("SWIFTSQL_MYTEST_PASS");
        if (host_.IsEmpty() || user_.IsEmpty() || pass_.IsEmpty()) { Log("SKIP no env"); return false; }
        port_ = EnvInt("SWIFTSQL_MYTEST_PORT", 3306);
        wxString boot = Env("SWIFTSQL_MYTEST_DB"); if (boot.IsEmpty()) boot = L"mysql";

        const wxString qSrc = QuoteIdent(srcDb_, Dialect::MySQL);
        const wxString qTgt = QuoteIdent(tgtDb_, Dialect::MySQL);
        maint_ = CreateConnection(DbType::MySQL);
        wxString e;
        if (!maint_->Connect(Profile(DbType::MySQL, host_, port_, user_, pass_, boot), e)) {
            Log("FAIL connect maint"); return false;
        }
        QueryResult r;
        maint_->Execute(L"DROP DATABASE IF EXISTS " + qSrc, r, e);
        maint_->Execute(L"CREATE DATABASE " + qSrc + L" DEFAULT CHARACTER SET utf8mb4", r, e);
        maint_->Execute(L"DROP DATABASE IF EXISTS " + qTgt, r, e);
        maint_->Execute(L"CREATE DATABASE " + qTgt + L" DEFAULT CHARACTER SET utf8mb4", r, e);

        src_ = CreateConnection(DbType::MySQL);
        tgt_ = CreateConnection(DbType::MySQL);
        if (!src_->Connect(Profile(DbType::MySQL, host_, port_, user_, pass_, srcDb_), e) ||
            !tgt_->Connect(Profile(DbType::MySQL, host_, port_, user_, pass_, tgtDb_), e)) {
            Log("FAIL connect src/tgt"); Cleanup(); return false;
        }
        if (!SetupSrc(*src_) || !SetupTgt(*tgt_)) { Log("FAIL fixtures"); Cleanup(); return false; }
        Log("fixtures ok");

        SyncScope scope; scope.structure = true; scope.data = true; scope.dropMissingTables = true;
        SyncEngine eng(*src_, *tgt_, srcDb_, tgtDb_);
        if (!eng.BuildPlan(scope, plan_, e, g_never, nullptr)) { Log("FAIL BuildPlan"); Cleanup(); return false; }
        routines_ = CompareRoutinesLive(*src_, *tgt_, srcDb_, tgtDb_);
        Log("plan+routines built");

        frame_ = new wxFrame(nullptr, wxID_ANY, "P0 widget repro", wxDefaultPosition, wxSize(1040, 720));
        page_  = new ui::SyncComparePage(frame_);
        Log("page created");
        page_->SetPlan(&plan_, &routines_);
        Log("SetPlan #1 done");
        frame_->Show();
        Log("frame shown");

        SetTopWindow(frame_);
        timer_.SetOwner(this);
        Bind(wxEVT_TIMER, &App::OnTick, this);
        timer_.Start(400, wxTIMER_ONE_SHOT);
        safety_.SetOwner(this, 2);
        Bind(wxEVT_TIMER, &App::OnSafety, this, 2);
        safety_.Start(20000, wxTIMER_ONE_SHOT);
        return true;
    }

    void OnTick(wxTimerEvent&)
    {
        Log("tick: forcing paint");
        for (int i = 0; i < 5; ++i) { wxYield(); frame_->Refresh(); frame_->Update(); }
        Log("painted #1");

        // Re-compare hazard: SetPlan again on the SAME page (the wizard's second
        // compare reassigns plan_ and re-SetPlans).
        Log("SetPlan #2 (re-compare)...");
        page_->SetPlan(&plan_, &routines_);
        Log("SetPlan #2 done");
        for (int i = 0; i < 5; ++i) { wxYield(); frame_->Refresh(); frame_->Update(); }
        Log("painted #2");

        // Destruction-order: destroy the page/frame (SyncComparePage member
        // session_ owns the DiffTree the wxDataViewCtrl borrows).
        Log("destroying frame...");
        frame_->Destroy();
        wxYield();
        Log("frame destroyed");
        Cleanup();
        Log("DONE clean");
        ExitMainLoop();
    }

    void OnSafety(wxTimerEvent&)
    {
        Log("SAFETY timeout — force exit");
        Cleanup();
        ExitMainLoop();
    }

    void Cleanup()
    {
        if (cleaned_) return; cleaned_ = true;
        if (src_) src_->Disconnect();
        if (tgt_) tgt_->Disconnect();
        if (maint_) {
            QueryResult r; wxString e;
            maint_->Execute(L"DROP DATABASE IF EXISTS " + QuoteIdent(srcDb_, Dialect::MySQL), r, e);
            maint_->Execute(L"DROP DATABASE IF EXISTS " + QuoteIdent(tgtDb_, Dialect::MySQL), r, e);
        }
    }

private:
    wxString host_, user_, pass_; int port_ = 3306;
    const wxString srcDb_ = L"swiftsql_p0w_src";
    const wxString tgtDb_ = L"swiftsql_p0w_tgt";
    std::unique_ptr<IConnection> maint_, src_, tgt_;
    SyncPlan plan_;
    RoutineDiffSet routines_;
    wxFrame* frame_ = nullptr;
    ui::SyncComparePage* page_ = nullptr;
    wxTimer timer_, safety_;
    bool cleaned_ = false;
};

} // namespace

wxIMPLEMENT_APP_CONSOLE(App);
