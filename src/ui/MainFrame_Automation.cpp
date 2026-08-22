// MainFrame_Automation.cpp — the Automation feature: the scheduled-jobs list tab
// (a reusable ObjectListPanel over core::AutomationStore), the new/edit/delete/run
// /schedule actions, the headless job runner (reusing db::sync::SyncEngine for the
// three sync flavours and a compact in-file serializer for export jobs), and the
// once-a-minute in-app scheduler tick. Kept in its own TU per the 1000-line file
// charter.
//
// Scheduling is in-app only (the timer runs while the app is open). OS-level
// scheduled tasks (running with the app closed) are a documented phase-2 item and
// are intentionally NOT implemented here.
#include "ui/MainFrame.h"

#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/busyinfo.h>
#include <wx/file.h>
#include <wx/filename.h>

#include "core/AutomationStore.h"
#include "db/SyncEngine.h"
#include "ui/AutomationJobDialog.h"
#include "ui/AutomationScheduleDialog.h"
#include "ui/ConnectionTree.h"
#include "ui/ObjectListPanel.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/MainFrameInternal.h"

namespace ui {

namespace {

using core::AutomationJob;
using core::JobType;
using core::ScheduleKind;
using core::RunResult;

wxString TypeLabel(JobType t)
{
    switch (t) {
    case JobType::SyncStructure: return tr(L"同步表结构");
    case JobType::SyncData:      return tr(L"同步数据");
    case JobType::SyncBoth:      return tr(L"同步表结构 + 数据");
    case JobType::Export:        return tr(L"导出");
    }
    return wxEmptyString;
}

wxString FmtDT(const wxDateTime& t)
{
    return t.IsValid() ? t.Format(L"%m-%d %H:%M") : wxString(L"—");
}

// 上次运行 cell: time + a success/failure mark (or 从未运行).
wxString LastRunLabel(const AutomationJob& j)
{
    if (j.lastResult == RunResult::Never || !j.lastRun.IsValid())
        return tr(L"从未运行");
    const wxString mark = (j.lastResult == RunResult::Success)
                              ? tr(L"✓ 成功") : tr(L"✗ 失败");
    return FmtDT(j.lastRun) + L"  " + mark;
}

// 计划 cell: the period plus the computed next-run time (or 未设定).
wxString ScheduleLabel(const AutomationJob& j)
{
    if (j.schedule == ScheduleKind::None) return tr(L"未设定");
    const wxDateTime next = j.NextRunAfter(wxDateTime::Now());
    wxString period;
    if (j.schedule == ScheduleKind::EveryMinutes) {
        const int m = j.intervalMinutes;
        period = (m % 60 == 0 && m >= 60)
                     ? wxString::Format(tr(L"每 %d 小时"), m / 60)
                     : wxString::Format(tr(L"每 %d 分钟"), m);
    } else {
        period = wxString::Format(tr(L"每天 %02d:%02d"), j.dailyHour, j.dailyMinute);
    }
    return period + L"  ·  " + tr(L"下次 ") + FmtDT(next);
}

// Headless database dump — the compact, dialog-free counterpart of
// DumpScriptDialog's worker (same output shape: per-table DROP+CREATE, optional
// INSERT rows, then views and routines). Runs synchronously on the caller's thread.
bool ExportDatabaseToFile(db::IConnection* conn, const wxString& db,
                          const wxString& path, bool withData, wxString& err)
{
    std::vector<db::TableInfo> tables;
    if (!conn->ListTables(db, tables, err)) return false;

    wxFile f(path, wxFile::write);
    if (!f.IsOpened()) { err = tr(L"无法写入文件: ") + path; return false; }

    const bool mysql = (conn->GetDialect() == db::Dialect::MySQL);
    const wxString q = mysql ? L"`" : L"\"";
    wxString buf;
    bool writeErr = false;
    auto flush = [&](bool force) {
        if (writeErr || buf.IsEmpty()) return;
        if (force || buf.length() >= (1u << 18)) {
            if (!f.Write(buf, wxConvUTF8)) writeErr = true;
            buf.clear();
        }
    };
    auto emit = [&](const wxString& s) { buf += s; flush(false); };

    emit(L"-- SwiftSQL 自动化导出 — 数据库 " + db + L"\n");
    emit(withData ? L"-- 内容: 结构 + 数据\n\n" : L"-- 内容: 仅结构\n\n");
    if (mysql) emit(L"SET FOREIGN_KEY_CHECKS=0;\n\n");

    wxString e2;
    for (const db::TableInfo& t : tables) {
        emit(L"-- 表 " + t.name + L"\n");
        wxString ddl;
        if (conn->GetCreateDdl(db, t.name, ddl, e2)) {
            emit(L"DROP TABLE IF EXISTS " + q + t.name + q + L";\n");
            emit(ddl.Trim() + (mysql ? L";\n\n" : L"\n\n"));
        } else {
            emit(L"-- (读取结构失败: " + e2 + L")\n\n");
        }
        if (withData && !writeErr) {
            auto dataEmit = [&](const wxString& s) { emit(s); };
            if (conn->DumpTableData(db, t.name, dataEmit, e2)) emit(L"\n");
            else emit(L"-- (导出数据失败: " + e2 + L")\n\n");
        }
        if (writeErr) break;
    }

    if (!writeErr) {
        std::vector<wxString> views;
        if (conn->ListViews(db, views, e2) && !views.empty()) {
            emit(L"\n-- 视图\n");
            for (const wxString& v : views) {
                wxString vddl;
                emit(L"DROP VIEW IF EXISTS " + q + v + q + L";\n");
                if (conn->GetViewDdl(db, v, vddl, e2))
                    emit(vddl.Trim() + (mysql ? L";\n\n" : L"\n\n"));
                if (writeErr) break;
            }
        }
    }
    if (!writeErr) {
        std::vector<db::RoutineInfo> routines;
        if (conn->ListRoutines(db, routines, e2) && !routines.empty()) {
            emit(L"\n-- 函数 / 存储过程\n");
            for (const db::RoutineInfo& rt : routines) {
                wxString rddl;
                if (!conn->GetRoutineDdl(db, rt, rddl, e2)) continue;
                if (mysql) {
                    emit(L"DROP " + rt.type + L" IF EXISTS " + q + rt.name + q + L";\n");
                    emit(L"DELIMITER $$\n" + rddl.Trim() + L"$$\nDELIMITER ;\n\n");
                } else {
                    emit(rddl.Trim() + L"\n\n");
                }
                if (writeErr) break;
            }
        }
    }
    flush(true);
    if (writeErr) { err = tr(L"写入文件失败: ") + path; return false; }
    return true;
}

// Snapshot the live connection tree as name→databases candidates for the job dialog.
std::vector<AutomationJobDialog::ConnDbs> GatherConnDbs(ConnectionTree* ct)
{
    std::vector<AutomationJobDialog::ConnDbs> out;
    if (!ct) return out;
    for (ConnEntry* e : ct->connectedEntries()) {
        if (!e || !e->IsConnected()) continue;
        AutomationJobDialog::ConnDbs c;
        c.name = e->profile.name;
        wxString err;
        std::vector<wxString> dbs;
        if (e->conn->ListDatabases(dbs, err)) c.databases = std::move(dbs);
        out.push_back(std::move(c));
    }
    return out;
}

// Execute one job now (synchronous, on the GUI thread). Mutates lastRun / lastResult
// / lastMessage; returns true on success. Never throws to the caller.
bool RunJobHeadless(ConnectionTree* ct, AutomationJob& job)
{
    job.lastRun = wxDateTime::Now();

    wxString connErr;
    ConnEntry* src = ct->EnsureConnectedByName(job.srcConn, connErr);   // auto-connect
    if (!src) {
        job.lastResult = RunResult::Failure;
        job.lastMessage = wxString::Format(tr(L"源连接无法连接: %s"), connErr);
        return false;
    }

    if (job.type == JobType::Export) {
        const wxString stem = core::AutomationStore::Sanitize(job.name);
        const wxString stamp = wxDateTime::Now().Format(L"%Y%m%d_%H%M%S");
        const wxString path = core::AutomationStore::ExportsDir() +
                              wxFileName::GetPathSeparator() +
                              stem + L"_" + stamp + L".sql";
        wxString err;
        if (!src->conn->UseDatabase(job.srcDb, err)) {   // MySQL/SQLServer USE, PG reconnect
            job.lastResult = RunResult::Failure;
            job.lastMessage = err;
            return false;
        }
        if (ExportDatabaseToFile(src->conn.get(), job.srcDb, path, job.withData, err)) {
            job.lastResult = RunResult::Success;
            job.lastMessage = tr(L"已导出至 ") + path;
            return true;
        }
        job.lastResult = RunResult::Failure;
        job.lastMessage = err;
        return false;
    }

    // --- sync flavours ---
    ConnEntry* tgt = ct->EnsureConnectedByName(job.tgtConn, connErr);   // auto-connect
    if (!tgt) {
        job.lastResult = RunResult::Failure;
        job.lastMessage = wxString::Format(tr(L"目标连接无法连接: %s"), connErr);
        return false;
    }

    db::sync::SyncScope scope;
    scope.structure = (job.type == JobType::SyncStructure || job.type == JobType::SyncBoth);
    scope.data      = (job.type == JobType::SyncData      || job.type == JobType::SyncBoth);
    scope.dropMissingTables = job.dropMissing;

    wxString err;
    // Bind each session to its database (MySQL/SQLServer USE, PG reconnect) so the
    // engine's unqualified SQL doesn't hit "no database selected". tgt set last so a
    // same-connection sync writes into tgtDb (reads are db-qualified by the engine).
    if (!src->conn->UseDatabase(job.srcDb, err) || !tgt->conn->UseDatabase(job.tgtDb, err)) {
        job.lastResult = RunResult::Failure;
        job.lastMessage = err;
        return false;
    }
    db::sync::SyncEngine eng(*src->conn, *tgt->conn, job.srcDb, job.tgtDb);
    db::sync::SyncPlan plan;
    std::atomic<bool> stop{false};
    if (!eng.BuildPlan(scope, plan, err, stop, {})) {
        job.lastResult = RunResult::Failure;
        job.lastMessage = tr(L"比对失败: ") + err;
        return false;
    }
    if (plan.Empty()) {
        job.lastResult = RunResult::Success;
        job.lastMessage = tr(L"目标已与源一致，无需同步");
        return true;
    }
    // The DATA authorization. An automation job has no compare screen and no
    // per-table checkboxes, so the scope flag is the whole of the user's intent:
    // every planned table may insert and update. DELETES ARE NEVER ENABLED HERE.
    // That is not an oversight — deleting rows the source no longer has is the
    // feature's safety red line (db::sync::DataSyncOptions::deleteMissing is off
    // by default for the same reason), and an unattended, scheduled job is the
    // worst possible place to arm it. There is no job field that requests it, so
    // no db::sync::DeleteAuthorization is ever minted on this path and the merge
    // cannot detect a delete, let alone apply one. (job.dropMissing is about
    // whole target-only TABLES in the structure phase — a different decision.)
    db::sync::DataExecPlan dataPlan;
    if (scope.data) {
        for (const auto& u : plan.units) {
            // Key(): TableDataSpec is looked up by DataExecPlan::Find, which
            // matches on the same identity string the plan unit carries.
            db::sync::TableDataSpec t(u.table.Key());
            t.EnableInserts();
            t.EnableUpdates();
            dataPlan.tables.push_back(std::move(t));
        }
    }

    // The data-bearing overload. The structure-only one fails closed on any
    // plan carrying data changes, so a SyncData / SyncBoth job could not run at
    // all through it.
    db::sync::DataExecResult dataResult;
    if (!eng.Execute(plan, dataPlan, job.useTransaction, dataResult, err, stop, {})) {
        job.lastResult = RunResult::Failure;
        // Say which tables already landed. A per-table-transactional data pass
        // leaves earlier tables committed on a mid-run failure, and reporting
        // only "执行失败" would leave the operator to guess the target's state.
        const std::vector<wxString> done = dataResult.Committed();
        job.lastMessage = tr(L"执行失败: ") + err;
        if (!done.empty())
            job.lastMessage += wxString::Format(tr(L"（已提交 %zu 张表的数据，未回滚）"),
                                                done.size());
        return false;
    }

    job.lastResult = RunResult::Success;
    job.lastMessage = wxString::Format(tr(L"同步完成，共 %zu 张表"), plan.units.size());
    if (scope.data) {
        // Rows ACTUALLY applied, which the execute pass counts by re-running the
        // diff — they can legitimately differ from the compare pass's numbers if
        // the source moved in between. Reporting the applied ones is the honest
        // choice for a log nobody was watching live.
        long long ins = 0, upd = 0, del = 0;
        for (const auto& t : dataResult.tables) { ins += t.inserts; upd += t.updates; del += t.deletes; }
        job.lastMessage += wxString::Format(tr(L"，实际写入 +%lld ~%lld -%lld"), ins, upd, del);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
wxWindow* MainFrame::EnsureAutomationTab()
{
    if (automationTab_ && editors_->GetPageIndex(automationTab_) != wxNOT_FOUND)
        return automationTab_;

    auto* panel = new ObjectListPanel(editors_);
    ConfigureAutomationList(panel);
    automationList_ = panel;
    automationTab_  = panel;

    editors_->AddPage(panel, tr(L"Automation"), /*select*/ true,
                      icons::Stroke(icons::Glyph::Timer, 14, theme::kAccent));
    UpdateQueryView();
    return automationTab_;
}

void MainFrame::OpenAutomationTab()
{
    EnsureAutomationTab();
    if (automationList_) automationList_->Reload();
    const int idx = editors_->GetPageIndex(automationTab_);
    if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
    SwitchView(View::Query);
}

void MainFrame::ReloadAutomation()
{
    if (automationList_ && automationTab_ &&
        editors_->GetPageIndex(automationTab_) != wxNOT_FOUND)
        automationList_->Reload();
}

void MainFrame::ConfigureAutomationList(ObjectListPanel* panel)
{
    using Row    = ObjectListPanel::Row;
    using Action = ObjectListPanel::Action;

    panel->SetLoader([](std::vector<Row>& out, wxString&) -> bool {
        for (const AutomationJob& j : core::AutomationStore::List()) {
            Row r;
            r.cells = { j.name, TypeLabel(j.type), LastRunLabel(j), ScheduleLabel(j) };
            out.push_back(std::move(r));
        }
        return true;
    });

    std::vector<ObjectListPanel::Column> cols = {
        { tr(L"名称"), 200 }, { tr(L"类型"), 160 },
        { tr(L"上次运行"), 200 }, { tr(L"计划（下次/周期）"), 260 },
    };

    std::vector<Action> actions = {
        { tr(L"新建"), icons::Glyph::Plus, theme::kGreen, false,
          [this](const wxString&, const std::vector<wxString>&) { AutomationNew(); } },
        { tr(L"修改"), icons::Glyph::Code, theme::kDotPurple, true,
          [this](const wxString& n, const std::vector<wxString>&) {
              if (!n.IsEmpty()) AutomationEdit(n); } },
        { tr(L"删除"), icons::Glyph::Close, theme::kDotRed, true,
          [this](const wxString& n, const std::vector<wxString>&) {
              if (!n.IsEmpty()) AutomationDelete(n); } },
        { tr(L"开始"), icons::Glyph::Play, theme::kGreen, true,
          [this](const wxString& n, const std::vector<wxString>&) {
              if (!n.IsEmpty()) AutomationRun(n, /*interactive*/ true); } },
        { tr(L"设定自动计划"), icons::Glyph::Timer, theme::kAccent, true,
          [this](const wxString& n, const std::vector<wxString>&) {
              if (!n.IsEmpty()) AutomationSetSchedule(n); } },
        { tr(L"删除自动计划"), icons::Glyph::Stop, theme::kDotAmber, true,
          [this](const wxString& n, const std::vector<wxString>&) {
              if (!n.IsEmpty()) AutomationClearSchedule(n); } },
        { tr(L"保存"), icons::Glyph::Save, theme::kPrimary, false,
          [this](const wxString&, const std::vector<wxString>&) {
              ReloadAutomation();
              SetStatusText(tr(L"作业已保存（更改即时生效）")); } },
        { tr(L"刷新"), icons::Glyph::Refresh, theme::kTextSecondary, false,
          [this](const wxString&, const std::vector<wxString>&) { ReloadAutomation(); } },
    };

    panel->Setup(std::move(cols), std::move(actions), /*doubleClick*/ 1);   // dbl → 修改
}

// ---------------------------------------------------------------------------
void MainFrame::AutomationNew()
{
    AutomationJob fresh;
    AutomationJobDialog dlg(this, GatherConnDbs(connTree_.get()), fresh, /*isNew*/ true);
    if (dlg.ShowModal() != wxID_OK) return;

    AutomationJob job = dlg.Job();
    if (core::AutomationStore::Exists(job.name) &&
        wxMessageBox(wxString::Format(tr(L"作业「%s」已存在，覆盖它吗?"), job.name),
                     tr(L"自动化作业"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;

    wxString err;
    if (!core::AutomationStore::Save(job, err)) {
        wxMessageBox(err, tr(L"自动化作业"), wxOK | wxICON_ERROR, this);
        return;
    }
    ReloadAutomation();
    SetStatusText(wxString::Format(tr(L"已创建作业: %s"), job.name));
}

void MainFrame::AutomationEdit(const wxString& name)
{
    AutomationJob job;
    if (!core::AutomationStore::Get(name, job)) return;

    AutomationJobDialog dlg(this, GatherConnDbs(connTree_.get()), job, /*isNew*/ false);
    if (dlg.ShowModal() != wxID_OK) return;

    AutomationJob edited = dlg.Job();
    // A rename must not orphan the old group (the store keys on the sanitized name).
    if (core::AutomationStore::Sanitize(edited.name) !=
        core::AutomationStore::Sanitize(name))
        core::AutomationStore::Remove(name);

    wxString err;
    if (!core::AutomationStore::Save(edited, err)) {
        wxMessageBox(err, tr(L"自动化作业"), wxOK | wxICON_ERROR, this);
        return;
    }
    ReloadAutomation();
    SetStatusText(wxString::Format(tr(L"已保存作业: %s"), edited.name));
}

void MainFrame::AutomationDelete(const wxString& name)
{
    if (wxMessageBox(wxString::Format(tr(L"确定删除作业「%s」吗?此操作不可撤销。"), name),
                     tr(L"删除作业"), wxYES_NO | wxICON_WARNING, this) != wxYES)
        return;
    core::AutomationStore::Remove(name);
    ReloadAutomation();
    SetStatusText(wxString::Format(tr(L"已删除作业: %s"), name));
}

void MainFrame::AutomationRun(const wxString& name, bool interactive)
{
    // Guard re-entry: a job already running (or queued) must not launch twice.
    if (runningJobs_.count(name)) {
        if (interactive)
            SetStatusText(wxString::Format(tr(L"作业正在运行: %s"), name));
        return;
    }
    AutomationJob job;
    if (!core::AutomationStore::Get(name, job)) return;

    runningJobs_.insert(name);
    SetStatusText(wxString::Format(tr(L"正在运行作业: %s…"), name));
    bool ok;
    {
        wxBusyCursor busy;   // large syncs block the UI briefly — noted as an MVP tradeoff
        ok = RunJobHeadless(connTree_.get(), job);
    }
    wxString err;
    core::AutomationStore::Save(job, err);   // persist lastRun / result
    runningJobs_.erase(name);

    ReloadAutomation();
    SetStatusText((ok ? tr(L"作业完成: ") : tr(L"作业失败: ")) + name +
                  L" — " + job.lastMessage);
    if (interactive && !ok)
        wxMessageBox(job.lastMessage, tr(L"自动化作业 — ") + name,
                     wxOK | wxICON_ERROR, this);
}

void MainFrame::AutomationSetSchedule(const wxString& name)
{
    AutomationJob job;
    if (!core::AutomationStore::Get(name, job)) return;

    AutomationScheduleDialog dlg(this, job);
    if (dlg.ShowModal() != wxID_OK) return;

    wxString err;
    if (!core::AutomationStore::Save(dlg.Job(), err)) {
        wxMessageBox(err, tr(L"自动化作业"), wxOK | wxICON_ERROR, this);
        return;
    }
    ReloadAutomation();
    SetStatusText(wxString::Format(tr(L"已设定自动计划: %s"), name));
}

void MainFrame::AutomationClearSchedule(const wxString& name)
{
    AutomationJob job;
    if (!core::AutomationStore::Get(name, job)) return;
    job.schedule = ScheduleKind::None;
    wxString err;
    core::AutomationStore::Save(job, err);
    ReloadAutomation();
    SetStatusText(wxString::Format(tr(L"已删除自动计划: %s"), name));
}

// ---------------------------------------------------------------------------
// In-app scheduler: a once-a-minute tick that runs every job whose next-due time
// has arrived. Runs on the GUI thread; each run is dispatched via CallAfter so a
// long sync never blocks inside the timer callback, and runningJobs_ prevents a
// still-queued run from being scheduled twice.
void MainFrame::StartAutomationTimer()
{
    automationTimer_.SetOwner(this, ID_AUTOMATION_TIMER);
    Bind(wxEVT_TIMER, &MainFrame::OnAutomationTick, this, ID_AUTOMATION_TIMER);
    automationTimer_.Start(60 * 1000);   // 60 s
}

void MainFrame::OnAutomationTick(wxTimerEvent&)
{
    const wxDateTime now = wxDateTime::Now();
    for (const AutomationJob& j : core::AutomationStore::List()) {
        if (j.schedule == ScheduleKind::None) continue;
        if (runningJobs_.count(j.name)) continue;
        const wxDateTime next = j.NextRunAfter(now);
        if (!next.IsValid() || next > now) continue;
        const wxString name = j.name;
        CallAfter([this, name]() { AutomationRun(name, /*interactive*/ false); });
    }
}

} // namespace ui
