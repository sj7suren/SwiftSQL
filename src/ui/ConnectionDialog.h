// ConnectionDialog.h — Navicat/DBeaver-class connection editor. Engine is fixed
// (chosen from the menu); the dialog shows it in a header. Tabs: 常规 / 高级 /
// 数据库 / SSL / SSH / HTTP, with auth-method dropdowns that reveal the relevant
// fields (e.g. SSL key + passphrase, HTTP Basic/Digest auth).
#pragma once

#include <wx/dialog.h>
#include "core/ConnectionProfile.h"
#include "ui/CenteredDialog.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

namespace ui {

class ConnectionDialog : public CenteredDialog {
public:
    ConnectionDialog(wxWindow* parent, db::DbType preset = db::DbType::MySQL,
                     const core::ConnectionProfile* edit = nullptr);

    core::ConnectionProfile GetProfile() const;

private:
    wxWindow* BuildGeneralTab(wxWindow* nb);
    wxWindow* BuildAdvancedTab(wxWindow* nb);
    wxWindow* BuildDatabasesTab(wxWindow* nb);
    wxWindow* BuildSslTab(wxWindow* nb);
    wxWindow* BuildSshTab(wxWindow* nb);
    wxWindow* BuildHttpTab(wxWindow* nb);

    void SelectType(db::DbType type);
    void LoadInto(const core::ConnectionProfile& p);
    void UpdateSslFields();
    void UpdateSshFields();
    void UpdateHttpFields();

    void OnTestConnection(wxCommandEvent&);
    void OnBrowseFile(wxCommandEvent&);   // SQLite: pick the .db file

    // General-tab rows toggled for file-based engines (SQLite hides host/port/…).
    wxWindow*     generalPanel_ = nullptr;
    wxStaticText* hostLabel_ = nullptr;
    wxStaticText* portLabel_ = nullptr;
    wxStaticText* userLabel_ = nullptr;
    wxStaticText* passwordLabel_ = nullptr;
    wxStaticText* databaseLabel_ = nullptr;
    wxButton*     browseFile_ = nullptr;
    // Network tabs disabled for SQLite (no SSL/SSH/HTTP).
    wxWindow* sslPage_ = nullptr;
    wxWindow* sshPage_ = nullptr;
    wxWindow* httpPage_ = nullptr;

    db::DbType selected_ = db::DbType::MySQL;

    // 常规
    wxTextCtrl* name_ = nullptr;
    wxTextCtrl* host_ = nullptr;
    wxSpinCtrl* port_ = nullptr;
    wxTextCtrl* user_ = nullptr;
    wxTextCtrl* password_ = nullptr;
    wxTextCtrl* database_ = nullptr;
    wxCheckBox* savePassword_ = nullptr;

    // 高级
    wxChoice*   charset_ = nullptr;
    wxSpinCtrl* timeout_ = nullptr;
    wxSpinCtrl* rwTimeout_ = nullptr;
    wxCheckBox* keepalive_ = nullptr;
    wxSpinCtrl* keepaliveInterval_ = nullptr;
    wxCheckBox* autoReconnect_ = nullptr;
    wxCheckBox* compression_ = nullptr;
    wxTextCtrl* timezone_ = nullptr;
    wxTextCtrl* appName_ = nullptr;
    wxTextCtrl* initCommands_ = nullptr;
    wxTextCtrl* socketFile_ = nullptr;
    wxTextCtrl* defaultSchema_ = nullptr;
    wxCheckBox* readOnly_ = nullptr;
    wxChoice*   isolation_ = nullptr;

    // 数据库
    wxChoice*   dbListMode_ = nullptr;
    wxTextCtrl* dbFilter_ = nullptr;
    wxCheckBox* showSystemDbs_ = nullptr;
    wxCheckBox* autoConnect_ = nullptr;

    // SSL
    wxCheckBox*   sslEnable_ = nullptr;
    wxChoice*     sslMode_ = nullptr;
    wxStaticText* sslModeLabel_ = nullptr;
    wxChoice*     sslAuth_ = nullptr;
    wxTextCtrl*   sslCa_ = nullptr;
    wxTextCtrl*   sslCert_ = nullptr;
    wxStaticText* sslCertLabel_ = nullptr;
    wxTextCtrl*   sslKey_ = nullptr;
    wxStaticText* sslKeyLabel_ = nullptr;
    wxTextCtrl*   sslKeyPass_ = nullptr;
    wxStaticText* sslKeyPassLabel_ = nullptr;
    wxCheckBox*   sslVerify_ = nullptr;
    wxCheckBox*   sslVerifyHost_ = nullptr;
    wxTextCtrl*   sslCipher_ = nullptr;
    wxChoice*     sslMinVer_ = nullptr;

    // SSH
    wxCheckBox*   sshEnable_ = nullptr;
    wxTextCtrl*   sshHost_ = nullptr;
    wxSpinCtrl*   sshPort_ = nullptr;
    wxTextCtrl*   sshUser_ = nullptr;
    wxChoice*     sshAuth_ = nullptr;
    wxTextCtrl*   sshPassword_ = nullptr;
    wxStaticText* sshPasswordLabel_ = nullptr;
    wxTextCtrl*   sshKey_ = nullptr;
    wxStaticText* sshKeyLabel_ = nullptr;
    wxTextCtrl*   sshKeyPass_ = nullptr;
    wxStaticText* sshKeyPassLabel_ = nullptr;
    wxSpinCtrl*   sshLocalPort_ = nullptr;
    wxSpinCtrl*   sshKeepalive_ = nullptr;
    wxCheckBox*   sshCompression_ = nullptr;

    // HTTP
    wxCheckBox*   httpEnable_ = nullptr;
    wxTextCtrl*   httpUrl_ = nullptr;
    wxChoice*     httpAuth_ = nullptr;
    wxTextCtrl*   httpUser_ = nullptr;
    wxStaticText* httpUserLabel_ = nullptr;
    wxTextCtrl*   httpPassword_ = nullptr;
    wxStaticText* httpPasswordLabel_ = nullptr;
    wxCheckBox*   httpVerify_ = nullptr;
};

} // namespace ui
