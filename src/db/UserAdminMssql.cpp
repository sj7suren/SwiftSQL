// UserAdminMssql.cpp — SQL Server LOGIN / server-role administration, driven
// through IConnection::Execute. See UserAdmin.h for the scope note.
//
// Scope (best-effort, P2): this manages SERVER-level principals — SQL logins and
// their membership in the eight fixed server roles. SQL Server layers a server
// LOGIN separately from per-database USERs; the database-user mapping + database
// roles are deliberately OUT of scope here (they need a per-database context switch
// and a much larger UI). The dialog therefore shows only 常规 + 服务器角色 for MSSQL.
//
// Model mapping:
//   UserSpec::globalPrivs ← fixed server-role membership checklist
// Sync: ALTER SERVER ROLE … ADD/DROP MEMBER (SQL Server 2012+), declarative diff.
#include "db/UserAdmin.h"
#include "db/DbDriver.h"

#include <algorithm>

namespace db {
namespace useradmin {
namespace {

wxString QLit(const wxString& s)   // 'escaped'
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}
wxString QId(const wxString& s)    // [escaped]
{
    wxString e = s;
    e.Replace(L"]", L"]]");
    return L"[" + e + L"]";
}

// The eight assignable fixed server roles (public is implicit — every login is a
// member and its membership can't be changed, so it's excluded from the checklist).
const std::vector<PrivilegeDef>& ServerRoles()
{
    static const std::vector<PrivilegeDef> k = {
        { L"sysadmin", L"sysadmin" }, { L"serveradmin", L"serveradmin" },
        { L"securityadmin", L"securityadmin" }, { L"processadmin", L"processadmin" },
        { L"setupadmin", L"setupadmin" }, { L"bulkadmin", L"bulkadmin" },
        { L"diskadmin", L"diskadmin" }, { L"dbcreator", L"dbcreator" },
    };
    return k;
}

bool Run(IConnection* c, const wxString& sql, wxString& err)
{
    QueryResult r;
    return c->Execute(sql, r, err);
}
void RunIgnore(IConnection* c, const wxString& sql)
{
    QueryResult r; wxString e;
    c->Execute(sql, r, e);
}

} // namespace

// ---------------------------------------------------------------------------
UserAdminModel MssqlModel()
{
    UserAdminModel m;
    m.supported           = true;
    m.hasHost             = false;
    m.hasAuthPlugin       = false;
    m.generalHint         = L"提示: 此处管理服务器登录 (LOGIN) 及其固定服务器角色;"
                            L"数据库级用户/角色映射暂不在此维护。编辑已有登录时不可改名。";
    m.userListSecondLabel = L"类型";
    m.serverTabLabel      = L"服务器角色";
    m.serverTabHint       = L"固定服务器角色 — 勾选本登录所属的角色:";
    m.serverPrivileges    = ServerRoles();
    m.hasDbTab            = false;
    m.hasRolesTab         = false;
    m.hasMembersTab       = false;
    m.hasResourceLimits   = false;
    return m;
}

// ---------------------------------------------------------------------------
bool MssqlListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err)
{
    out.clear();
    QueryResult r;
    if (!conn->Execute(
            L"SELECT name, type_desc FROM sys.server_principals "
            L"WHERE type IN ('S','U','G') AND name NOT LIKE '##%' ORDER BY name",
            r, err))
        return false;
    for (const auto& row : r.rows) {
        if (row.empty()) continue;
        UserInfo u;
        u.name = row[0];
        u.authPlugin = row.size() > 1 ? row[1] : wxString();
        out.push_back(std::move(u));
    }
    return true;
}

bool MssqlGetGrants(IConnection* conn, const wxString& user, UserGrants& out, wxString& err)
{
    out = UserGrants{};
    QueryResult r;
    if (!conn->Execute(
            L"SELECT r.name FROM sys.server_role_members m "
            L"JOIN sys.server_principals r ON r.principal_id = m.role_principal_id "
            L"JOIN sys.server_principals u ON u.principal_id = m.member_principal_id "
            L"WHERE u.name = " + QLit(user) + L" ORDER BY r.name", r, err))
        return false;
    for (const auto& row : r.rows)
        if (!row.empty()) out.globalPrivs.push_back(row[0]);
    return true;
}

bool MssqlSaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err)
{
    const wxString login = QId(spec.name);

    // ---- 1. CREATE / ALTER LOGIN (password) ----
    if (isNew) {
        wxString sql = L"CREATE LOGIN " + login + L" WITH PASSWORD = " +
                       QLit(spec.password);
        if (!Run(conn, sql, err)) return false;
    } else if (spec.setPassword && !spec.password.IsEmpty()) {
        if (!Run(conn, L"ALTER LOGIN " + login + L" WITH PASSWORD = " +
                       QLit(spec.password), err))
            return false;
    }

    // ---- 2. fixed server-role membership (declarative diff) ----
    UserGrants cur;
    if (!isNew) { wxString e; MssqlGetGrants(conn, spec.name, cur, e); }
    auto has = [](const std::vector<wxString>& v, const wxString& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    for (const wxString& r : cur.globalPrivs)
        if (!has(spec.globalPrivs, r))
            RunIgnore(conn, L"ALTER SERVER ROLE " + QId(r) + L" DROP MEMBER " + login);
    for (const wxString& r : spec.globalPrivs)
        if (!has(cur.globalPrivs, r))
            RunIgnore(conn, L"ALTER SERVER ROLE " + QId(r) + L" ADD MEMBER " + login);

    return true;
}

bool MssqlDropUser(IConnection* conn, const wxString& user, wxString& err)
{
    return Run(conn, L"DROP LOGIN " + QId(user), err);
}

} // namespace useradmin
} // namespace db
