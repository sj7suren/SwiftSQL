// OracleOciDriver.cpp — Oracle Database over the *native* OCI C API (Oracle Call
// Interface). No ODBC, no third-party wrapper. The Instant Client's oci.dll is
// *not* linked at build time (no oci.lib): every OCI function is late-bound from
// oci.dll at first connect via the OciApi loader (OciLoader.h). SwiftSQL builds
// and runs with no Oracle client present; Oracle just reports "client library
// not found" until the user supplies one. 达梦 DM keeps its own ODBC driver
// (see OracleDriver.cpp). We still #include <oci.h> for the types/structs/
// constants only — never call an OCI function directly (that would drag oci.lib
// symbols back in); everything goes through oci_->Fn(...).
//
// Design notes / deliberate simplifications:
//   * Connection uses OCILogon2 with an EZCONNECT descriptor "host:port/service"
//     (service = profile.database). One call, no tnsnames.ora needed.
//   * The environment is created with OCIEnvNlsCreate forcing the client charset
//     to AL32UTF8 (id 873), so every string OCI hands us is UTF-8 — we decode with
//     wxString::FromUTF8 and sidestep NLS_LANG entirely on the read path. SQL text
//     we send is likewise UTF-8 (OCI transcodes to the DB charset).
//   * Every result column is bound with OCIDefineByPos as SQLT_STR (string), with a
//     NULL indicator. Numbers/dates come back as their default text form; RAW comes
//     back as hex (OCI's implicit RAW→string conversion), which the dump path wraps
//     in HEXTORAW(). LOB columns (CLOB/BLOB) are best-effort only — see honest gate.
//   * Cancel() calls OCIBreak on the service context (async-safe, hence OCI_THREADED).
//
// Schema introspection reuses the Oracle data dictionary (ALL_TABLES,
// ALL_TAB_COLUMNS, ALL_CONSTRAINTS, ALL_INDEXES, DBMS_METADATA.GET_DDL) — the same
// SQL the DM/ODBC driver runs, copied here so each file is self-contained. Reports
// Dialect::Oracle (double-quoted identifiers, OFFSET/FETCH paging).
//
// HONEST GATE: compile-verified only. No Oracle server or runtime OCI DLLs on the
// build host, so nothing below was executed. Unverified at runtime: the EZCONNECT
// string shape, UTF-8/NLS decoding of non-ASCII data, the SQLT_STR buffer sizing
// for wide/number/date columns, LOB (CLOB/BLOB) fetch (SQLT_STR conversion of a LOB
// may fail or truncate — treat large-object export as untested), and OCIBreak
// interaction with an in-flight fetch.
#include "db/DbDriver.h"
#include "db/OciLoader.h"

#include <oci.h>
#include <wx/stopwatch.h>
#include <cstring>
#include <mutex>
#include <vector>

namespace db {
namespace {

// Oracle charset id for AL32UTF8 (client-side); forces UTF-8 on every OCI string.
constexpr ub2 kCharsetAl32Utf8 = 873;

// ---- pure helpers (no server needed) ---------------------------------------

// Escape a string literal for '…' (double single quotes — standard SQL / Oracle).
wxString EscLit(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return e;
}

// Numeric OCI internal type codes (OCI_ATTR_DATA_TYPE) — emitted unquoted in dumps.
bool IsNumericOci(ub2 t)
{
    switch (t) {
    case SQLT_NUM: case SQLT_INT: case SQLT_FLT: case SQLT_VNU:
    case SQLT_UIN: case SQLT_IBFLOAT: case SQLT_IBDOUBLE:
        return true;
    default:
        return false;
    }
}

// Binary OCI type codes — bound as SQLT_STR still yields hex text for RAW; wrapped
// in HEXTORAW() on dump. LOB binaries (BLOB/BFILE) are best-effort (see gate).
bool IsBinaryOci(ub2 t)
{
    switch (t) {
    case SQLT_BIN: case SQLT_LBI: case SQLT_BLOB: case SQLT_BFILEE:
        return true;
    default:
        return false;
    }
}

bool IsDateOci(ub2 t)
{
    switch (t) {
    case SQLT_DAT: case SQLT_TIMESTAMP: case SQLT_TIMESTAMP_TZ:
    case SQLT_TIMESTAMP_LTZ:
        return true;
    default:
        return false;
    }
}

// Byte length of a SQLT_STR define buffer for a column of the given OCI type/size.
// Numbers/dates convert to short text; RAW → 2× hex; char types may widen when the
// DB charset transcodes to UTF-8, so allow 4×. Capped so ub2 rlen can hold it.
ub4 DefineBufLen(ub2 dtype, ub2 dsize)
{
    ub4 n;
    if (IsNumericOci(dtype))                 n = 64;
    else if (IsDateOci(dtype))               n = 128;
    else if (dtype == SQLT_BIN || dtype == SQLT_LBI)
                                             n = static_cast<ub4>(dsize) * 2u + 2u;
    else {
        const ub4 base = dsize ? dsize : 4000u;   // LONG/LOB report 0 → best-effort
        n = base * 4u + 2u;
    }
    if (n > 65530u) n = 65530u;   // rlen is ub2; keep buffer addressable by it
    if (n < 8u)     n = 8u;
    return n;
}

// ---- OCI statement scratch: buffers must outlive the whole fetch loop -------
// All vectors are sized once (before any OCIDefineByPos) so element addresses
// passed to OCI stay stable across fetches.
struct OciStmt {
    OCIStmt*                        h = nullptr;
    int                             ncols = 0;
    bool                            isSelect = false;
    std::vector<std::vector<char>>  buf;    // per-column value buffer
    std::vector<sb2>                ind;    // per-column NULL indicator (-1 = NULL)
    std::vector<ub2>                rlen;   // per-column returned length
    std::vector<ub2>                dtype;  // per-column OCI internal type code
    std::vector<wxString>           names;  // per-column name
};

// RAII: release the statement handle when a fetch/dump loop leaves scope — even
// via an exception thrown by CellText/CellRaw/push_back/emit/sink. Mirrors
// Close(): it guards on s.h, so it is a harmless no-op after an explicit Close()
// on the normal path (Close() nulls s.h) and only fires on the exception path.
struct OciStmtGuard {
    OciStmt&      s;
    const OciApi* oci;
    OCIError*     err;
    OciStmtGuard(OciStmt& stmt, const OciApi* api, OCIError* e)
        : s(stmt), oci(api), err(e) {}
    ~OciStmtGuard()
    {
        if (s.h) {
            oci->OCIStmtRelease(s.h, err, nullptr, 0, OCI_DEFAULT);
            s.h = nullptr;
        }
    }
    OciStmtGuard(const OciStmtGuard&) = delete;
    OciStmtGuard& operator=(const OciStmtGuard&) = delete;
};

// ---- driver ----------------------------------------------------------------

class OracleOciConnection : public IConnection {
public:
    OracleOciConnection() = default;
    ~OracleOciConnection() override { Disconnect(); }

    Dialect GetDialect() const override { return Dialect::Oracle; }

    // profile_ holds exactly the profile passed to Connect() below (the caller's
    // tunnel-rewritten `eff`, when SSH is in play).
    const core::ConnectionProfile& EffectiveProfile() const override { return profile_; }

    bool Connect(const core::ConnectionProfile& p, wxString& err) override
    {
        Disconnect();
        profile_ = p;
        // Late-bind oci.dll (cached). Missing client library → honest error, no crash.
        const OciApi* api = GetOciApi(err);
        if (!api) return false;    // err set by GetOciApi (client library not found)
        // Pin a PRIVATE copy of the entry-point table for this connection's lifetime.
        // GetOciApi() returns &g_api (the loader's global); a later SetOciLibraryPath()
        // (Preferences → OCI path change) does `g_api = OciApi{}`, nulling every pointer
        // in that global. A live connection still pointing at &g_api would then call a
        // null function pointer on its next query → hard crash. Copying the table here
        // (and never FreeLibrary'ing the dll — see OciLoader's leak-by-design note) keeps
        // our entry points valid for as long as this connection lives.
        ociTable_ = *api;
        oci_ = &ociTable_;

        // Threaded env so OCIBreak may fire from the UI thread; UTF-8 client charset.
        sword rc = oci_->OCIEnvNlsCreate(&env_, OCI_THREADED | OCI_OBJECT, nullptr,
                                   nullptr, nullptr, nullptr, 0, nullptr,
                                   kCharsetAl32Utf8, kCharsetAl32Utf8);
        if ((rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) || !env_) {
            err = L"无法创建 OCI 环境 (OCIEnvNlsCreate)"; return false;
        }
        if (oci_->OCIHandleAlloc(env_, reinterpret_cast<void**>(&err_), OCI_HTYPE_ERROR,
                           0, nullptr) != OCI_SUCCESS || !err_) {
            err = L"无法分配 OCI 错误句柄";
            Disconnect();
            return false;
        }

        // EZCONNECT: host:port/service (service = profile.database, may be blank).
        wxString dsn = p.host + wxString::Format(L":%d", p.port);
        if (!p.database.IsEmpty()) dsn += L"/" + p.database;

        const wxScopedCharBuffer u = p.user.utf8_str();
        const wxScopedCharBuffer pw = p.password.utf8_str();
        const wxScopedCharBuffer d  = dsn.utf8_str();
        rc = oci_->OCILogon2(env_, err_, &svc_,
                       reinterpret_cast<const OraText*>(u.data()),
                       static_cast<ub4>(u.length()),
                       reinterpret_cast<const OraText*>(pw.data()),
                       static_cast<ub4>(pw.length()),
                       reinterpret_cast<const OraText*>(d.data()),
                       static_cast<ub4>(d.length()), OCI_DEFAULT);
        if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
            err = OciError(rc);
            Disconnect();
            return false;
        }
        connected_ = true;

        // Connected user's schema — the owner used when a call passes empty database.
        QueryResult r; wxString e2;
        if (Execute(L"SELECT USER FROM DUAL", r, e2) && !r.rows.empty() &&
            !r.rows[0].empty())
            defaultSchema_ = r.rows[0][0];

        // Server banner (best-effort; blank on failure).
        if (Execute(L"SELECT version FROM product_component_version WHERE ROWNUM=1",
                    r, e2) && !r.rows.empty() && !r.rows[0].empty())
            version_ = r.rows[0][0];
        else if (Execute(L"SELECT banner FROM v$version WHERE ROWNUM=1", r, e2) &&
                 !r.rows.empty() && !r.rows[0].empty())
            version_ = r.rows[0][0];
        return true;
    }

    void Disconnect() override
    {
        std::lock_guard<std::mutex> lk(svcMx_);
        if (oci_) {
            if (svc_) { if (err_) oci_->OCILogoff(svc_, err_); svc_ = nullptr; }
            if (err_) { oci_->OCIHandleFree(err_, OCI_HTYPE_ERROR); err_ = nullptr; }
            if (env_) { oci_->OCIHandleFree(env_, OCI_HTYPE_ENV);   env_ = nullptr; }
        }
        svc_ = nullptr; err_ = nullptr; env_ = nullptr;
        connected_ = false;
    }

    bool IsConnected() const override { return connected_; }

    // Async interrupt of an in-flight OCI call on this service context.
    void Cancel() override
    {
        std::lock_guard<std::mutex> lk(svcMx_);
        if (oci_ && svc_ && err_) oci_->OCIBreak(svc_, err_);
    }

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        if (!connected_) { err = L"未连接"; return false; }
        out = QueryResult();

        wxStopWatch sw;
        OciStmt s;
        if (!Open(sql, s, err)) return false;
        OciStmtGuard guard(s, oci_, err_);   // exception-safe release of s.h

        if (s.isSelect) {
            out.isSelect = true;
            out.columns = s.names;
            while (Fetch(s)) {
                std::vector<wxString> row;
                row.reserve(s.ncols);
                for (int i = 0; i < s.ncols; ++i) row.push_back(CellText(s, i));
                out.rows.push_back(std::move(row));
            }
            out.affected = out.rows.size();
        } else {
            out.isSelect = false;
            ub4 rc = 0;
            oci_->OCIAttrGet(s.h, OCI_HTYPE_STMT, &rc, nullptr, OCI_ATTR_ROW_COUNT, err_);
            out.affected = rc;
        }
        Close(s);
        out.elapsedMs = sw.Time();
        return true;
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
        std::vector<wxString> pk;
        wxString ign; GetPrimaryKey(database, table, pk, ign);

        QueryResult r;
        if (!Execute(
                L"SELECT column_id, column_name, data_type, data_length, "
                L"data_precision, data_scale, nullable, data_default "
                L"FROM all_tab_columns WHERE owner='" + owner + L"' "
                L"AND table_name='" + EscLit(table) + L"' ORDER BY column_id", r, err))
            return false;

        // Best-effort extended introspection via side queries (see OracleDriver).
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
            c.notNull = (row[6] == L"N");
            c.defaultVal = row[7];
            c.defaultVal.Trim(true).Trim(false);
            if (c.defaultVal == L"NULL") c.defaultVal.clear();
            for (const auto& k : pk) if (k == c.name) { c.key = L"PK"; break; }
            // ---- extended introspection (best-effort) ----
            if (row[5] != L"NULL") c.scale = row[5];   // DATA_SCALE (numeric only)
            c.comment = commentFor(c.name);
            wxString ve = virtExprFor(c.name);
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

        OciStmt s;
        if (!Open(L"SELECT * FROM " + qtable, s, err)) return false;
        OciStmtGuard guard(s, oci_, err_);   // exception-safe release of s.h

        wxString cols;
        for (int i = 0; i < s.ncols; ++i)
            cols += (i ? L", " : L"") + QuoteIdent(s.names[i], Dialect::Oracle);
        const wxString prefix = L"INSERT INTO " + qtable + L" (" + cols + L") VALUES (";

        while (Fetch(s)) {
            wxString vals;
            for (int i = 0; i < s.ncols; ++i) {
                if (i) vals += L", ";
                if (s.ind[i] == -1) { vals += L"NULL"; continue; }
                const wxString cell = CellRaw(s, i);
                if (IsBinaryOci(s.dtype[i]))
                    vals += cell.IsEmpty() ? wxString(L"NULL")
                                           : L"HEXTORAW('" + cell + L"')";
                else if (IsNumericOci(s.dtype[i]))
                    vals += cell;
                else
                    vals += L"'" + EscLit(cell) + L"'";
            }
            emit(prefix + vals + L");\n");
        }
        Close(s);
        return true;
    }

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

        OciStmt s;
        if (!Open(sql, s, err)) return false;
        OciStmtGuard guard(s, oci_, err_);   // exception-safe release of s.h

        while (Fetch(s)) {
            std::vector<Cell> cells;
            cells.reserve(s.ncols);
            for (int i = 0; i < s.ncols; ++i) {
                Cell cell;
                if (s.ind[i] == -1) {
                    cell.kind = CellKind::Null;
                } else if (IsBinaryOci(s.dtype[i])) {
                    cell.kind = CellKind::Binary; cell.text = CellRaw(s, i);
                } else if (IsNumericOci(s.dtype[i])) {
                    cell.kind = CellKind::Numeric; cell.text = CellRaw(s, i);
                } else {
                    cell.kind = CellKind::Text; cell.text = CellRaw(s, i);
                }
                cells.push_back(std::move(cell));
            }
            if (!sink(cells)) break;   // cancellation
        }
        Close(s);
        return true;
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

    // ---- OCI statement plumbing --------------------------------------------

    // Prepare + execute `sql`. For SELECT, describe columns and bind them all as
    // SQLT_STR so a plain Fetch loop can follow. Non-SELECT autocommits (parity
    // with the ODBC drivers' default autocommit).
    bool Open(const wxString& sql, OciStmt& s, wxString& err)
    {
        const wxScopedCharBuffer q = sql.utf8_str();
        sword rc = oci_->OCIStmtPrepare2(svc_, &s.h, err_,
                                   reinterpret_cast<const OraText*>(q.data()),
                                   static_cast<ub4>(q.length()), nullptr, 0,
                                   OCI_NTV_SYNTAX, OCI_DEFAULT);
        if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
            // Release the handle OCIStmtPrepare2 allocated before bailing — the
            // execute/param/define failure paths below all do this; keep the two
            // (now three) error routes consistent so prepare failure can't leak.
            err = OciError(rc);
            oci_->OCIStmtRelease(s.h, err_, nullptr, 0, OCI_DEFAULT);
            s.h = nullptr;
            return false;
        }

        ub2 stmtType = 0;
        oci_->OCIAttrGet(s.h, OCI_HTYPE_STMT, &stmtType, nullptr, OCI_ATTR_STMT_TYPE, err_);
        s.isSelect = (stmtType == OCI_STMT_SELECT);

        const ub4 iters = s.isSelect ? 0u : 1u;
        const ub4 mode  = s.isSelect ? OCI_DEFAULT : OCI_COMMIT_ON_SUCCESS;
        rc = oci_->OCIStmtExecute(svc_, s.h, err_, iters, 0, nullptr, nullptr, mode);
        if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
            err = OciError(rc);
            oci_->OCIStmtRelease(s.h, err_, nullptr, 0, OCI_DEFAULT);
            s.h = nullptr;
            return false;
        }
        if (!s.isSelect) return true;

        ub4 ncols = 0;
        oci_->OCIAttrGet(s.h, OCI_HTYPE_STMT, &ncols, nullptr, OCI_ATTR_PARAM_COUNT, err_);
        s.ncols = static_cast<int>(ncols);

        // Pre-size everything before any define, so element addresses stay stable.
        s.buf.resize(s.ncols);
        s.ind.assign(s.ncols, 0);
        s.rlen.assign(s.ncols, 0);
        s.dtype.assign(s.ncols, 0);
        s.names.resize(s.ncols);

        for (int i = 0; i < s.ncols; ++i) {
            OCIParam* param = nullptr;
            if (oci_->OCIParamGet(s.h, OCI_HTYPE_STMT, err_,
                            reinterpret_cast<void**>(&param),
                            static_cast<ub4>(i + 1)) != OCI_SUCCESS || !param) {
                err = OciError(OCI_ERROR);
                oci_->OCIStmtRelease(s.h, err_, nullptr, 0, OCI_DEFAULT);
                s.h = nullptr;
                return false;
            }
            OraText* nm = nullptr; ub4 nmLen = 0;
            oci_->OCIAttrGet(param, OCI_DTYPE_PARAM, &nm, &nmLen, OCI_ATTR_NAME, err_);
            s.names[i] = wxString::FromUTF8(reinterpret_cast<const char*>(nm), nmLen);

            ub2 dtype = 0, dsize = 0;
            oci_->OCIAttrGet(param, OCI_DTYPE_PARAM, &dtype, nullptr, OCI_ATTR_DATA_TYPE, err_);
            oci_->OCIAttrGet(param, OCI_DTYPE_PARAM, &dsize, nullptr, OCI_ATTR_DATA_SIZE, err_);
            s.dtype[i] = dtype;

            const ub4 bufLen = DefineBufLen(dtype, dsize);
            s.buf[i].assign(bufLen, '\0');

            OCIDefine* def = nullptr;
            rc = oci_->OCIDefineByPos(s.h, &def, err_, static_cast<ub4>(i + 1),
                                s.buf[i].data(), static_cast<sb4>(bufLen), SQLT_STR,
                                &s.ind[i], &s.rlen[i], nullptr, OCI_DEFAULT);
            if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
                err = OciError(rc);
                oci_->OCIStmtRelease(s.h, err_, nullptr, 0, OCI_DEFAULT);
                s.h = nullptr;
                return false;
            }
        }
        return true;
    }

    bool Fetch(OciStmt& s)
    {
        const sword rc = oci_->OCIStmtFetch2(s.h, err_, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
        return rc == OCI_SUCCESS || rc == OCI_SUCCESS_WITH_INFO;
    }

    void Close(OciStmt& s)
    {
        if (s.h) { oci_->OCIStmtRelease(s.h, err_, nullptr, 0, OCI_DEFAULT); s.h = nullptr; }
    }

    // Raw cell text (NULL caller-checked via s.ind). SQLT_STR is null-terminated;
    // rlen bounds the copy for values that may embed a null.
    static wxString CellRaw(const OciStmt& s, int i)
    {
        const char* p = s.buf[i].data();
        size_t n = s.rlen[i];
        if (n > s.buf[i].size()) n = s.buf[i].size();
        const size_t z = ::strnlen(p, n);   // stop at the OCI null terminator
        return wxString::FromUTF8(p, z);
    }

    // Text for the stringified QueryResult path: NULL → the literal "NULL".
    static wxString CellText(const OciStmt& s, int i)
    {
        if (s.ind[i] == -1) return wxString(L"NULL");
        return CellRaw(s, i);
    }

    // Latest OCI error text (client charset = UTF-8). rc used only to shape the
    // fallback when no error record is available.
    wxString OciError(sword rc)
    {
        if (oci_ && err_) {
            OraText buf[2048]; sb4 code = 0;
            const sword r = oci_->OCIErrorGet(err_, 1, nullptr, &code, buf,
                                        static_cast<ub4>(sizeof(buf)), OCI_HTYPE_ERROR);
            if (r == OCI_SUCCESS || r == OCI_SUCCESS_WITH_INFO) {
                wxString msg = wxString::FromUTF8(reinterpret_cast<const char*>(buf));
                msg.Trim(true).Trim(false);
                if (!msg.IsEmpty()) return msg;
            }
        }
        return wxString::Format(L"OCI 错误 (rc=%d)", static_cast<int>(rc));
    }

    OciApi     ociTable_{};         // per-connection copy of the oci.dll entry points,
                                    // pinned at Connect() so a global reload can't null it
    const OciApi* oci_ = nullptr;   // → &ociTable_ once connected (kept for call-site style)
    OCIEnv*    env_ = nullptr;
    OCIError*  err_ = nullptr;
    OCISvcCtx* svc_ = nullptr;     // from OCILogon2 (owns server+session internally)
    std::mutex svcMx_;             // guards svc_ across Cancel()/Disconnect() vs worker
    bool       connected_ = false;
    wxString   version_;
    wxString   defaultSchema_;
    core::ConnectionProfile profile_;   // for EffectiveProfile()
};

} // namespace

std::unique_ptr<IConnection> CreateOracleConnection()
{
    return std::make_unique<OracleOciConnection>();
}

} // namespace db
