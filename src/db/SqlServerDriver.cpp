// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SqlServerDriver.cpp — Microsoft SQL Server via the Win32 ODBC API (no vcpkg
// dependency; links odbc32 + the installed "ODBC Driver 17 for SQL Server").
// Schema introspection is done through the sys.* catalog views and
// OBJECT_DEFINITION(); identifiers use double quotes (QUOTED_IDENTIFIER ON).
//
// On Windows wxString's internal buffer is UTF-16 == SQLWCHAR, so we use the
// wide ODBC API (…W) throughout and skip any UTF-8 round-trip.
#include "db/DbDriver.h"
#include "db/StmtGuard.h"

#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <wx/stopwatch.h>
#include <mutex>

namespace db {
namespace {

// ---- pure helpers (unit-tested offline, no server needed) -------------------

// Wrap an ODBC connection-string value in {…} if it contains a delimiter,
// doubling any embedded '}'. Needed for passwords with ';' '{' or '}'.
wxString EscapeOdbc(const wxString& v)
{
    if (v.find_first_of(L";{}") == wxString::npos) return v;
    wxString e = v;
    e.Replace(L"}", L"}}");
    return L"{" + e + L"}";
}

wxString BuildConnStr(const core::ConnectionProfile& p)
{
    wxString cs = L"Driver={ODBC Driver 17 for SQL Server};";
    cs += L"Server=" + p.host + wxString::Format(L",%d;", p.port);
    if (!p.database.IsEmpty()) cs += L"Database=" + EscapeOdbc(p.database) + L";";
    cs += L"UID=" + EscapeOdbc(p.user) + L";";
    cs += L"PWD=" + EscapeOdbc(p.password) + L";";
    // Driver 17 defaults Encrypt=no; TrustServerCertificate keeps Driver 18
    // (Encrypt=yes by default) working too. TLS verification is a later milestone.
    cs += L"Encrypt=no;TrustServerCertificate=yes;APP=SwiftSQL;";
    if (p.connectTimeout > 0)
        cs += wxString::Format(L"Connection Timeout=%d;", p.connectTimeout);
    return cs;
}

// "schema.table" → {schema, table}; bare name → {dbo, name}.
std::pair<wxString, wxString> SplitSchema(const wxString& qualified)
{
    const int dot = qualified.Find(L'.');
    if (dot == wxNOT_FOUND) return { L"dbo", qualified };
    return { qualified.Left(dot), qualified.Mid(dot + 1) };
}

// Escape a string literal for '…' (double single quotes).
wxString EscLit(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return e;
}

// Raw bytes → T-SQL binary literal 0xDEADBEEF (empty → 0x).
wxString HexLiteral(const unsigned char* p, size_t n)
{
    static const wchar_t* kHex = L"0123456789ABCDEF";
    wxString h = L"0x";
    for (size_t i = 0; i < n && p; ++i) { h += kHex[p[i] >> 4]; h += kHex[p[i] & 0xF]; }
    return h;
}

bool IsNumericSqlType(SQLSMALLINT t)
{
    switch (t) {
    case SQL_INTEGER: case SQL_BIGINT: case SQL_SMALLINT: case SQL_TINYINT:
    case SQL_DECIMAL: case SQL_NUMERIC: case SQL_FLOAT: case SQL_REAL:
    case SQL_DOUBLE: case SQL_BIT:
        return true;
    default:
        return false;
    }
}

bool IsBinarySqlType(SQLSMALLINT t)
{
    return t == SQL_BINARY || t == SQL_VARBINARY || t == SQL_LONGVARBINARY;
}

// ---- driver ----------------------------------------------------------------

class SqlServerConnection : public IConnection {
public:
    ~SqlServerConnection() override { Disconnect(); }

    Dialect GetDialect() const override { return Dialect::SqlServer; }

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

        QueryResult r; wxString e2;
        if (Execute(L"SELECT CAST(SERVERPROPERTY('ProductVersion') AS nvarchar(128))",
                    r, e2) && !r.rows.empty() && !r.rows[0].empty())
            version_ = r.rows[0][0];
        Execute(L"SET QUOTED_IDENTIFIER ON", r, e2);   // belt-and-braces
        return true;
    }

    void Disconnect() override
    {
        // Hold the same mutex Cancel() takes while we tear down the connection
        // handles: Cancel() reads curStmt_ (a child of dbc_) and calls SQLCancel,
        // so freeing dbc_/env_ without the lock is a theoretical UAF if the two
        // race. Cancel() only ever takes this lock (never re-enters Disconnect),
        // and the tiny curStmt_ swaps in Execute/Dump never call Disconnect, so
        // there is no nesting / deadlock.
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
        // Serialize against Execute/DumpTableData freeing the statement, so we
        // never SQLCancel a handle another thread is about to SQLFreeHandle.
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

    bool ListDatabases(std::vector<wxString>& out, wxString& err) override
    {
        QueryResult r;
        // database_id > 4 skips master/tempdb/model/msdb.
        if (!Execute(L"SELECT name FROM sys.databases WHERE database_id > 4 "
                     L"ORDER BY name", r, err))
            return false;
        for (const auto& row : r.rows) if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool UseDatabase(const wxString& db, wxString& err) override
    {
        if (db.IsEmpty()) return true;
        QueryResult r;
        return Execute(L"USE " + QuoteIdent(db, Dialect::SqlServer), r, err);
    }

    bool ListTables(const wxString& /*database*/, std::vector<TableInfo>& out,
                    wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT s.name+'.'+t.name, CONVERT(varchar,ISNULL(SUM(p.rows),0)) "
                L"FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id "
                L"LEFT JOIN sys.partitions p ON p.object_id=t.object_id "
                L"AND p.index_id IN (0,1) "
                L"GROUP BY s.name,t.name ORDER BY s.name,t.name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    // Rich per-table metadata for the database-overview grid. Row counts come from
    // sys.partitions (heap/clustered only, index_id 0/1); size sums every
    // allocation unit's pages (8 KB each). engine is N/A for SQL Server.
    bool GetTableList(const wxString& /*database*/,
                      std::vector<TableMeta>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT s.name+'.'+t.name, "
                L"ISNULL(CONVERT(nvarchar(4000),ep.value),''), "
                L"CONVERT(varchar,ISNULL((SELECT SUM(p.rows) FROM sys.partitions p "
                L"  WHERE p.object_id=t.object_id AND p.index_id IN (0,1)),0)), "
                L"CONVERT(varchar,ISNULL((SELECT SUM(a.total_pages)*8*1024 "
                L"  FROM sys.partitions p "
                L"  JOIN sys.allocation_units a ON a.container_id=p.partition_id "
                L"  WHERE p.object_id=t.object_id),0)), "
                L"(SELECT COUNT(*) FROM sys.columns c WHERE c.object_id=t.object_id), "
                L"CASE WHEN OBJECTPROPERTY(t.object_id,'TableHasIdentity')=1 "
                L"  THEN 1 ELSE -1 END, "
                L"CONVERT(varchar(19),t.modify_date,120) "
                L"FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id "
                L"LEFT JOIN sys.extended_properties ep ON ep.major_id=t.object_id "
                L"  AND ep.minor_id=0 AND ep.class=1 AND ep.name='MS_Description' "
                L"ORDER BY s.name, t.name", r, err))
            return false;
        out.clear();
        for (const auto& row : r.rows) {
            if (row.size() < 7) continue;
            TableMeta m;
            m.name    = row[0];
            m.comment = row[1];
            row[2].ToLongLong(&m.rows);
            row[3].ToLongLong(&m.sizeBytes);
            long long cols = -1; row[4].ToLongLong(&cols); m.columns = static_cast<int>(cols);
            row[5].ToLongLong(&m.autoIncrement);   // 1 = has IDENTITY column, else -1
            m.updatedAt = row[6];
            out.push_back(m);
        }
        return true;
    }

    // Database-level summary: collation from DATABASEPROPERTYEX (SQL Server has no
    // separate "charset" — left blank), table count from sys.tables, and total
    // on-disk size summed across every allocation unit.
    bool GetDatabaseInfo(const wxString& database, DbInfo& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT ISNULL(CONVERT(varchar(128),"
                L"  DATABASEPROPERTYEX(DB_NAME(),'Collation')),''), "
                L"(SELECT COUNT(*) FROM sys.tables), "
                L"CONVERT(varchar,ISNULL((SELECT SUM(a.total_pages)*8*1024 "
                L"  FROM sys.allocation_units a "
                L"  JOIN sys.partitions p ON p.partition_id=a.container_id),0))", r, err))
            return false;
        out = DbInfo{};
        out.name = database;
        if (!r.rows.empty() && r.rows[0].size() >= 3) {
            const auto& row = r.rows[0];
            out.collation = row[0];
            row[1].ToLongLong(&out.tableCount);
            row[2].ToLongLong(&out.sizeBytes);
        }
        return true;
    }

    bool GetColumns(const wxString& /*db*/, const wxString& table,
                    std::vector<ColumnInfo>& out, wxString& err) override
    {
        QueryResult r;
        // Cols 0-6 legacy; 7-13 ADR-014 extended introspection. Comment comes from
        // sys.extended_properties (MS_Description, class=1); computed/identity from
        // sys.columns + sys.computed_columns.
        if (!Execute(
                L"SELECT c.column_id, c.name, tp.name, "
                L"CASE WHEN c.max_length=-1 THEN 'max' "
                L"  WHEN tp.name IN ('nvarchar','nchar') THEN CONVERT(varchar,c.max_length/2) "
                L"  WHEN tp.name IN ('varchar','char','varbinary','binary') THEN CONVERT(varchar,c.max_length) "
                L"  WHEN tp.name IN ('decimal','numeric') THEN CONVERT(varchar,c.precision)+','+CONVERT(varchar,c.scale) "
                L"  ELSE '' END, "
                L"c.is_nullable, ISNULL(OBJECT_DEFINITION(c.default_object_id),''), "
                L"CASE WHEN EXISTS(SELECT 1 FROM sys.index_columns ic "
                L"  JOIN sys.indexes i ON i.object_id=ic.object_id AND i.index_id=ic.index_id "
                L"  WHERE i.is_primary_key=1 AND ic.object_id=c.object_id "
                L"  AND ic.column_id=c.column_id) THEN 'PK' ELSE '' END, "
                L"CONVERT(varchar,c.scale), ISNULL(c.collation_name,''), "
                L"c.is_identity, c.is_computed, ISNULL(cc.definition,''), "
                L"ISNULL(cc.is_persisted,0), "
                L"ISNULL(CONVERT(nvarchar(max),ep.value),'') "
                L"FROM sys.columns c JOIN sys.types tp ON tp.user_type_id=c.user_type_id "
                L"LEFT JOIN sys.computed_columns cc "
                L"  ON cc.object_id=c.object_id AND cc.column_id=c.column_id "
                L"LEFT JOIN sys.extended_properties ep "
                L"  ON ep.major_id=c.object_id AND ep.minor_id=c.column_id "
                L"  AND ep.class=1 AND ep.name='MS_Description' "
                L"WHERE c.object_id=OBJECT_ID('" + QualLit(table) + L"') "
                L"ORDER BY c.column_id", r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 7) continue;
            ColumnInfo c;
            long ord = 0; row[0].ToLong(&ord); c.ordinal = static_cast<int>(ord);
            c.name = row[1];
            c.type = row[2];
            c.length = (row[3] == L"0") ? wxString() : row[3];
            c.notNull = (row[4] == L"0");   // is_nullable=0 → NOT NULL
            c.defaultVal = row[5];
            c.key = row[6];
            // ---- extended introspection (best-effort) ----
            if (row.size() >= 14) {
                c.scale         = (row[7] == L"0") ? wxString() : row[7];
                c.collation     = row[8];
                c.autoIncrement = (row[9] == L"1");   // is_identity
                if (row[10] == L"1") {                 // is_computed
                    c.generatedExpr   = row[11];       // computed_column_definition
                    c.generatedStored = (row[12] == L"1");   // is_persisted
                }
                c.comment = row[13];
            }
            out.push_back(c);
        }
        return true;
    }

    bool GetForeignKeys(const wxString& /*db*/, const wxString& table,
                        std::vector<ForeignKey>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT sp.name+'.'+pt.name, pc.name, sr.name+'.'+rt.name, rc.name "
                L"FROM sys.foreign_key_columns fkc "
                L"JOIN sys.tables pt ON pt.object_id=fkc.parent_object_id "
                L"JOIN sys.schemas sp ON sp.schema_id=pt.schema_id "
                L"JOIN sys.columns pc ON pc.object_id=fkc.parent_object_id AND pc.column_id=fkc.parent_column_id "
                L"JOIN sys.tables rt ON rt.object_id=fkc.referenced_object_id "
                L"JOIN sys.schemas sr ON sr.schema_id=rt.schema_id "
                L"JOIN sys.columns rc ON rc.object_id=fkc.referenced_object_id AND rc.column_id=fkc.referenced_column_id "
                L"WHERE fkc.parent_object_id=OBJECT_ID('" + QualLit(table) + L"')", r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 4) continue;
            ForeignKey fk;
            fk.fromTable = row[0]; fk.fromColumn = row[1];
            fk.toTable   = row[2]; fk.toColumn   = row[3];
            out.push_back(fk);
        }
        return true;
    }

    bool GetIndexes(const wxString& /*db*/, const wxString& table,
                    std::vector<IndexInfo>& out, wxString& err) override
    {
        QueryResult r;
        // FOR XML PATH concat works on all supported versions (unlike STRING_AGG).
        if (!Execute(
                L"SELECT i.name, i.is_unique, "
                L"STUFF((SELECT ', '+c.name FROM sys.index_columns ic "
                L"  JOIN sys.columns c ON c.object_id=ic.object_id AND c.column_id=ic.column_id "
                L"  WHERE ic.object_id=i.object_id AND ic.index_id=i.index_id "
                L"  ORDER BY ic.key_ordinal FOR XML PATH('')),1,2,'') "
                L"FROM sys.indexes i WHERE i.object_id=OBJECT_ID('" + QualLit(table) +
                L"') AND i.type>0 AND i.name IS NOT NULL ORDER BY i.name", r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            IndexInfo idx;
            idx.name = row[0];
            idx.unique = (row[1] == L"1");
            idx.columns = row[2];
            out.push_back(idx);
        }
        return true;
    }

    // SQL Server has no SHOW CREATE TABLE — synthesise a simplified definition.
    bool GetCreateDdl(const wxString& db, const wxString& table,
                      wxString& ddl, wxString& err) override
    {
        std::vector<ColumnInfo> cols;
        if (!GetColumns(db, table, cols, err)) return false;
        const auto st = SplitSchema(table);
        ddl = L"-- 简化定义(不含 CHECK/计算列/复杂约束)\n";
        ddl += L"CREATE TABLE " + QuoteIdent(st.first, Dialect::SqlServer) + L"." +
               QuoteIdent(st.second, Dialect::SqlServer) + L" (\n";
        wxString pk;
        for (size_t i = 0; i < cols.size(); ++i) {
            const ColumnInfo& c = cols[i];
            ddl += L"    " + QuoteIdent(c.name, Dialect::SqlServer) + L" " + c.type;
            if (!c.length.IsEmpty()) ddl += L"(" + c.length + L")";
            if (c.notNull) ddl += L" NOT NULL";
            if (!c.defaultVal.IsEmpty()) ddl += L" DEFAULT " + c.defaultVal;
            if (c.key == L"PK") pk += (pk.IsEmpty() ? L"" : L", ") +
                                       QuoteIdent(c.name, Dialect::SqlServer);
            ddl += (i + 1 < cols.size() || !pk.IsEmpty()) ? L",\n" : L"\n";
        }
        if (!pk.IsEmpty()) ddl += L"    PRIMARY KEY (" + pk + L")\n";
        ddl += L")";
        return true;
    }

    bool ListViews(const wxString& /*db*/, std::vector<wxString>& out,
                   wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT s.name+'.'+v.name FROM sys.views v "
                     L"JOIN sys.schemas s ON s.schema_id=v.schema_id ORDER BY 1", r, err))
            return false;
        for (const auto& row : r.rows) if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool GetViewDdl(const wxString& /*db*/, const wxString& view,
                    wxString& ddl, wxString& err) override
    { return ObjectDef(view, ddl, err); }

    bool ListRoutines(const wxString& /*db*/, std::vector<RoutineInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT s.name+'.'+o.name, "
                     L"CASE WHEN o.type='P' THEN 'PROCEDURE' ELSE 'FUNCTION' END "
                     L"FROM sys.objects o JOIN sys.schemas s ON s.schema_id=o.schema_id "
                     L"WHERE o.type IN ('P','FN','TF','IF') "
                     L"ORDER BY (CASE WHEN o.type='P' THEN 1 ELSE 0 END), o.name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetRoutineDdl(const wxString& /*db*/, const RoutineInfo& rt,
                       wxString& ddl, wxString& err) override
    { return ObjectDef(rt.name, ddl, err); }

    bool ListTriggers(const wxString& /*db*/, std::vector<TriggerInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT tr.name, s.name+'.'+t.name FROM sys.triggers tr "
                     L"JOIN sys.tables t ON t.object_id=tr.parent_id "
                     L"JOIN sys.schemas s ON s.schema_id=t.schema_id "
                     L"WHERE tr.is_ms_shipped=0 ORDER BY tr.name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetTriggerDdl(const wxString& /*db*/, const TriggerInfo& tg,
                       wxString& ddl, wxString& err) override
    {
        // Trigger lives in its table's schema.
        const wxString schema = SplitSchema(tg.table).first;
        return ObjectDef(schema + L"." + tg.name, ddl, err);
    }

    bool DumpTableData(const wxString& /*db*/, const wxString& table,
                       const std::function<void(const wxString&)>& emit,
                       wxString& err) override
    {
        if (!connected_) { err = L"未连接"; return false; }
        const auto st = SplitSchema(table);
        const wxString qtable = QuoteIdent(st.first, Dialect::SqlServer) + L"." +
                                QuoteIdent(st.second, Dialect::SqlServer);

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
                               Dialect::SqlServer);
        }
        const wxString prefix = L"INSERT INTO " + qtable + L" (" + cols + L") VALUES (";

        while (SQL_SUCCEEDED(SQLFetch(stmt))) {
            wxString vals;
            for (SQLSMALLINT i = 1; i <= nf; ++i) {
                if (i > 1) vals += L", ";
                if (IsBinarySqlType(types[i])) {
                    bool isNull = false;
                    vals += GetCellBinary(stmt, i, isNull);
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

    bool GetCreateDatabaseCaps(DbCreateCaps& out, wxString& err) override
    {
        (void)err;
        out = DbCreateCaps{};
        out.supported = true;
        DbCreateOption coll;
        coll.id = L"collation"; coll.label = L"排序规则 (COLLATE)";
        coll.kind = DbCreateOption::Kind::FreeText;
        coll.choices = { L"SQL_Latin1_General_CP1_CI_AS", L"Chinese_PRC_CI_AS",
                         L"Latin1_General_100_CI_AS_SC" };
        out.options.push_back(coll);
        return true;
    }

    wxString BuildCreateDatabaseSql(const DbCreateRequest& req) const override
    {
        wxString sql = L"CREATE DATABASE " + QuoteIdent(req.name, Dialect::SqlServer);
        const wxString coll = req.Value(L"collation");
        // COLLATE takes an identifier — whitelist [A-Za-z0-9_].
        if (!coll.IsEmpty() && IsSafeKeyword(coll)) sql += L" COLLATE " + coll;
        return sql;
    }

    bool GetDatabaseOverview(DbOverview& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT name, state_desc, ISNULL(collation_name,''), "
                     L"recovery_model_desc FROM sys.databases ORDER BY name", r, err))
            return false;
        out = DbOverview{};
        out.columns = { {L"name", L"名称"}, {L"state", L"状态"},
                        {L"collation", L"排序规则"}, {L"recovery", L"恢复模式"} };
        out.rows = std::move(r.rows);
        return true;
    }

    wxString ServerVersion() const override { return version_; }

    // ---- user / privilege administration (logic in db/UserAdminMssql.cpp) ----
    bool SupportsUserAdmin() const override { return true; }
    bool ListUsers(std::vector<UserInfo>& o, wxString& e) override
        { return useradmin::MssqlListUsers(this, o, e); }
    bool GetUserGrants(const wxString& u, const wxString&, UserGrants& o, wxString& e) override
        { return useradmin::MssqlGetGrants(this, u, o, e); }
    bool SaveUser(const UserSpec& s, bool isNew, wxString& e) override
        { return useradmin::MssqlSaveUser(this, s, isNew, e); }
    bool DropUser(const wxString& u, const wxString&, wxString& e) override
        { return useradmin::MssqlDropUser(this, u, e); }
    UserAdminModel GetUserAdminModel() const override { return useradmin::MssqlModel(); }

private:
    static bool IsSafeKeyword(const wxString& s)
    {
        if (s.IsEmpty()) return false;
        for (wxUniChar ch : s) {
            const wchar_t c = static_cast<wchar_t>(ch.GetValue());
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'_'))
                return false;
        }
        return true;
    }

    // "schema.name" literal for OBJECT_ID('…'), single quotes escaped.
    static wxString QualLit(const wxString& qualified)
    {
        const auto st = SplitSchema(qualified);
        return EscLit(st.first + L"." + st.second);
    }

    bool ObjectDef(const wxString& qualified, wxString& ddl, wxString& err)
    {
        QueryResult r;
        if (!Execute(L"SELECT OBJECT_DEFINITION(OBJECT_ID('" + QualLit(qualified) +
                     L"'))", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty() && r.rows[0][0] != L"NULL")
            ddl = r.rows[0][0];
        return true;
    }

    // Read a cell as text (any type ODBC can convert). NULL → the string "NULL",
    // matching the QueryResult convention used across the drivers.
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

    // Read a binary cell as an 0x… literal; sets isNull if the value is NULL.
    static wxString GetCellBinary(SQLHSTMT st, SQLUSMALLINT col, bool& isNull)
    {
        isNull = false;
        unsigned char buf[4096];
        SQLLEN ind = 0;
        wxString hex = L"0x";
        bool any = false;
        for (;;) {
            const SQLRETURN rc = SQLGetData(st, col, SQL_C_BINARY, buf, sizeof(buf), &ind);
            if (rc == SQL_NO_DATA || !SQL_SUCCEEDED(rc)) break;
            if (ind == SQL_NULL_DATA) { isNull = true; return L"NULL"; }
            const size_t n = (ind == SQL_NO_TOTAL || ind > (SQLLEN)sizeof(buf))
                                 ? sizeof(buf) : static_cast<size_t>(ind);
            hex += HexLiteral(buf, n).Mid(2);   // append without the "0x" prefix
            any = true;
            if (rc == SQL_SUCCESS) break;
        }
        return any ? hex : L"0x";
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
    core::ConnectionProfile profile_;   // for EffectiveProfile()
};

} // namespace

std::unique_ptr<IConnection> CreateSqlServerConnection()
{
    return std::make_unique<SqlServerConnection>();
}

// Test seam: expose the pure connection-string builder without a live server.
wxString SqlServer_BuildConnStr(const core::ConnectionProfile& p)
{
    return BuildConnStr(p);
}

} // namespace db
