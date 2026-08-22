// UserEditDialog.cpp — see UserEditDialog.h. Standard wx controls on the app's
// white surface; all server work goes through db::IConnection (SaveUser does the
// CREATE/ALTER + declarative GRANT/REVOKE). Small metadata reads (grants,
// databases, accounts) run synchronously in the ctor under a busy cursor.
#include "ui/UserEditDialog.h"

#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/listbox.h>
#include <wx/spinctrl.h>
#include <wx/button.h>
#include <wx/statline.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>
#include <algorithm>

#include "db/DbDriver.h"
#include "ui/Theme.h"
#include "ui/I18n.h"

namespace ui {

using db::PrivilegeDef;

UserEditDialog::UserEditDialog(wxWindow* parent, db::IConnection* conn,
                               const wxString& user, const wxString& host, bool isNew,
                               int startPage)
    : CenteredDialog(parent, wxID_ANY,
                     isNew ? tr(L"新建用户")
                           : tr(L"编辑用户: ") + user +
                             (conn->GetUserAdminModel().hasHost ? L"@" + host : wxString()),
                     wxDefaultPosition, wxSize(640, 560),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      conn_(conn), isNew_(isNew), origUser_(user), origHost_(host),
      model_(conn->GetUserAdminModel())
{
    SetBackgroundColour(theme::kWhite);

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* nb = new wxNotebook(this, wxID_ANY);
    // Tabs follow the dialect model: 常规 + the server-level checklist are always
    // shown; the rest appear only when the model enables them. startPage is a tab
    // INDEX into this dynamic set — page 0 is 常规, page 1 is the server checklist
    // (used by "Privilege Manager" to jump straight there).
    nb->AddPage(BuildGeneralTab(nb), tr(L"常规"), true);
    nb->AddPage(BuildGlobalTab(nb),  tr(model_.serverTabLabel));
    if (model_.hasDbTab)          nb->AddPage(BuildDbTab(nb),       tr(model_.dbTabLabel));
    if (model_.hasRolesTab)       nb->AddPage(BuildRolesTab(nb),    tr(L"隶属成员"));
    if (model_.hasMembersTab)     nb->AddPage(BuildMembersTab(nb),  tr(L"成员"));
    if (model_.hasResourceLimits) nb->AddPage(BuildAdvancedTab(nb), tr(L"高级"));
    root->Add(nb, 1, wxEXPAND | wxALL, 10);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* ok = new wxButton(this, wxID_OK, tr(L"保存"));
    auto* cancel = new wxButton(this, wxID_CANCEL, tr(L"取消"));
    btns->Add(ok, 0, wxRIGHT, 8);
    btns->Add(cancel, 0);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    SetSizer(root);

    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnOk(); });

    LoadInitial();
    if (startPage > 0 && startPage < static_cast<int>(nb->GetPageCount()))
        nb->SetSelection(startPage);
}

// ---------------------------------------------------------------------------
wxWindow* UserEditDialog::BuildGeneralTab(wxWindow* nb)
{
    auto* p = new wxPanel(nb);
    p->SetBackgroundColour(theme::kWhite);
    auto* g = new wxFlexGridSizer(2, wxSize(10, 10));
    g->AddGrowableCol(1, 1);

    auto label = [&](const wxString& t) {
        return new wxStaticText(p, wxID_ANY, t);
    };

    name_ = new wxTextCtrl(p, wxID_ANY, origUser_);
    if (!isNew_) name_->Enable(false);          // rename = separate op (staged)

    pw_ = new wxTextCtrl(p, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                         wxTE_PASSWORD);
    setPw_ = new wxCheckBox(p, wxID_ANY, tr(L"修改密码"));
    setPw_->SetValue(isNew_);
    pw_->Enable(isNew_);
    setPw_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { pw_->Enable(setPw_->IsChecked()); });

    g->Add(label(tr(L"用户名")), 0, wxALIGN_CENTER_VERTICAL);
    g->Add(name_, 1, wxEXPAND);

    if (model_.hasHost) {                        // MySQL-style user@host identity
        host_ = new wxTextCtrl(p, wxID_ANY, isNew_ ? wxString(L"%") : origHost_);
        if (!isNew_) host_->Enable(false);
        g->Add(label(tr(L"主机")), 0, wxALIGN_CENTER_VERTICAL);
        g->Add(host_, 1, wxEXPAND);
    }

    g->Add(label(tr(L"密码")), 0, wxALIGN_CENTER_VERTICAL);
    g->Add(pw_, 1, wxEXPAND);
    g->Add(new wxStaticText(p, wxID_ANY, wxEmptyString), 0);
    g->Add(setPw_, 0);

    if (model_.hasAuthPlugin) {                  // MySQL authentication plugin
        plugin_ = new wxChoice(p, wxID_ANY);
        plugin_->Append(tr(L"(服务器默认)"));
        for (const auto& pn : model_.authPlugins) plugin_->Append(pn);
        plugin_->SetSelection(0);
        g->Add(label(tr(L"认证插件")), 0, wxALIGN_CENTER_VERTICAL);
        g->Add(plugin_, 1, wxEXPAND);
    }

    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(g, 0, wxEXPAND | wxALL, 14);
    if (!model_.generalHint.IsEmpty()) {
        auto* hint = new wxStaticText(p, wxID_ANY, tr(model_.generalHint));
        hint->Wrap(580);
        s->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    }
    p->SetSizer(s);
    return p;
}

wxWindow* UserEditDialog::BuildGlobalTab(wxWindow* nb)
{
    auto* p = new wxPanel(nb);
    p->SetBackgroundColour(theme::kWhite);
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(new wxStaticText(p, wxID_ANY, tr(model_.serverTabHint)), 0, wxALL, 12);
    globalList_ = new wxCheckListBox(p, wxID_ANY);
    for (const auto& d : model_.serverPrivileges)
        globalList_->Append(d.name);
    s->Add(globalList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    p->SetSizer(s);
    return p;
}

wxWindow* UserEditDialog::BuildDbTab(wxWindow* nb)
{
    auto* p = new wxPanel(nb);
    p->SetBackgroundColour(theme::kWhite);
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    auto* left = new wxBoxSizer(wxVERTICAL);
    left->Add(new wxStaticText(p, wxID_ANY, tr(L"数据库")), 0, wxBOTTOM, 4);
    dbList_ = new wxListBox(p, wxID_ANY);
    left->Add(dbList_, 1, wxEXPAND);

    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(new wxStaticText(p, wxID_ANY, tr(model_.dbTabHint)), 0, wxBOTTOM, 4);
    dbPrivs_ = new wxCheckListBox(p, wxID_ANY);
    for (const auto& d : model_.dbPrivileges)
        dbPrivs_->Append(d.name);
    dbPrivs_->Enable(false);
    right->Add(dbPrivs_, 1, wxEXPAND);

    row->Add(left, 2, wxEXPAND | wxALL, 12);
    row->Add(right, 3, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 12);
    p->SetSizer(row);

    dbList_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { OnDbSelected(); });
    return p;
}

wxWindow* UserEditDialog::BuildRolesTab(wxWindow* nb)
{
    auto* p = new wxPanel(nb);
    p->SetBackgroundColour(theme::kWhite);
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(new wxStaticText(p, wxID_ANY, tr(model_.rolesTabHint)), 0, wxALL, 12);
    rolesList_ = new wxCheckListBox(p, wxID_ANY);
    s->Add(rolesList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    p->SetSizer(s);
    return p;
}

wxWindow* UserEditDialog::BuildMembersTab(wxWindow* nb)
{
    auto* p = new wxPanel(nb);
    p->SetBackgroundColour(theme::kWhite);
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(new wxStaticText(p, wxID_ANY,
        tr(L"成员 — 将本账户当作角色持有的用户 (只读):")), 0, wxALL, 12);
    membersList_ = new wxListBox(p, wxID_ANY);
    s->Add(membersList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    p->SetSizer(s);
    return p;
}

wxWindow* UserEditDialog::BuildAdvancedTab(wxWindow* nb)
{
    auto* p = new wxPanel(nb);
    p->SetBackgroundColour(theme::kWhite);
    auto* s = new wxBoxSizer(wxVERTICAL);

    resLimits_ = new wxCheckBox(p, wxID_ANY, tr(L"设置资源限制 (WITH MAX_…)"));
    s->Add(resLimits_, 0, wxALL, 12);

    auto* g = new wxFlexGridSizer(2, wxSize(10, 8));
    g->AddGrowableCol(1, 1);
    auto spin = [&](const wxString& lbl) {
        g->Add(new wxStaticText(p, wxID_ANY, lbl), 0, wxALIGN_CENTER_VERTICAL);
        auto* sc = new wxSpinCtrl(p, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxSP_ARROW_KEYS, 0, 1000000, 0);
        g->Add(sc, 1, wxEXPAND);
        return sc;
    };
    maxQueries_    = spin(tr(L"每小时最大查询数 (0=无限)"));
    maxUpdates_    = spin(tr(L"每小时最大更新数 (0=无限)"));
    maxConns_      = spin(tr(L"每小时最大连接数 (0=无限)"));
    maxUserConns_  = spin(tr(L"最大并发连接数 (0=无限)"));
    s->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT, 24);

    requireSsl_ = new wxCheckBox(p, wxID_ANY, tr(L"要求 SSL 连接 (REQUIRE SSL)"));
    s->Add(requireSsl_, 0, wxALL, 12);

    s->Add(new wxStaticText(p, wxID_ANY,
        tr(L"提示: 未勾选\"设置资源限制\"时不改动这些项;已有限制不会回填显示。")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    p->SetSizer(s);
    return p;
}

// ---------------------------------------------------------------------------
void UserEditDialog::LoadInitial()
{
    wxBusyCursor busy;
    wxString e;

    // Databases → 数据库权限 left list (only when the model has that tab).
    std::vector<wxString> dbs;
    if (model_.hasDbTab) conn_->ListDatabases(dbs, e);

    // Grantable roles → 隶属成员 checklist (exclude self). Tokens are "name@host"
    // for host-based dialects, plain names otherwise — matching UserGrants::roles.
    const wxString selfTok = model_.hasHost ? (origUser_ + L"@" + origHost_) : origUser_;
    if (rolesList_) {
        std::vector<wxString> roles;
        conn_->ListGrantableRoles(roles, e);
        for (const auto& r : roles) {
            allAccounts_.push_back(r);
            if (r != selfTok) rolesList_->Append(r);
        }
    }

    // Existing account: read current grants and pre-check everything.
    if (!isNew_) {
        if (conn_->GetUserGrants(origUser_, origHost_, initial_, e)) {
            // server-level checklist
            std::set<wxString> gset(initial_.globalPrivs.begin(), initial_.globalPrivs.end());
            if (initial_.globalGrantOption) gset.insert(L"GRANT OPTION");
            for (unsigned i = 0; i < globalList_->GetCount(); ++i)
                if (gset.count(globalList_->GetString(i))) globalList_->Check(i);
            // per-db
            for (const auto& dg : initial_.dbPrivs)
                dbPrivMap_[dg.database] =
                    std::set<wxString>(dg.privs.begin(), dg.privs.end());
            // roles
            if (rolesList_) {
                std::set<wxString> rset(initial_.roles.begin(), initial_.roles.end());
                for (unsigned i = 0; i < rolesList_->GetCount(); ++i)
                    if (rset.count(rolesList_->GetString(i))) rolesList_->Check(i);
            }
            // auth plugin selection (MySQL)
            if (plugin_ && !initial_.authPlugin.IsEmpty()) {
                const int idx = plugin_->FindString(initial_.authPlugin);
                if (idx != wxNOT_FOUND) plugin_->SetSelection(idx);
            }
            // members (read-only, best-effort MySQL 8 role_edges — MySQL only)
            if (membersList_ && model_.hasMembersTab) {
                db::QueryResult mr; wxString me;
                if (conn_->Execute(
                        L"SELECT CONCAT(TO_USER,'@',TO_HOST) FROM mysql.role_edges WHERE FROM_USER='" +
                        origUser_ + L"' AND FROM_HOST='" + origHost_ + L"'", mr, me))
                    for (const auto& r : mr.rows)
                        if (!r.empty()) membersList_->Append(r[0]);
            }
        }
    }

    // Left db list = all databases ∪ any db already granted (so a grant on a
    // non-listable db still shows). Selecting one loads its checkboxes.
    if (dbList_) {
        std::set<wxString> all(dbs.begin(), dbs.end());
        for (const auto& kv : dbPrivMap_) all.insert(kv.first);
        for (const auto& d : all) dbList_->Append(d);
    }
}

void UserEditDialog::CommitDbSelection()
{
    if (curDb_.IsEmpty()) return;
    std::set<wxString> picked;
    for (unsigned i = 0; i < dbPrivs_->GetCount(); ++i)
        if (dbPrivs_->IsChecked(i)) picked.insert(dbPrivs_->GetString(i));
    if (picked.empty()) dbPrivMap_.erase(curDb_);
    else dbPrivMap_[curDb_] = std::move(picked);
}

void UserEditDialog::OnDbSelected()
{
    CommitDbSelection();                       // save the outgoing db's checks
    const int sel = dbList_->GetSelection();
    if (sel == wxNOT_FOUND) { curDb_.clear(); dbPrivs_->Enable(false); return; }
    curDb_ = dbList_->GetString(sel);
    dbPrivs_->Enable(true);
    const auto it = dbPrivMap_.find(curDb_);
    const std::set<wxString> cur = it == dbPrivMap_.end() ? std::set<wxString>() : it->second;
    for (unsigned i = 0; i < dbPrivs_->GetCount(); ++i)
        dbPrivs_->Check(i, cur.count(dbPrivs_->GetString(i)) > 0);
}

// ---------------------------------------------------------------------------
void UserEditDialog::OnOk()
{
    CommitDbSelection();

    db::UserSpec spec;
    spec.name = name_->GetValue(); spec.name.Trim(true).Trim(false);
    if (host_) { spec.host = host_->GetValue(); spec.host.Trim(true).Trim(false); }
    if (spec.host.IsEmpty()) spec.host = L"%";
    if (spec.name.IsEmpty()) {
        wxMessageBox(tr(L"请填写用户名。"), tr(L"编辑用户"), wxOK | wxICON_WARNING, this);
        return;
    }

    spec.setPassword = setPw_->IsChecked();
    spec.password = pw_->GetValue();
    if (plugin_ && plugin_->GetSelection() > 0)
        spec.authPlugin = plugin_->GetStringSelection();

    // global privileges (+ GRANT OPTION → flag)
    for (unsigned i = 0; i < globalList_->GetCount(); ++i)
        if (globalList_->IsChecked(i)) {
            const wxString p = globalList_->GetString(i);
            if (p == L"GRANT OPTION") spec.globalGrantOption = true;
            else spec.globalPrivs.push_back(p);
        }

    // per-db privileges
    for (const auto& kv : dbPrivMap_) {
        if (kv.second.empty()) continue;
        db::DbPrivGrant g; g.database = kv.first;
        g.privs.assign(kv.second.begin(), kv.second.end());
        spec.dbPrivs.push_back(std::move(g));
    }

    // roles
    if (rolesList_)
        for (unsigned i = 0; i < rolesList_->GetCount(); ++i)
            if (rolesList_->IsChecked(i)) spec.roles.push_back(rolesList_->GetString(i));

    // advanced
    if (resLimits_ && resLimits_->IsChecked()) {
        spec.setResourceLimits     = true;
        spec.maxQueriesPerHour     = maxQueries_->GetValue();
        spec.maxUpdatesPerHour     = maxUpdates_->GetValue();
        spec.maxConnectionsPerHour = maxConns_->GetValue();
        spec.maxUserConnections    = maxUserConns_->GetValue();
        spec.requireSsl            = requireSsl_->IsChecked();
    }

    wxString err;
    {
        wxBusyCursor busy;
        if (!conn_->SaveUser(spec, isNew_, err)) {
            wxMessageBox(tr(L"保存用户失败:") + L"\n\n" + err, tr(L"编辑用户"),
                         wxOK | wxICON_ERROR, this);
            return;   // keep the dialog open
        }
    }
    EndModal(wxID_OK);
}

} // namespace ui
