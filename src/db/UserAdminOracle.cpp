// UserAdminOracle.cpp — Oracle Database / 达梦(DM) user administration, driven
// through IConnection::Execute. Shared by BOTH Oracle drivers (the native OCI
// OracleOciConnection and the ODBC DmConnection — both report Dialect::Oracle and
// both talk pure SQL through Execute). See UserAdmin.h for the scope note.
//
// Scope (best-effort, P3): manages users, a CURATED system-privilege catalog, and
// roles. Object privileges (GRANT SELECT ON t TO u), tablespace quotas and PROFILE
// assignment are deliberately OUT of scope. System privileges NOT in the curated
// catalog are preserved untouched on save (declarative sync only diffs the catalog
// set + the role set) — we never silently revoke a privilege we don't surface.
//
// Notes / caveats:
//   * Requires DBA-level visibility (DBA_USERS / DBA_SYS_PRIVS / DBA_ROLE_PRIVS).
//   * Oracle folds unquoted identifiers to UPPERCASE. Catalog names are already
//     uppercase; the name field is disabled when editing an existing user, so
//     lookups use the catalog's uppercase name. New users are created quoted
//     (case preserved) — a lowercase name becomes case-sensitive.
//   * DROP USER is emitted WITHOUT CASCADE: a user that owns schema objects must be
//     dropped manually with CASCADE (the error is surfaced, not swallowed).
//   * Every statement is a single Execute with no trailing ';' or '/' (OCI-safe).
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
wxString QId(const wxString& s)    // "escaped"
{
    wxString e = s;
    e.Replace(L"\"", L"\"\"");
    return L"\"" + e + L"\"";
}
// Catalog lookups match on the uppercase, unquoted-folded form of the name.
wxString UpperLit(const wxString& s) { return QLit(s.Upper()); }

// Curated system-privilege catalog (names as they appear in DBA_SYS_PRIVS).
const std::vector<PrivilegeDef>& SysPrivileges()
{
    static const std::vector<PrivilegeDef> k = {
        { L"CREATE SESSION", L"CREATE SESSION" }, { L"CREATE TABLE", L"CREATE TABLE" },
        { L"CREATE VIEW", L"CREATE VIEW" }, { L"CREATE PROCEDURE", L"CREATE PROCEDURE" },
        { L"CREATE SEQUENCE", L"CREATE SEQUENCE" }, { L"CREATE TRIGGER", L"CREATE TRIGGER" },
        { L"CREATE SYNONYM", L"CREATE SYNONYM" }, { L"CREATE USER", L"CREATE USER" },
        { L"CREATE ROLE", L"CREATE ROLE" }, { L"ALTER USER", L"ALTER USER" },
        { L"DROP USER", L"DROP USER" }, { L"UNLIMITED TABLESPACE", L"UNLIMITED TABLESPACE" },
        { L"CREATE ANY TABLE", L"CREATE ANY TABLE" }, { L"ALTER ANY TABLE", L"ALTER ANY TABLE" },
        { L"DROP ANY TABLE", L"DROP ANY TABLE" }, { L"SELECT ANY TABLE", L"SELECT ANY TABLE" },
        { L"INSERT ANY TABLE", L"INSERT ANY TABLE" }, { L"UPDATE ANY TABLE", L"UPDATE ANY TABLE" },
        { L"DELETE ANY TABLE", L"DELETE ANY TABLE" }, { L"CREATE ANY VIEW", L"CREATE ANY VIEW" },
        { L"SELECT ANY DICTIONARY", L"SELECT ANY DICTIONARY" },
        { L"GRANT ANY PRIVILEGE", L"GRANT ANY PRIVILEGE" },
        { L"GRANT ANY ROLE", L"GRANT ANY ROLE" },
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
bool InCatalog(const wxString& p)
{
    for (const auto& d : SysPrivileges()) if (d.name == p) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
UserAdminModel OracleModel()
{
    UserAdminModel m;
    m.supported           = true;
    m.hasHost             = false;
    m.hasAuthPlugin       = false;
    m.generalHint         = L"提示: 需 DBA 权限;标识符默认折叠为大写。删除拥有对象的用户需手动 DROP USER … CASCADE。"
                            L"表空间配额/PROFILE/对象权限暂不在此维护。";
    m.userListSecondLabel = L"账户状态";
    m.serverTabLabel      = L"系统权限";
    m.serverTabHint       = L"系统权限 (System Privileges) — 勾选授予本用户的系统权限"
                            L"(目录外已有系统权限保持不变):";
    m.serverPrivileges    = SysPrivileges();
    m.hasDbTab            = false;
    m.hasRolesTab         = true;
    m.rolesTabHint        = L"角色 — 勾选授予本用户的角色 (GRANT role TO …):";
    m.hasMembersTab       = false;
    m.hasResourceLimits   = false;
    return m;
}

// ---------------------------------------------------------------------------
bool OraListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err)
{
    out.clear();
    QueryResult r;
    if (!conn->Execute(L"SELECT username, account_status FROM dba_users ORDER BY username",
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

bool OraListRoles(IConnection* conn, std::vector<wxString>& out, wxString& err)
{
    out.clear();
    QueryResult r;
    if (!conn->Execute(L"SELECT role FROM dba_roles ORDER BY role", r, err))
        return false;
    for (const auto& row : r.rows)
        if (!row.empty()) out.push_back(row[0]);
    return true;
}

bool OraGetGrants(IConnection* conn, const wxString& user, UserGrants& out, wxString& err)
{
    out = UserGrants{};
    const wxString g = UpperLit(user);

    // system privileges → globalPrivs
    QueryResult r;
    if (!conn->Execute(L"SELECT privilege FROM dba_sys_privs WHERE grantee = " + g +
                       L" ORDER BY privilege", r, err))
        return false;
    for (const auto& row : r.rows)
        if (!row.empty()) out.globalPrivs.push_back(row[0]);

    // roles → roles (best-effort)
    QueryResult rr; wxString e;
    if (conn->Execute(L"SELECT granted_role FROM dba_role_privs WHERE grantee = " + g +
                      L" ORDER BY granted_role", rr, e))
        for (const auto& row : rr.rows)
            if (!row.empty()) out.roles.push_back(row[0]);
    return true;
}

bool OraSaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err)
{
    const wxString acct = QId(spec.name);

    // ---- 1. CREATE / ALTER USER (identity) ----
    if (isNew) {
        wxString sql = L"CREATE USER " + acct + L" IDENTIFIED BY " + QId(spec.password);
        if (!Run(conn, sql, err)) return false;
    } else if (spec.setPassword && !spec.password.IsEmpty()) {
        if (!Run(conn, L"ALTER USER " + acct + L" IDENTIFIED BY " + QId(spec.password), err))
            return false;
    }

    UserGrants cur;
    if (!isNew) { wxString e; OraGetGrants(conn, spec.name, cur, e); }
    auto has = [](const std::vector<wxString>& v, const wxString& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };

    // ---- 2. system privileges (declarative diff, catalog scope only) ----
    for (const wxString& p : cur.globalPrivs)
        if (InCatalog(p) && !has(spec.globalPrivs, p))
            RunIgnore(conn, L"REVOKE " + p + L" FROM " + acct);
    for (const wxString& p : spec.globalPrivs)
        if (!has(cur.globalPrivs, p)) {
            if (!Run(conn, L"GRANT " + p + L" TO " + acct, err)) return false;
        }

    // ---- 3. roles (declarative diff) ----
    for (const wxString& r : cur.roles)
        if (!has(spec.roles, r)) RunIgnore(conn, L"REVOKE " + QId(r) + L" FROM " + acct);
    for (const wxString& r : spec.roles)
        if (!has(cur.roles, r)) RunIgnore(conn, L"GRANT " + QId(r) + L" TO " + acct);

    return true;
}

bool OraDropUser(IConnection* conn, const wxString& user, wxString& err)
{
    return Run(conn, L"DROP USER " + QId(user), err);
}

} // namespace useradmin
} // namespace db
