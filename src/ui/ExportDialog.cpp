// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ExportDialog.cpp — see header. Config page collects scope/format/path/options;
// the run page streams the write off a worker thread. Format serialization is
// delegated to db::TableWriter (TableExport.h) — this file only takes rows from
// the driver (or the request's in-memory snapshots) and streams bytes to disk.
#include "ui/ExportDialog.h"
#include "core/CrashLog.h"

#include <wx/wx.h>
#include <wx/radiobox.h>
#include <wx/filepicker.h>
#include <wx/gauge.h>
#include <wx/simplebook.h>
#include <wx/statline.h>
#include <wx/file.h>
#include <wx/wfstream.h>
#include <wx/filename.h>
#include <wx/stopwatch.h>

#include <algorithm>
#include <memory>

#include "db/TableExport.h"
#include "db/TableExportBin.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// Format choice order → db::ExportFormat + file extension.
db::ExportFormat FormatOf(int idx)
{
    switch (idx) {
    case 1:  return db::ExportFormat::Txt;
    case 2:  return db::ExportFormat::Json;
    case 3:  return db::ExportFormat::Xml;
    case 4:  return db::ExportFormat::Sql;
    case 5:  return db::ExportFormat::Xlsx;
    case 6:  return db::ExportFormat::Dbf;
    default: return db::ExportFormat::Csv;
    }
}

// Xlsx / Dbf are binary: they bypass the text sink and stream through the
// binary writers (db::XlsxWriter / db::DbfWriter) instead of db::TableWriter.
bool IsBinaryFormat(db::ExportFormat f)
{
    return f == db::ExportFormat::Xlsx || f == db::ExportFormat::Dbf;
}

// TXT delimiter choice → the actual separator character.
wxString DelimOf(int idx)
{
    switch (idx) {
    case 1:  return L",";
    case 2:  return L";";
    case 3:  return L"|";
    default: return L"\t";
    }
}

} // namespace

ExportDialog::ExportDialog(wxWindow* parent, db::IConnection* conn,
                           const ExportRequest& req)
    : CenteredDialog(parent, wxID_ANY, tr(L"导出数据"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , conn_(conn)
    , req_(req)
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->AddPage(BuildConfigPage(), tr(L"配置"));
    book_->AddPage(BuildRunPage(),    tr(L"导出"));
    book_->SetSelection(0);
    root->Add(book_, 1, wxEXPAND);
    SetSizerAndFit(root);
    SetSize(FromDIP(wxSize(560, 480)));

    Bind(wxEVT_CLOSE_WINDOW, &ExportDialog::OnCloseWindow, this);
}

ExportDialog::~ExportDialog() { JoinWorker(); }

// ---------------------------------------------------------------------------
wxWindow* ExportDialog::BuildConfigPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(page, wxID_ANY, tr(L"导出数据"));
    title->SetFont(Ui(13, /*bold*/ true));
    title->SetForegroundColour(theme::kTextStrong);
    s->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    auto* sub = new wxStaticText(page, wxID_ANY,
        tr(L"表") + L"： " + (req_.table.IsEmpty() ? tr(L"（查询结果）") : req_.table));
    sub->SetFont(Ui(9));
    sub->SetForegroundColour(theme::kTextSecondary);
    s->Add(sub, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // --- scope ---
    wxArrayString scopes;
    scopes.Add(tr(L"当前页"));
    scopes.Add(tr(L"整表全部"));
    scopes.Add(tr(L"选中记录"));
    scopeBox_ = new wxRadioBox(page, wxID_ANY, tr(L"导出范围"), wxDefaultPosition,
                               wxDefaultSize, scopes, 1, wxRA_SPECIFY_ROWS);
    if (!req_.browse)           scopeBox_->Enable(1, false);   // no single-table target
    if (req_.selectedRows.empty()) scopeBox_->Enable(2, false); // nothing selected
    scopeBox_->SetSelection(0);
    s->Add(scopeBox_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* grid = new wxFlexGridSizer(2, wxSize(12, 10));
    grid->AddGrowableCol(1, 1);
    auto label = [&](const wxString& t) {
        auto* l = new wxStaticText(page, wxID_ANY, t);
        l->SetForegroundColour(theme::kTextBody);
        return l;
    };

    // --- format ---
    formatChoice_ = new wxChoice(page, wxID_ANY);
    for (const wchar_t* f : { L"CSV", L"TXT", L"JSON", L"XML", L"SQL (INSERT)",
                              L"Excel (.xlsx)", L"DBF" })
        formatChoice_->Append(f);
    formatChoice_->SetSelection(0);
    formatChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnFormatChanged(); });
    grid->Add(label(tr(L"格式")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(formatChoice_, 0, wxALIGN_LEFT);

    // --- TXT delimiter ---
    delimChoice_ = new wxChoice(page, wxID_ANY);
    delimChoice_->Append(L"Tab");
    delimChoice_->Append(tr(L"逗号 ,"));
    delimChoice_->Append(tr(L"分号 ;"));
    delimChoice_->Append(tr(L"竖线 |"));
    delimChoice_->SetSelection(0);
    delimChoice_->Enable(false);   // TXT only; enabled when format=TXT
    grid->Add(label(tr(L"TXT 分隔符")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(delimChoice_, 0, wxALIGN_LEFT);

    // --- encoding ---
    encoding_ = new wxChoice(page, wxID_ANY);
    encoding_->Append(L"UTF-8");
    encoding_->Append(tr(L"UTF-8 (带 BOM)"));
    encoding_->SetSelection(0);
    grid->Add(label(tr(L"编码")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(encoding_, 0, wxALIGN_LEFT);

    s->Add(grid, 0, wxEXPAND | wxALL, 16);

    // --- path ---
    const wxString defName = (req_.table.IsEmpty() ? wxString(L"export") : req_.table)
                             + L"." + DefaultExt();
    pathPicker_ = new wxFilePickerCtrl(
        page, wxID_ANY, defName, tr(L"选择导出文件"),
        L"*.*", wxDefaultPosition, wxDefaultSize,
        wxFLP_SAVE | wxFLP_USE_TEXTCTRL);
    pathPicker_->SetPath(defName);
    auto* pathRow = new wxBoxSizer(wxHORIZONTAL);
    auto* pl = new wxStaticText(page, wxID_ANY, tr(L"文件"));
    pl->SetForegroundColour(theme::kTextBody);
    pathRow->Add(pl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    pathRow->Add(pathPicker_, 1, wxEXPAND);
    s->Add(pathRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);

    header_ = new wxCheckBox(page, wxID_ANY, tr(L"包含表头行（列名）"));
    header_->SetValue(true);
    s->Add(header_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

    overwrite_ = new wxCheckBox(page, wxID_ANY, tr(L"覆盖同名文件（不再询问）"));
    overwrite_->SetValue(true);
    s->Add(overwrite_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

    s->AddStretchSpacer(1);
    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* cancel = new wxButton(page, wxID_ANY, tr(L"关闭"));
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    btns->Add(cancel, 0, wxRIGHT, 10);
    startBtn_ = new wxButton(page, wxID_ANY, tr(L"开始导出"));
    startBtn_->SetBackgroundColour(theme::kPrimary);
    startBtn_->SetForegroundColour(theme::kWhite);
    startBtn_->Bind(wxEVT_BUTTON, &ExportDialog::OnStart, this);
    btns->Add(startBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    page->SetSizer(s);
    return page;
}

wxWindow* ExportDialog::BuildRunPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    runTitle_ = new wxStaticText(page, wxID_ANY, tr(L"正在导出…"));
    runTitle_->SetFont(Ui(13, /*bold*/ true));
    runTitle_->SetForegroundColour(theme::kTextStrong);
    s->Add(runTitle_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    gauge_ = new wxGauge(page, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 10),
                         wxGA_HORIZONTAL | wxGA_SMOOTH);
    s->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    rowTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    rowTxt_->SetFont(Ui(10, /*bold*/ true));
    rowTxt_->SetForegroundColour(theme::kPrimary);
    s->Add(rowTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    log_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                          wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    log_->SetFont(Ui(9));
    s->Add(log_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    primaryBtn_ = new wxButton(page, wxID_ANY, tr(L"停止"));
    primaryBtn_->Bind(wxEVT_BUTTON, &ExportDialog::OnStopOrClose, this);
    btns->Add(primaryBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    page->SetSizer(s);
    return page;
}

// ---------------------------------------------------------------------------
wxString ExportDialog::DefaultExt() const
{
    switch (formatChoice_ ? formatChoice_->GetSelection() : 0) {
    case 1:  return L"txt";
    case 2:  return L"json";
    case 3:  return L"xml";
    case 4:  return L"sql";
    case 5:  return L"xlsx";
    case 6:  return L"dbf";
    default: return L"csv";
    }
}

void ExportDialog::OnFormatChanged()
{
    // Swap the path's extension to match the newly-selected format.
    wxFileName fn(pathPicker_->GetPath());
    if (fn.GetName().IsEmpty())
        fn.SetName(req_.table.IsEmpty() ? wxString(L"export") : req_.table);
    fn.SetExt(DefaultExt());
    pathPicker_->SetPath(fn.GetFullPath());
    delimChoice_->Enable(formatChoice_->GetSelection() == 1);   // TXT only
}

wxString ExportDialog::QualifiedTable() const
{
    // Quote each identifier through db::QuoteIdent so an embedded quote in a db /
    // table name is doubled rather than bare-spliced (consistent with the codebase).
    if (req_.dialect == db::Dialect::MySQL && !req_.db.IsEmpty())
        return db::QuoteIdent(req_.db, req_.dialect) + L"." +
               db::QuoteIdent(req_.table, req_.dialect);
    return db::QuoteIdent(req_.table, req_.dialect);
}

// ---------------------------------------------------------------------------
void ExportDialog::OnStart(wxCommandEvent&)
{
    const wxString path = pathPicker_->GetPath().Trim().Trim(false);
    if (path.IsEmpty()) {
        wxMessageBox(tr(L"请先选择导出文件。"), tr(L"导出数据"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!overwrite_->IsChecked() && wxFileName::FileExists(path)) {
        if (wxMessageBox(tr(L"文件已存在，是否覆盖？"), tr(L"导出数据"),
                         wxYES_NO | wxICON_QUESTION, this) != wxYES)
            return;
    }

    const Scope scope = static_cast<Scope>(scopeBox_->GetSelection());
    const db::ExportFormat fmt = FormatOf(formatChoice_->GetSelection());

    db::ExportOptions opt;
    opt.format    = fmt;
    opt.dialect   = req_.dialect;
    opt.header    = header_->IsChecked();
    opt.delimiter = DelimOf(delimChoice_->GetSelection());
    opt.tableName = req_.table.IsEmpty() ? wxString(L"table_name") : req_.table;

    const bool bom = (encoding_->GetSelection() == 1);

    book_->SetSelection(1);
    Layout();
    running_ = true;
    stopFlag_ = false;
    primaryBtn_->SetLabel(tr(L"停止"));

    db::IConnection* conn = conn_;
    std::vector<wxString> columns = req_.columns;
    // snapshot in-memory sources for the worker
    std::vector<std::vector<wxString>> memRows =
        (scope == Scope::Selected) ? req_.selectedRows : req_.currentPage.rows;
    const wxString qual = QualifiedTable(), where = req_.where, orderBy = req_.orderBy;
    const bool wholeSql = (scope == Scope::WholeTable && fmt == db::ExportFormat::Sql);
    const bool isBinary = IsBinaryFormat(fmt);
    const wxString db = req_.db, table = req_.table;

    JoinWorker();
    worker_ = core::CrashLog::GuardedThread(L"数据导出", [this, conn, path, opt, bom, scope, columns, memRows,
                           qual, where, orderBy, wholeSql, isBinary, fmt, db, table]() {
        wxStopWatch sw;

        // ---- binary formats (Excel / DBF): raw bytes, never through a text sink ----
        if (isBinary) {
            RunBinaryExport(conn, path, fmt, opt, scope, columns, memRows,
                            qual, where, orderBy, sw);
            return;
        }

        wxFile f(path, wxFile::write);
        if (!f.IsOpened()) {
            CallAfter([this, path]() {
                Log(tr(L"无法写入文件：") + path);
                Finish(false, /*writeErr*/ true, 0, 0);
            });
            return;
        }
        if (bom) f.Write("\xEF\xBB\xBF", 3);   // UTF-8 BOM

        // 256 KB buffered sink → the whole table never lands in memory.
        wxString buf;
        bool writeErr = false;
        auto flush = [&](bool force) {
            if (writeErr || buf.IsEmpty()) return;
            if (force || buf.length() >= (1u << 18)) {
                if (!f.Write(buf, wxConvUTF8)) writeErr = true;
                buf.clear();
            }
        };
        auto sink = [&](const wxString& s) { buf += s; flush(false); };

        wxString err;
        long long written = 0;
        bool cancelled = false;

        if (wholeSql) {
            // 整表 + SQL: the driver's streaming dump already emits INSERTs.
            auto emit = [&](const wxString& s) {
                sink(s);
                if (++written % 4096 == 0) {
                    const long long w = written;
                    CallAfter([this, w]() { SetRows(w); });
                }
                if (stopFlag_.load()) { /* driver checks Cancel() */ }
            };
            if (!conn->DumpTableData(db, table, emit, err)) {
                const wxString e = err;
                CallAfter([this, e]() { Log(tr(L"导出数据失败：") + e); });
            }
        } else if (scope == Scope::WholeTable) {
            // Row count → the gauge's denominator (same query the pager uses).
            {
                wxString csql = L"SELECT COUNT(*) FROM " + qual;
                if (!where.IsEmpty()) csql += L" WHERE " + where;
                db::QueryResult cr; wxString ce; long long n = -1;
                if (conn->Execute(csql, cr, ce) && !cr.rows.empty() && !cr.rows[0].empty())
                    cr.rows[0][0].ToLongLong(&n);
                total_.store(n);
            }
            // 整表 streaming: pull 5000-row chunks; only one block in memory at a time.
            db::TableWriter writer(opt, columns, sink);
            const int CH = 5000;
            long long offset = 0;
            while (!stopFlag_.load()) {
                wxString sql = L"SELECT * FROM " + qual;
                if (!where.IsEmpty())   sql += L" WHERE " + where;
                if (!orderBy.IsEmpty()) sql += L" ORDER BY " + orderBy;
                sql += db::LimitOffsetClause(req_.dialect, CH, offset);
                db::QueryResult r;
                if (!conn->Execute(sql, r, err)) {
                    const wxString e = err;
                    CallAfter([this, e]() { Log(tr(L"读取数据失败：") + e); });
                    break;
                }
                if (r.rows.empty()) break;
                writer.WriteRows(r.rows);
                written += static_cast<long long>(r.rows.size());
                offset  += static_cast<long long>(r.rows.size());
                const long long w = written;
                CallAfter([this, w]() { SetRows(w); });
                if (writeErr || static_cast<int>(r.rows.size()) < CH) break;
            }              // r released each iteration → flat memory
            if (stopFlag_.load()) cancelled = true;
            writer.Finish();
        } else {
            // 当前页 / 选中记录: bounded in-memory rows.
            total_.store(static_cast<long long>(memRows.size()));
            db::TableWriter writer(opt, columns, sink);
            writer.WriteRows(memRows);
            writer.Finish();
            written = static_cast<long long>(memRows.size());
            const long long w = written;
            CallAfter([this, w]() { SetRows(w); });
        }

        flush(/*force*/ true);
        const long ms = sw.Time();
        CallAfter([this, cancelled, writeErr, written, ms]() {
            Finish(cancelled, writeErr, written, ms);
        });
    });
}

// ---------------------------------------------------------------------------
void ExportDialog::RunBinaryExport(
    db::IConnection* conn, const wxString& path, db::ExportFormat fmt,
    const db::ExportOptions& opt, Scope scope,
    const std::vector<wxString>& columns,
    const std::vector<std::vector<wxString>>& memRows,
    const wxString& qual, const wxString& where, const wxString& orderBy,
    wxStopWatch& sw)
{
    wxFileOutputStream fout(path);
    if (!fout.IsOk()) {
        CallAfter([this, path]() {
            Log(tr(L"无法写入文件：") + path);
            Finish(false, /*writeErr*/ true, 0, 0);
        });
        return;
    }

    std::unique_ptr<db::IBinaryTableWriter> writer;
    if (fmt == db::ExportFormat::Xlsx)
        writer.reset(new db::XlsxWriter(columns, fout, opt.header, opt.nullText));
    else
        writer.reset(new db::DbfWriter(columns, fout, opt.nullText));

    long long written = 0;
    bool cancelled = false;

    auto consume = [&](const std::vector<std::vector<wxString>>& rows) {
        writer->WriteRows(rows);
        written += static_cast<long long>(rows.size());
        const long long w = written;
        CallAfter([this, w]() { SetRows(w); });
    };

    if (scope == Scope::WholeTable) {
        // Row count → the gauge's denominator (same query the pager uses).
        {
            wxString csql = L"SELECT COUNT(*) FROM " + qual;
            if (!where.IsEmpty()) csql += L" WHERE " + where;
            db::QueryResult cr; wxString ce; long long n = -1;
            if (conn->Execute(csql, cr, ce) && !cr.rows.empty() && !cr.rows[0].empty())
                cr.rows[0][0].ToLongLong(&n);
            total_.store(n);
        }
        // Stream 5000-row chunks; only one block is ever in memory.
        const int CH = 5000;
        long long offset = 0;
        wxString err;
        while (!stopFlag_.load()) {
            wxString sql = L"SELECT * FROM " + qual;
            if (!where.IsEmpty())   sql += L" WHERE " + where;
            if (!orderBy.IsEmpty()) sql += L" ORDER BY " + orderBy;
            sql += db::LimitOffsetClause(req_.dialect, CH, offset);
            db::QueryResult r;
            if (!conn->Execute(sql, r, err)) {
                const wxString e = err;
                CallAfter([this, e]() { Log(tr(L"读取数据失败：") + e); });
                break;
            }
            if (r.rows.empty()) break;
            consume(r.rows);
            offset += static_cast<long long>(r.rows.size());
            if (!writer->Ok() || static_cast<int>(r.rows.size()) < CH) break;
        }                        // r released each iteration → flat memory
        if (stopFlag_.load()) cancelled = true;
    } else {
        // 当前页 / 选中记录: bounded in-memory rows.
        total_.store(static_cast<long long>(memRows.size()));
        consume(memRows);
    }

    writer->Finish();
    const bool writeErr = !writer->Ok();
    writer.reset();            // close the ZIP / patch the DBF header before fout dies
    const long ms = sw.Time();
    CallAfter([this, cancelled, writeErr, written, ms]() {
        Finish(cancelled, writeErr, written, ms);
    });
}

// ---------------------------------------------------------------------------
void ExportDialog::SetRows(long long rows)
{
    rowTxt_->SetLabel(wxString::Format(tr(L"已写入 %lld 行"), rows));
    const long long t = total_.load();
    if (t > 0) {
        gauge_->SetRange(100);
        gauge_->SetValue(static_cast<int>(std::min<long long>(100, rows * 100 / t)));
    }
}

void ExportDialog::Log(const wxString& line) { log_->AppendText(line + L"\n"); }

void ExportDialog::Finish(bool cancelled, bool writeErr, long long rows, long elapsedMs)
{
    running_ = false;
    gauge_->SetValue(gauge_->GetRange());
    if (writeErr) {
        runTitle_->SetLabel(tr(L"导出失败"));
        runTitle_->SetForegroundColour(theme::kDotRed);
    } else if (cancelled) {
        runTitle_->SetLabel(tr(L"已停止（文件不完整）"));
        runTitle_->SetForegroundColour(theme::kDotAmber);
    } else {
        runTitle_->SetLabel(tr(L"导出完成"));
        runTitle_->SetForegroundColour(theme::kGreen);
    }
    SetRows(rows);
    Log(wxString::Format(tr(L"共 %lld 行 · 用时 %ld ms"), rows, elapsedMs));
    primaryBtn_->SetLabel(tr(L"关闭"));
    primaryBtn_->Enable(true);
    Layout();
}

// ---------------------------------------------------------------------------
void ExportDialog::OnStopOrClose(wxCommandEvent&)
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

void ExportDialog::OnCloseWindow(wxCloseEvent& ev)
{
    if (running_.load()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        ev.Veto();
        return;
    }
    JoinWorker();
    EndModal(wxID_OK);
}

void ExportDialog::JoinWorker()
{
    if (worker_.joinable()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        worker_.join();
    }
}

} // namespace ui
