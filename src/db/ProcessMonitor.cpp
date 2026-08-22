// ProcessMonitor.cpp — see ProcessMonitor.h. All per-dialect session SQL + kill
// primitives live here so the driver TUs (several already at the file-size charter
// ceiling) need no edits.
#include "db/ProcessMonitor.h"

namespace db {

namespace {

// Every driver's Execute() stringifies a real NULL to the literal "NULL"
// (PgDriver/MySqlDriver/OCI all do). Fold that sentinel back to empty so idle
// sessions (NULL db / SQL text) render blank instead of the word "NULL".
wxString Nz(const wxString& s) { return s == L"NULL" ? wxString() : s; }

// Map a row whose columns are ALREADY in the canonical order
// [id, user, host, db, command, time, state, info] — used by MySQL / PG / SQL Server,
// whose SELECTs are written to emit exactly those 8 columns in that order.
ProcessInfo Canon8(const std::vector<wxString>& r)
{
    auto at = [&](size_t i) { return i < r.size() ? Nz(r[i]) : wxString(); };
    ProcessInfo p;
    p.id      = at(0);
    p.user    = at(1);
    p.host    = at(2);
    p.db      = at(3);
    p.command = at(4);
    p.time    = at(5);
    p.state   = at(6);
    p.info    = at(7);
    return p;
}

// ---- per-dialect list queries ----------------------------------------------

// MySQL / OceanBase: SHOW FULL PROCESSLIST is already Id,User,Host,db,Command,Time,
// State,Info — a canonical 8-column row (FULL keeps the Info/SQL text untruncated).
bool MySqlList(IConnection& c, std::vector<ProcessInfo>& out, wxString& err)
{
    QueryResult r;
    if (!c.Execute(L"SHOW FULL PROCESSLIST", r, err)) return false;
    for (const auto& row : r.rows) out.push_back(Canon8(row));
    return true;
}

// PostgreSQL: pg_stat_activity, projected into canonical order. host() renders the
// client_addr inet as text (falling back to client_hostname, then blank for local
// unix-socket backends); every nullable column is COALESCE'd to '' in SQL.
bool PgList(IConnection& c, std::vector<ProcessInfo>& out, wxString& err)
{
    QueryResult r;
    if (!c.Execute(
            L"SELECT pid, COALESCE(usename,''), "
            L"COALESCE(host(client_addr), client_hostname, ''), "
            L"COALESCE(datname,''), COALESCE(backend_type,''), "
            L"COALESCE(EXTRACT(EPOCH FROM (now()-query_start))::bigint::text,''), "
            L"COALESCE(state,''), COALESCE(query,'') "
            L"FROM pg_stat_activity ORDER BY pid", r, err))
        return false;
    for (const auto& row : r.rows) out.push_back(Canon8(row));
    return true;
}

// SQL Server: dm_exec_sessions (all sessions) LEFT JOIN dm_exec_requests (running
// command/elapsed for active ones) OUTER APPLY dm_exec_sql_text (current statement).
// is_user_process = 1 hides the ~30 background/system sessions. Canonical order.
bool SqlServerList(IConnection& c, std::vector<ProcessInfo>& out, wxString& err)
{
    QueryResult r;
    if (!c.Execute(
            L"SELECT s.session_id, ISNULL(s.login_name,''), ISNULL(s.host_name,''), "
            L"ISNULL(DB_NAME(r.database_id), ISNULL(DB_NAME(s.database_id),'')), "
            L"ISNULL(r.command,''), "
            L"ISNULL(CAST(r.total_elapsed_time/1000 AS varchar(20)),''), "
            L"ISNULL(s.status,''), ISNULL(t.text,'') "
            L"FROM sys.dm_exec_sessions s "
            L"LEFT JOIN sys.dm_exec_requests r ON r.session_id = s.session_id "
            L"OUTER APPLY sys.dm_exec_sql_text(r.sql_handle) t "
            L"WHERE s.is_user_process = 1 ORDER BY s.session_id", r, err))
        return false;
    for (const auto& row : r.rows) out.push_back(Canon8(row));
    return true;
}

// Oracle family. Real Oracle exposes V$SESSION (kill token "sid,serial#"); 达梦 DM
// exposes the DM-native V$SESSIONS (kill token = bare SESS_ID). Both report
// Dialect::Oracle, so try Oracle first and fall back to DM on failure. The differing
// id token later routes KillServerProcess to the right primitive. Oracle has no
// textual "command" column, so it stays blank.
bool OracleList(IConnection& c, std::vector<ProcessInfo>& out, wxString& err)
{
    QueryResult r;
    if (c.Execute(
            L"SELECT s.sid || ',' || s.serial#, s.username, s.machine, s.schemaname, "
            L"TO_CHAR(s.last_call_et), s.status, q.sql_text "
            L"FROM v$session s "
            L"LEFT JOIN v$sql q ON q.sql_id = s.sql_id "
            L"AND q.child_number = s.sql_child_number "
            L"WHERE s.type = 'USER' ORDER BY s.sid", r, err)) {
        for (const auto& row : r.rows) {
            auto at = [&](size_t i) { return i < row.size() ? Nz(row[i]) : wxString(); };
            ProcessInfo p;
            p.id = at(0); p.user = at(1); p.host = at(2); p.db = at(3);
            p.time = at(4); p.state = at(5); p.info = at(6);
            out.push_back(std::move(p));
        }
        return true;
    }
    // 达梦 DM fallback — its live-session view is V$SESSIONS (column set is best-effort
    // for DM8; if a name differs the query errors and the dialog shows that message).
    QueryResult r2; wxString e2;
    if (c.Execute(
            L"SELECT SESS_ID, USER_NAME, CLNT_HOST, CURR_SCH, STATE, SQL_TEXT "
            L"FROM V$SESSIONS ORDER BY SESS_ID", r2, e2)) {
        for (const auto& row : r2.rows) {
            auto at = [&](size_t i) { return i < row.size() ? Nz(row[i]) : wxString(); };
            ProcessInfo p;
            p.id = at(0); p.user = at(1); p.host = at(2); p.db = at(3);
            p.state = at(4); p.info = at(5);
            out.push_back(std::move(p));
        }
        return true;
    }
    return false;   // err already holds the primary (Oracle) failure
}

// A numeric-only guard for the engines whose kill token is a bare id (defends the
// KILL / terminate statement from any spliced text).
bool ParseId(const wxString& id, long& v, wxString& err)
{
    if (!id.ToLong(&v)) { err = L"无效的会话号"; return false; }
    return true;
}

} // namespace

bool SupportsProcessList(Dialect d)
{
    return d != Dialect::Sqlite;   // SQLite is an in-process file — no server sessions
}

bool ListServerProcesses(IConnection& conn, std::vector<ProcessInfo>& out, wxString& err)
{
    out.clear();
    switch (conn.GetDialect()) {
        case Dialect::MySQL:     return MySqlList(conn, out, err);
        case Dialect::Postgres:  return PgList(conn, out, err);
        case Dialect::SqlServer: return SqlServerList(conn, out, err);
        case Dialect::Oracle:    return OracleList(conn, out, err);
        case Dialect::Sqlite:
        default:
            err = L"当前数据库暂不支持会话监控";
            return false;
    }
}

bool KillServerProcess(IConnection& conn, const wxString& id, wxString& err)
{
    QueryResult r;
    long v = 0;
    switch (conn.GetDialect()) {
        case Dialect::MySQL:
            if (!ParseId(id, v, err)) return false;
            return conn.Execute(wxString::Format(L"KILL %ld", v), r, err);
        case Dialect::SqlServer:
            if (!ParseId(id, v, err)) return false;
            return conn.Execute(wxString::Format(L"KILL %ld", v), r, err);
        case Dialect::Postgres:
            if (!ParseId(id, v, err)) return false;
            return conn.Execute(
                wxString::Format(L"SELECT pg_terminate_backend(%ld)", v), r, err);
        case Dialect::Oracle:
            // Token shape disambiguates the engine: "sid,serial#" → real Oracle;
            // a bare number → 达梦 DM's SESS_ID.
            if (id.Find(',') != wxNOT_FOUND) {
                for (size_t i = 0; i < id.length(); ++i) {
                    const wxUniChar ch = id[i];
                    if (!((ch >= '0' && ch <= '9') || ch == ',')) {
                        err = L"无效的会话标识"; return false;
                    }
                }
                return conn.Execute(
                    L"ALTER SYSTEM KILL SESSION '" + id + L"' IMMEDIATE", r, err);
            }
            if (!ParseId(id, v, err)) return false;
            return conn.Execute(wxString::Format(L"SP_CLOSE_SESSION(%ld)", v), r, err);
        case Dialect::Sqlite:
        default:
            err = L"当前数据库暂不支持结束会话";
            return false;
    }
}

} // namespace db
