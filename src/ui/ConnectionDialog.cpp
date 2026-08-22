// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/ConnectionDialog.h"

#include <wx/wx.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stopwatch.h>

#include "db/DbDriver.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

namespace ui {
namespace {
enum { ID_TEST_CONN = wxID_HIGHEST + 200 };

// A scrolled tab holding a two-column form grid.
struct Tab { wxScrolledWindow* panel; wxFlexGridSizer* grid; wxBoxSizer* vbox; };

Tab MakeTab(wxWindow* nb)
{
    auto* p = new wxScrolledWindow(nb, wxID_ANY);
    p->SetScrollRate(0, 12);
    p->SetBackgroundColour(theme::kWhite);
    auto* grid = new wxFlexGridSizer(2, 8, 10);
    grid->AddGrowableCol(1, 1);
    auto* v = new wxBoxSizer(wxVERTICAL);
    v->Add(grid, 0, wxEXPAND | wxALL, 14);
    p->SetSizer(v);
    return { p, grid, v };
}

void Row(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label,
         wxWindow* field)
{
    auto* st = new wxStaticText(parent, wxID_ANY, label);
    st->SetForegroundColour(theme::kTextBody);
    grid->Add(st, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(field, 1, wxEXPAND);
}

// A label + field where the label pointer is kept (so we can grey it out).
void RowL(wxWindow* parent, wxFlexGridSizer* grid, wxStaticText** labelOut,
          const wxString& label, wxWindow* field)
{
    auto* st = new wxStaticText(parent, wxID_ANY, label);
    st->SetForegroundColour(theme::kTextBody);
    *labelOut = st;
    grid->Add(st, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(field, 1, wxEXPAND);
}

void CheckRow(wxFlexGridSizer* grid, wxCheckBox* cb)
{
    grid->AddSpacer(0);
    grid->Add(cb, 0, wxALIGN_CENTER_VERTICAL);
}
} // namespace

ConnectionDialog::ConnectionDialog(wxWindow* parent, db::DbType preset,
                                   const core::ConnectionProfile* edit)
    : CenteredDialog(parent, wxID_ANY, tr(L"连接"), wxDefaultPosition, wxSize(520, 600),
                     wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(theme::kWhite);
    selected_ = edit ? edit->type : preset;
    const db::DbTypeInfo& info = db::InfoOf(selected_);
    SetTitle((edit ? tr(L"编辑连接 — ") : tr(L"新建连接 — ")) + info.name);

    // The engine is conveyed by the window title + its brand icon; no in-dialog
    // header needed.
    wxIcon brand;
    brand.CopyFromBitmap(icons::DbBrand(selected_, 16));
    SetIcon(brand);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- tabs ----
    auto* nb = new wxNotebook(this, wxID_ANY);
    nb->AddPage(BuildGeneralTab(nb),   tr(L"常规"), true);
    nb->AddPage(BuildAdvancedTab(nb),  tr(L"高级"));
    nb->AddPage(BuildDatabasesTab(nb), tr(L"数据库"));
    sslPage_ = BuildSslTab(nb);   nb->AddPage(sslPage_,  L"SSL");
    sshPage_ = BuildSshTab(nb);   nb->AddPage(sshPage_,  L"SSH");
    httpPage_ = BuildHttpTab(nb); nb->AddPage(httpPage_, L"HTTP");
    root->Add(nb, 1, wxEXPAND | wxALL, 16);

    // Scriptable: preselect a tab index (verification hook).
    long tabIdx = 0; wxString ts;
    if (wxGetEnv(L"SWIFTSQL_AUTOTAB", &ts) && ts.ToLong(&tabIdx))
        nb->SetSelection(static_cast<size_t>(tabIdx));

    // ---- buttons ----
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto* test = new wxButton(this, ID_TEST_CONN, tr(L"测试连接"));
    test->Bind(wxEVT_BUTTON, &ConnectionDialog::OnTestConnection, this);
    btns->Add(test, 0);
    btns->AddStretchSpacer();
    btns->Add(new wxButton(this, wxID_CANCEL, tr(L"取消")), 0, wxRIGHT, 8);
    auto* ok = new wxButton(this, wxID_OK, tr(L"确定"));
    ok->SetBackgroundColour(theme::kPrimary);
    ok->SetForegroundColour(theme::kWhite);
    btns->Add(ok, 0);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);

    SetSizer(root);
    SetMinSize(wxSize(520, 560));
    CentreOnParent();

    if (edit) LoadInto(*edit);
    else SelectType(preset);
    UpdateSslFields();
    UpdateSshFields();
    UpdateHttpFields();
}

// ===========================================================================
wxWindow* ConnectionDialog::BuildGeneralTab(wxWindow* nb)
{
    Tab t = MakeTab(nb);
    name_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    name_->SetHint(tr(L"例如:Production"));
    Row(t.panel, t.grid, tr(L"连接名"), name_);
    host_ = new wxTextCtrl(t.panel, wxID_ANY, L"127.0.0.1");
    RowL(t.panel, t.grid, &hostLabel_, tr(L"主机"), host_);
    port_ = new wxSpinCtrl(t.panel, wxID_ANY); port_->SetRange(1, 65535);
    RowL(t.panel, t.grid, &portLabel_, tr(L"端口"), port_);
    user_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    RowL(t.panel, t.grid, &userLabel_, tr(L"用户名"), user_);
    password_ = new wxTextCtrl(t.panel, wxID_ANY, L"", wxDefaultPosition,
                               wxDefaultSize, wxTE_PASSWORD);
    RowL(t.panel, t.grid, &passwordLabel_, tr(L"密码"), password_);
    savePassword_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"保存密码"));
    savePassword_->SetValue(true);
    CheckRow(t.grid, savePassword_);

    // Database row: for SQLite this becomes "数据库文件" + a Browse button.
    databaseLabel_ = new wxStaticText(t.panel, wxID_ANY, tr(L"数据库"));
    databaseLabel_->SetForegroundColour(theme::kTextBody);
    t.grid->Add(databaseLabel_, 0, wxALIGN_CENTER_VERTICAL);
    database_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    database_->SetHint(tr(L"初始数据库(可留空)"));
    browseFile_ = new wxButton(t.panel, wxID_ANY, tr(L"浏览…"), wxDefaultPosition,
                               wxDefaultSize, wxBU_EXACTFIT);
    browseFile_->Bind(wxEVT_BUTTON, &ConnectionDialog::OnBrowseFile, this);
    browseFile_->Show(false);   // shown only for SQLite
    {
        auto* dbRow = new wxBoxSizer(wxHORIZONTAL);
        dbRow->Add(database_, 1, wxEXPAND);
        dbRow->Add(browseFile_, 0, wxLEFT, 6);
        t.grid->Add(dbRow, 1, wxEXPAND);
    }
    generalPanel_ = t.panel;
    return t.panel;
}

wxWindow* ConnectionDialog::BuildAdvancedTab(wxWindow* nb)
{
    Tab t = MakeTab(nb);
    charset_ = new wxChoice(t.panel, wxID_ANY);
    Row(t.panel, t.grid, tr(L"编码"), charset_);
    timeout_ = new wxSpinCtrl(t.panel, wxID_ANY); timeout_->SetRange(1, 300);
    timeout_->SetValue(8);
    Row(t.panel, t.grid, tr(L"连接超时(秒)"), timeout_);
    rwTimeout_ = new wxSpinCtrl(t.panel, wxID_ANY); rwTimeout_->SetRange(0, 3600);
    Row(t.panel, t.grid, tr(L"读写超时(秒,0=无)"), rwTimeout_);
    keepalive_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"保持连接(keepalive)"));
    keepalive_->SetValue(true);
    CheckRow(t.grid, keepalive_);
    keepaliveInterval_ = new wxSpinCtrl(t.panel, wxID_ANY);
    keepaliveInterval_->SetRange(1, 3600); keepaliveInterval_->SetValue(30);
    Row(t.panel, t.grid, tr(L"keepalive 间隔(秒)"), keepaliveInterval_);
    autoReconnect_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"断线自动重连"));
    CheckRow(t.grid, autoReconnect_);
    compression_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"使用协议压缩"));
    CheckRow(t.grid, compression_);
    readOnly_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"只读连接"));
    CheckRow(t.grid, readOnly_);
    isolation_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* i : { L"数据库默认", L"READ UNCOMMITTED", L"READ COMMITTED",
                              L"REPEATABLE READ", L"SERIALIZABLE" })
        isolation_->Append(tr(i));
    isolation_->SetSelection(0);
    Row(t.panel, t.grid, tr(L"隔离级别"), isolation_);
    timezone_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    timezone_->SetHint(tr(L"如 +08:00 / Asia/Shanghai(留空=服务器)"));
    Row(t.panel, t.grid, tr(L"会话时区"), timezone_);
    appName_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    appName_->SetHint(tr(L"application_name / 程序名"));
    Row(t.panel, t.grid, tr(L"应用名"), appName_);
    defaultSchema_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    defaultSchema_->SetHint(tr(L"PG search_path / 默认 schema"));
    Row(t.panel, t.grid, tr(L"默认 schema"), defaultSchema_);
    socketFile_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    socketFile_->SetHint(tr(L"Unix socket / 命名管道(本地)"));
    Row(t.panel, t.grid, L"Socket", socketFile_);
    initCommands_ = new wxTextCtrl(t.panel, wxID_ANY, L"", wxDefaultPosition,
                                   wxSize(-1, 54), wxTE_MULTILINE);
    initCommands_->SetHint(tr(L"连接后执行的 SQL,用 ; 分隔"));
    Row(t.panel, t.grid, tr(L"初始 SQL"), initCommands_);
    return t.panel;
}

wxWindow* ConnectionDialog::BuildDatabasesTab(wxWindow* nb)
{
    Tab t = MakeTab(nb);
    dbListMode_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* m : { L"显示全部", L"仅显示以下", L"排除以下" })
        dbListMode_->Append(tr(m));
    dbListMode_->SetSelection(0);
    Row(t.panel, t.grid, tr(L"数据库列表"), dbListMode_);
    dbFilter_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    dbFilter_->SetHint(tr(L"逗号分隔,如 shop_production, analytics"));
    Row(t.panel, t.grid, tr(L"数据库名单"), dbFilter_);
    showSystemDbs_ = new wxCheckBox(t.panel, wxID_ANY,
        tr(L"显示系统库(information_schema/mysql/pg_catalog…)"));
    CheckRow(t.grid, showSystemDbs_);
    autoConnect_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"启动时自动连接"));
    CheckRow(t.grid, autoConnect_);
    return t.panel;
}

wxWindow* ConnectionDialog::BuildSslTab(wxWindow* nb)
{
    Tab t = MakeTab(nb);
    sslEnable_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"使用 SSL"));
    sslEnable_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateSslFields(); });
    CheckRow(t.grid, sslEnable_);

    sslMode_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* m : { L"disable", L"allow", L"prefer", L"require",
                              L"verify-ca", L"verify-full" })
        sslMode_->Append(m);
    sslMode_->SetSelection(2);
    RowL(t.panel, t.grid, &sslModeLabel_, tr(L"SSL 模式(PG)"), sslMode_);

    sslAuth_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* a : { L"无客户端认证", L"仅密钥", L"密钥 + 密码短语",
                              L"证书 + 密钥", L"证书 + 密钥 + 密码短语" })
        sslAuth_->Append(tr(a));
    sslAuth_->SetSelection(0);
    sslAuth_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateSslFields(); });
    Row(t.panel, t.grid, tr(L"客户端认证"), sslAuth_);

    sslCa_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    sslCa_->SetHint(tr(L"CA 证书路径(可选)"));
    Row(t.panel, t.grid, tr(L"CA 证书"), sslCa_);
    sslCert_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    RowL(t.panel, t.grid, &sslCertLabel_, tr(L"客户端证书"), sslCert_);
    sslKey_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    RowL(t.panel, t.grid, &sslKeyLabel_, tr(L"客户端密钥"), sslKey_);
    sslKeyPass_ = new wxTextCtrl(t.panel, wxID_ANY, L"", wxDefaultPosition,
                                 wxDefaultSize, wxTE_PASSWORD);
    RowL(t.panel, t.grid, &sslKeyPassLabel_, tr(L"密钥密码短语"), sslKeyPass_);

    sslVerify_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"验证服务器证书"));
    CheckRow(t.grid, sslVerify_);
    sslVerifyHost_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"验证服务器主机名"));
    CheckRow(t.grid, sslVerifyHost_);

    sslCipher_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    sslCipher_->SetHint(tr(L"cipher 列表(可选)"));
    Row(t.panel, t.grid, tr(L"加密套件"), sslCipher_);
    sslMinVer_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* v : { L"默认", L"TLSv1.2", L"TLSv1.3" })
        sslMinVer_->Append(tr(v));
    sslMinVer_->SetSelection(0);
    Row(t.panel, t.grid, tr(L"最低 TLS 版本"), sslMinVer_);
    return t.panel;
}

wxWindow* ConnectionDialog::BuildSshTab(wxWindow* nb)
{
    Tab t = MakeTab(nb);
    sshEnable_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"使用 SSH 隧道"));
    sshEnable_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateSshFields(); });
    CheckRow(t.grid, sshEnable_);

    sshHost_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    Row(t.panel, t.grid, tr(L"SSH 主机"), sshHost_);
    sshPort_ = new wxSpinCtrl(t.panel, wxID_ANY); sshPort_->SetRange(1, 65535);
    sshPort_->SetValue(22);
    Row(t.panel, t.grid, tr(L"SSH 端口"), sshPort_);
    sshUser_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    Row(t.panel, t.grid, tr(L"SSH 用户"), sshUser_);

    sshAuth_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* a : { L"密码", L"公钥", L"密码 + 公钥", L"SSH Agent",
                              L"键盘交互" })
        sshAuth_->Append(tr(a));
    sshAuth_->SetSelection(0);
    sshAuth_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateSshFields(); });
    Row(t.panel, t.grid, tr(L"认证方式"), sshAuth_);

    sshPassword_ = new wxTextCtrl(t.panel, wxID_ANY, L"", wxDefaultPosition,
                                  wxDefaultSize, wxTE_PASSWORD);
    RowL(t.panel, t.grid, &sshPasswordLabel_, tr(L"SSH 密码"), sshPassword_);
    sshKey_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    sshKey_->SetHint(tr(L"私钥文件路径"));
    RowL(t.panel, t.grid, &sshKeyLabel_, tr(L"私钥文件"), sshKey_);
    sshKeyPass_ = new wxTextCtrl(t.panel, wxID_ANY, L"", wxDefaultPosition,
                                 wxDefaultSize, wxTE_PASSWORD);
    RowL(t.panel, t.grid, &sshKeyPassLabel_, tr(L"私钥密码短语"), sshKeyPass_);

    sshLocalPort_ = new wxSpinCtrl(t.panel, wxID_ANY); sshLocalPort_->SetRange(0, 65535);
    Row(t.panel, t.grid, tr(L"本地绑定端口(0=自动)"), sshLocalPort_);
    sshKeepalive_ = new wxSpinCtrl(t.panel, wxID_ANY); sshKeepalive_->SetRange(0, 3600);
    sshKeepalive_->SetValue(30);
    Row(t.panel, t.grid, tr(L"隧道 keepalive(秒)"), sshKeepalive_);
    sshCompression_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"隧道压缩"));
    CheckRow(t.grid, sshCompression_);

    auto* note = new wxStaticText(t.panel, wxID_ANY,
        tr(L"注:连接时自动建立 SSH 隧道(支持密码 / 公钥 / 密码+公钥认证)。"));
    note->SetForegroundColour(theme::kTextFaint);
    t.vbox->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    return t.panel;
}

wxWindow* ConnectionDialog::BuildHttpTab(wxWindow* nb)
{
    Tab t = MakeTab(nb);
    httpEnable_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"使用 HTTP 隧道"));
    httpEnable_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateHttpFields(); });
    CheckRow(t.grid, httpEnable_);
    httpUrl_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    httpUrl_->SetHint(L"http(s)://…/tunnel.php");
    Row(t.panel, t.grid, tr(L"隧道 URL"), httpUrl_);
    httpAuth_ = new wxChoice(t.panel, wxID_ANY);
    for (const wchar_t* a : { L"无", L"Basic", L"Digest" }) httpAuth_->Append(tr(a));
    httpAuth_->SetSelection(0);
    httpAuth_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateHttpFields(); });
    Row(t.panel, t.grid, tr(L"鉴权"), httpAuth_);
    httpUser_ = new wxTextCtrl(t.panel, wxID_ANY, L"");
    RowL(t.panel, t.grid, &httpUserLabel_, tr(L"鉴权用户"), httpUser_);
    httpPassword_ = new wxTextCtrl(t.panel, wxID_ANY, L"", wxDefaultPosition,
                                   wxDefaultSize, wxTE_PASSWORD);
    RowL(t.panel, t.grid, &httpPasswordLabel_, tr(L"鉴权密码"), httpPassword_);
    httpVerify_ = new wxCheckBox(t.panel, wxID_ANY, tr(L"验证服务器证书(https)"));
    httpVerify_->SetValue(true);
    CheckRow(t.grid, httpVerify_);

    auto* note = new wxStaticText(t.panel, wxID_ANY,
        tr(L"注:隧道配置将被保存;隧道转发功能规划中。"));
    note->SetForegroundColour(theme::kTextFaint);
    t.vbox->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    return t.panel;
}

// ===========================================================================
void ConnectionDialog::SelectType(db::DbType type)
{
    selected_ = type;
    const db::DbTypeInfo& info = db::InfoOf(type);

    const int curPort = port_->GetValue();
    bool portIsDefault = (curPort <= 1);
    const wxString curUser = user_->GetValue();
    bool userIsDefault = curUser.IsEmpty();
    for (const db::DbTypeInfo& o : db::AllDbTypes()) {
        if (curPort == o.defaultPort) portIsDefault = true;
        if (curUser == o.defaultUser) userIsDefault = true;
    }
    if (portIsDefault) port_->SetValue(info.defaultPort);
    if (userIsDefault) user_->SetValue(info.defaultUser);

    // KingBase speaks the PostgreSQL wire protocol → PG-style options.
    const bool pg = (type == db::DbType::PostgreSQL || type == db::DbType::KingBase);
    const wxString keep = charset_->GetStringSelection();
    charset_->Clear();
    if (pg)
        for (const wchar_t* c : { L"UTF8", L"GBK", L"LATIN1", L"SQL_ASCII" })
            charset_->Append(c);
    else if (type == db::DbType::DM)
        for (const wchar_t* c : { L"UTF-8", L"GBK", L"GB18030" })
            charset_->Append(c);
    else
        for (const wchar_t* c : { L"utf8mb4", L"utf8", L"latin1", L"gbk", L"big5" })
            charset_->Append(c);
    if (!keep.IsEmpty() && charset_->FindString(keep) != wxNOT_FOUND)
        charset_->SetStringSelection(keep);
    else
        charset_->SetSelection(0);

    // SSL mode is a PG concept; the verify checkbox governs MySQL/OceanBase.
    sslMode_->Enable(pg);
    sslModeLabel_->Enable(pg);
    UpdateSslFields();

    // SQLite is file-based: hide host/port/user/password, turn the database row
    // into a file picker, and grey out the network tabs.
    const bool sqlite = (type == db::DbType::Sqlite);
    auto showRow = [](wxStaticText* lbl, wxWindow* field, bool show) {
        if (lbl)   lbl->Show(show);
        if (field) field->Show(show);
    };
    showRow(hostLabel_, host_, !sqlite);
    showRow(portLabel_, port_, !sqlite);
    showRow(userLabel_, user_, !sqlite);
    showRow(passwordLabel_, password_, !sqlite);
    if (savePassword_) savePassword_->Show(!sqlite);
    if (databaseLabel_) databaseLabel_->SetLabel(sqlite ? tr(L"数据库文件") : tr(L"数据库"));
    if (database_) database_->SetHint(sqlite ? tr(L"选择 .db / .sqlite 文件")
                                             : tr(L"初始数据库(可留空)"));
    if (browseFile_) browseFile_->Show(sqlite);
    for (wxWindow* pg2 : { sslPage_, sshPage_, httpPage_ })
        if (pg2) pg2->Enable(!sqlite);
    if (generalPanel_) generalPanel_->Layout();
}

// ---------------------------------------------------------------------------
void ConnectionDialog::OnBrowseFile(wxCommandEvent&)
{
    wxFileDialog dlg(this, tr(L"选择 SQLite 数据库文件"), wxEmptyString, wxEmptyString,
                     tr(L"SQLite 文件 (*.db;*.sqlite;*.sqlite3)|*.db;*.sqlite;*.sqlite3|所有文件 (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) database_->SetValue(dlg.GetPath());
}

// ---------------------------------------------------------------------------
void ConnectionDialog::UpdateSslFields()
{
    const bool on = sslEnable_->GetValue();
    const int a = sslAuth_->GetSelection();   // 0 none,1 key,2 key+pass,3 cert+key,4 cert+key+pass
    const bool useKey  = on && (a >= 1);
    const bool useCert = on && (a >= 3);
    const bool usePass = on && (a == 2 || a == 4);

    for (wxWindow* w : { (wxWindow*)sslAuth_, (wxWindow*)sslCa_,
                         (wxWindow*)sslVerify_, (wxWindow*)sslVerifyHost_,
                         (wxWindow*)sslCipher_, (wxWindow*)sslMinVer_ })
        w->Enable(on);
    sslMode_->Enable(on && selected_ == db::DbType::PostgreSQL);
    sslModeLabel_->Enable(on && selected_ == db::DbType::PostgreSQL);

    sslKey_->Enable(useKey);      sslKeyLabel_->Enable(useKey);
    sslCert_->Enable(useCert);    sslCertLabel_->Enable(useCert);
    sslKeyPass_->Enable(usePass); sslKeyPassLabel_->Enable(usePass);
}

void ConnectionDialog::UpdateSshFields()
{
    const bool on = sshEnable_->GetValue();
    const int a = sshAuth_->GetSelection();   // 0 password,1 key,2 pwd+key,3 agent,4 keyboard
    const bool usePwd = on && (a == 0 || a == 2 || a == 4);
    const bool useKey = on && (a == 1 || a == 2);

    for (wxWindow* w : { (wxWindow*)sshHost_, (wxWindow*)sshPort_,
                         (wxWindow*)sshUser_, (wxWindow*)sshAuth_,
                         (wxWindow*)sshLocalPort_, (wxWindow*)sshKeepalive_,
                         (wxWindow*)sshCompression_ })
        w->Enable(on);
    sshPassword_->Enable(usePwd); sshPasswordLabel_->Enable(usePwd);
    sshKey_->Enable(useKey);      sshKeyLabel_->Enable(useKey);
    sshKeyPass_->Enable(useKey);  sshKeyPassLabel_->Enable(useKey);
}

void ConnectionDialog::UpdateHttpFields()
{
    const bool on = httpEnable_->GetValue();
    const bool auth = on && (httpAuth_->GetSelection() >= 1);
    for (wxWindow* w : { (wxWindow*)httpUrl_, (wxWindow*)httpAuth_,
                         (wxWindow*)httpVerify_ })
        w->Enable(on);
    httpUser_->Enable(auth);     httpUserLabel_->Enable(auth);
    httpPassword_->Enable(auth); httpPasswordLabel_->Enable(auth);
}

// ---------------------------------------------------------------------------
void ConnectionDialog::LoadInto(const core::ConnectionProfile& p)
{
    name_->SetValue(p.name);
    host_->SetValue(p.host);
    port_->SetValue(p.port);
    user_->SetValue(p.user);
    password_->SetValue(p.password);
    database_->SetValue(p.database);
    savePassword_->SetValue(p.savePassword);

    timeout_->SetValue(p.connectTimeout);
    rwTimeout_->SetValue(p.readWriteTimeout);
    keepalive_->SetValue(p.keepalive);
    keepaliveInterval_->SetValue(p.keepaliveInterval);
    autoReconnect_->SetValue(p.autoReconnect);
    compression_->SetValue(p.compression);
    readOnly_->SetValue(p.readOnly);
    timezone_->SetValue(p.timezone);
    appName_->SetValue(p.appName);
    defaultSchema_->SetValue(p.defaultSchema);
    socketFile_->SetValue(p.socketFile);
    initCommands_->SetValue(p.initCommands);

    dbListMode_->SetSelection(static_cast<int>(p.dbListMode));
    dbFilter_->SetValue(p.dbFilter);
    showSystemDbs_->SetValue(p.showSystemDbs);
    autoConnect_->SetValue(p.autoConnectOnStartup);

    sslEnable_->SetValue(p.sslEnabled);
    sslAuth_->SetSelection(static_cast<int>(p.sslAuth));
    sslCa_->SetValue(p.sslCaCert);
    sslCert_->SetValue(p.sslClientCert);
    sslKey_->SetValue(p.sslClientKey);
    sslKeyPass_->SetValue(p.sslClientKeyPassword);
    sslVerify_->SetValue(p.sslVerifyServerCert);
    sslVerifyHost_->SetValue(p.sslVerifyHostname);
    sslCipher_->SetValue(p.sslCipher);

    sshEnable_->SetValue(p.sshEnabled);
    sshHost_->SetValue(p.sshHost);
    sshPort_->SetValue(p.sshPort);
    sshUser_->SetValue(p.sshUser);
    sshAuth_->SetSelection(static_cast<int>(p.sshAuth));
    sshPassword_->SetValue(p.sshPassword);
    sshKey_->SetValue(p.sshKeyFile);
    sshKeyPass_->SetValue(p.sshKeyPassword);
    sshLocalPort_->SetValue(p.sshLocalPort);
    sshKeepalive_->SetValue(p.sshKeepalive);
    sshCompression_->SetValue(p.sshCompression);

    httpEnable_->SetValue(p.httpEnabled);
    httpUrl_->SetValue(p.httpUrl);
    httpAuth_->SetSelection(static_cast<int>(p.httpAuth));
    httpUser_->SetValue(p.httpUser);
    httpPassword_->SetValue(p.httpPassword);
    httpVerify_->SetValue(p.httpVerifyCert);

    SelectType(p.type);
    if (!p.charset.IsEmpty() && charset_->FindString(p.charset) != wxNOT_FOUND)
        charset_->SetStringSelection(p.charset);
    if (!p.sslMode.IsEmpty() && sslMode_->FindString(p.sslMode) != wxNOT_FOUND)
        sslMode_->SetStringSelection(p.sslMode);
    if (!p.isolationLevel.IsEmpty() &&
        isolation_->FindString(p.isolationLevel) != wxNOT_FOUND)
        isolation_->SetStringSelection(p.isolationLevel);
    if (!p.sslMinVersion.IsEmpty() &&
        sslMinVer_->FindString(p.sslMinVersion) != wxNOT_FOUND)
        sslMinVer_->SetStringSelection(p.sslMinVersion);
}

// ---------------------------------------------------------------------------
void ConnectionDialog::OnTestConnection(wxCommandEvent&)
{
    auto conn = db::CreateConnection(selected_);
    wxString err;
    bool ok = false;
    long ms = 0;
    {
        wxBusyCursor busy;
        wxStopWatch sw;
        ok = conn->Connect(GetProfile(), err);
        ms = sw.Time();
    }
    if (ok)
        wxMessageBox(wxString::Format(
            tr(L"✓ 连接成功(%ld ms)\n\n%s %s @ %s:%d"),
            ms, db::InfoOf(selected_).name, conn->ServerVersion(),
            host_->GetValue(), port_->GetValue()),
            tr(L"测试连接"), wxOK | wxICON_INFORMATION, this);
    else
        wxMessageBox(wxString::Format(tr(L"✗ 连接失败\n\n%s"), err),
                     tr(L"测试连接"), wxOK | wxICON_ERROR, this);
}

// ---------------------------------------------------------------------------
core::ConnectionProfile ConnectionDialog::GetProfile() const
{
    core::ConnectionProfile p;
    p.name = name_->GetValue().IsEmpty() ? tr(L"未命名连接") : name_->GetValue();
    p.type = selected_;
    p.host = host_->GetValue();
    p.port = port_->GetValue();
    p.user = user_->GetValue();
    p.password = password_->GetValue();
    p.database = database_->GetValue();
    p.savePassword = savePassword_->GetValue();

    p.charset = charset_->GetStringSelection();
    p.connectTimeout = timeout_->GetValue();
    p.readWriteTimeout = rwTimeout_->GetValue();
    p.keepalive = keepalive_->GetValue();
    p.keepaliveInterval = keepaliveInterval_->GetValue();
    p.autoReconnect = autoReconnect_->GetValue();
    p.compression = compression_->GetValue();
    p.timezone = timezone_->GetValue();
    p.appName = appName_->GetValue();
    p.defaultSchema = defaultSchema_->GetValue();
    p.socketFile = socketFile_->GetValue();
    p.initCommands = initCommands_->GetValue();
    p.readOnly = readOnly_->GetValue();
    p.isolationLevel = isolation_->GetSelection() == 0 ? wxString()
                                                       : isolation_->GetStringSelection();

    p.dbListMode = static_cast<core::DbListMode>(dbListMode_->GetSelection());
    p.dbFilter = dbFilter_->GetValue();
    p.showSystemDbs = showSystemDbs_->GetValue();
    p.autoConnectOnStartup = autoConnect_->GetValue();

    p.sslEnabled = sslEnable_->GetValue();
    p.sslMode = sslMode_->GetStringSelection();
    p.sslAuth = static_cast<core::SslAuth>(sslAuth_->GetSelection());
    p.sslCaCert = sslCa_->GetValue();
    p.sslClientCert = sslCert_->GetValue();
    p.sslClientKey = sslKey_->GetValue();
    p.sslClientKeyPassword = sslKeyPass_->GetValue();
    p.sslVerifyServerCert = sslVerify_->GetValue();
    p.sslVerifyHostname = sslVerifyHost_->GetValue();
    p.sslCipher = sslCipher_->GetValue();
    p.sslMinVersion = sslMinVer_->GetSelection() == 0 ? wxString()
                                                      : sslMinVer_->GetStringSelection();

    p.sshEnabled = sshEnable_->GetValue();
    p.sshHost = sshHost_->GetValue();
    p.sshPort = sshPort_->GetValue();
    p.sshUser = sshUser_->GetValue();
    p.sshAuth = static_cast<core::SshAuth>(sshAuth_->GetSelection());
    p.sshPassword = sshPassword_->GetValue();
    p.sshKeyFile = sshKey_->GetValue();
    p.sshKeyPassword = sshKeyPass_->GetValue();
    p.sshLocalPort = sshLocalPort_->GetValue();
    p.sshKeepalive = sshKeepalive_->GetValue();
    p.sshCompression = sshCompression_->GetValue();

    p.httpEnabled = httpEnable_->GetValue();
    p.httpUrl = httpUrl_->GetValue();
    p.httpAuth = static_cast<core::HttpAuth>(httpAuth_->GetSelection());
    p.httpUser = httpUser_->GetValue();
    p.httpPassword = httpPassword_->GetValue();
    p.httpVerifyCert = httpVerify_->GetValue();
    return p;
}

} // namespace ui
