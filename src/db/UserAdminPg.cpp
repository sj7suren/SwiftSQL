// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// UserAdminPg.cpp — PostgreSQL role / privilege administration, driven entirely
// through IConnection::Execute (no libpq here). See UserAdmin.h for the scope note.
//
// Model mapping (how the generic UserSpec / UserGrants map onto PG concepts):
//   UserSpec::globalPrivs  ← role ATTRIBUTES (LOGIN/SUPERUSER/CREATEDB/…) checklist
//   UserSpec::dbPrivs      ← per-database privileges (CONNECT/CREATE/TEMP)
//   UserSpec::roles        ← role memberships granted to this role (GRANT role TO x)
//
// Sync strategy (declarative):
//   attributes  ALTER ROLE … WITH — emit ONLY the tokens that differ from the
//               role's current state (for a new role, from CREATE ROLE defaults),
//               so SUPERUSER/BYPASSRLS are never touched unless actually changed
//               (they need superuser to set — minimizing privilege friction).
//   db privs    for every db in (current ∪ desired): REVOKE ALL (best-effort) then
//               GRANT the checked set ON DATABASE.
//   memberships diff GRANT/REVOKE against the current membership set.
// Password is set with ALTER/CREATE … PASSWORD 'literal' only when requested.
#include "db/UserAdmin.h"
#include "db/DbDriver.h"

#include <algorithm>
#include <set>

namespace db {
namespace useradmin {
namespace {

// ---- escaping ----------------------------------------------------------------
wxString QLit(const wxString& s)   // 'escaped'  (single quotes doubled)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}
wxString QId(const wxString& s)    // "escaped"  (double quotes doubled)
{
    wxString e = s;
    e.Replace(L"\"", L"\"\"");
    return L"\"" + e + L"\"";
}

// ---- role attribute catalog (the 服务器权限/角色属性 checklist) --------------
struct Attr { const wxChar* kw; const wxChar* neg; };
const std::vector<Attr>& AttrCatalog()
{
    static const std::vector<Attr> k = {
        { L"LOGIN",       L"NOLOGIN" },
        { L"SUPERUSER",   L"NOSUPERUSER" },
        { L"CREATEDB",    L"NOCREATEDB" },
        { L"CREATEROLE",  L"NOCREATEROLE" },
        { L"REPLICATION", L"NOREPLICATION" },
        { L"BYPASSRLS",   L"NOBYPASSRLS" },
        { L"INHERIT",     L"NOINHERIT" },
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
UserAdminModel PgModel()
{
    UserAdminModel m;
    m.supported           = true;
    m.hasHost             = false;
    m.hasAuthPlugin       = false;
    m.generalHint         = L"提示: PostgreSQL 用户即\"角色\"(ROLE);勾选 LOGIN 才可作为登录账户。"
                            L"编辑已有角色时不可改名(重命名为独立操作)。";
    m.userListSecondLabel = L"角色属性";
    m.serverTabLabel      = L"角色属性";
    m.serverTabHint       = L"角色属性 — 作用于整个数据库集群:";
    m.serverPrivileges    = {
        { L"LOGIN", L"LOGIN" }, { L"SUPERUSER", L"SUPERUSER" },
        { L"CREATEDB", L"CREATEDB" }, { L"CREATEROLE", L"CREATEROLE" },
        { L"REPLICATION", L"REPLICATION" }, { L"BYPASSRLS", L"BYPASSRLS" },
        { L"INHERIT", L"INHERIT" },
    };
    m.hasDbTab            = true;
    m.dbTabLabel          = L"数据库权限";
    m.dbTabHint           = L"该库权限 (ON DATABASE)";
    m.dbPrivileges        = {
        { L"CONNECT", L"CONNECT" }, { L"CREATE", L"CREATE" }, { L"TEMP", L"TEMP" },
    };
    m.hasRolesTab         = true;
    m.rolesTabHint        = L"隶属成员 — 勾选授予本角色的其他角色 (GRANT role TO …):";
    m.hasMembersTab       = false;
    m.hasResourceLimits   = false;
    return m;
}

// ---------------------------------------------------------------------------
bool PgListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err)
{
    out.clear();
    QueryResult r;
    if (!conn->Execute(
            L"SELECT rolname, rolcanlogin, rolsuper, rolcreatedb, rolcreaterole, "
            L"rolreplication FROM pg_roles WHERE rolname NOT LIKE 'pg\\_%' "
            L"ORDER BY rolname", r, err))
        return false;
    for (const auto& row : r.rows) {
        if (row.empty()) continue;
        UserInfo u;
        u.name = row[0];
        // Second-column summary: the short list of set attributes.
        auto on = [&](size_t i) { return row.size() > i && row[i] == L"t"; };
        std::vector<wxString> tags;
        if (on(1)) tags.push_back(L"LOGIN");
        if (on(2)) tags.push_back(L"SUPERUSER");
        if (on(3)) tags.push_back(L"CREATEDB");
        if (on(4)) tags.push_back(L"CREATEROLE");
        if (on(5)) tags.push_back(L"REPLICATION");
        for (size_t i = 0; i < tags.size(); ++i)
            u.authPlugin += (i ? L", " : L"") + tags[i];
        out.push_back(std::move(u));
    }
    return true;
}

bool PgGetGrants(IConnection* conn, const wxString& user, UserGrants& out, wxString& err)
{
    out = UserGrants{};

    // 1. role attributes → globalPrivs
    {
        QueryResult r;
        if (!conn->Execute(
                L"SELECT rolcanlogin, rolsuper, rolcreatedb, rolcreaterole, "
                L"rolreplication, rolbypassrls, rolinherit FROM pg_roles "
                L"WHERE rolname = " + QLit(user), r, err))
            return false;
        if (!r.rows.empty()) {
            const auto& row = r.rows[0];
            auto on = [&](size_t i) { return row.size() > i && row[i] == L"t"; };
            if (on(0)) out.globalPrivs.push_back(L"LOGIN");
            if (on(1)) out.globalPrivs.push_back(L"SUPERUSER");
            if (on(2)) out.globalPrivs.push_back(L"CREATEDB");
            if (on(3)) out.globalPrivs.push_back(L"CREATEROLE");
            if (on(4)) out.globalPrivs.push_back(L"REPLICATION");
            if (on(5)) out.globalPrivs.push_back(L"BYPASSRLS");
            if (on(6)) out.globalPrivs.push_back(L"INHERIT");
        }
    }

    // 2. memberships → roles (best-effort)
    {
        QueryResult r; wxString e;
        if (conn->Execute(
                L"SELECT g.rolname FROM pg_auth_members m "
                L"JOIN pg_roles g ON g.oid = m.roleid "
                L"JOIN pg_roles u ON u.oid = m.member "
                L"WHERE u.rolname = " + QLit(user) + L" ORDER BY g.rolname", r, e))
            for (const auto& row : r.rows)
                if (!row.empty()) out.roles.push_back(row[0]);
    }

    // 3. per-database privileges → dbPrivs (direct grants only, via aclexplode).
    {
        QueryResult r; wxString e;
        if (conn->Execute(
                L"SELECT d.datname, a.privilege_type "
                L"FROM pg_database d, LATERAL aclexplode(d.datacl) a "
                L"JOIN pg_roles r ON r.oid = a.grantee "
                L"WHERE r.rolname = " + QLit(user) +
                L" AND d.datistemplate = false ORDER BY d.datname", r, e)) {
            for (const auto& row : r.rows) {
                if (row.size() < 2) continue;
                wxString db = row[0];
                wxString pv = row[1];
                if (pv == L"TEMPORARY") pv = L"TEMP";
                DbPrivGrant* g = nullptr;
                for (auto& x : out.dbPrivs) if (x.database == db) { g = &x; break; }
                if (!g) { out.dbPrivs.push_back({ db, {} }); g = &out.dbPrivs.back(); }
                g->privs.push_back(pv);
            }
        }
    }
    return true;
}

// Build the WITH-attribute token list = the attributes whose state changes from
// `cur` (current/on set) to `want` (checked set). Empty → no attribute change.
static wxString AttrDelta(const std::set<wxString>& want, const std::set<wxString>& cur)
{
    wxString out;
    for (const Attr& a : AttrCatalog()) {
        const bool w = want.count(a.kw) > 0;
        const bool c = cur.count(a.kw) > 0;
        if (w == c) continue;
        if (!out.IsEmpty()) out += L" ";
        out += w ? a.kw : a.neg;
    }
    return out;
}

bool PgSaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err)
{
    const wxString role = QId(spec.name);

    std::set<wxString> want(spec.globalPrivs.begin(), spec.globalPrivs.end());

    // Current state: read existing grants; for a new role use CREATE ROLE defaults
    // (only INHERIT is on by default) so unchecked attributes need no negation.
    UserGrants cur;
    std::set<wxString> curAttr;
    if (isNew) curAttr.insert(L"INHERIT");
    else {
        wxString e;
        PgGetGrants(conn, spec.name, cur, e);
        curAttr.insert(cur.globalPrivs.begin(), cur.globalPrivs.end());
    }

    // ---- 1. CREATE / ALTER ROLE with the changed attributes (+ password) ----
    const wxString attrs = AttrDelta(want, curAttr);
    wxString pwClause;
    if (spec.setPassword && !spec.password.IsEmpty())
        pwClause = L" PASSWORD " + QLit(spec.password);

    if (isNew) {
        wxString sql = L"CREATE ROLE " + role;
        if (!attrs.IsEmpty() || !pwClause.IsEmpty())
            sql += L" WITH " + attrs + pwClause;
        if (!Run(conn, sql, err)) return false;
    } else if (!attrs.IsEmpty() || !pwClause.IsEmpty()) {
        wxString sql = L"ALTER ROLE " + role + L" WITH " + attrs + pwClause;
        if (!Run(conn, sql, err)) return false;
    }

    // ---- 2. per-database privileges (declarative) ----
    std::set<wxString> dbs;
    for (const auto& g : cur.dbPrivs)  dbs.insert(g.database);
    for (const auto& g : spec.dbPrivs) dbs.insert(g.database);
    for (const wxString& db : dbs) {
        RunIgnore(conn, L"REVOKE ALL PRIVILEGES ON DATABASE " + QId(db) + L" FROM " + role);
        const DbPrivGrant* wantDb = nullptr;
        for (const auto& g : spec.dbPrivs) if (g.database == db) { wantDb = &g; break; }
        if (!wantDb || wantDb->privs.empty()) continue;
        wxString list;
        for (const auto& p : wantDb->privs) {
            if (!list.IsEmpty()) list += L", ";
            list += p;
        }
        if (!Run(conn, L"GRANT " + list + L" ON DATABASE " + QId(db) + L" TO " + role, err))
            return false;
    }

    // ---- 3. memberships (declarative diff) ----
    auto has = [](const std::vector<wxString>& v, const wxString& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    for (const wxString& r : cur.roles)
        if (!has(spec.roles, r)) RunIgnore(conn, L"REVOKE " + QId(r) + L" FROM " + role);
    for (const wxString& r : spec.roles)
        if (!has(cur.roles, r)) RunIgnore(conn, L"GRANT " + QId(r) + L" TO " + role);

    return true;
}

bool PgDropUser(IConnection* conn, const wxString& user, wxString& err)
{
    return Run(conn, L"DROP ROLE " + QId(user), err);
}

} // namespace useradmin
} // namespace db
