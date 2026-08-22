// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// UserAdmin.h — database user / privilege administration value types + the
// per-dialect implementation helpers. These are consumed through IConnection's
// user-admin virtuals (ListUsers / GetUserGrants / SaveUser / DropUser +
// GetUserAdminModel / ListGrantableRoles); the default IConnection implementation
// reports "unsupported" and each real driver overrides them by delegating to the
// useradmin:: helpers below (which drive the connection purely through
// IConnection::Execute, so they hold no driver internals and live in their own
// translation units: UserAdmin.cpp = MySQL, UserAdminPg.cpp = PostgreSQL,
// UserAdminMssql.cpp = SQL Server, UserAdminOracle.cpp = Oracle/达梦).
//
// Model-driven UI: each dialect describes its account shape + privilege catalogs
// through a UserAdminModel; UserEditDialog renders itself from that model instead
// of hardcoding MySQL's shape (host field, auth plugin, *.* / db.* privilege sets).
//
// Scope note: MySQL + PostgreSQL are implemented end-to-end. SQL Server manages
// server logins + fixed server roles (database-user mapping is deliberately out of
// scope). Oracle/达梦 manage users + a curated system-privilege catalog + roles
// (object privileges / quotas / profiles are out of scope). Privileges outside a
// dialect's exposed catalog are preserved untouched on save (declarative sync only
// touches the catalog scope). SQLite has no user concept and stays unsupported.
#pragma once

#include <wx/string.h>
#include <vector>

namespace db {

class IConnection;

// One database account. For MySQL the identity is (name, host); engines without a
// host concept leave host empty.
struct UserInfo {
    wxString name;
    wxString host;        // MySQL host part ("%", "localhost"…); "" if N/A
    wxString authPlugin;  // authentication plugin (MySQL); "" if unknown
};

// A single privilege in the checkbox matrix: the SQL keyword + a short label.
struct PrivilegeDef {
    wxString name;    // "SELECT", "INSERT", "CREATE ROUTINE"…
    wxString label;   // localized short description (may equal name)
};

// Describes how UserEditDialog should render for a given dialect — the account
// shape (host? auth plugin?), which tabs to show, the tab labels/hints, and the
// server-level + database-level privilege catalogs. Built by each driver via the
// useradmin::*Model() helpers. Label/hint strings are the app's base language
// (Chinese); the dialog runs them through tr() so LangPackEn localizes them.
struct UserAdminModel {
    bool supported = false;

    // ---- 常规 (general) tab ----
    bool                  hasHost = false;         // MySQL true; PG/MSSQL/Oracle false
    bool                  hasAuthPlugin = false;   // MySQL only
    std::vector<wxString> authPlugins;             // auth-plugin combo choices
    wxString              generalHint;             // note shown under the general fields

    // Second column of the user-list tab (name is always col 0).
    wxString userListSecondLabel;   // "认证插件" / "角色属性" / "类型" / "账户状态"

    // ---- 服务器/系统权限 tab (checklist folded into UserSpec::globalPrivs) ----
    wxString                  serverTabLabel;      // "服务器权限"/"角色属性"/"系统权限"/"服务器角色"
    wxString                  serverTabHint;
    std::vector<PrivilegeDef> serverPrivileges;

    // ---- 数据库权限 tab (per-db checklist folded into UserSpec::dbPrivs) ----
    bool                      hasDbTab = false;
    wxString                  dbTabLabel;
    wxString                  dbTabHint;
    std::vector<PrivilegeDef> dbPrivileges;

    // ---- 隶属成员 / 成员 tabs ----
    bool     hasRolesTab   = false;   // roles/memberships granted to this account
    wxString rolesTabHint;
    bool     hasMembersTab = false;   // read-only reverse membership (MySQL only)

    // ---- 高级 (resource limits / SSL) tab ----
    bool hasResourceLimits = false;
};

// Privileges held on one database scope (database name unquoted; "*" = global,
// but global privileges travel in UserSpec::globalPrivs, not here).
struct DbPrivGrant {
    wxString              database;   // db name, unquoted
    std::vector<wxString> privs;      // privilege keywords
};

// The full editable spec for CREATE / ALTER USER + GRANT / REVOKE.
struct UserSpec {
    wxString name;
    wxString host = L"%";
    bool     setPassword = false;    // true → (re)set the password to `password`
    wxString password;
    wxString authPlugin;             // "" = leave / server default
    bool     globalGrantOption = false;   // WITH GRANT OPTION at *.*
    std::vector<wxString>    globalPrivs;  // *.* privilege keywords (excl. GRANT OPTION)
    std::vector<DbPrivGrant> dbPrivs;      // per-database privileges
    std::vector<wxString>    roles;        // MySQL 8 roles granted to this user ("name@host")
    // ---- advanced resource limits / SSL (emitted only if setResourceLimits) ----
    bool setResourceLimits     = false;
    long maxQueriesPerHour     = 0;   // 0 = unlimited
    long maxUpdatesPerHour     = 0;
    long maxConnectionsPerHour = 0;
    long maxUserConnections    = 0;
    bool requireSsl            = false;
};

// The parsed current grants of an existing account (drives the editor's initial
// checkbox state).
struct UserGrants {
    std::vector<wxString>    globalPrivs;         // expanded (ALL → full catalog)
    bool                     globalGrantOption = false;
    std::vector<DbPrivGrant> dbPrivs;
    std::vector<wxString>    roles;               // "name@host"
    wxString                 authPlugin;
};

namespace useradmin {

// The fixed MySQL privilege catalog. globalPrivileges is the server-level (*.* )
// set; dbPrivileges is what GRANT … ON db.* accepts. GRANT OPTION is exposed as
// a checkbox in both sets but handled specially on save (WITH GRANT OPTION).
const std::vector<PrivilegeDef>& MySqlGlobalPrivileges();
const std::vector<PrivilegeDef>& MySqlDbPrivileges();

// ---- MySQL (UserAdmin.cpp) ----
UserAdminModel MySqlModel();
bool MyListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err);
bool MyGetGrants(IConnection* conn, const wxString& user, const wxString& host,
                 UserGrants& out, wxString& err);
bool MySaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err);
bool MyDropUser(IConnection* conn, const wxString& user, const wxString& host,
                wxString& err);

// ---- PostgreSQL (UserAdminPg.cpp) ----
UserAdminModel PgModel();
bool PgListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err);
bool PgGetGrants(IConnection* conn, const wxString& user, UserGrants& out, wxString& err);
bool PgSaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err);
bool PgDropUser(IConnection* conn, const wxString& user, wxString& err);

// ---- SQL Server (UserAdminMssql.cpp) ----
UserAdminModel MssqlModel();
bool MssqlListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err);
bool MssqlGetGrants(IConnection* conn, const wxString& user, UserGrants& out, wxString& err);
bool MssqlSaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err);
bool MssqlDropUser(IConnection* conn, const wxString& user, wxString& err);

// ---- Oracle / 达梦 (UserAdminOracle.cpp) ----
UserAdminModel OracleModel();
bool OraListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err);
bool OraListRoles(IConnection* conn, std::vector<wxString>& out, wxString& err);
bool OraGetGrants(IConnection* conn, const wxString& user, UserGrants& out, wxString& err);
bool OraSaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err);
bool OraDropUser(IConnection* conn, const wxString& user, wxString& err);

} // namespace useradmin
} // namespace db
