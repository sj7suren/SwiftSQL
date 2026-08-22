// UserEditDialog.h — multi-tab create / edit dialog for a database account
// (Navicat-style user management). Dialect-adaptive: the dialog renders itself
// from the driver's db::UserAdminModel (conn->GetUserAdminModel()) instead of
// hardcoding one engine's shape. The caller guards on conn->SupportsUserAdmin().
//
// Tabs are shown per the model: 常规 (name / [host] / password / [auth-plugin]) ·
// 服务器权限|角色属性|系统权限|服务器角色 (server-level checklist) · [数据库权限]
// (per-database checklist) · [隶属成员] (roles granted to this account) · [成员]
// (read-only reverse membership, MySQL) · [高级] (resource limits / SSL, MySQL).
// Bracketed tabs appear only when the model enables them. On OK it assembles a
// db::UserSpec and calls conn->SaveUser (CREATE/ALTER + declarative GRANT/REVOKE).
#pragma once

#include "ui/CenteredDialog.h"
#include "db/UserAdmin.h"

#include <wx/string.h>
#include <map>
#include <set>
#include <vector>

class wxTextCtrl;
class wxChoice;
class wxCheckBox;
class wxCheckListBox;
class wxListBox;
class wxSpinCtrl;

namespace db { class IConnection; }

namespace ui {

class UserEditDialog : public CenteredDialog {
public:
    // `conn` must be live. When isNew, user/host seed the 常规 fields (host="%").
    // startPage selects the initially-shown notebook tab (used by "Privilege
    // Manager" → jump straight to 服务器权限).
    UserEditDialog(wxWindow* parent, db::IConnection* conn,
                   const wxString& user, const wxString& host, bool isNew,
                   int startPage = 0);

private:
    wxWindow* BuildGeneralTab(wxWindow* nb);
    wxWindow* BuildGlobalTab(wxWindow* nb);
    wxWindow* BuildDbTab(wxWindow* nb);
    wxWindow* BuildRolesTab(wxWindow* nb);
    wxWindow* BuildMembersTab(wxWindow* nb);
    wxWindow* BuildAdvancedTab(wxWindow* nb);

    void LoadInitial();         // pull grants / databases / accounts from the server
    void CommitDbSelection();   // fold the db-privilege checkboxes into dbPrivMap_
    void OnDbSelected();        // repopulate the db-privilege checkboxes for the picked db
    void OnOk();                // build the UserSpec + SaveUser

    db::IConnection* conn_;
    bool             isNew_;
    wxString         origUser_, origHost_;
    db::UserAdminModel model_;   // dialect shape + privilege catalogs (drives layout)

    // 常规
    wxTextCtrl* name_ = nullptr;
    wxTextCtrl* host_ = nullptr;
    wxTextCtrl* pw_   = nullptr;
    wxCheckBox* setPw_ = nullptr;
    wxChoice*   plugin_ = nullptr;

    // 服务器权限
    wxCheckListBox* globalList_ = nullptr;

    // 数据库权限
    wxListBox*      dbList_    = nullptr;
    wxCheckListBox* dbPrivs_   = nullptr;
    wxString        curDb_;     // db currently shown in dbPrivs_
    std::map<wxString, std::set<wxString>> dbPrivMap_;   // db → granted priv set

    // 隶属成员 / 成员
    wxCheckListBox* rolesList_   = nullptr;   // all accounts; checked = granted role
    wxListBox*      membersList_ = nullptr;   // read-only
    std::vector<wxString> allAccounts_;       // "user@host" for every server account

    // 高级
    wxCheckBox* resLimits_ = nullptr;
    wxSpinCtrl* maxQueries_ = nullptr;
    wxSpinCtrl* maxUpdates_ = nullptr;
    wxSpinCtrl* maxConns_   = nullptr;
    wxSpinCtrl* maxUserConns_ = nullptr;
    wxCheckBox* requireSsl_ = nullptr;

    db::UserGrants initial_;    // grants read for an existing account (empty if new)
};

} // namespace ui
