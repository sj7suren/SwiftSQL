// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// OracleDriver.cpp — 达梦 DM (an Oracle-compatible engine) over the Win32 ODBC
// wide API. No vcpkg / OCI dependency: links odbc32 only, and talks to the
// "DM8 ODBC DRIVER" the user installs at runtime. Modelled on SqlServerDriver.cpp.
//
// NOTE: Oracle *Database* itself no longer travels this ODBC route — it now has a
// dedicated native-OCI driver (OracleOciDriver.cpp, CreateOracleConnection). This
// file is DM-only; the two share nothing at build time except the Oracle data
// dictionary SQL (copied, since DM is Oracle-compatible), so the Oracle vs DM
// engine choice is fully decoupled.
//
// Schema introspection goes through the Oracle-compatible data dictionary
// (ALL_TABLES, ALL_TAB_COLUMNS, ALL_CONSTRAINTS, ALL_INDEXES, DBMS_METADATA.GET_DDL)
// that DM mirrors. Reports Dialect::Oracle (double-quoted identifiers, standard-SQL
// literals, OFFSET/FETCH paging).
//
// A "database" in this app's model maps to a DM *schema/owner*: ListDatabases
// returns schema names, and every table call carries the owner in `database`.
//
// NOTE (honest gate): this file is compile-verified only. No DM ODBC driver is
// installed on the build host, so no live connection was made. At runtime the user
// must install the DM8 ODBC driver; the DRIVER={...} name below and the
// introspection SQL may need adjustment for their exact driver/version.
#include "db/DbDriver.h"
#include "db/StmtGuard.h"

#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <wx/stopwatch.h>
#include <mutex>

namespace db {
namespace {

// ---- pure helpers (no server needed) ---------------------------------------

// Wrap an ODBC connection-string value in {…} if it contains a delimiter,
// doubling any embedded '}'. Needed for passwords with ';' '{' or '}'.
wxString EscapeOdbc(const wxString& v)
{
    if (v.find_first_of(L";{}") == wxString::npos) return v;
    wxString e = v;
    e.Replace(L"}", L"}}");
    return L"{" + e + L"}";
}

// Escape a string literal for '…' (double single quotes — standard SQL / Oracle).
wxString EscLit(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return e;
}

// Raw bytes → uppercase hex (no prefix); RenderLiteral/HEXTORAW frame it.
wxString HexDigits(const unsigned char* p, size_t n)
{
    static const wchar_t* kHex = L"0123456789ABCDEF";
    wxString h;
    h.Alloc(n * 2);
    for (size_t i = 0; i < n && p; ++i) { h += kHex[p[i] >> 4]; h += kHex[p[i] & 0xF]; }
    return h;
}

bool IsNumericSqlType(SQLSMALLINT t)
{
    switch (t) {
    case SQL_INTEGER: case SQL_BIGINT: case SQL_SMALLINT: case SQL_TINYINT:
    case SQL_DECIMAL: case SQL_NUMERIC: case SQL_FLOAT: case SQL_REAL:
    case SQL_DOUBLE:
        return true;
    default:
        return false;
    }
}

bool IsBinarySqlType(SQLSMALLINT t)
{
    return t == SQL_BINARY || t == SQL_VARBINARY || t == SQL_LONGVARBINARY;
}

// Build the DM8 ODBC connection string. The DRIVER name is the one runtime piece
// the user most likely must adjust (older DM installs may register the driver under
// a different display name — check the ODBC Data Source Administrator for the exact
// name). Kept as a small, reviewable, testable function on purpose.
wxString BuildConnStr(const core::ConnectionProfile& p)
{
    // 达梦 DM8 ODBC. Default driver display name is "DM8 ODBC DRIVER"; older
    // installs may register it differently — adjust to match the machine.
    wxString cs = L"DRIVER={DM8 ODBC DRIVER};";
    cs += L"SERVER=" + EscapeOdbc(p.host) + L";";
    cs += wxString::Format(L"TCP_PORT=%d;", p.port);
    cs += L"UID=" + EscapeOdbc(p.user) + L";";
    cs += L"PWD=" + EscapeOdbc(p.password) + L";";
    return cs;
}

// ---- driver ----------------------------------------------------------------

class DmConnection : public IConnection {
public:
    DmConnection() = default;
    ~DmConnection() override { Disconnect(); }

    Dialect GetDialect() const override { return Dialect::Oracle; }

    // profile_ holds exactly the profile passed to Connect() below (the caller's
    // tunnel-rewritten `eff`, when SSH is in play).
    const core::ConnectionProfile& EffectiveProfile() const override { return profile_; }

    bool Connect(const core::ConnectionProfile& p, wxString& err) override
    {
        Disconnect();
        profile_ = p;
        if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_) != SQL_SUCCESS) {
            err = L"无法分配 ODBC 环境句柄"; return false;
        }
        SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION,
                      reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);

        wxWCharBuffer cs = BuildConnStr(p).wc_str();
        SQLWCHAR outStr[1024]; SQLSMALLINT outLen = 0;
        const SQLRETURN rc = SQLDriverConnectW(
            dbc_, nullptr, reinterpret_cast<SQLWCHAR*>(cs.data()), SQL_NTS,
            outStr, 1024, &outLen, SQL_DRIVER_NOPROMPT);
        if (!SQL_SUCCEEDED(rc)) {
            err = DiagText(SQL_HANDLE_DBC, dbc_);
            Disconnect();
            return false;
        }
        connected_ = true;

        // Current schema — used when a call passes an empty "database".
        QueryResult r; wxString e2;
        if (Execute(L"SELECT USER FROM DUAL", r, e2) && !r.rows.empty() &&
            !r.rows[0].empty())
            defaultSchema_ = r.rows[0][0];

        // Server banner — several dictionary views exist; try the most portable
        // first, then fall back. Best-effort (leave blank on failure).
        if (Execute(L"SELECT version FROM product_component_version "
                    L"WHERE ROWNUM=1", r, e2) && !r.rows.empty() && !r.rows[0].empty())
            version_ = r.rows[0][0];
        else if (Execute(L"SELECT banner FROM v$version WHERE ROWNUM=1", r, e2) &&
                 !r.rows.empty() && !r.rows[0].empty())
            version_ = r.rows[0][0];
        return true;
    }

    void Disconnect() override
    {
        // Hold the same mutex Cancel() takes while tearing down the connection
        // handles: Cancel() reads curStmt_ (a child of dbc_) and SQLCancels it, so
        // freeing dbc_/env_ without the lock is a theoretical UAF if the two race.
        // Cancel() only takes this lock (never re-enters Disconnect), and the tiny
        // ClearStmt/guard swaps never call Disconnect — no nesting / deadlock.
        std::lock_guard<std::mutex> lk(stmtMx_);
        if (dbc_) {
            if (connected_) SQLDisconnect(dbc_);
            SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
            dbc_ = nullptr;
        }
        if (env_) { SQLFreeHandle(SQL_HANDLE_ENV, env_); env_ = nullptr; }
        connected_ = false;
    }

    bool IsConnected() const override { return connected_; }

    void Cancel() override {
        // Serialize against Execute/StreamRows freeing the statement, so we never
        // SQLCancel a handle another thread is about to SQLFreeHandle (the UAF the
        // SQL Server driver already fixed — keep the same discipline here).
        std::lock_guard<std::mutex> lk(stmtMx_);
        if (curStmt_) SQLCancel(curStmt_);
    }

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        if (!connected_) { err = L"未连接"; return false; }
        out = QueryResult();

        wxStopWatch sw;
        SQLHSTMT st = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st);
        { std::lock_guard<std::mutex> lk(stmtMx_); curStmt_ = st; }
        OdbcStmtGuard stGuard(st, stmtMx_, curStmt_);   // clears curStmt_ + frees st

        wxWCharBuffer wsql = sql.wc_str();
        SQLRETURN rc = SQLExecDirectW(st, reinterpret_cast<SQLWCHAR*>(wsql.data()),
                                      SQL_NTS);
        if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA) {
            err = DiagText(SQL_HANDLE_STMT, st);
            return false;
        }

        SQLSMALLINT ncol = 0;
        SQLNumResultCols(st, &ncol);
        if (ncol > 0) {
            out.isSelect = true;
            for (SQLSMALLINT i = 1; i <= ncol; ++i) {
                SQLWCHAR name[256]; SQLSMALLINT nlen = 0;
                SQLDescribeColW(st, i, name, 256, &nlen, nullptr, nullptr,
                                nullptr, nullptr);
                out.columns.push_back(wxString(reinterpret_cast<const wchar_t*>(name)));
            }
            while (SQL_SUCCEEDED(SQLFetch(st))) {
                std::vector<wxString> row;
                row.reserve(ncol);
                for (SQLSMALLINT i = 1; i <= ncol; ++i) row.push_back(GetCellW(st, i));
                out.rows.push_back(std::move(row));
            }
            out.affected = out.rows.size();
        } else {
            SQLLEN n = 0; SQLRowCount(st, &n);
            out.isSelect = false;
            out.affected = (n > 0) ? static_cast<unsigned long long>(n) : 0;
        }
        out.elapsedMs = sw.Time();
        return true;   // stGuard clears curStmt_ and frees st
    }

    // Schemas/users double as "databases" in this app's model.
    bool ListDatabases(std::vector<wxString>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT username FROM all_users ORDER BY username", r, err))
            return false;
        for (const auto& row : r.rows) if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool ListTables(const wxString& database, std::vector<TableInfo>& out,
                    wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT table_name, NVL(TO_CHAR(num_rows),'-') FROM all_tables "
                L"WHERE owner='" + Owner(database) + L"' ORDER BY table_name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    // Per-table metadata for the overview grid. num_rows is optimizer statistics
    // (may be stale/NULL until ANALYZE/DBMS_STATS runs — reported as-is, -1 if
    // unknown). On-disk size needs DBA_SEGMENTS privilege, so it's left unknown.
    bool GetTableList(const wxString& database,
                      std::vector<TableMeta>& out, wxString& err) override
    {
        const wxString owner = Owner(database);
        QueryResult r;
        if (!Execute(
                L"SELECT t.table_name, NVL(tc.comments,''), NVL(t.num_rows,-1), "
                L"(SELECT COUNT(*) FROM all_tab_columns c "
                L"  WHERE c.owner=t.owner AND c.table_name=t.table_name) "
                L"FROM all_tables t "
                L"LEFT JOIN all_tab_comments tc ON tc.owner=t.owner "
                L"  AND tc.table_name=t.table_name AND tc.table_type='TABLE' "
                L"WHERE t.owner='" + owner + L"' ORDER BY t.table_name", r, err))
            return false;
        out.clear();
        for (const auto& row : r.rows) {
            if (row.size() < 4) continue;
            TableMeta m;
            m.name    = row[0];
            m.comment = row[1];
            row[2].ToLongLong(&m.rows);
            long long cols = -1; row[3].ToLongLong(&cols);
            m.columns = static_cast<int>(cols);
            out.push_back(m);
        }
        return true;
    }

    bool GetDatabaseInfo(const wxString& database, DbInfo& out, wxString& err) override
    {
        out = DbInfo{};
        out.name = database;
        QueryResult r;
        if (Execute(L"SELECT COUNT(*) FROM all_tables WHERE owner='" +
                    Owner(database) + L"'", r, err) &&
            !r.rows.empty() && !r.rows[0].empty())
            r.rows[0][0].ToLongLong(&out.tableCount);
        return true;
    }

    bool GetColumns(const wxString& database, const wxString& table,
                    std::vector<ColumnInfo>& out, wxString& err) override
    {
        const wxString owner = Owner(database);
        // PK column set first, so we can mark key='PK'.
        std::vector<wxString> pk;
        wxString ign; GetPrimaryKey(database, table, pk, ign);

        QueryResult r;
        if (!Execute(
                L"SELECT column_id, column_name, data_type, data_length, "
                L"data_precision, data_scale, nullable, data_default "
                L"FROM all_tab_columns WHERE owner='" + owner + L"' "
                L"AND table_name='" + EscLit(table) + L"' ORDER BY column_id", r, err))
            return false;

        // Best-effort extended introspection via side queries — kept separate so a
        // failure (or a DM build lacking the view/column) never breaks the core
        // call. Comments from ALL_COL_COMMENTS; generated (virtual) columns from
        // ALL_TAB_COLS.VIRTUAL_COLUMN with the expression in DATA_DEFAULT.
        QueryResult cmt, virt;
        Execute(L"SELECT column_name, comments FROM all_col_comments "
                L"WHERE owner='" + owner + L"' AND table_name='" +
                EscLit(table) + L"'", cmt, ign);
        Execute(L"SELECT column_name, data_default FROM all_tab_cols "
                L"WHERE owner='" + owner + L"' AND table_name='" +
                EscLit(table) + L"' AND virtual_column='YES'", virt, ign);
        auto commentFor = [&](const wxString& col) -> wxString {
            for (const auto& row : cmt.rows)
                if (row.size() >= 2 && row[0] == col && row[1] != L"NULL") return row[1];
            return wxString();
        };
        auto virtExprFor = [&](const wxString& col) -> wxString {
            for (const auto& row : virt.rows)
                if (row.size() >= 2 && row[0] == col) return row[1];
            return wxString();
        };

        for (const auto& row : r.rows) {
            if (row.size() < 8) continue;
            ColumnInfo c;
            long ord = 0; row[0].ToLong(&ord); c.ordinal = static_cast<int>(ord);
            c.name = row[1];
            c.type = row[2];
            c.length = LengthText(row[2], row[3], row[4], row[5]);
            c.notNull = (row[6] == L"N");        // nullable = 'N' → NOT NULL
            c.defaultVal = row[7];               // DATA_DEFAULT is a LONG expr text
            c.defaultVal.Trim(true).Trim(false);
            if (c.defaultVal == L"NULL") c.defaultVal.clear();
            for (const auto& k : pk) if (k == c.name) { c.key = L"PK"; break; }
            // ---- extended introspection (best-effort) ----
            if (row[5] != L"NULL") c.scale = row[5];   // DATA_SCALE (numeric only)
            c.comment = commentFor(c.name);
            wxString ve = virtExprFor(c.name);         // Oracle virtual = computed-on-read
            if (!ve.IsEmpty()) {
                ve.Trim(true).Trim(false);
                c.generatedExpr = ve;                  // generatedStored stays false
            }
            out.push_back(c);
        }
        return true;
    }

    bool GetPrimaryKey(const wxString& database, const wxString& table,
                       std::vector<wxString>& pkColumns, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT cc.column_name FROM all_constraints c "
                L"JOIN all_cons_columns cc ON c.owner=cc.owner "
                L"  AND c.constraint_name=cc.constraint_name "
                L"WHERE c.owner='" + Owner(database) + L"' "
                L"AND c.table_name='" + EscLit(table) + L"' "
                L"AND c.constraint_type='P' ORDER BY cc.position", r, err))
            return false;
        pkColumns.clear();
        for (const auto& row : r.rows) if (!row.empty()) pkColumns.push_back(row[0]);
        return true;
    }

    bool GetForeignKeys(const wxString& database, const wxString& table,
                        std::vector<ForeignKey>& out, wxString& err) override
    {
        // Join the child FK constraint's columns to the referenced (P/U) constraint
        // columns by position; owner-qualified throughout.
        QueryResult r;
        if (!Execute(
                L"SELECT cc.column_name, rc.table_name, rcc.column_name "
                L"FROM all_constraints c "
                L"JOIN all_cons_columns cc ON c.owner=cc.owner "
                L"  AND c.constraint_name=cc.constraint_name "
                L"JOIN all_constraints rc ON c.r_owner=rc.owner "
                L"  AND c.r_constraint_name=rc.constraint_name "
                L"JOIN all_cons_columns rcc ON rc.owner=rcc.owner "
                L"  AND rc.constraint_name=rcc.constraint_name "
                L"  AND cc.position=rcc.position "
                L"WHERE c.owner='" + Owner(database) + L"' "
                L"AND c.table_name='" + EscLit(table) + L"' "
                L"AND c.constraint_type='R' ORDER BY cc.position", r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            ForeignKey fk;
            fk.fromTable = table; fk.fromColumn = row[0];
            fk.toTable   = row[1]; fk.toColumn   = row[2];
            out.push_back(fk);
        }
        return true;
    }

    bool GetIndexes(const wxString& database, const wxString& table,
                    std::vector<IndexInfo>& out, wxString& err) override
    {
        // Rows come sorted by index then column position; aggregate the column
        // list per index in C++ (portable — no LISTAGG version dependency).
        QueryResult r;
        if (!Execute(
                L"SELECT i.index_name, i.uniqueness, ic.column_name "
                L"FROM all_indexes i "
                L"JOIN all_ind_columns ic ON i.owner=ic.index_owner "
                L"  AND i.index_name=ic.index_name "
                L"WHERE i.table_owner='" + Owner(database) + L"' "
                L"AND i.table_name='" + EscLit(table) + L"' "
                L"ORDER BY i.index_name, ic.column_position", r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            if (!out.empty() && out.back().name == row[0]) {
                out.back().columns += L", " + row[2];
            } else {
                IndexInfo idx;
                idx.name = row[0];
                idx.unique = (row[1] == L"UNIQUE");
                idx.columns = row[2];
                out.push_back(idx);
            }
        }
        return true;
    }

    // Prefer DBMS_METADATA.GET_DDL (exact, respects grants); degrade to a
    // synthesised skeleton if it's unavailable (privilege / disabled).
    bool GetCreateDdl(const wxString& database, const wxString& table,
                      wxString& ddl, wxString& err) override
    {
        const wxString owner = Owner(database);
        QueryResult r; wxString e2;
        if (Execute(L"SELECT DBMS_METADATA.GET_DDL('TABLE','" + EscLit(table) +
                    L"','" + owner + L"') FROM DUAL", r, e2) &&
            !r.rows.empty() && !r.rows[0].empty() && r.rows[0][0] != L"NULL" &&
            !r.rows[0][0].IsEmpty()) {
            ddl = r.rows[0][0];
            return true;
        }
        return SynthCreate(database, table, ddl, err);
    }

    bool ListViews(const wxString& database, std::vector<wxString>& out,
                   wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT view_name FROM all_views WHERE owner='" +
                     Owner(database) + L"' ORDER BY view_name", r, err))
            return false;
        for (const auto& row : r.rows) if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool GetViewDdl(const wxString& database, const wxString& view,
                    wxString& ddl, wxString& err) override
    {
        const wxString owner = Owner(database);
        QueryResult r; wxString e2;
        if (Execute(L"SELECT DBMS_METADATA.GET_DDL('VIEW','" + EscLit(view) +
                    L"','" + owner + L"') FROM DUAL", r, e2) &&
            !r.rows.empty() && !r.rows[0].empty() && r.rows[0][0] != L"NULL") {
            ddl = r.rows[0][0];
            return true;
        }
        // Degrade: wrap the stored view text as a CREATE OR REPLACE VIEW.
        if (Execute(L"SELECT text FROM all_views WHERE owner='" + owner +
                    L"' AND view_name='" + EscLit(view) + L"'", r, err) &&
            !r.rows.empty() && !r.rows[0].empty()) {
            ddl = L"CREATE OR REPLACE VIEW " + QuoteIdent(view, Dialect::Oracle) +
                  L" AS\n" + r.rows[0][0];
            return true;
        }
        return false;
    }

    bool ListRoutines(const wxString& database, std::vector<RoutineInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT object_name, object_type FROM all_objects "
                     L"WHERE owner='" + Owner(database) + L"' "
                     L"AND object_type IN ('PROCEDURE','FUNCTION') "
                     L"ORDER BY object_type, object_name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetRoutineDdl(const wxString& database, const RoutineInfo& rt,
                       wxString& ddl, wxString& err) override
    {
        // object_type is exactly the DBMS_METADATA object kind ('PROCEDURE'/'FUNCTION').
        QueryResult r;
        if (!Execute(L"SELECT DBMS_METADATA.GET_DDL('" + EscLit(rt.type) + L"','" +
                     EscLit(rt.name) + L"','" + Owner(database) + L"') FROM DUAL", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty() && r.rows[0][0] != L"NULL")
            ddl = r.rows[0][0];
        return true;
    }

    bool ListTriggers(const wxString& database, std::vector<TriggerInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT trigger_name, table_name FROM all_triggers "
                     L"WHERE owner='" + Owner(database) + L"' ORDER BY trigger_name",
                     r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetTriggerDdl(const wxString& database, const TriggerInfo& tg,
                       wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT DBMS_METADATA.GET_DDL('TRIGGER','" + EscLit(tg.name) +
                     L"','" + Owner(database) + L"') FROM DUAL", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty() && r.rows[0][0] != L"NULL")
            ddl = r.rows[0][0];
        return true;
    }

    bool GetDatabaseOverview(DbOverview& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT u.username, "
                     L"(SELECT COUNT(*) FROM all_tables t WHERE t.owner=u.username) "
                     L"FROM all_users u ORDER BY u.username", r, err))
            return false;
        out = DbOverview{};
        out.columns = { {L"name", L"名称"}, {L"tables", L"表数"} };
        out.rows = std::move(r.rows);
        return true;
    }

    bool DumpTableData(const wxString& database, const wxString& table,
                       const std::function<void(const wxString&)>& emit,
                       wxString& err) override
    {
        if (!connected_) { err = L"未连接"; return false; }
        const wxString qtable = Qual(database, table);

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &stmt);
        { std::lock_guard<std::mutex> lk(stmtMx_); curStmt_ = stmt; }
        OdbcStmtGuard stGuard(stmt, stmtMx_, curStmt_);   // clears curStmt_ + frees stmt
        wxWCharBuffer wsql = (L"SELECT * FROM " + qtable).wc_str();
        if (!SQL_SUCCEEDED(SQLExecDirectW(stmt,
                reinterpret_cast<SQLWCHAR*>(wsql.data()), SQL_NTS))) {
            err = DiagText(SQL_HANDLE_STMT, stmt);
            return false;
        }
        SQLSMALLINT nf = 0;
        SQLNumResultCols(stmt, &nf);
        std::vector<SQLSMALLINT> types(nf + 1, 0);
        wxString cols;
        for (SQLSMALLINT i = 1; i <= nf; ++i) {
            SQLWCHAR name[256]; SQLSMALLINT nlen = 0, dt = 0;
            SQLDescribeColW(stmt, i, name, 256, &nlen, &dt, nullptr, nullptr, nullptr);
            types[i] = dt;
            cols += (i > 1 ? L", " : L"") +
                    QuoteIdent(wxString(reinterpret_cast<const wchar_t*>(name)),
                               Dialect::Oracle);
        }
        const wxString prefix = L"INSERT INTO " + qtable + L" (" + cols + L") VALUES (";

        while (SQL_SUCCEEDED(SQLFetch(stmt))) {
            wxString vals;
            for (SQLSMALLINT i = 1; i <= nf; ++i) {
                if (i > 1) vals += L", ";
                if (IsBinarySqlType(types[i])) {
                    bool isNull = false;
                    const wxString hx = GetCellHex(stmt, i, isNull);
                    vals += isNull ? wxString(L"NULL")
                                   : (hx.IsEmpty() ? wxString(L"NULL")
                                                   : L"HEXTORAW('" + hx + L"')");
                } else {
                    const wxString cell = GetCellW(stmt, i);
                    if (cell == L"NULL") vals += L"NULL";           // (see GetCellW)
                    else if (IsNumericSqlType(types[i])) vals += cell;
                    else vals += L"'" + EscLit(cell) + L"'";
                }
            }
            emit(prefix + vals + L");\n");
        }
        return true;   // stGuard clears curStmt_ and frees stmt
    }

    // Keyed, chunked structured row stream (typed Cells for the data-sync diff).
    bool StreamRows(const wxString& database, const QualifiedName& table,
                    const StreamOptions& opt, const RowSink& sink,
                    wxString& err) override
    {
        if (!connected_) { err = L"未连接"; return false; }

        wxString proj;
        if (opt.columns.empty()) proj = L"*";
        else
            for (size_t i = 0; i < opt.columns.size(); ++i)
                proj += (i ? L", " : L"") +
                        QuoteIdent(opt.columns[i], Dialect::Oracle);

        wxString sql = L"SELECT " + proj + L" FROM " + Qual(database, table.Name());
        // Single-column cursor paging (compares against the first orderBy column),
        // matching the MySQL driver's contract.
        if (!opt.keyLowExcl.IsEmpty() && !opt.orderBy.empty())
            sql += L" WHERE " + QuoteIdent(opt.orderBy[0], Dialect::Oracle) +
                   L" > '" + EscLit(opt.keyLowExcl) + L"'";
        if (!opt.orderBy.empty()) {
            sql += L" ORDER BY ";
            for (size_t i = 0; i < opt.orderBy.size(); ++i)
                sql += (i ? L", " : L"") +
                       QuoteIdent(opt.orderBy[i], Dialect::Oracle);
        }
        if (opt.limit >= 0)
            sql += LimitOffsetClause(Dialect::Oracle, opt.limit, 0);

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &stmt);
        { std::lock_guard<std::mutex> lk(stmtMx_); curStmt_ = stmt; }
        OdbcStmtGuard stGuard(stmt, stmtMx_, curStmt_);   // clears curStmt_ + frees stmt
        wxWCharBuffer wsql = sql.wc_str();
        if (!SQL_SUCCEEDED(SQLExecDirectW(stmt,
                reinterpret_cast<SQLWCHAR*>(wsql.data()), SQL_NTS))) {
            err = DiagText(SQL_HANDLE_STMT, stmt);
            return false;
        }
        SQLSMALLINT nf = 0;
        SQLNumResultCols(stmt, &nf);
        std::vector<SQLSMALLINT> types(nf + 1, 0);
        for (SQLSMALLINT i = 1; i <= nf; ++i) {
            SQLSMALLINT dt = 0;
            SQLDescribeColW(stmt, i, nullptr, 0, nullptr, &dt, nullptr, nullptr, nullptr);
            types[i] = dt;
        }

        while (SQL_SUCCEEDED(SQLFetch(stmt))) {
            std::vector<Cell> cells;
            cells.reserve(nf);
            for (SQLSMALLINT i = 1; i <= nf; ++i) {
                Cell cell;
                if (IsBinarySqlType(types[i])) {
                    bool isNull = false;
                    const wxString hx = GetCellHex(stmt, i, isNull);
                    if (isNull) cell.kind = CellKind::Null;
                    else { cell.kind = CellKind::Binary; cell.text = hx; }
                } else {
                    const wxString cell_text = GetCellW(stmt, i);
                    if (cell_text == L"NULL" && IsColNull(stmt, i))
                        cell.kind = CellKind::Null;
                    else if (IsNumericSqlType(types[i])) {
                        cell.kind = CellKind::Numeric; cell.text = cell_text;
                    } else {
                        cell.kind = CellKind::Text; cell.text = cell_text;
                    }
                }
                cells.push_back(std::move(cell));
            }
            if (!sink(cells)) break;   // cancellation
        }
        return true;   // stGuard clears curStmt_ and frees stmt
    }

    wxString ServerVersion() const override { return version_; }

    // ---- user / privilege administration (logic in db/UserAdminOracle.cpp) ----
    bool SupportsUserAdmin() const override { return true; }
    bool ListUsers(std::vector<UserInfo>& o, wxString& e) override
        { return useradmin::OraListUsers(this, o, e); }
    bool ListGrantableRoles(std::vector<wxString>& o, wxString& e) override
        { return useradmin::OraListRoles(this, o, e); }
    bool GetUserGrants(const wxString& u, const wxString&, UserGrants& o, wxString& e) override
        { return useradmin::OraGetGrants(this, u, o, e); }
    bool SaveUser(const UserSpec& s, bool isNew, wxString& e) override
        { return useradmin::OraSaveUser(this, s, isNew, e); }
    bool DropUser(const wxString& u, const wxString&, wxString& e) override
        { return useradmin::OraDropUser(this, u, e); }
    UserAdminModel GetUserAdminModel() const override { return useradmin::OracleModel(); }

private:
    // Owner (schema) to query — the passed database, or the connected user's schema.
    wxString Owner(const wxString& database) const
    {
        return database.IsEmpty() ? EscLit(defaultSchema_) : EscLit(database);
    }

    // Fully-qualified "OWNER"."TABLE" for DML/SELECT.
    wxString Qual(const wxString& database, const wxString& table) const
    {
        const wxString owner = database.IsEmpty() ? defaultSchema_ : database;
        return QuoteIdent(owner, Dialect::Oracle) + L"." +
               QuoteIdent(table, Dialect::Oracle);
    }

    // Column length text for the design view: VARCHAR2/CHAR → byte length;
    // NUMBER(p,s) → "p,s" or "p"; everything else → "".
    static wxString LengthText(const wxString& type, const wxString& len,
                               const wxString& prec, const wxString& scale)
    {
        if (type.StartsWith(L"VARCHAR") || type.StartsWith(L"CHAR") ||
            type.StartsWith(L"NVARCHAR") || type.StartsWith(L"NCHAR") ||
            type == L"RAW")
            return (len == L"0" || len.IsEmpty()) ? wxString() : len;
        if (type == L"NUMBER" && !prec.IsEmpty() && prec != L"NULL") {
            if (!scale.IsEmpty() && scale != L"NULL" && scale != L"0")
                return prec + L"," + scale;
            return prec;
        }
        return wxString();
    }

    // Synthesised CREATE TABLE when DBMS_METADATA is unavailable.
    bool SynthCreate(const wxString& database, const wxString& table,
                     wxString& ddl, wxString& err)
    {
        std::vector<ColumnInfo> cols;
        if (!GetColumns(database, table, cols, err)) return false;
        ddl = L"-- 简化定义(DBMS_METADATA 不可用时的降级,不含约束/存储参数)\n";
        ddl += L"CREATE TABLE " + Qual(database, table) + L" (\n";
        wxString pk;
        for (size_t i = 0; i < cols.size(); ++i) {
            const ColumnInfo& c = cols[i];
            ddl += L"    " + QuoteIdent(c.name, Dialect::Oracle) + L" " + c.type;
            if (!c.length.IsEmpty()) ddl += L"(" + c.length + L")";
            if (c.notNull) ddl += L" NOT NULL";
            if (!c.defaultVal.IsEmpty()) ddl += L" DEFAULT " + c.defaultVal;
            if (c.key == L"PK") pk += (pk.IsEmpty() ? L"" : L", ") +
                                      QuoteIdent(c.name, Dialect::Oracle);
            ddl += (i + 1 < cols.size() || !pk.IsEmpty()) ? L",\n" : L"\n";
        }
        if (!pk.IsEmpty()) ddl += L"    PRIMARY KEY (" + pk + L")\n";
        ddl += L")";
        return true;
    }

    // Read a cell as text. NULL → the string "NULL" (the QueryResult convention).
    static wxString GetCellW(SQLHSTMT st, SQLUSMALLINT col)
    {
        wxString val;
        SQLWCHAR buf[4096];
        SQLLEN ind = 0;
        for (;;) {
            const SQLRETURN rc = SQLGetData(st, col, SQL_C_WCHAR, buf, sizeof(buf), &ind);
            if (rc == SQL_NO_DATA || !SQL_SUCCEEDED(rc)) break;
            if (ind == SQL_NULL_DATA) return wxString(L"NULL");
            val += wxString(reinterpret_cast<const wchar_t*>(buf));
            if (rc == SQL_SUCCESS) break;   // last chunk
        }
        return val;
    }

    // Distinguish a genuine SQL NULL from the literal string "NULL" (StreamRows
    // typing). Cheap re-probe: SQLGetData of zero length reports the indicator.
    static bool IsColNull(SQLHSTMT st, SQLUSMALLINT col)
    {
        SQLWCHAR tiny[1]; SQLLEN ind = 0;
        SQLGetData(st, col, SQL_C_WCHAR, tiny, sizeof(tiny), &ind);
        return ind == SQL_NULL_DATA;
    }

    // Read a binary cell as uppercase hex digits (no prefix); sets isNull on NULL.
    static wxString GetCellHex(SQLHSTMT st, SQLUSMALLINT col, bool& isNull)
    {
        isNull = false;
        unsigned char buf[4096];
        SQLLEN ind = 0;
        wxString hex;
        for (;;) {
            const SQLRETURN rc = SQLGetData(st, col, SQL_C_BINARY, buf, sizeof(buf), &ind);
            if (rc == SQL_NO_DATA || !SQL_SUCCEEDED(rc)) break;
            if (ind == SQL_NULL_DATA) { isNull = true; return wxString(); }
            const size_t n = (ind == SQL_NO_TOTAL || ind > (SQLLEN)sizeof(buf))
                                 ? sizeof(buf) : static_cast<size_t>(ind);
            hex += HexDigits(buf, n);
            if (rc == SQL_SUCCESS) break;
        }
        return hex;
    }

    static wxString DiagText(SQLSMALLINT type, SQLHANDLE h)
    {
        wxString msg;
        SQLWCHAR state[6], text[1024];
        SQLINTEGER native = 0; SQLSMALLINT len = 0;
        for (SQLSMALLINT i = 1; ; ++i) {
            const SQLRETURN rc = SQLGetDiagRecW(type, h, i, state, &native,
                                                text, 1024, &len);
            if (rc == SQL_NO_DATA || !SQL_SUCCEEDED(rc)) break;
            if (!msg.IsEmpty()) msg += L"\n";
            msg += wxString::Format(L"[%s] %s",
                       wxString(reinterpret_cast<const wchar_t*>(state)),
                       wxString(reinterpret_cast<const wchar_t*>(text)));
        }
        return msg.IsEmpty() ? wxString(L"ODBC 错误") : msg;
    }

    SQLHENV env_ = nullptr;
    SQLHDBC dbc_ = nullptr;
    SQLHSTMT curStmt_ = nullptr;   // in-flight statement, for Cancel()
    std::mutex stmtMx_;            // guards curStmt_ across worker/UI threads
    bool connected_ = false;
    wxString version_;
    wxString defaultSchema_;       // connected user's schema (owner for empty db)
    core::ConnectionProfile profile_;   // for EffectiveProfile()
};

} // namespace

// 达梦 DM over ODBC. Oracle Database is served separately by the native-OCI
// driver (CreateOracleConnection, OracleOciDriver.cpp).
std::unique_ptr<IConnection> CreateDmConnection()
{
    return std::make_unique<DmConnection>();
}

// Test seam: expose the pure connection-string builder without a live server.
wxString Dm_BuildConnStr(const core::ConnectionProfile& p)
{
    return BuildConnStr(p);
}

} // namespace db
