// ImportDialog.cpp — see header. Config page collects file/encoding/target/error
// strategy; the run page executes parsed-row INSERTs off a worker thread. A .sql
// file is handed to RunScriptDialog instead of parsed here.
#include "ui/ImportDialog.h"
#include "core/CrashLog.h"

#include <wx/wx.h>
#include <wx/filepicker.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
#include <wx/simplebook.h>
#include <wx/statline.h>
#include <wx/clipbrd.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stopwatch.h>

#include <algorithm>

#include "db/TableImport.h"
#include "db/SyncTypes.h"     // db::Cell / db::RenderLiteral
#include "ui/I18n.h"
#include "ui/RunScriptDialog.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

enum Enc { Enc_Auto = 0, Enc_Utf8, Enc_Gbk, Enc_Big5, Enc_Utf16LE, Enc_Latin1 };

wxString DelimOf(int idx)
{
    switch (idx) { case 1: return L","; case 2: return L";"; case 3: return L"|";
                   default: return L"\t"; }
}

wxString Snippet(const wxString& s)
{
    wxString t = s;
    t.Replace(L"\r", L" "); t.Replace(L"\n", L" "); t.Replace(L"\t", L" ");
    while (t.Replace(L"  ", L" ")) {}
    t.Trim(true).Trim(false);
    if (t.length() > 160) t = t.Left(160) + L"…";
    return t;
}

} // namespace

ImportDialog::ImportDialog(wxWindow* parent, db::IConnection* conn,
                           const wxString& targetDb, const wxString& targetTable)
    : CenteredDialog(parent, wxID_ANY, tr(L"导入数据"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , conn_(conn)
    , targetDb_(targetDb)
    , targetTable_(targetTable)
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->AddPage(BuildConfigPage(), tr(L"配置"));
    book_->AddPage(BuildRunPage(),    tr(L"导入"));
    book_->SetSelection(0);
    root->Add(book_, 1, wxEXPAND);
    SetSizerAndFit(root);
    SetSize(FromDIP(wxSize(620, 520)));

    Bind(wxEVT_CLOSE_WINDOW, &ImportDialog::OnCloseWindow, this);
}

ImportDialog::~ImportDialog() { JoinWorker(); }

// ---------------------------------------------------------------------------
wxWindow* ImportDialog::BuildConfigPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(page, wxID_ANY, tr(L"导入数据"));
    title->SetFont(Ui(13, /*bold*/ true));
    title->SetForegroundColour(theme::kTextStrong);
    s->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    auto* sub = new wxStaticText(page, wxID_ANY,
        tr(L"支持 CSV / TXT / JSON / XML；.sql 文件将转交“执行 SQL 脚本”。"));
    sub->SetFont(Ui(9));
    sub->SetForegroundColour(theme::kTextSecondary);
    s->Add(sub, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

    auto* grid = new wxFlexGridSizer(2, wxSize(12, 12));
    grid->AddGrowableCol(1, 1);
    auto label = [&](const wxString& t) {
        auto* l = new wxStaticText(page, wxID_ANY, t);
        l->SetForegroundColour(theme::kTextBody);
        return l;
    };

    filePicker_ = new wxFilePickerCtrl(
        page, wxID_ANY, wxEmptyString, tr(L"选择数据文件"),
        tr(L"数据文件 (*.csv;*.txt;*.json;*.xml;*.sql)|*.csv;*.txt;*.json;*.xml;*.sql|"
           L"所有文件 (*.*)|*.*"),
        wxDefaultPosition, wxDefaultSize,
        wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
    grid->Add(label(tr(L"数据文件")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(filePicker_, 1, wxEXPAND);

    encoding_ = new wxChoice(page, wxID_ANY);
    encoding_->Append(tr(L"自动检测"));
    encoding_->Append(L"UTF-8");
    encoding_->Append(tr(L"GBK / GB18030 (简体中文)"));
    encoding_->Append(tr(L"Big5 (繁体中文)"));
    encoding_->Append(L"UTF-16 LE");
    encoding_->Append(L"Latin-1 (ISO-8859-1)");
    encoding_->SetSelection(Enc_Auto);
    grid->Add(label(tr(L"文件编码")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(encoding_, 0, wxALIGN_LEFT);

    delimChoice_ = new wxChoice(page, wxID_ANY);
    delimChoice_->Append(L"Tab");
    delimChoice_->Append(tr(L"逗号 ,"));
    delimChoice_->Append(tr(L"分号 ;"));
    delimChoice_->Append(tr(L"竖线 |"));
    delimChoice_->SetSelection(0);
    grid->Add(label(tr(L"TXT 分隔符")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(delimChoice_, 0, wxALIGN_LEFT);

    tableCtrl_ = new wxTextCtrl(page, wxID_ANY, targetTable_);
    grid->Add(label(tr(L"目标表")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(tableCtrl_, 1, wxEXPAND);

    s->Add(grid, 0, wxEXPAND | wxALL, 16);

    hasHeader_ = new wxCheckBox(page, wxID_ANY, tr(L"首行是列名（CSV/TXT）"));
    hasHeader_->SetValue(true);
    s->Add(hasHeader_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

    ignoreError_ = new wxCheckBox(page, wxID_ANY, tr(L"遇到错误继续（逐行提交，记录失败）"));
    useTxn_ = new wxCheckBox(page, wxID_ANY, tr(L"在事务中导入（全部成功才提交，出错回滚）"));
    useTxn_->SetValue(true);
    ignoreError_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (ignoreError_->IsChecked()) useTxn_->SetValue(false);
        useTxn_->Enable(!ignoreError_->IsChecked());
    });
    useTxn_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (useTxn_->IsChecked()) ignoreError_->SetValue(false);
        ignoreError_->Enable(!useTxn_->IsChecked());
    });
    s->Add(ignoreError_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
    s->Add(useTxn_,      0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

    s->AddStretchSpacer(1);
    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* cancel = new wxButton(page, wxID_ANY, tr(L"关闭"));
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    btns->Add(cancel, 0, wxRIGHT, 10);
    startBtn_ = new wxButton(page, wxID_ANY, tr(L"开始导入"));
    startBtn_->SetBackgroundColour(theme::kPrimary);
    startBtn_->SetForegroundColour(theme::kWhite);
    startBtn_->Enable(false);
    startBtn_->Bind(wxEVT_BUTTON, &ImportDialog::OnStart, this);
    btns->Add(startBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    filePicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
        startBtn_->Enable(!filePicker_->GetPath().Trim().IsEmpty());
    });

    page->SetSizer(s);
    return page;
}

wxWindow* ImportDialog::BuildRunPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    runTitle_ = new wxStaticText(page, wxID_ANY, tr(L"正在导入…"));
    runTitle_->SetFont(Ui(13, /*bold*/ true));
    runTitle_->SetForegroundColour(theme::kTextStrong);
    s->Add(runTitle_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    gauge_ = new wxGauge(page, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 10),
                         wxGA_HORIZONTAL | wxGA_SMOOTH);
    s->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    progressTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    progressTxt_->SetFont(Ui(9));
    progressTxt_->SetForegroundColour(theme::kTextSecondary);
    s->Add(progressTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    counterTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    counterTxt_->SetFont(Ui(10, /*bold*/ true));
    s->Add(counterTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    auto* failHdr = new wxStaticText(page, wxID_ANY, tr(L"失败明细"));
    failHdr->SetFont(Ui(9, /*bold*/ true));
    failHdr->SetForegroundColour(theme::kTextBody);
    s->Add(failHdr, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    failList_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    failList_->AppendColumn(tr(L"行"),      wxLIST_FORMAT_RIGHT, FromDIP(56));
    failList_->AppendColumn(tr(L"错误信息"), wxLIST_FORMAT_LEFT, FromDIP(240));
    failList_->AppendColumn(tr(L"原始行"),   wxLIST_FORMAT_LEFT, FromDIP(240));
    s->Add(failList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    copyBtn_ = new wxButton(page, wxID_ANY, tr(L"复制错误"));
    copyBtn_->Bind(wxEVT_BUTTON, &ImportDialog::OnCopyErrors, this);
    exportBtn_ = new wxButton(page, wxID_ANY, tr(L"导出错误…"));
    exportBtn_->Bind(wxEVT_BUTTON, &ImportDialog::OnExportErrors, this);
    actions->Add(copyBtn_, 0, wxRIGHT, 8);
    actions->Add(exportBtn_, 0);
    actions->AddStretchSpacer(1);
    s->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);
    copyBtn_->Hide(); exportBtn_->Hide();

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    backBtn_ = new wxButton(page, wxID_ANY, tr(L"返回"));
    backBtn_->Bind(wxEVT_BUTTON, &ImportDialog::OnBack, this);
    backBtn_->Enable(false);
    btns->Add(backBtn_, 0);
    btns->AddStretchSpacer(1);
    primaryBtn_ = new wxButton(page, wxID_ANY, tr(L"停止"));
    primaryBtn_->Bind(wxEVT_BUTTON, &ImportDialog::OnStopOrClose, this);
    btns->Add(primaryBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    page->SetSizer(s);
    return page;
}

// ---------------------------------------------------------------------------
wxString ImportDialog::QualifiedTable() const
{
    const db::Dialect d = conn_ ? conn_->GetDialect() : db::Dialect::MySQL;
    const wxString table = tableCtrl_->GetValue().Trim().Trim(false);
    if (d == db::Dialect::MySQL && !targetDb_.IsEmpty())
        return db::QuoteIdent(targetDb_, d) + L"." + db::QuoteIdent(table, d);
    return db::QuoteIdent(table, d);
}

void ImportDialog::OnStart(wxCommandEvent&)
{
    const wxString path = filePicker_->GetPath().Trim().Trim(false);
    if (path.IsEmpty() || !conn_) return;

    const wxString table = tableCtrl_->GetValue().Trim().Trim(false);
    if (table.IsEmpty()) {
        wxMessageBox(tr(L"请填写目标表名。"), tr(L"导入数据"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }

    // .sql → hand off to the script runner (a dump is a script, not a table file).
    if (wxFileName(path).GetExt().Lower() == L"sql") {
        RunScriptDialog dlg(this, conn_, wxEmptyString, targetDb_, /*aiConfigured*/ false, path);
        dlg.ShowModal();
        if (dlg.DidRun()) didImport_ = true;
        Close();
        return;
    }

    const db::ImportFormat fmt = db::DetectFormatByExtension(path);
    if (fmt == db::ImportFormat::Unknown) {
        wxMessageBox(tr(L"无法识别的文件格式（支持 CSV / TXT / JSON / XML / SQL）。"),
                     tr(L"导入数据"), wxOK | wxICON_ERROR, this);
        return;
    }

    wxString err;
    wxString content = ReadFile(path, err);
    if (!err.IsEmpty()) {
        wxMessageBox(err + L"\n\n" + path, tr(L"导入数据"), wxOK | wxICON_ERROR, this);
        return;
    }

    db::ImportOptions opt;
    opt.format    = fmt;
    opt.delimiter = DelimOf(delimChoice_->GetSelection());
    opt.hasHeader = hasHeader_->IsChecked();
    opt.nullText  = L"NULL";

    // reset run page
    failures_.clear();
    failList_->DeleteAllItems();
    copyBtn_->Hide(); exportBtn_->Hide();
    gauge_->SetValue(0);
    runTitle_->SetLabel(tr(L"正在解析…"));
    runTitle_->SetForegroundColour(theme::kTextStrong);
    progressTxt_->SetLabel(wxEmptyString);
    counterTxt_->SetLabel(wxEmptyString);
    backBtn_->Enable(false);
    primaryBtn_->SetLabel(tr(L"停止"));
    book_->SetSelection(1);
    Layout();

    didImport_ = true;
    running_   = true;
    stopFlag_  = false;
    const wxString qtable = QualifiedTable();
    const db::Dialect dialect = conn_->GetDialect();
    const bool ignoreErr = ignoreError_->IsChecked();
    const bool useTxn    = useTxn_->IsChecked();

    JoinWorker();
    worker_ = core::CrashLog::GuardedThread(L"数据导入", [this, content = std::move(content), opt, qtable, dialect,
                           ignoreErr, useTxn]() mutable {
        wxStopWatch sw;
        db::IConnection* c = conn_;

        // 1) parse into memory (rows + column names), collecting recoverable errors.
        std::vector<std::vector<wxString>> rows;
        std::vector<wxString> columns;
        auto rowSink = [&](const std::vector<wxString>& r) -> bool {
            rows.push_back(r);
            return !stopFlag_.load();
        };
        auto errSink = [&](const db::ImportError& e) {
            Failure f{ e.lineNo, Snippet(e.raw), e.message };
            CallAfter([this, f]() { AppendFailure(f); });
        };
        db::ImportResult pr = db::ImportTable(content, opt, rowSink, errSink);
        columns = pr.columns;
        wxString().swap(content);   // file text parsed into rows; free it now
        if (!pr.ok) {
            const wxString fatal = pr.fatal;
            CallAfter([this, fatal]() {
                Failure f{ 0, wxEmptyString, fatal };   // surface the fatal reason
                AppendFailure(f);
                Finish(false, 0, 0);
                runTitle_->SetLabel(tr(L"解析失败"));
                runTitle_->SetForegroundColour(theme::kDotRed);
            });
            return;
        }

        // 2) build + run INSERTs. To avoid one network round-trip per row (the P0
        // perf fix) we batch rows into a single multi-row VALUES statement. On a
        // batch failure we fall back to per-row execution for that batch so the
        // exact offending row is still located and reported (AppendFailure); the
        // ignoreErr / stopFlag_ / progress-tick semantics stay identical to the
        // original per-row loop.
        wxString colList;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i) colList += L", ";
            colList += db::QuoteIdent(columns[i], dialect);
        }
        auto lit = [&](const wxString& v) -> wxString {
            db::Cell cell;
            if (v == opt.nullText) cell.kind = db::CellKind::Null;
            else { cell.kind = db::CellKind::Text; cell.text = v; }
            return db::RenderLiteral(cell, dialect);
        };
        // Comma-joined literals for one row (no surrounding parentheses).
        auto rowVals = [&](const std::vector<wxString>& row) -> wxString {
            wxString vals;
            for (size_t ci = 0; ci < row.size(); ++ci) {
                if (ci) vals += L", ";
                vals += lit(row[ci]);
            }
            return vals;
        };

        wxString insertHead = L"INSERT INTO " + qtable;
        if (!colList.IsEmpty()) insertHead += L" (" + colList + L")";
        insertHead += L" VALUES ";

        // Multi-row VALUES is supported by MySQL / Postgres / SQLite / SQL Server;
        // Oracle has no such syntax (every batch would just error into fallback),
        // so it stays on the per-row path. Keep batches <= 1000 rows to respect
        // SQL Server's row-constructor limit, with a char cap for very wide rows.
        const bool   multiRow    = (dialect != db::Dialect::Oracle);
        const size_t kBatchRows  = 500;
        const size_t kBatchChars = 1u << 20;   // ~1M chars of SQL text per statement

        // A failed statement poisons the connection until rollback on some engines
        // (notably Postgres) inside a transaction, which would wreck per-row
        // pinpointing during fallback. Guard each attempt with a savepoint so we
        // can roll the failure back and keep probing. SQL Server uses different
        // savepoint syntax and does not poison on statement error, so skip it.
        const bool     useSavepoint = useTxn && dialect != db::Dialect::SqlServer;
        const wxString kSp = L"swiftsql_import_sp";
        auto savepoint = [&]() {
            if (!useSavepoint) return;
            db::QueryResult r; wxString e; c->Execute(L"SAVEPOINT " + kSp, r, e);
        };
        auto rollbackToSavepoint = [&]() {
            if (!useSavepoint) return;
            db::QueryResult r; wxString e;
            c->Execute(L"ROLLBACK TO SAVEPOINT " + kSp, r, e);
        };
        auto releaseSavepoint = [&]() {
            if (!useSavepoint) return;
            db::QueryResult r; wxString e; c->Execute(L"RELEASE SAVEPOINT " + kSp, r, e);
        };

        wxString err;
        if (useTxn) { db::QueryResult r; c->Execute(L"BEGIN", r, err); }

        long long ok = 0, fail = 0, done = 0;
        bool aborted = false;

        auto tick = [&]() {
            const long long d = done, o = ok, fl = fail;
            CallAfter([this, d, o, fl]() { Tick(d, o, fl); });
        };

        // Single-row INSERT with per-row bookkeeping. `lineNo` is the 1-based row
        // index used for the failure list. Returns false on failure.
        auto runOne = [&](const std::vector<wxString>& row, long long lineNo) -> bool {
            const wxString vals = rowVals(row);
            savepoint();
            db::QueryResult r; wxString e;
            if (c->Execute(insertHead + L"(" + vals + L")", r, e)) { ++ok; return true; }
            ++fail;
            rollbackToSavepoint();   // undo the failed statement, un-poison the txn
            Failure f{ lineNo, Snippet(vals), e };
            CallAfter([this, f]() { AppendFailure(f); });
            return false;
        };

        // Per-row fallback over rows[start, end). Honours stopFlag_, ignoreErr and
        // the 200-row progress cadence exactly like the original loop.
        auto runPerRow = [&](size_t start, size_t end) {
            for (size_t i = start; i < end; ++i) {
                if (stopFlag_.load()) { aborted = true; break; }
                const bool okRow = runOne(rows[i], static_cast<long long>(i) + 1);
                ++done;
                if (!okRow && !ignoreErr) { aborted = true; break; }
                if (done % 200 == 0) tick();
            }
        };

        size_t pos = 0;
        while (pos < rows.size() && !aborted) {
            if (stopFlag_.load()) { aborted = true; break; }

            if (!multiRow) {                       // Oracle: no multi-row VALUES
                const size_t end = std::min(rows.size(), pos + kBatchRows);
                runPerRow(pos, end);
                if (!aborted) tick();
                pos = end;
                continue;
            }

            // Assemble one multi-row INSERT, bounded by row count and text size.
            size_t end = pos, nRows = 0;
            wxString sql = insertHead;
            while (end < rows.size() && nRows < kBatchRows) {
                if (nRows) sql += L", ";
                sql += L"(" + rowVals(rows[end]) + L")";
                ++end; ++nRows;
                if (sql.length() >= kBatchChars) break;
            }

            savepoint();
            db::QueryResult r; wxString e;
            if (c->Execute(sql, r, e)) {
                releaseSavepoint();
                ok   += static_cast<long long>(nRows);
                done += static_cast<long long>(nRows);
                tick();
            } else {
                rollbackToSavepoint();             // discard the failed batch...
                runPerRow(pos, end);               // ...then locate the bad row(s)
                if (!aborted) tick();
            }
            pos = end;
        }

        if (useTxn) {
            db::QueryResult r; wxString e;
            const bool commit = (fail == 0 && !aborted);
            c->Execute(commit ? wxString(L"COMMIT") : wxString(L"ROLLBACK"), r, e);
        }

        const bool cancelled = aborted && stopFlag_.load();
        const long ms = sw.Time();
        const long long okC = ok, fl = fail, dn = done;
        CallAfter([this, cancelled, okC, fl, dn, ms]() {
            Tick(dn, okC, fl);
            Finish(cancelled, okC, ms);
        });
    });
}

// ---------------------------------------------------------------------------
void ImportDialog::AppendFailure(const Failure& f)
{
    failures_.push_back(f);
    const long row = failList_->InsertItem(failList_->GetItemCount(),
                                           wxString::Format(L"%lld", f.lineNo));
    failList_->SetItem(row, 1, f.error);
    failList_->SetItem(row, 2, f.raw);
    failList_->EnsureVisible(row);
    if (!copyBtn_->IsShown()) { copyBtn_->Show(); exportBtn_->Show(); Layout(); }
}

void ImportDialog::Tick(long long done, long long ok, long long fail)
{
    if (total_ <= 0) total_ = done;   // first tick establishes a soft range
    if (total_ > 0) { gauge_->SetRange(100); gauge_->SetValue(
        static_cast<int>(done * 100 / std::max<long long>(done, total_))); }
    runTitle_->SetLabel(tr(L"正在导入…"));
    progressTxt_->SetLabel(wxString::Format(tr(L"已处理 %lld 行"), done));
    counterTxt_->SetLabel(wxString::Format(tr(L"成功 %lld    失败 %lld"), ok, fail));
    counterTxt_->SetForegroundColour(fail > 0 ? theme::kDotRed : theme::kGreen);
}

void ImportDialog::Finish(bool cancelled, long long ok, long elapsedMs)
{
    running_ = false;
    const long long fail = static_cast<long long>(failures_.size());
    gauge_->SetRange(100); gauge_->SetValue(100);
    if (cancelled) {
        runTitle_->SetLabel(tr(L"已停止"));
        runTitle_->SetForegroundColour(theme::kDotAmber);
    } else if (fail == 0) {
        runTitle_->SetLabel(tr(L"导入完成"));
        runTitle_->SetForegroundColour(theme::kGreen);
    } else {
        runTitle_->SetLabel(tr(L"导入完成（有失败）"));
        runTitle_->SetForegroundColour(theme::kDotRed);
    }
    progressTxt_->SetLabel(wxString::Format(
        tr(L"成功 %lld · 失败 %lld · 用时 %ld ms"), ok, fail, elapsedMs));
    backBtn_->Enable(true);
    primaryBtn_->SetLabel(tr(L"关闭"));
    Layout();
}

// ---------------------------------------------------------------------------
void ImportDialog::OnStopOrClose(wxCommandEvent&)
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

void ImportDialog::OnBack(wxCommandEvent&)
{
    if (running_.load()) return;
    book_->SetSelection(0);
}

void ImportDialog::OnCopyErrors(wxCommandEvent&)
{
    if (failures_.empty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(AllFailuresText()));
        wxTheClipboard->Close();
    }
}

void ImportDialog::OnExportErrors(wxCommandEvent&)
{
    if (failures_.empty()) return;
    wxFileDialog fd(this, tr(L"导出错误"), wxEmptyString, L"import_errors.txt",
                    tr(L"文本文件 (*.txt)|*.txt|所有文件 (*.*)|*.*"),
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK) return;
    wxFile f(fd.GetPath(), wxFile::write);
    if (!f.IsOpened() || !f.Write(AllFailuresText(), wxConvUTF8))
        wxMessageBox(tr(L"无法写入文件:") + L"\n\n" + fd.GetPath(),
                     tr(L"导出错误"), wxOK | wxICON_ERROR, this);
}

void ImportDialog::OnCloseWindow(wxCloseEvent& ev)
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

// ---------------------------------------------------------------------------
wxString ImportDialog::ReadFile(const wxString& path, wxString& err) const
{
    wxFile f(path, wxFile::read);
    if (!f.IsOpened()) { err = tr(L"无法打开文件"); return {}; }
    const wxFileOffset len = f.Length();
    if (len <= 0) return {};
    wxCharBuffer raw(static_cast<size_t>(len));
    if (f.Read(raw.data(), len) != len) { err = tr(L"读取文件失败"); return {}; }

    auto convert = [&](const wxMBConv& conv) {
        return wxString(raw.data(), conv, static_cast<size_t>(len));
    };
    wxString text;
    switch (encoding_->GetSelection()) {
    case Enc_Utf8:    text = convert(wxConvUTF8); break;
    case Enc_Gbk:     { wxCSConv c(wxFONTENCODING_CP936);      text = convert(c); break; }
    case Enc_Big5:    { wxCSConv c(wxFONTENCODING_CP950);      text = convert(c); break; }
    case Enc_Utf16LE: { wxMBConvUTF16LE c;                     text = convert(c); break; }
    case Enc_Latin1:  { wxCSConv c(wxFONTENCODING_ISO8859_1);  text = convert(c); break; }
    case Enc_Auto:
    default:          { wxConvAuto c(wxFONTENCODING_UTF8);     text = convert(c); break; }
    }
    if (text.IsEmpty()) {
        wxConvAuto c(wxFONTENCODING_UTF8); text = convert(c);
        if (text.IsEmpty()) text = wxString::FromUTF8(raw.data(), static_cast<size_t>(len));
    }
    return text;
}

wxString ImportDialog::AllFailuresText() const
{
    wxString out = wxString::Format(
        tr(L"# 数据导入错误报告 · 目标 %s · 失败 %zu 条\n\n"),
        targetTable_, failures_.size());
    for (const Failure& f : failures_)
        out += wxString::Format(tr(L"[行 %lld] %s\n  原始: %s\n\n"),
                                f.lineNo, f.error, f.raw);
    return out;
}

void ImportDialog::JoinWorker()
{
    if (worker_.joinable()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        worker_.join();
    }
}

} // namespace ui
