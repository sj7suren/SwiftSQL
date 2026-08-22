// UserAdmin.cpp — MySQL user / privilege administration, driven entirely through
// IConnection::Execute (no libmariadb here). See UserAdmin.h for the scope note.
//
// Privilege sync strategy (declarative): on save we re-read the account's current
// grants, then bring each scope to the desired state by REVOKE-all-then-GRANT:
//   *.*      REVOKE ALL PRIVILEGES, GRANT OPTION … then GRANT the checked set
//   db.*     for every db in (current ∪ desired): REVOKE all, then GRANT the checked set
//   roles    diff grant/revoke (MySQL 8; best-effort — a 5.7 server just ignores)
// The revoke steps are best-effort (a brand-new user has nothing to revoke, which
// MySQL reports as an error); the GRANTs of the desired set are hard errors.
#include "db/UserAdmin.h"
#include "db/DbDriver.h"

#include <algorithm>

namespace db {
namespace useradmin {
namespace {

// ---- SQL literal / identifier escaping ---------------------------------------
wxString QLit(const wxString& s)   // 'escaped'
{
    wxString e = s;
    e.Replace(L"\\", L"\\\\");
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}
wxString QIdent(const wxString& s) // `escaped`
{
    wxString e = s;
    e.Replace(L"`", L"``");
    return L"`" + e + L"`";
}
wxString QAcct(const wxString& user, const wxString& host)   // 'u'@'h'
{
    return QLit(user) + L"@" + QLit(host);
}
// Split a stored role token "name@host" (host optional) into its parts.
void SplitAccount(const wxString& tok, wxString& name, wxString& host)
{
    const int at = tok.Find(L'@', /*fromEnd*/ true);
    if (at == wxNOT_FOUND) { name = tok; host = L"%"; }
    else { name = tok.Left(at); host = tok.Mid(at + 1); }
}

// ---- Execute helpers ---------------------------------------------------------
bool Run(IConnection* c, const wxString& sql, wxString& err)
{
    QueryResult r;
    return c->Execute(sql, r, err);
}
void RunIgnore(IConnection* c, const wxString& sql)   // best-effort (revoke/role)
{
    QueryResult r; wxString e;
    c->Execute(sql, r, e);
}

// ---- SHOW GRANTS parsing -----------------------------------------------------
// Read a quoted token (backtick or single-quote) at s[i]; doubled quote → literal.
// On success sets out, advances i past the closing quote, returns true.
bool ReadQuoted(const wxString& s, size_t& i, wxString& out)
{
    if (i >= s.size()) return false;
    const wxChar q = s[i];
    if (q != L'`' && q != L'\'') return false;
    ++i; out.clear();
    while (i < s.size()) {
        if (s[i] == q) {
            if (i + 1 < s.size() && s[i + 1] == q) { out += q; i += 2; continue; }
            ++i; return true;
        }
        out += s[i]; ++i;
    }
    return false;
}
// Read an account "`name`@`host`" (or 'quoted' form) at s[i]. Advances i.
bool ReadAccount(const wxString& s, size_t& i, wxString& name, wxString& host)
{
    while (i < s.size() && s[i] == L' ') ++i;
    if (!ReadQuoted(s, i, name)) return false;
    host = L"%";
    if (i < s.size() && s[i] == L'@') { ++i; ReadQuoted(s, i, host); }
    return true;
}
// Parse the object between " ON " and " TO " (e.g. "*.*", "`db`.*", "`db`.`t`").
// scope: 0 = global(*.*), 1 = db.*, 2 = table (db + table set).
void ParseObject(const wxString& obj, wxString& db, wxString& tbl, int& scope)
{
    db.clear(); tbl.clear(); scope = 0;
    size_t i = 0;
    wxString left, right;
    if (i < obj.size() && obj[i] == L'*') { left = L"*"; ++i; }
    else ReadQuoted(obj, i, left);
    if (i < obj.size() && obj[i] == L'.') ++i;
    if (i < obj.size() && obj[i] == L'*') { right = L"*"; ++i; }
    else ReadQuoted(obj, i, right);

    if (left == L"*" && right == L"*") { scope = 0; return; }
    db = left;
    if (right == L"*") { scope = 1; return; }
    tbl = right; scope = 2;
}
// Split a privilege list on top-level commas, dropping any "(col,col)" segments,
// trimming + upper-casing each keyword.
std::vector<wxString> SplitPrivs(const wxString& s)
{
    std::vector<wxString> out;
    wxString cur;
    int depth = 0;
    auto flush = [&] {
        wxString t = cur; t.Trim(true).Trim(false);
        if (!t.IsEmpty()) out.push_back(t.Upper());
        cur.clear();
    };
    for (wxChar ch : s) {
        if (ch == L'(') { ++depth; continue; }
        if (ch == L')') { if (depth) --depth; continue; }
        if (depth > 0) continue;
        if (ch == L',') flush();
        else cur += ch;
    }
    flush();
    return out;
}
// Expand a parsed keyword list into concrete catalog privileges: "ALL PRIVILEGES"
// → every keyword of `catalog`; "USAGE" → nothing; "GRANT OPTION" → set the flag.
void ExpandPrivs(const std::vector<wxString>& raw,
                 const std::vector<PrivilegeDef>& catalog,
                 std::vector<wxString>& out, bool& grantOption)
{
    for (const wxString& p : raw) {
        if (p == L"USAGE") continue;
        if (p == L"GRANT OPTION") { grantOption = true; continue; }
        if (p == L"ALL" || p == L"ALL PRIVILEGES") {
            for (const auto& d : catalog)
                if (d.name != L"GRANT OPTION") out.push_back(d.name);
            continue;
        }
        out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Fixed MySQL privilege catalogs.
const std::vector<PrivilegeDef>& MySqlGlobalPrivileges()
{
    static const std::vector<PrivilegeDef> kGlobal = {
        { L"SELECT", L"SELECT" }, { L"INSERT", L"INSERT" }, { L"UPDATE", L"UPDATE" },
        { L"DELETE", L"DELETE" }, { L"CREATE", L"CREATE" }, { L"DROP", L"DROP" },
        { L"RELOAD", L"RELOAD" }, { L"SHUTDOWN", L"SHUTDOWN" }, { L"PROCESS", L"PROCESS" },
        { L"FILE", L"FILE" }, { L"REFERENCES", L"REFERENCES" }, { L"INDEX", L"INDEX" },
        { L"ALTER", L"ALTER" }, { L"SHOW DATABASES", L"SHOW DATABASES" },
        { L"SUPER", L"SUPER" }, { L"CREATE TEMPORARY TABLES", L"CREATE TEMPORARY TABLES" },
        { L"LOCK TABLES", L"LOCK TABLES" }, { L"EXECUTE", L"EXECUTE" },
        { L"REPLICATION SLAVE", L"REPLICATION SLAVE" },
        { L"REPLICATION CLIENT", L"REPLICATION CLIENT" },
        { L"CREATE VIEW", L"CREATE VIEW" }, { L"SHOW VIEW", L"SHOW VIEW" },
        { L"CREATE ROUTINE", L"CREATE ROUTINE" }, { L"ALTER ROUTINE", L"ALTER ROUTINE" },
        { L"CREATE USER", L"CREATE USER" }, { L"EVENT", L"EVENT" },
        { L"TRIGGER", L"TRIGGER" }, { L"CREATE TABLESPACE", L"CREATE TABLESPACE" },
        { L"GRANT OPTION", L"GRANT OPTION" },
    };
    return kGlobal;
}
const std::vector<PrivilegeDef>& MySqlDbPrivileges()
{
    static const std::vector<PrivilegeDef> kDb = {
        { L"SELECT", L"SELECT" }, { L"INSERT", L"INSERT" }, { L"UPDATE", L"UPDATE" },
        { L"DELETE", L"DELETE" }, { L"CREATE", L"CREATE" }, { L"DROP", L"DROP" },
        { L"REFERENCES", L"REFERENCES" }, { L"INDEX", L"INDEX" }, { L"ALTER", L"ALTER" },
        { L"CREATE TEMPORARY TABLES", L"CREATE TEMPORARY TABLES" },
        { L"LOCK TABLES", L"LOCK TABLES" }, { L"EXECUTE", L"EXECUTE" },
        { L"CREATE VIEW", L"CREATE VIEW" }, { L"SHOW VIEW", L"SHOW VIEW" },
        { L"CREATE ROUTINE", L"CREATE ROUTINE" }, { L"ALTER ROUTINE", L"ALTER ROUTINE" },
        { L"EVENT", L"EVENT" }, { L"TRIGGER", L"TRIGGER" },
        { L"GRANT OPTION", L"GRANT OPTION" },
    };
    return kDb;
}

// ---------------------------------------------------------------------------
UserAdminModel MySqlModel()
{
    UserAdminModel m;
    m.supported            = true;
    m.hasHost              = true;
    m.hasAuthPlugin        = true;
    m.authPlugins          = { L"mysql_native_password", L"caching_sha2_password",
                               L"sha256_password" };
    m.generalHint          = L"提示: 主机 % 表示允许任意来源;编辑已有用户时不可改用户名/主机(重命名为独立操作)。";
    m.userListSecondLabel  = L"认证插件";
    m.serverTabLabel       = L"服务器权限";
    m.serverTabHint        = L"全局权限 (*.*) — 作用于服务器上所有数据库:";
    m.serverPrivileges     = MySqlGlobalPrivileges();
    m.hasDbTab             = true;
    m.dbTabLabel           = L"数据库权限";
    m.dbTabHint            = L"该库权限 (db.*)";
    m.dbPrivileges         = MySqlDbPrivileges();
    m.hasRolesTab          = true;
    m.rolesTabHint         = L"隶属成员 — 勾选授予本账户的角色 (MySQL 8;5.7 忽略):";
    m.hasMembersTab        = true;
    m.hasResourceLimits    = true;
    return m;
}

// ---------------------------------------------------------------------------
bool MyListUsers(IConnection* conn, std::vector<UserInfo>& out, wxString& err)
{
    out.clear();
    QueryResult r;
    if (!conn->Execute(L"SELECT User, Host, plugin FROM mysql.user ORDER BY User, Host",
                       r, err))
        return false;
    for (const auto& row : r.rows) {
        UserInfo u;
        u.name = row.size() > 0 ? row[0] : wxString();
        u.host = row.size() > 1 ? row[1] : wxString();
        u.authPlugin = row.size() > 2 ? row[2] : wxString();
        out.push_back(std::move(u));
    }
    return true;
}

bool MyGetGrants(IConnection* conn, const wxString& user, const wxString& host,
                 UserGrants& out, wxString& err)
{
    out = UserGrants{};

    // authentication plugin (best-effort).
    {
        QueryResult r; wxString e;
        if (conn->Execute(L"SELECT plugin FROM mysql.user WHERE User=" + QLit(user) +
                          L" AND Host=" + QLit(host), r, e) &&
            !r.rows.empty() && !r.rows[0].empty())
            out.authPlugin = r.rows[0][0];
    }

    QueryResult r;
    if (!conn->Execute(L"SHOW GRANTS FOR " + QAcct(user, host), r, err))
        return false;

    const auto& globalCat = MySqlGlobalPrivileges();
    const auto& dbCat     = MySqlDbPrivileges();

    for (const auto& row : r.rows) {
        if (row.empty()) continue;
        wxString g = row[0];
        if (!g.StartsWith(L"GRANT ")) continue;
        g = g.Mid(6);

        const int onPos = g.Find(L" ON ");
        if (onPos == wxNOT_FOUND) {
            // Role grant: "`role`@`host`[, …] TO `user`@`host`".
            const int toPos = g.Find(L" TO ");
            wxString rolesPart = (toPos == wxNOT_FOUND) ? g : g.Left(toPos);
            size_t i = 0;
            while (i < rolesPart.size()) {
                wxString rn, rh;
                if (!ReadAccount(rolesPart, i, rn, rh)) break;
                out.roles.push_back(rn + L"@" + rh);
                while (i < rolesPart.size() && (rolesPart[i] == L',' || rolesPart[i] == L' ')) ++i;
            }
            continue;
        }

        const wxString privPart = g.Left(onPos);
        wxString rest = g.Mid(onPos + 4);          // after " ON "
        const int toPos = rest.Find(L" TO ");
        const wxString objPart = (toPos == wxNOT_FOUND) ? rest : rest.Left(toPos);
        const wxString tail     = (toPos == wxNOT_FOUND) ? wxString() : rest.Mid(toPos + 4);
        const bool grantOpt = tail.Contains(L"WITH GRANT OPTION");

        wxString db, tbl; int scope = 0;
        ParseObject(objPart, db, tbl, scope);
        if (scope == 2) continue;                  // table/column level → preserved, not surfaced

        std::vector<wxString> raw = SplitPrivs(privPart);
        std::vector<wxString> expanded;
        bool go = false;
        ExpandPrivs(raw, scope == 0 ? globalCat : dbCat, expanded, go);

        if (scope == 0) {
            for (auto& p : expanded) out.globalPrivs.push_back(p);
            if (go || grantOpt) out.globalGrantOption = true;
        } else {
            if (go || grantOpt) expanded.push_back(L"GRANT OPTION");
            DbPrivGrant dg; dg.database = db; dg.privs = std::move(expanded);
            if (!dg.privs.empty()) out.dbPrivs.push_back(std::move(dg));
        }
    }
    std::sort(out.globalPrivs.begin(), out.globalPrivs.end());
    out.globalPrivs.erase(std::unique(out.globalPrivs.begin(), out.globalPrivs.end()),
                          out.globalPrivs.end());
    return true;
}

bool MySaveUser(IConnection* conn, const UserSpec& spec, bool isNew, wxString& err)
{
    const wxString acct = QAcct(spec.name, spec.host);

    // ---- 1. account existence + identity ----
    if (isNew) {
        wxString sql = L"CREATE USER " + acct;
        if (!spec.authPlugin.IsEmpty() && spec.setPassword)
            sql += L" IDENTIFIED WITH " + QIdent(spec.authPlugin) + L" BY " + QLit(spec.password);
        else if (!spec.authPlugin.IsEmpty())
            sql += L" IDENTIFIED WITH " + QIdent(spec.authPlugin);
        else if (spec.setPassword)
            sql += L" IDENTIFIED BY " + QLit(spec.password);
        if (!Run(conn, sql, err)) return false;
    } else if (spec.setPassword || !spec.authPlugin.IsEmpty()) {
        wxString sql = L"ALTER USER " + acct;
        if (!spec.authPlugin.IsEmpty() && spec.setPassword)
            sql += L" IDENTIFIED WITH " + QIdent(spec.authPlugin) + L" BY " + QLit(spec.password);
        else if (!spec.authPlugin.IsEmpty())
            sql += L" IDENTIFIED WITH " + QIdent(spec.authPlugin);
        else
            sql += L" IDENTIFIED BY " + QLit(spec.password);
        if (!Run(conn, sql, err)) return false;
    }

    // Current grants (for computing the db scopes to reset). New users have none.
    UserGrants cur;
    if (!isNew) { wxString e; MyGetGrants(conn, spec.name, spec.host, cur, e); }

    // ---- 2. global (*.* ) privileges: revoke-all then grant the checked set ----
    RunIgnore(conn, L"REVOKE ALL PRIVILEGES, GRANT OPTION ON *.* FROM " + acct);
    if (!spec.globalPrivs.empty() || spec.globalGrantOption) {
        wxString list;
        for (const auto& p : spec.globalPrivs) {
            if (p == L"GRANT OPTION") continue;
            if (!list.IsEmpty()) list += L", ";
            list += p;
        }
        if (list.IsEmpty()) list = L"USAGE";     // WITH GRANT OPTION needs a priv
        wxString sql = L"GRANT " + list + L" ON *.* TO " + acct;
        if (spec.globalGrantOption) sql += L" WITH GRANT OPTION";
        if (!Run(conn, sql, err)) return false;
    }

    // ---- 3. per-database privileges ----
    std::vector<wxString> dbs;
    for (const auto& g : cur.dbPrivs)  dbs.push_back(g.database);
    for (const auto& g : spec.dbPrivs) dbs.push_back(g.database);
    std::sort(dbs.begin(), dbs.end());
    dbs.erase(std::unique(dbs.begin(), dbs.end()), dbs.end());

    for (const wxString& db : dbs) {
        RunIgnore(conn, L"REVOKE ALL PRIVILEGES, GRANT OPTION ON " + QIdent(db) +
                        L".* FROM " + acct);
        const DbPrivGrant* want = nullptr;
        for (const auto& g : spec.dbPrivs) if (g.database == db) { want = &g; break; }
        if (!want || want->privs.empty()) continue;
        wxString list; bool go = false;
        for (const auto& p : want->privs) {
            if (p == L"GRANT OPTION") { go = true; continue; }
            if (!list.IsEmpty()) list += L", ";
            list += p;
        }
        if (list.IsEmpty()) { if (!go) continue; list = L"USAGE"; }
        wxString sql = L"GRANT " + list + L" ON " + QIdent(db) + L".* TO " + acct;
        if (go) sql += L" WITH GRANT OPTION";
        if (!Run(conn, sql, err)) return false;
    }

    // ---- 4. roles (MySQL 8; best-effort) ----
    auto has = [](const std::vector<wxString>& v, const wxString& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    for (const wxString& r : cur.roles)
        if (!has(spec.roles, r)) {
            wxString n, h; SplitAccount(r, n, h);
            RunIgnore(conn, L"REVOKE " + QAcct(n, h) + L" FROM " + acct);
        }
    for (const wxString& r : spec.roles)
        if (!has(cur.roles, r)) {
            wxString n, h; SplitAccount(r, n, h);
            RunIgnore(conn, L"GRANT " + QAcct(n, h) + L" TO " + acct);
        }

    // ---- 5. resource limits / SSL ----
    if (spec.setResourceLimits) {
        wxString sql = wxString::Format(
            L"ALTER USER %s WITH MAX_QUERIES_PER_HOUR %ld MAX_UPDATES_PER_HOUR %ld "
            L"MAX_CONNECTIONS_PER_HOUR %ld MAX_USER_CONNECTIONS %ld",
            acct, spec.maxQueriesPerHour, spec.maxUpdatesPerHour,
            spec.maxConnectionsPerHour, spec.maxUserConnections);
        if (!Run(conn, sql, err)) return false;
        RunIgnore(conn, L"ALTER USER " + acct + (spec.requireSsl ? L" REQUIRE SSL"
                                                                 : L" REQUIRE NONE"));
    }
    return true;
}

bool MyDropUser(IConnection* conn, const wxString& user, const wxString& host,
                wxString& err)
{
    return Run(conn, L"DROP USER " + QAcct(user, host), err);
}

} // namespace useradmin
} // namespace db
