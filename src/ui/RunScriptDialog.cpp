// RunScriptDialog.cpp — see header. Config page collects file + encoding +
// error-handling; the run page shows live progress off a worker thread.
#include "ui/RunScriptDialog.h"
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

#include "db/SqlScript.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// Encoding menu order — index maps to the switch in ReadScriptFile.
enum Enc { Enc_Auto = 0, Enc_Utf8, Enc_Gbk, Enc_Big5, Enc_Utf16LE, Enc_Latin1 };

// Transaction control statements for the connection's dialect. BEGIN / COMMIT /
// ROLLBACK are portable across MySQL, PG and SQLite; SQL Server spells the start
// differently. Returned without a terminator — a single Execute autocommits.
wxString BeginSql(db::Dialect d)
{
    // Oracle/DM: no standalone BEGIN (starts a PL/SQL block); the transaction opens
    // implicitly and is closed by the trailing COMMIT/ROLLBACK. Emit nothing.
    if (d == db::Dialect::Oracle)    return wxString();
    return d == db::Dialect::SqlServer ? L"BEGIN TRANSACTION" : L"BEGIN";
}

// Trim a statement to a compact one-line snippet for the failure list.
wxString Snippet(const wxString& sql)
{
    wxString s = sql;
    s.Replace(L"\r", L" ");
    s.Replace(L"\n", L" ");
    s.Replace(L"\t", L" ");
    while (s.Replace(L"  ", L" ")) {}
    s.Trim(true).Trim(false);
    if (s.length() > 160) s = s.Left(160) + L"…";
    return s;
}

} // namespace

RunScriptDialog::RunScriptDialog(wxWindow* parent, db::IConnection* conn,
                                 const wxString& connName, const wxString& dbName,
                                 bool aiConfigured, const wxString& presetFile)
    : CenteredDialog(parent, wxID_ANY, tr(L"执行 SQL 脚本"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , conn_(conn)
    , connName_(connName)
    , dbName_(dbName)
    , aiConfigured_(aiConfigured)
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->AddPage(BuildConfigPage(), tr(L"配置"));
    book_->AddPage(BuildRunPage(),    tr(L"执行"));
    book_->SetSelection(0);
    root->Add(book_, 1, wxEXPAND);
    SetSizerAndFit(root);
    SetSize(FromDIP(wxSize(620, 520)));

    Bind(wxEVT_CLOSE_WINDOW, &RunScriptDialog::OnCloseWindow, this);

    // Preset mode (粘贴表): pre-fill the file and auto-start once the modal loop
    // is running, so the user lands straight on the live progress page.
    if (!presetFile.IsEmpty()) {
        filePicker_->SetPath(presetFile);
        startBtn_->Enable(true);
        CallAfter([this]() { wxCommandEvent e; OnStart(e); });
    }
}

RunScriptDialog::~RunScriptDialog()
{
    JoinWorker();
}

// ---------------------------------------------------------------------------
// Config page
// ---------------------------------------------------------------------------
wxWindow* RunScriptDialog::BuildConfigPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    // --- header: which connection / database this runs against ---
    auto* title = new wxStaticText(page, wxID_ANY, tr(L"执行 SQL 脚本"));
    title->SetFont(Ui(13, /*bold*/ true));
    title->SetForegroundColour(theme::kTextStrong);
    s->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    auto* sub = new wxStaticText(page, wxID_ANY,
        tr(L"连接") + L"： " + connName_ + L"        " +
        tr(L"数据库") + L"： " + (dbName_.IsEmpty() ? wxString(L"—") : dbName_));
    sub->SetFont(Ui(9));
    sub->SetForegroundColour(theme::kTextSecondary);
    s->Add(sub, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

    auto* grid = new wxFlexGridSizer(2, wxSize(12, 12));
    grid->AddGrowableCol(1, 1);
    auto label = [&](const wxString& t) {
        auto* l = new wxStaticText(page, wxID_ANY, t);
        l->SetForegroundColour(theme::kTextBody);
        return l;
    };

    // --- file ---
    filePicker_ = new wxFilePickerCtrl(
        page, wxID_ANY, wxEmptyString, tr(L"选择 SQL 脚本文件"),
        tr(L"SQL 文件 (*.sql)|*.sql|所有文件 (*.*)|*.*"),
        wxDefaultPosition, wxDefaultSize,
        wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
    grid->Add(label(tr(L"脚本文件")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(filePicker_, 1, wxEXPAND);

    // --- encoding ---
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

    s->Add(grid, 0, wxEXPAND | wxALL, 16);

    // --- error handling ---
    ignoreError_ = new wxCheckBox(page, wxID_ANY, tr(L"遇到错误继续执行（记录失败，不中断）"));
    useTxn_ = new wxCheckBox(page, wxID_ANY, tr(L"在事务中执行（全部成功才提交，出错回滚）"));
    // The two are mutually exclusive: you cannot both keep going past errors and
    // roll the whole thing back on the first one.
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

    // --- buttons ---
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* cancel = new wxButton(page, wxID_ANY, tr(L"关闭"));
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    btns->Add(cancel, 0, wxRIGHT, 10);

    startBtn_ = new wxButton(page, wxID_ANY, tr(L"开始"));
    startBtn_->SetBackgroundColour(theme::kPrimary);
    startBtn_->SetForegroundColour(theme::kWhite);
    startBtn_->Enable(false);   // until a file is chosen
    startBtn_->Bind(wxEVT_BUTTON, &RunScriptDialog::OnStart, this);
    btns->Add(startBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    filePicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
        startBtn_->Enable(!filePicker_->GetPath().Trim().IsEmpty());
    });

    page->SetSizer(s);
    return page;
}

// ---------------------------------------------------------------------------
// Run page
// ---------------------------------------------------------------------------
wxWindow* RunScriptDialog::BuildRunPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    runTitle_ = new wxStaticText(page, wxID_ANY, tr(L"正在执行…"));
    runTitle_->SetFont(Ui(13, /*bold*/ true));
    runTitle_->SetForegroundColour(theme::kTextStrong);
    s->Add(runTitle_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    gauge_ = new wxGauge(page, wxID_ANY, 100, wxDefaultPosition,
                         wxSize(-1, 10), wxGA_HORIZONTAL | wxGA_SMOOTH);
    s->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    progressTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    progressTxt_->SetFont(Ui(9));
    progressTxt_->SetForegroundColour(theme::kTextSecondary);
    s->Add(progressTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    counterTxt_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    counterTxt_->SetFont(Ui(10, /*bold*/ true));
    s->Add(counterTxt_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    // --- failure list (populated only on errors) ---
    auto* failHdr = new wxStaticText(page, wxID_ANY, tr(L"失败明细"));
    failHdr->SetFont(Ui(9, /*bold*/ true));
    failHdr->SetForegroundColour(theme::kTextBody);
    s->Add(failHdr, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    failList_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    failList_->AppendColumn(L"#", wxLIST_FORMAT_RIGHT, FromDIP(44));
    failList_->AppendColumn(tr(L"错误信息"), wxLIST_FORMAT_LEFT, FromDIP(260));
    failList_->AppendColumn(tr(L"语句"),     wxLIST_FORMAT_LEFT, FromDIP(240));
    s->Add(failList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    // --- error actions + AI diagnose (hidden until there are failures) ---
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    copyBtn_ = new wxButton(page, wxID_ANY, tr(L"复制错误"));
    copyBtn_->Bind(wxEVT_BUTTON, &RunScriptDialog::OnCopyErrors, this);
    exportBtn_ = new wxButton(page, wxID_ANY, tr(L"导出错误…"));
    exportBtn_->Bind(wxEVT_BUTTON, &RunScriptDialog::OnExportErrors, this);
    aiBtn_ = new wxButton(page, wxID_ANY, tr(L"AI 诊断"));
    aiBtn_->SetBackgroundColour(theme::kAccent);
    aiBtn_->SetForegroundColour(theme::kWhite);
    aiBtn_->Bind(wxEVT_BUTTON, &RunScriptDialog::OnAiDiagnose, this);
    actions->Add(copyBtn_, 0, wxRIGHT, 8);
    actions->Add(exportBtn_, 0, wxRIGHT, 8);
    actions->Add(aiBtn_, 0);
    actions->AddStretchSpacer(1);
    s->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);
    copyBtn_->Hide();
    exportBtn_->Hide();
    aiBtn_->Hide();

    // --- AI diagnose placeholder (front-end only for now) ---
    aiPanel_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(-1, FromDIP(96)),
                              wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
    aiPanel_->SetFont(Ui(9));
    s->Add(aiPanel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);
    aiPanel_->Hide();

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    // --- bottom buttons ---
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    backBtn_ = new wxButton(page, wxID_ANY, tr(L"返回"));
    backBtn_->Bind(wxEVT_BUTTON, &RunScriptDialog::OnBack, this);
    backBtn_->Enable(false);
    btns->Add(backBtn_, 0);
    btns->AddStretchSpacer(1);
    primaryBtn_ = new wxButton(page, wxID_ANY, tr(L"停止"));
    primaryBtn_->Bind(wxEVT_BUTTON, &RunScriptDialog::OnStopOrClose, this);
    btns->Add(primaryBtn_, 0);
    s->Add(btns, 0, wxEXPAND | wxALL, 16);

    page->SetSizer(s);
    return page;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
void RunScriptDialog::OnStart(wxCommandEvent&)
{
    const wxString path = filePicker_->GetPath().Trim().Trim(false);
    if (path.IsEmpty() || !conn_) return;

    wxString err;
    const wxString text = ReadScriptFile(path, err);
    if (!err.IsEmpty()) {
        wxMessageBox(err + L"\n\n" + path, tr(L"执行 SQL 脚本"),
                     wxOK | wxICON_ERROR, this);
        return;
    }

    std::vector<wxString> stmts =
        db::SplitSqlScript(text, conn_->GetDialect());
    if (stmts.empty()) {
        wxMessageBox(tr(L"文件中没有可执行的 SQL 语句。"), tr(L"执行 SQL 脚本"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Attach to the target database first. A SwiftSQL dump has no USE / CREATE
    // DATABASE header — tables are created with bare names (see DumpDatabase) —
    // so an unqualified CREATE TABLE lands in whatever database the session is
    // currently on, not the one whose node the user invoked 执行 on. Selecting
    // it here closes the dump→import round-trip. MySQL family only: PostgreSQL
    // connections are bound to one database (no cross-db USE) and SQLite is a
    // single file, so both already run against the right database.
    if (conn_->GetDialect() == db::Dialect::MySQL && !dbName_.IsEmpty())
        stmts.insert(stmts.begin(),
                     L"USE " + db::QuoteIdent(dbName_, db::Dialect::MySQL));

    // reset run-page state
    total_ = static_cast<int>(stmts.size());
    failures_.clear();
    failList_->DeleteAllItems();
    copyBtn_->Hide(); exportBtn_->Hide(); aiBtn_->Hide();
    aiPanel_->Hide(); aiPanel_->Clear();
    gauge_->SetRange(total_);
    gauge_->SetValue(0);
    runTitle_->SetLabel(tr(L"正在执行…"));
    runTitle_->SetForegroundColour(theme::kTextStrong);
    progressTxt_->SetLabel(wxString::Format(tr(L"共 %d 条语句"), total_));
    counterTxt_->SetLabel(wxEmptyString);
    backBtn_->Enable(false);
    primaryBtn_->SetLabel(tr(L"停止"));

    book_->SetSelection(1);
    Layout();

    didRun_  = true;
    running_ = true;
    stopFlag_ = false;
    const bool ignoreErr = ignoreError_->IsChecked();
    const bool useTxn    = useTxn_->IsChecked();
    const db::Dialect dialect = conn_->GetDialect();

    JoinWorker();
    worker_ = core::CrashLog::GuardedThread(L"脚本执行", [this, stmts, ignoreErr, useTxn, dialect]() {
        wxStopWatch sw;
        db::IConnection* c = conn_;
        int okCount = 0, failCount = 0;
        wxString err;

        if (useTxn) {
            const wxString begin = BeginSql(dialect);
            if (!begin.IsEmpty()) { db::QueryResult r; c->Execute(begin, r, err); }
        }

        bool aborted = false;
        for (int i = 0; i < static_cast<int>(stmts.size()); ++i) {
            if (stopFlag_.load()) { aborted = true; break; }

            db::QueryResult r; wxString e;
            if (c->Execute(stmts[i], r, e)) {
                ++okCount;
            } else {
                ++failCount;
                Failure f{ i + 1, Snippet(stmts[i]), e };
                CallAfter([this, f]() { AppendFailure(f); });
                if (!ignoreErr) { err = e; aborted = true; break; }
            }
            const int done = i + 1, ok = okCount, fail = failCount;
            CallAfter([this, done, ok, fail]() { Tick(done, ok, fail); });
        }

        if (useTxn) {
            db::QueryResult r; wxString e;
            const bool commit = (failCount == 0 && !aborted);
            c->Execute(commit ? wxString(L"COMMIT") : wxString(L"ROLLBACK"), r, e);
        }

        const bool cancelled = aborted && stopFlag_.load();
        const long ms = sw.Time();
        const int ok = okCount;
        CallAfter([this, cancelled, ok, ms]() { Finish(cancelled, ok, ms); });
    });
}

// ---------------------------------------------------------------------------
// Worker → UI callbacks (always on the GUI thread via CallAfter)
// ---------------------------------------------------------------------------
void RunScriptDialog::Tick(int done, int okCount, int failCount)
{
    gauge_->SetValue(done);
    progressTxt_->SetLabel(
        wxString::Format(tr(L"正在执行  %d / %d"), done, total_));
    counterTxt_->SetLabel(
        wxString::Format(tr(L"成功 %d    失败 %d"), okCount, failCount));
    counterTxt_->SetForegroundColour(failCount > 0 ? theme::kDotRed
                                                    : theme::kGreen);
}

void RunScriptDialog::AppendFailure(const Failure& f)
{
    failures_.push_back(f);
    const long row = failList_->InsertItem(failList_->GetItemCount(),
                                           wxString::Format(L"%d", f.index));
    failList_->SetItem(row, 1, f.error);
    failList_->SetItem(row, 2, f.statement);
    failList_->EnsureVisible(row);

    if (!copyBtn_->IsShown()) {
        copyBtn_->Show(); exportBtn_->Show();
        if (aiConfigured_) aiBtn_->Show();   // gated: only when AI is configured
        Layout();
    }
}

void RunScriptDialog::Finish(bool cancelled, int ok, long elapsedMs)
{
    running_ = false;
    const int fail = static_cast<int>(failures_.size());
    gauge_->SetValue(gauge_->GetRange());

    if (cancelled) {
        runTitle_->SetLabel(tr(L"已停止"));
        runTitle_->SetForegroundColour(theme::kDotAmber);
    } else if (fail == 0) {
        runTitle_->SetLabel(tr(L"执行完成"));
        runTitle_->SetForegroundColour(theme::kGreen);
    } else {
        runTitle_->SetLabel(tr(L"执行完成（有失败）"));
        runTitle_->SetForegroundColour(theme::kDotRed);
    }
    progressTxt_->SetLabel(wxString::Format(
        tr(L"共 %d 条 · 成功 %d · 失败 %d · 用时 %ld ms"),
        total_, ok, fail, elapsedMs));

    backBtn_->Enable(true);
    primaryBtn_->SetLabel(tr(L"关闭"));
    Layout();
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
void RunScriptDialog::OnStopOrClose(wxCommandEvent&)
{
    if (running_.load()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();     // interrupt an in-flight statement
        primaryBtn_->Enable(false);
        primaryBtn_->SetLabel(tr(L"正在停止…"));
    } else {
        Close();
    }
}

void RunScriptDialog::OnBack(wxCommandEvent&)
{
    if (running_.load()) return;
    book_->SetSelection(0);
}

void RunScriptDialog::OnCopyErrors(wxCommandEvent&)
{
    if (failures_.empty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(AllFailuresText()));
        wxTheClipboard->Close();
    }
}

void RunScriptDialog::OnExportErrors(wxCommandEvent&)
{
    if (failures_.empty()) return;
    wxFileDialog fd(this, tr(L"导出错误"), wxEmptyString, L"sql_errors.txt",
                    tr(L"文本文件 (*.txt)|*.txt|所有文件 (*.*)|*.*"),
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK) return;
    wxFile f(fd.GetPath(), wxFile::write);
    if (!f.IsOpened() || !f.Write(AllFailuresText(), wxConvUTF8)) {
        wxMessageBox(tr(L"无法写入文件:") + L"\n\n" + fd.GetPath(),
                     tr(L"导出错误"), wxOK | wxICON_ERROR, this);
        return;
    }
}

void RunScriptDialog::OnAiDiagnose(wxCommandEvent&)
{
    // Front-end placeholder only — no model call yet. When an AI backend lands,
    // this feeds AllFailuresText() to the diagnose endpoint and streams a reply.
    aiPanel_->Show();
    aiPanel_->SetValue(
        tr(L"AI 诊断（预览）\n"
           L"————————————————\n"
           L"已收集 ") + wxString::Format(L"%zu", failures_.size()) +
        tr(L" 条失败语句，等待 AI 分析…\n"
           L"该功能即将上线：将结合报错信息与语句上下文，给出可能原因与修复建议。"));
    Layout();
}

void RunScriptDialog::OnCloseWindow(wxCloseEvent& ev)
{
    if (running_.load()) {
        // Ask the worker to stop and let Finish() re-enable closing; veto for now
        // so we never tear the dialog down with a live worker touching it.
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        ev.Veto();
        return;
    }
    JoinWorker();
    EndModal(wxID_OK);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
wxString RunScriptDialog::ReadScriptFile(const wxString& path, wxString& err) const
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
    // BOM-aware; when there is no BOM, fall back to UTF-8 (not the system
    // codepage) — SwiftSQL's own dumps are UTF-8 without a BOM, so a GBK-locale
    // machine must not reinterpret their CJK bytes and corrupt the script.
    case Enc_Auto:
    default:          { wxConvAuto c(wxFONTENCODING_UTF8);     text = convert(c); break; }
    }
    // If the chosen codec produced nothing from non-empty bytes, fall back so the
    // user at least sees something rather than a silently empty script.
    if (text.IsEmpty()) {
        wxConvAuto c(wxFONTENCODING_UTF8); text = convert(c);
        if (text.IsEmpty()) text = wxString::FromUTF8(raw.data(), static_cast<size_t>(len));
    }
    return text;
}

wxString RunScriptDialog::AllFailuresText() const
{
    wxString out = wxString::Format(
        tr(L"# SQL 脚本执行错误报告\n# 连接：%s   数据库：%s\n# 失败 %zu 条\n\n"),
        connName_, dbName_.IsEmpty() ? wxString(L"—") : dbName_, failures_.size());
    for (const Failure& f : failures_) {
        out += wxString::Format(tr(L"[第 %d 条] %s\n  语句: %s\n\n"),
                                f.index, f.error, f.statement);
    }
    return out;
}

void RunScriptDialog::JoinWorker()
{
    if (worker_.joinable()) {
        stopFlag_ = true;
        if (conn_) conn_->Cancel();
        worker_.join();
    }
}

} // namespace ui
