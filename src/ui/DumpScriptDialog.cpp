// DumpScriptDialog.cpp — see header. The dump body is the same serialization the
// synchronous ConnectionTree::DumpDatabase used, moved onto a worker thread with
// progress ticks and a cooperative stop check between tables.
#include "ui/DumpScriptDialog.h"
#include "core/CrashLog.h"

#include <wx/wx.h>
#include <wx/gauge.h>
#include <wx/statline.h>
#include <wx/file.h>
#include <wx/stopwatch.h>

#include "core/DbTypes.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

DumpScriptDialog::DumpScriptDialog(wxWindow* parent, db::IConnection* conn,
                                   const wxString& connName, const wxString& engineName,
                                   const wxString& db, bool withData,
                                   std::vector<db::TableInfo> tables,
                                   const wxString& path)
    : CenteredDialog(parent, wxID_ANY, tr(L"转储 SQL 文件"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , conn_(conn)
    , connName_(connName)
    , engineName_(engineName)
    , db_(db)
    , path_(path)
    , withData_(withData)
    , tables_(std::move(tables))
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(BuildUi(), 1, wxEXPAND);
    SetSizerAndFit(root);
    SetSize(FromDIP(wxSize(560, 420)));

    Bind(wxEVT_CLOSE_WINDOW, &DumpScriptDialog::OnCloseWindow, this);
    // Start after the modal loop is running, so an instant finish can't EndModal
    // before ShowModal() has begun.
    CallAfter([this]() { StartWorker(); });
}

DumpScriptDialog::~DumpScriptDialog()
{
    JoinWorker();
}

wxWindow* DumpScriptDialog::BuildUi()
{
    auto* page = new wxPanel(this);
    auto* s = new wxBoxSizer(wxVERTICAL);

    title_ = new wxStaticText(page, wxID_ANY, tr(L"正在转储…"));
    title_->SetFont(Ui(13, /*bold*/ true));
    title_->SetForegroundColour(theme::kTextStrong);
    s->Add(title_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    auto* sub = new wxStaticText(page, wxID_ANY,
        tr(L"数据库") + L"： " + db_ + L"        " + tr(L"文件") + L"： " + path_);
    sub->SetFont(Ui(9));
    sub->SetForegroundColour(theme::kTextSecondary);
    s->Add(sub, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    gauge_ = new wxGauge(page, wxID_ANY, tables_.empty() ? 1 : (int)tables_.size(),
                         wxDefaultPosition, wxSize(-1, 10),
                         wxGA_HORIZONTAL | wxGA_SMOOTH);
    s->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    statusTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    statusTxt_->SetFont(Ui(9));
    statusTxt_->SetForegroundColour(theme::kTextSecondary);
    s->Add(statusTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    rowTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    rowTxt_->SetFont(Ui(10, /*bold*/ true));
    rowTxt_->SetForegroundColour(theme::kPrimary);
    s->Add(rowTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    auto* logHdr = new wxStaticText(page, wxID_ANY, tr(L"日志"));
    logHdr->SetFont(Ui(9, /*bold*/ true));
    logHdr->SetForegroundColour(theme::kTextBody);
    s->Add(logHdr, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    log_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                          wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    log_->SetFont(Ui(9));
    s->Add(log_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    primaryBtn_ = new wxButton(page, wxID_ANY, tr(L"停止"));
    primaryBtn_->Bind(wxEVT_BUTTON, &DumpScriptDialog::OnStopOrClose, this);
    btns->Add(primaryBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    page->SetSizer(s);
    return page;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
void DumpScriptDialog::StartWorker()
{
    if (running_.exchange(true)) return;
    total_ = static_cast<int>(tables_.size());

    db::IConnection* conn = conn_;
    const wxString db = db_, path = path_, engineName = engineName_;
    const bool withData = withData_;

    worker_ = core::CrashLog::GuardedThread(L"导出脚本", [this, conn, db, path, engineName, withData]() {
        wxStopWatch sw;

        wxFile f(path, wxFile::write);
        if (!f.IsOpened()) {
            CallAfter([this]() {
                Warn(tr(L"无法写入文件：") + path_);
                Finish(/*cancelled*/ false, 0, 0, /*writeErr*/ true, 0);
            });
            return;
        }

        const bool mysql = (conn->GetDialect() == db::Dialect::MySQL);
        const wxString q = mysql ? L"`" : L"\"";

        // Buffered sink — flush to disk in ~256 KB chunks so neither the driver
        // nor the UI ever holds a whole table in memory.
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

        wxString err;
        emit(L"-- SwiftSQL 转储 — 数据库 " + db + L"\n");
        emit(L"-- 引擎 " + engineName + L"\n");
        emit(withData ? L"-- 内容: 结构 + 数据\n\n" : L"-- 内容: 仅结构\n\n");
        if (mysql) emit(L"SET FOREIGN_KEY_CHECKS=0;\n\n");

        int okTables = 0;
        long totalRows = 0;
        bool cancelled = false;

        for (int i = 0; i < static_cast<int>(tables_.size()); ++i) {
            if (stopFlag_.load()) { cancelled = true; break; }
            const wxString name = tables_[i].name;
            const int done = i + 1;
            CallAfter([this, done, name]() { SetTable(done, name); });

            emit(L"-- ----------------------------\n-- 表 " + name +
                 L"\n-- ----------------------------\n");
            wxString ddl;
            if (conn->GetCreateDdl(db, name, ddl, err)) {
                emit(L"DROP TABLE IF EXISTS " + q + name + q + L";\n");
                emit(ddl.Trim() + (mysql ? L";\n\n" : L"\n\n"));  // PG DDL already terminated
                ++okTables;
            } else {
                emit(L"-- (读取结构失败: " + err + L")\n\n");
                const wxString w = name + L"： " + tr(L"读取结构失败 — ") + err;
                CallAfter([this, w]() { Warn(w); });
            }

            if (withData && !writeErr) {
                long tableRows = 0;
                auto dataEmit = [&](const wxString& s) {
                    emit(s);
                    // One emit per row — throttle GUI updates to every 4096 rows.
                    if (++tableRows % 4096 == 0) {
                        const long shown = totalRows + tableRows;
                        CallAfter([this, shown]() { SetRows(shown); });
                    }
                };
                if (conn->DumpTableData(db, name, dataEmit, err)) emit(L"\n");
                else {
                    emit(L"-- (导出数据失败: " + err + L")\n\n");
                    const wxString w = name + L"： " + tr(L"导出数据失败 — ") + err;
                    CallAfter([this, w]() { Warn(w); });
                }
                totalRows += tableRows;
                const long shown = totalRows;
                CallAfter([this, shown]() { SetRows(shown); });
            }
            if (writeErr) break;
        }

        // ---- views (schema only) ----
        if (!writeErr && !cancelled) {
            std::vector<wxString> views;
            if (conn->ListViews(db, views, err) && !views.empty()) {
                CallAfter([this]() { statusTxt_->SetLabel(tr(L"正在导出视图…")); });
                emit(L"\n-- ============================\n-- 视图\n"
                     L"-- ============================\n");
                for (const auto& v : views) {
                    emit(L"-- 视图 " + v + L"\n");
                    emit(L"DROP VIEW IF EXISTS " + q + v + q + L";\n");
                    wxString vddl;
                    if (conn->GetViewDdl(db, v, vddl, err))
                        emit(vddl.Trim() + (mysql ? L";\n\n" : L"\n\n"));
                    else
                        emit(L"-- (读取视图失败: " + err + L")\n\n");
                    if (writeErr) break;
                }
            }
        }

        // ---- routines (functions then procedures) ----
        if (!writeErr && !cancelled) {
            std::vector<db::RoutineInfo> routines;
            if (conn->ListRoutines(db, routines, err) && !routines.empty()) {
                CallAfter([this]() { statusTxt_->SetLabel(tr(L"正在导出函数 / 存储过程…")); });
                emit(L"\n-- ============================\n-- 函数 / 存储过程\n"
                     L"-- ============================\n");
                for (const auto& rt : routines) {
                    wxString rddl;
                    if (!conn->GetRoutineDdl(db, rt, rddl, err)) {
                        emit(L"-- (读取 " + rt.name + L" 失败: " + err + L")\n\n");
                        if (writeErr) break; else continue;
                    }
                    emit(L"-- " + rt.type + L" " + rt.name + L"\n");
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

        // ---- triggers ----
        if (!writeErr && !cancelled) {
            std::vector<db::TriggerInfo> triggers;
            if (conn->ListTriggers(db, triggers, err) && !triggers.empty()) {
                CallAfter([this]() { statusTxt_->SetLabel(tr(L"正在导出触发器…")); });
                emit(L"\n-- ============================\n-- 触发器\n"
                     L"-- ============================\n");
                for (const auto& tg : triggers) {
                    wxString tddl;
                    if (!conn->GetTriggerDdl(db, tg, tddl, err)) {
                        emit(L"-- (读取触发器 " + tg.name + L" 失败: " + err + L")\n\n");
                        if (writeErr) break; else continue;
                    }
                    emit(L"-- 触发器 " + tg.name + L"\n");
                    if (mysql) {
                        emit(L"DROP TRIGGER IF EXISTS " + q + tg.name + q + L";\n");
                        emit(L"DELIMITER $$\n" + tddl.Trim() + L"$$\nDELIMITER ;\n\n");
                    } else {
                        emit(L"DROP TRIGGER IF EXISTS " + q + tg.name + q + L" ON " +
                             q + tg.table + q + L";\n");
                        emit(tddl.Trim() + L"\n\n");
                    }
                    if (writeErr) break;
                }
            }
        }

        if (mysql && !cancelled) emit(L"SET FOREIGN_KEY_CHECKS=1;\n");
        flush(/*force*/ true);

        const long ms = sw.Time();
        CallAfter([this, cancelled, okTables, totalRows, writeErr, ms]() {
            Finish(cancelled, okTables, totalRows, writeErr, ms);
        });
    });
}

// ---------------------------------------------------------------------------
// Worker → UI
// ---------------------------------------------------------------------------
void DumpScriptDialog::SetTable(int done, const wxString& table)
{
    gauge_->SetValue(done);
    statusTxt_->SetLabel(wxString::Format(
        tr(L"正在导出表  %d / %d ： %s"), done, total_, table));
}

void DumpScriptDialog::SetRows(long rows)
{
    rowTxt_->SetLabel(wxString::Format(tr(L"已写入 %ld 行"), rows));
}

void DumpScriptDialog::Warn(const wxString& line)
{
    log_->AppendText(line + L"\n");
}

void DumpScriptDialog::Finish(bool cancelled, int okTables, long totalRows,
                              bool writeErr, long elapsedMs)
{
    running_ = false;
    gauge_->SetValue(gauge_->GetRange());

    if (writeErr) {
        title_->SetLabel(tr(L"转储失败"));
        title_->SetForegroundColour(theme::kDotRed);
        succeeded_ = false;
        summary_ = tr(L"写入文件时出错");
    } else if (cancelled) {
        title_->SetLabel(tr(L"已停止（文件不完整）"));
        title_->SetForegroundColour(theme::kDotAmber);
        succeeded_ = false;
        summary_ = tr(L"已停止");
    } else {
        title_->SetLabel(tr(L"转储完成"));
        title_->SetForegroundColour(theme::kGreen);
        succeeded_ = true;
        summary_ = wxString::Format(tr(L"%d/%d 表%s"), okTables, total_,
                                    withData_ ? L" +数据" : L"");
    }
    statusTxt_->SetLabel(wxString::Format(
        tr(L"共 %d 表 · 成功 %d · %ld 行 · 用时 %ld ms"),
        total_, okTables, totalRows, elapsedMs));
    primaryBtn_->SetLabel(tr(L"关闭"));
    primaryBtn_->Enable(true);
    Layout();
}

// ---------------------------------------------------------------------------
// Buttons / lifecycle
// ---------------------------------------------------------------------------
void DumpScriptDialog::OnStopOrClose(wxCommandEvent&)
{
    if (running_.load()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        primaryBtn_->Enable(false);
        primaryBtn_->SetLabel(tr(L"正在停止…"));
    } else {
        Close();
    }
}

void DumpScriptDialog::OnCloseWindow(wxCloseEvent& ev)
{
    if (running_.load()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        ev.Veto();      // let Finish() re-enable closing once the worker drains
        return;
    }
    JoinWorker();
    EndModal(wxID_OK);
}

void DumpScriptDialog::JoinWorker()
{
    if (worker_.joinable()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        worker_.join();
    }
}

} // namespace ui
