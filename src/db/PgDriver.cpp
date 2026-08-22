// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// PgDriver.cpp — PostgreSQL connection via libpq.
#include "db/DbDriver.h"
#include "db/PgSql.h"          // detail:: pure DDL-rendering helpers
#include "db/PgSchemaRead.h"   // real GetTableSchema introspection (A2, kept out of
                               // this file to stay under the 1000-line charter)
#include "db/PgStream.h"       // pgstream:: byte-order ORDER BY + batch INSERT
#include "core/ConnectionProfile.h"   // stored by value for ensureDb() reconnects

#include <libpq-fe.h>
#include <wx/stopwatch.h>

namespace db {
namespace {

// RAII: PQclear the result when it leaves scope — even via an exception from
// FromUTF8/push_back/emit/sink. Guards on the pointer so it is safe for the
// null result PQexec can hand back on OOM. In the single-row loops one guard
// wraps each per-row PGresult so a throw mid-row cannot leak it.
struct PgResultGuard {
    PGresult* res;
    explicit PgResultGuard(PGresult* r) : res(r) {}
    ~PgResultGuard() { if (res) PQclear(res); }
    PgResultGuard(const PgResultGuard&) = delete;
    PgResultGuard& operator=(const PgResultGuard&) = delete;
};

// conninfo values need single quotes + backslash escaping
wxString EscapeConninfo(const wxString& v)
{
    wxString e = v;
    e.Replace(L"\\", L"\\\\");
    e.Replace(L"'", L"\\'");
    return L"'" + e + L"'";
}

class PgConnection : public IConnection {
public:
    ~PgConnection() override { Disconnect(); }

    Dialect GetDialect() const override { return Dialect::Postgres; }

    // prof_ already holds exactly the profile passed to Connect() below (the
    // caller's tunnel-rewritten `eff`, when SSH is in play) — reuse it verbatim.
    const core::ConnectionProfile& EffectiveProfile() const override { return prof_; }

    // Build a libpq conninfo string for `p`, but connecting to `dbname`. Extracted so
    // ensureDb() can reconnect to another database with the same host/user/SSL/options.
    wxString BuildConninfo(const core::ConnectionProfile& p, const wxString& dbname) const
    {
        const int timeout = p.connectTimeout > 0 ? p.connectTimeout : 8;
        const wxString enc = p.charset.IsEmpty() ? wxString(L"UTF8") : p.charset;
        const wxString sslmode = p.sslMode.IsEmpty() ? wxString(L"prefer") : p.sslMode;
        wxString conninfo = wxString::Format(
            L"host=%s port=%d user=%s password=%s dbname=%s "
            L"connect_timeout=%d client_encoding=%s sslmode=%s",
            EscapeConninfo(p.host), p.port,
            EscapeConninfo(p.user), EscapeConninfo(p.password),
            EscapeConninfo(dbname),
            timeout, enc, EscapeConninfo(sslmode));
        if (!p.sslCaCert.IsEmpty())
            conninfo += L" sslrootcert=" + EscapeConninfo(p.sslCaCert);
        if (!p.sslClientCert.IsEmpty())
            conninfo += L" sslcert=" + EscapeConninfo(p.sslClientCert);
        if (!p.sslClientKey.IsEmpty())
            conninfo += L" sslkey=" + EscapeConninfo(p.sslClientKey);
        if (!p.sslClientKeyPassword.IsEmpty())
            conninfo += L" sslpassword=" + EscapeConninfo(p.sslClientKeyPassword);
        if (!p.appName.IsEmpty())
            conninfo += L" application_name=" + EscapeConninfo(p.appName);
        if (p.keepalive)
            conninfo += wxString::Format(L" keepalives=1 keepalives_idle=%d",
                                         p.keepaliveInterval > 0 ? p.keepaliveInterval : 30);

        // server-side session options via `options=-c key=val`
        wxString opts;
        if (!p.defaultSchema.IsEmpty()) opts += L" -c search_path=" + p.defaultSchema;
        if (!p.timezone.IsEmpty())      opts += L" -c timezone=" + p.timezone;
        if (p.readOnly)                 opts += L" -c default_transaction_read_only=on";
        if (!p.isolationLevel.IsEmpty()) {
            wxString iso = p.isolationLevel.Lower();
            iso.Replace(L" ", L"\\ ");
            opts += L" -c default_transaction_isolation=" + iso;
        }
        if (!opts.IsEmpty())
            conninfo += L" options=" + EscapeConninfo(opts.Trim(false));
        return conninfo;
    }

    // PostgreSQL binds one database per libpq session and can't query another
    // database's catalog (no cross-DB queries, no USE). So to list/browse a database
    // OTHER than the one we connected to, reconnect to it. No-op if already there or
    // `db` is empty. This is why expanding a non-connected DB showed no tables.
    bool ensureDb(const wxString& db, wxString& err)
    {
        if (db.IsEmpty() || db == boundDb_) return true;
        PGconn* nc = PQconnectdb(BuildConninfo(prof_, db).utf8_str());
        if (PQstatus(nc) != CONNECTION_OK) {
            err = wxString::FromUTF8(PQerrorMessage(nc)).Trim();
            PQfinish(nc);
            return false;
        }
        if (conn_) PQfinish(conn_);
        conn_ = nc;
        boundDb_ = db;
        return true;
    }

    bool Connect(const core::ConnectionProfile& p, wxString& err) override
    {
        Disconnect();
        prof_ = p;
        const wxString dbname = p.database.IsEmpty() ? wxString(L"postgres") : p.database;
        conn_ = PQconnectdb(BuildConninfo(p, dbname).utf8_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            err = wxString::FromUTF8(PQerrorMessage(conn_)).Trim();
            PQfinish(conn_);
            conn_ = nullptr;
            return false;
        }
        boundDb_ = dbname;
        const int v = PQserverVersion(conn_);   // 160004 → "16.4"
        version_ = wxString::Format(L"%d.%d", v / 10000, v % 10000);
        return true;
    }

    void Disconnect() override
    {
        if (conn_) { PQfinish(conn_); conn_ = nullptr; }
    }

    bool IsConnected() const override
    {
        return conn_ && PQstatus(conn_) == CONNECTION_OK;
    }

    // Cancel the running query. PQgetCancel/PQcancel are thread-safe and can be
    // called while another thread is blocked in PQexec.
    void Cancel() override
    {
        if (!conn_) return;
        if (PGcancel* c = PQgetCancel(conn_)) {
            char errbuf[256];
            PQcancel(c, errbuf, sizeof errbuf);
            PQfreeCancel(c);
        }
    }

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        if (!conn_) { err = L"未连接"; return false; }
        out = QueryResult();

        wxStopWatch sw;
        PGresult* res = PQexec(conn_, sql.utf8_str());
        PgResultGuard resGuard(res);   // clears at scope exit (was manual PQclear)
        const ExecStatusType st = PQresultStatus(res);

        if (st == PGRES_TUPLES_OK) {
            out.isSelect = true;
            const int nf = PQnfields(res);
            const int nr = PQntuples(res);
            for (int i = 0; i < nf; ++i)
                out.columns.push_back(wxString::FromUTF8(PQfname(res, i)));
            out.rows.reserve(nr);
            for (int r = 0; r < nr; ++r) {
                std::vector<wxString> row;
                row.reserve(nf);
                for (int c = 0; c < nf; ++c)
                    row.push_back(PQgetisnull(res, r, c)
                                  ? wxString(L"NULL")
                                  : wxString::FromUTF8(PQgetvalue(res, r, c)));
                out.rows.push_back(std::move(row));
            }
            out.affected = nr;
        } else if (st == PGRES_COMMAND_OK) {
            out.isSelect = false;
            const wxString cmdRows = wxString::FromUTF8(PQcmdTuples(res));
            unsigned long long n = 0;
            if (cmdRows.ToULongLong(&n)) out.affected = n;
        } else {
            err = wxString::FromUTF8(PQresultErrorMessage(res)).Trim();
            return false;
        }
        out.elapsedMs = sw.Time();
        return true;
    }

    bool ListDatabases(std::vector<wxString>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT datname FROM pg_database "
                     L"WHERE NOT datistemplate ORDER BY datname", r, err))
            return false;
        for (const auto& row : r.rows)
            if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    // PG binds one database per session → "switch" means reconnect.
    bool UseDatabase(const wxString& db, wxString& err) override { return ensureDb(db, err); }

    bool ListTables(const wxString& database,
                    std::vector<TableInfo>& out, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;   // reconnect to the target DB
        // libpq sessions are bound to one database; list its user tables from the
        // catalog (pg_class), NOT pg_stat_user_tables — that view's rows depend on the
        // stats collector and can be empty on a fresh / managed / stats-disabled server,
        // which made every table invisible in the tree. pg_class is always accurate and
        // matches GetTableList's scope. reltuples is the planner estimate (−1 = never
        // analyzed → shown as —).
        // nspname on every row: without it this returned "orders" TWICE, and
        // indistinguishably, for public.orders + archive.orders (QualifiedName.h).
        QueryResult r;
        if (!Execute(L"SELECT c.relname, c.reltuples::bigint, n.nspname "
                     L"FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
                     L"WHERE c.relkind IN ('r','p') "
                     L"AND n.nspname NOT IN ('pg_catalog','information_schema') "
                     L"ORDER BY n.nspname, c.relname", r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            TableInfo t;
            t.name = row[0];
            t.approxRows = (row[1].IsEmpty() || row[1] == L"0" || row[1][0] == '-')
                               ? wxString(L"—") : row[1];
            t.schema = row[2];
            out.push_back(t);
        }
        return true;
    }

    // Rich per-table metadata for the database-overview grid. Sizes/row counts
    // come from the planner catalogs (pg_class.reltuples, pg_total_relation_size)
    // — instant even on huge tables. Scope matches ListTables (all user tables in
    // non-system schemas). engine/updatedAt are N/A for PG → left blank.
    bool GetTableList(const wxString& database,
                      std::vector<TableMeta>& out, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        QueryResult r;
        if (!Execute(
                L"SELECT c.relname, "
                L"COALESCE(obj_description(c.oid,'pg_class'),''), "
                L"c.reltuples::bigint, "
                L"pg_total_relation_size(c.oid), "
                L"(SELECT COUNT(*) FROM pg_attribute a "
                L"   WHERE a.attrelid=c.oid AND a.attnum>0 AND NOT a.attisdropped), "
                L"CASE WHEN EXISTS(SELECT 1 FROM pg_attribute a "
                L"   LEFT JOIN pg_attrdef ad ON ad.adrelid=a.attrelid AND ad.adnum=a.attnum "
                L"   WHERE a.attrelid=c.oid AND a.attnum>0 AND NOT a.attisdropped "
                L"   AND (a.attidentity IN ('a','d') "
                L"        OR pg_get_expr(ad.adbin,ad.adrelid) LIKE 'nextval(%')) "
                L"THEN 1 ELSE -1 END "
                L"FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
                L"WHERE c.relkind='r' "
                L"AND n.nspname NOT IN ('pg_catalog','information_schema') "
                L"ORDER BY c.relname", r, err))
            return false;
        out.clear();
        for (const auto& row : r.rows) {
            if (row.size() < 6) continue;
            TableMeta m;
            m.name    = row[0];
            m.comment = row[1];
            row[2].ToLongLong(&m.rows);          // reltuples; -1 when never analysed
            row[3].ToLongLong(&m.sizeBytes);
            long long cols = -1; row[4].ToLongLong(&cols); m.columns = static_cast<int>(cols);
            row[5].ToLongLong(&m.autoIncrement); // 1 = has serial/identity, else -1
            out.push_back(m);
        }
        return true;
    }

    // Database-level summary: encoding + collation from pg_database, table count
    // from the catalog, and whole-database on-disk size from pg_database_size.
    bool GetDatabaseInfo(const wxString& database, DbInfo& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT pg_encoding_to_char(d.encoding), d.datcollate, "
                L"(SELECT COUNT(*) FROM pg_class c "
                L"   JOIN pg_namespace n ON n.oid=c.relnamespace "
                L"   WHERE c.relkind='r' "
                L"   AND n.nspname NOT IN ('pg_catalog','information_schema')), "
                L"pg_database_size(d.datname) "
                L"FROM pg_database d WHERE d.datname=current_database()", r, err))
            return false;
        out = DbInfo{};
        out.name = database;
        if (!r.rows.empty() && r.rows[0].size() >= 4) {
            const auto& row = r.rows[0];
            out.charset   = row[0];
            out.collation = row[1];
            row[2].ToLongLong(&out.tableCount);
            row[3].ToLongLong(&out.sizeBytes);
        }
        return true;
    }

    // ---- schema introspection (current database, public schema) ----

    // Real column/index/FK introspection (A2) — thin delegate; the actual
    // catalog queries live in PgSchemaRead.cpp (free functions) to keep this
    // file under the 1000-line charter. ensureDb() first (PgSchemaRead only
    // Execute()s against whatever database the session is already bound to).
    bool GetTableSchema(const wxString& database, const QualifiedName& table,
                        TableSchema& out, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        return pgschema::GetTableSchema(*this, table, out, err);
    }

    bool GetColumns(const wxString& database, const wxString& table,
                    std::vector<ColumnInfo>& out, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        const wxString tb = Esc(table);
        QueryResult r;
        // Cols 0-5 legacy; 6 numeric_scale, 7 column comment via col_description
        // (works on every PG version — no catalog column dependency).
        if (!Execute(wxString::Format(
                L"SELECT ordinal_position, column_name, data_type, "
                L"COALESCE(character_maximum_length::text, "
                L"  CASE WHEN numeric_precision IS NOT NULL "
                L"       THEN numeric_precision || COALESCE(','||numeric_scale,'') END, '') AS len, "
                L"is_nullable, COALESCE(column_default, ''), "
                L"COALESCE(numeric_scale::text, ''), "
                L"COALESCE(col_description("
                L"  (quote_ident(table_schema)||'.'||quote_ident(table_name))::regclass, "
                L"  ordinal_position), '') "
                L"FROM information_schema.columns "
                L"WHERE table_schema='public' AND table_name='%s' "
                L"ORDER BY ordinal_position", tb), r, err))
            return false;

        // constraint_type per column: PRIMARY KEY / FOREIGN KEY / UNIQUE
        QueryResult ck;
        wxString ignore;
        Execute(wxString::Format(
            L"SELECT kcu.column_name, tc.constraint_type "
            L"FROM information_schema.table_constraints tc "
            L"JOIN information_schema.key_column_usage kcu "
            L"  ON tc.constraint_name=kcu.constraint_name "
            L"  AND tc.table_schema=kcu.table_schema "
            L"WHERE tc.table_schema='public' AND tc.table_name='%s'", tb), ck, ignore);

        auto keyFor = [&](const wxString& col) -> wxString {
            wxString best;
            for (const auto& row : ck.rows) {
                if (row.size() < 2 || row[0] != col) continue;
                if (row[1] == L"PRIMARY KEY") return L"PK";
                if (row[1] == L"FOREIGN KEY") best = L"FK";
                else if (row[1] == L"UNIQUE" && best.IsEmpty()) best = L"UQ";
            }
            return best;
        };

        // Generated columns (PG12+): is_generated='ALWAYS' + generation_expression.
        // Best-effort separate query — on pre-12 it errors and simply yields no
        // rows, leaving the fields empty rather than failing the whole call.
        QueryResult gen;
        Execute(wxString::Format(
            L"SELECT column_name, generation_expression "
            L"FROM information_schema.columns "
            L"WHERE table_schema='public' AND table_name='%s' "
            L"AND is_generated='ALWAYS'", tb), gen, ignore);
        auto genExprFor = [&](const wxString& col) -> wxString {
            for (const auto& row : gen.rows)
                if (row.size() >= 2 && row[0] == col) return row[1];
            return wxString();
        };

        for (const auto& row : r.rows) {
            if (row.size() < 6) continue;
            ColumnInfo c;
            long ord = 0; row[0].ToLong(&ord); c.ordinal = static_cast<int>(ord);
            c.name = row[1];
            c.type = row[2];
            c.length = row[3];
            c.notNull = (row[4] == L"NO");
            c.defaultVal = row[5];
            c.key = keyFor(c.name);
            // ---- extended introspection (best-effort) ----
            if (row.size() >= 8) {
                c.scale   = row[6];
                c.comment = row[7];
            }
            // PG generated columns are always STORED (no VIRTUAL as of PG16).
            wxString ge = genExprFor(c.name);
            if (!ge.IsEmpty()) { c.generatedExpr = ge; c.generatedStored = true; }
            // PG has no per-column unsigned/charset — left empty.
            out.push_back(std::move(c));
        }
        return true;
    }

    bool GetForeignKeys(const wxString& database, const wxString& table,
                        std::vector<ForeignKey>& out, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        wxString where = L"tc.constraint_type='FOREIGN KEY' AND tc.table_schema='public'";
        if (!table.IsEmpty())
            where += wxString::Format(L" AND tc.table_name='%s'", Esc(table));
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT tc.table_name, kcu.column_name, ccu.table_name, ccu.column_name "
                L"FROM information_schema.table_constraints tc "
                L"JOIN information_schema.key_column_usage kcu "
                L"  ON tc.constraint_name=kcu.constraint_name "
                L"JOIN information_schema.constraint_column_usage ccu "
                L"  ON ccu.constraint_name=tc.constraint_name "
                L"WHERE %s ORDER BY tc.table_name", where), r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 4) continue;
            out.push_back({ row[0], row[1], row[2], row[3] });
        }
        return true;
    }

    bool GetIndexes(const wxString& database, const wxString& table,
                    std::vector<IndexInfo>& out, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT i.relname, "
                L"  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
                L"   FROM unnest(ix.indkey) WITH ORDINALITY k(attnum, ord) "
                L"   JOIN pg_attribute a ON a.attrelid=t.oid AND a.attnum=k.attnum), "
                L"  ix.indisunique "
                L"FROM pg_index ix JOIN pg_class i ON i.oid=ix.indexrelid "
                L"JOIN pg_class t ON t.oid=ix.indrelid "
                L"JOIN pg_namespace n ON n.oid=t.relnamespace "
                L"WHERE n.nspname='public' AND t.relname='%s' ORDER BY i.relname",
                Esc(table)), r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            out.push_back({ row[0], row[1], row[2] == L"t" });
        }
        return true;
    }

    bool GetCreateDdl(const wxString& database, const wxString& table,
                      wxString& ddl, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // PostgreSQL has no SHOW CREATE TABLE; reconstruct a readable form
        // from column metadata (types, nullability, defaults).
        std::vector<ColumnInfo> cols;
        if (!GetColumns(wxString(), table, cols, err)) return false;

        ddl = wxString::Format(L"CREATE TABLE %s (\n", table);
        for (size_t i = 0; i < cols.size(); ++i) {
            const ColumnInfo& c = cols[i];
            ddl += L"    " + c.name + L" " + c.type;
            if (!c.length.IsEmpty()) ddl += L"(" + c.length + L")";
            if (c.notNull) ddl += L" NOT NULL";
            if (!c.defaultVal.IsEmpty()) ddl += L" DEFAULT " + c.defaultVal;
            if (c.key == L"PK") ddl += L" PRIMARY KEY";
            ddl += (i + 1 < cols.size()) ? L",\n" : L"\n";
        }
        ddl += L");\n\n-- 由列元数据重建(PostgreSQL 无 SHOW CREATE TABLE)";
        return true;
    }

    bool ListViews(const wxString& database, std::vector<wxString>& out,
                   wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // Session is bound to one database; list views in the visible schemas.
        QueryResult r;
        if (!Execute(L"SELECT table_name FROM information_schema.views "
                     L"WHERE table_schema NOT IN ('pg_catalog','information_schema') "
                     L"ORDER BY table_name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool GetViewDdl(const wxString& database, const wxString& view,
                    wxString& ddl, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // PG has no SHOW CREATE VIEW; pg_get_viewdef returns the SELECT body
        // (pretty-printed, already ';'-terminated) — wrap it into CREATE VIEW.
        QueryResult r;
        if (!Execute(L"SELECT pg_get_viewdef('\"" + Esc(view) +
                     L"\"'::regclass, true)", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty())
            ddl = L"CREATE OR REPLACE VIEW \"" + view + L"\" AS\n" + r.rows[0][0];
        return true;
    }

    bool ListRoutines(const wxString& database, std::vector<RoutineInfo>& out,
                      wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // prokind: f=function, p=procedure (exclude a=aggregate, w=window).
        // ORDER BY (prokind='p') → functions (false) sort before procedures.
        QueryResult r;
        if (!Execute(
                L"SELECT p.proname, CASE p.prokind WHEN 'p' THEN 'PROCEDURE' "
                L"ELSE 'FUNCTION' END "
                L"FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace "
                L"WHERE n.nspname NOT IN ('pg_catalog','information_schema') "
                L"AND p.prokind IN ('f','p') "
                L"ORDER BY (p.prokind='p'), p.proname", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetRoutineDdl(const wxString& database, const RoutineInfo& rt,
                       wxString& ddl, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // pg_get_functiondef yields a complete CREATE OR REPLACE …, already
        // $$-quoted and ';'-terminated. Overloads → take the first (see limits).
        QueryResult r;
        if (!Execute(L"SELECT pg_get_functiondef(p.oid) "
                     L"FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace "
                     L"WHERE p.proname='" + Esc(rt.name) + L"' "
                     L"AND n.nspname NOT IN ('pg_catalog','information_schema') "
                     L"LIMIT 1", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty()) ddl = r.rows[0][0];
        return true;
    }

    bool ListTriggers(const wxString& database, std::vector<TriggerInfo>& out,
                      wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // tgisinternal filters out the implicit triggers behind FK constraints.
        QueryResult r;
        if (!Execute(
                L"SELECT t.tgname, c.relname "
                L"FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid "
                L"JOIN pg_namespace n ON n.oid=c.relnamespace "
                L"WHERE NOT t.tgisinternal "
                L"AND n.nspname NOT IN ('pg_catalog','information_schema') "
                L"ORDER BY t.tgname", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetTriggerDdl(const wxString& database, const TriggerInfo& tg,
                       wxString& ddl, wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        // pg_get_triggerdef → a complete, single CREATE TRIGGER … ; (no inner ';').
        QueryResult r;
        if (!Execute(L"SELECT pg_get_triggerdef(t.oid, true) "
                     L"FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid "
                     L"WHERE t.tgname='" + Esc(tg.name) + L"' "
                     L"AND c.relname='" + Esc(tg.table) + L"' LIMIT 1", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty()) ddl = r.rows[0][0];
        return true;
    }

    bool GetCreateDatabaseCaps(DbCreateCaps& out, wxString& err) override
    {
        out = DbCreateCaps{};
        out.supported = true;

        auto queryCol = [&](const wxString& q, std::vector<wxString>& dst) -> bool {
            QueryResult r;
            if (!Execute(q, r, err)) return false;
            for (const auto& row : r.rows)
                if (!row.empty() && !row[0].IsEmpty()) dst.push_back(row[0]);
            return true;
        };

        DbCreateOption owner;
        owner.id = L"owner"; owner.label = L"属主 (OWNER)";
        if (!queryCol(L"SELECT rolname FROM pg_roles ORDER BY rolname", owner.choices))
            return false;
        out.options.push_back(owner);

        DbCreateOption enc;
        enc.id = L"encoding"; enc.label = L"编码 (ENCODING)"; enc.defaultVal = L"UTF8";
        if (!queryCol(L"SELECT DISTINCT pg_encoding_to_char(encoding) AS e "
                      L"FROM pg_database ORDER BY e", enc.choices))
            return false;
        if (enc.choices.empty()) enc.choices = { L"UTF8", L"LATIN1", L"SQL_ASCII" };
        out.options.push_back(enc);

        DbCreateOption lcoll;
        lcoll.id = L"lc_collate"; lcoll.label = L"排序区域 (LC_COLLATE)";
        lcoll.kind = DbCreateOption::Kind::FreeText;
        queryCol(L"SELECT DISTINCT collcollate FROM pg_collation "
                 L"WHERE collcollate <> '' ORDER BY collcollate", lcoll.choices);
        out.options.push_back(lcoll);

        DbCreateOption lctype;
        lctype.id = L"lc_ctype"; lctype.label = L"字符分类区域 (LC_CTYPE)";
        lctype.kind = DbCreateOption::Kind::FreeText;
        queryCol(L"SELECT DISTINCT collctype FROM pg_collation "
                 L"WHERE collctype <> '' ORDER BY collctype", lctype.choices);
        out.options.push_back(lctype);

        DbCreateOption tmpl;
        tmpl.id = L"template"; tmpl.label = L"模板 (TEMPLATE)";
        if (!queryCol(L"SELECT datname FROM pg_database WHERE datistemplate "
                      L"ORDER BY datname", tmpl.choices))
            return false;
        out.options.push_back(tmpl);

        DbCreateOption ts;
        ts.id = L"tablespace"; ts.label = L"表空间 (TABLESPACE)";
        if (!queryCol(L"SELECT spcname FROM pg_tablespace ORDER BY spcname", ts.choices))
            return false;
        out.options.push_back(ts);

        DbCreateOption cl;
        cl.id = L"conn_limit"; cl.label = L"连接数限制 (-1=无限)";
        cl.kind = DbCreateOption::Kind::Int; cl.defaultVal = L"-1";
        out.options.push_back(cl);
        return true;
    }

    wxString BuildCreateDatabaseSql(const DbCreateRequest& req) const override
    {
        wxString sql = L"CREATE DATABASE " + QuoteIdent(req.name, Dialect::Postgres);
        const wxString owner  = req.Value(L"owner");
        const wxString enc    = req.Value(L"encoding");
        const wxString lcoll  = req.Value(L"lc_collate");
        const wxString lctype = req.Value(L"lc_ctype");
        wxString       tmpl   = req.Value(L"template");
        const wxString ts     = req.Value(L"tablespace");
        const wxString cl     = req.Value(L"conn_limit");

        // Custom locales must be built from template0 (template1 may carry a
        // different locale). Auto-apply unless the user chose a template.
        if ((!lcoll.IsEmpty() || !lctype.IsEmpty()) && tmpl.IsEmpty())
            tmpl = L"template0";

        wxString w;
        if (!owner.IsEmpty())  w += L" OWNER " + QuoteIdent(owner, Dialect::Postgres);
        if (!enc.IsEmpty())    w += L" ENCODING '" + Esc(enc) + L"'";
        if (!lcoll.IsEmpty())  w += L" LC_COLLATE '" + Esc(lcoll) + L"'";
        if (!lctype.IsEmpty()) w += L" LC_CTYPE '" + Esc(lctype) + L"'";
        if (!tmpl.IsEmpty())   w += L" TEMPLATE " + QuoteIdent(tmpl, Dialect::Postgres);
        if (!ts.IsEmpty())     w += L" TABLESPACE " + QuoteIdent(ts, Dialect::Postgres);
        long n = 0;
        if (cl.ToLong(&n) && n >= 0) w += wxString::Format(L" CONNECTION LIMIT %ld", n);

        if (!w.IsEmpty()) sql += L" WITH" + w;
        return sql;
    }

    bool DumpTableData(const wxString& database, const wxString& table,
                       const std::function<void(const wxString&)>& emit,
                       wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        if (!conn_) { err = L"未连接"; return false; }
        const wxString stmt = L"SELECT * FROM \"" + table + L"\"";
        if (!PQsendQuery(conn_, stmt.utf8_str())) {
            err = wxString::FromUTF8(PQerrorMessage(conn_)).Trim();
            return false;
        }
        // Single-row mode delivers one PGresult per row instead of materialising
        // the whole table in the client — memory stays flat for big tables.
        PQsetSingleRowMode(conn_);

        wxString prefix;          // built lazily once we know the columns
        bool havePrefix = false;
        bool ok = true;
        while (PGresult* res = PQgetResult(conn_)) {
            PgResultGuard rowGuard(res);   // clears this row's result at loop-body exit
            const ExecStatusType st = PQresultStatus(res);
            if (st == PGRES_SINGLE_TUPLE || st == PGRES_TUPLES_OK) {
                const int nf = PQnfields(res);
                if (!havePrefix) {
                    wxString cols;
                    for (int i = 0; i < nf; ++i)
                        cols += (i ? L", \"" : L"\"") +
                                wxString::FromUTF8(PQfname(res, i)) + L"\"";
                    prefix = L"INSERT INTO \"" + table + L"\" (" + cols + L") VALUES (";
                    havePrefix = true;
                }
                if (st == PGRES_SINGLE_TUPLE) {           // exactly one row here
                    wxString vals;
                    for (int c = 0; c < nf; ++c) {
                        if (c) vals += L", ";
                        if (PQgetisnull(res, 0, c)) { vals += L"NULL"; continue; }
                        const wxString cell = wxString::FromUTF8(PQgetvalue(res, 0, c));
                        if (IsNumericOid(PQftype(res, c))) vals += cell;   // numeric → bare
                        else vals += L"'" + Esc(cell) + L"'";              // else → quoted
                    }
                    emit(prefix + vals + L");\n");
                }
            } else if (st != PGRES_COMMAND_OK) {
                err = wxString::FromUTF8(PQresultErrorMessage(res)).Trim();
                ok = false;
            }
            if (!ok) {   // drain remaining results so the connection stays usable
                while (PGresult* r = PQgetResult(conn_)) PQclear(r);
                break;   // rowGuard clears `res` as we leave the loop body
            }
        }
        return ok;
    }

    // ---- cross-database synchronization primitives ----

    // Keyed, chunked structured row stream over libpq single-row mode: one
    // PGresult per row (PQsetSingleRowMode) keeps client memory flat regardless
    // of table size — the same discipline as DumpTableData. Each row becomes
    // typed Cells (NULL / numeric-bare / prefix-less uppercase hex / UTF-8 text)
    // so RenderLiteral can round-trip them into the target dialect. Scope matches
    // the rest of this driver: the current database's public-schema table.
    bool StreamRows(const wxString& database, const QualifiedName& table,
                    const StreamOptions& opt, const RowSink& sink,
                    wxString& err) override
    {
        if (!ensureDb(database, err)) return false;
        if (!conn_) { err = L"未连接"; return false; }

        wxString proj;
        if (opt.columns.empty()) proj = L"*";
        else
            for (size_t i = 0; i < opt.columns.size(); ++i)
                proj += (i ? L", " : L"") +
                        QuoteIdent(opt.columns[i], Dialect::Postgres);

        // Schema-qualified FROM: a bare name resolved through search_path, i.e.
        // whichever same-named table the session saw. Unqualified still falls
        // through to search_path, which is right for single-namespace callers.
        wxString sql = L"SELECT " + proj + L" FROM " +
                       (table.HasSchema()
                            ? QuoteIdent(table.Schema(), Dialect::Postgres) + L"." +
                                  QuoteIdent(table.Name(), Dialect::Postgres)
                            : QuoteIdent(table.Name(), Dialect::Postgres));
        // Single-column cursor paging (composite-PK paging is handled upstream by
        // DataSync's dual-stream merge); compares against the first orderBy column.
        if (!opt.keyLowExcl.IsEmpty() && !opt.orderBy.empty())
            sql += L" WHERE " + QuoteIdent(opt.orderBy[0], Dialect::Postgres) +
                   L" > '" + Esc(opt.keyLowExcl) + L"'";
        sql += pgstream::OrderByClause(opt);   // honors opt.binaryOrder (T2)
        if (opt.limit >= 0)
            sql += wxString::Format(L" LIMIT %lld", opt.limit);

        if (!PQsendQuery(conn_, sql.utf8_str())) {
            err = wxString::FromUTF8(PQerrorMessage(conn_)).Trim();
            return false;
        }
        PQsetSingleRowMode(conn_);

        bool stopped = false, ok = true;
        while (PGresult* res = PQgetResult(conn_)) {
            PgResultGuard rowGuard(res);   // clears this row's result at loop-body exit
            const ExecStatusType st = PQresultStatus(res);
            if (st == PGRES_SINGLE_TUPLE) {          // exactly one row per result
                const int nf = PQnfields(res);
                std::vector<Cell> cells;
                cells.reserve(nf);
                for (int c = 0; c < nf; ++c) {
                    Cell cell;
                    if (PQgetisnull(res, 0, c)) {
                        cell.kind = CellKind::Null;
                    } else {
                        const Oid oid = PQftype(res, c);
                        const char* v = PQgetvalue(res, 0, c);
                        if (oid == 17) {             // bytea → prefix-less UC hex
                            cell.kind = CellKind::Binary;
                            cell.text = ByteaHex(v);
                        } else if (IsNumericOid(oid)) {   // numeric text → bare
                            cell.kind = CellKind::Numeric;
                            cell.text = wxString::FromUTF8(v);
                        } else {                     // incl. bool ('t'/'f') → text
                            cell.kind = CellKind::Text;
                            cell.text = wxString::FromUTF8(v);
                        }
                    }
                    cells.push_back(std::move(cell));
                }
                if (!sink(cells)) stopped = true;    // cancellation
            } else if (st == PGRES_TUPLES_OK) {
                // end-of-stream sentinel result (no rows) — nothing to do
            } else if (st != PGRES_COMMAND_OK) {
                err = wxString::FromUTF8(PQresultErrorMessage(res)).Trim();
                ok = false;
            }
            if (stopped || !ok) {   // drain the wire so the connection stays usable
                while (PGresult* r = PQgetResult(conn_)) PQclear(r);
                break;   // rowGuard clears `res` as we leave the loop body
            }
        }
        return ok;   // early stop (cancellation) is not an error
    }

    bool ExecuteBatch(const wxString& sql, const std::vector<std::vector<Cell>>& rows,
                      wxString& err) override
    { return pgstream::ExecuteBatch(conn_, sql, rows, err); }

    // Abstract change set → PostgreSQL statements (no trailing ';'; SyncEngine
    // Execute()s each one). Order mirrors the MySQL driver: create / drop whole
    // table short-circuit; else ADD/MODIFY columns, then index/FK adds, then
    // DROP-class in reverse (FK → index → column) so dependents precede targets.
    // PG specifics: indexes are schema-level objects (CREATE/DROP INDEX, not
    // ALTER); a column MODIFY expands into independent ALTER COLUMN sub-clauses.
    bool RenderSchemaChange(const SchemaChangeSet& ch,
                            std::vector<wxString>& stmts, wxString& err) override
    {
        (void)err;
        const wxString t = QuoteIdent(ch.table, Dialect::Postgres);

        if (ch.dropTable) {
            stmts.push_back(L"DROP TABLE IF EXISTS " + t);
            return true;
        }
        if (ch.createTable) {
            detail::RenderCreateTablePg(ch.createSchema, stmts);
            return true;
        }

        for (const auto& cc : ch.columns) {
            if (cc.op == ColumnChange::Op::Add) {
                stmts.push_back(L"ALTER TABLE " + t + L" ADD COLUMN " +
                                detail::PgColumnDef(cc.column));
            } else if (cc.op == ColumnChange::Op::Modify) {
                // The change set carries only the desired end state (not the prior
                // one), so emit every sub-clause to converge the column: TYPE, then
                // nullability, then default. Redundant clauses (e.g. DROP DEFAULT
                // when there was none) are harmless no-ops.
                const wxString col = QuoteIdent(cc.column.name, Dialect::Postgres);
                stmts.push_back(L"ALTER TABLE " + t + L" ALTER COLUMN " + col +
                                L" TYPE " + detail::PgTypeText(cc.column));
                stmts.push_back(L"ALTER TABLE " + t + L" ALTER COLUMN " + col +
                                (cc.column.notNull ? L" SET NOT NULL"
                                                   : L" DROP NOT NULL"));
                // An IDENTITY column carries no plain DEFAULT — PostgreSQL rejects
                // BOTH `SET DEFAULT` and `DROP DEFAULT` on it ("column ... is an
                // identity column"). So a Modify touching an identity column must
                // emit no default clause at all; the unconditional `DROP DEFAULT`
                // in the else-branch is precisely what turned a (now-eliminated)
                // phantom int->bigint Modify into a hard error. Guard it so even a
                // LEGITIMATE future Modify of an identity column stays valid DDL.
                if (!cc.column.autoIncrement) {
                    if (cc.column.hasDefault)
                        stmts.push_back(L"ALTER TABLE " + t + L" ALTER COLUMN " + col +
                                        L" SET DEFAULT " + cc.column.defaultExpr);
                    else
                        stmts.push_back(L"ALTER TABLE " + t + L" ALTER COLUMN " + col +
                                        L" DROP DEFAULT");
                }
            }
        }
        for (const auto& ic : ch.indexes)
            if (ic.op == IndexChange::Op::Add)
                // ch.table is a Key(); Parse is its exact inverse.
                stmts.push_back(detail::RenderCreateIndexPg(
                    QualifiedName::Parse(ch.table), ic.index));
        for (const auto& fc : ch.foreignKeys)
            if (fc.op == FkChange::Op::Add)
                stmts.push_back(L"ALTER TABLE " + t + L" ADD " +
                                detail::RenderFkConstraintPg(fc.fk));

        for (auto it = ch.foreignKeys.rbegin(); it != ch.foreignKeys.rend(); ++it)
            if (it->op == FkChange::Op::Drop)
                stmts.push_back(L"ALTER TABLE " + t + L" DROP CONSTRAINT " +
                                QuoteIdent(it->fk.name, Dialect::Postgres));
        for (auto it = ch.indexes.rbegin(); it != ch.indexes.rend(); ++it)
            if (it->op == IndexChange::Op::Drop)
                stmts.push_back(L"DROP INDEX IF EXISTS " +
                                QuoteIdent(it->index.name, Dialect::Postgres));
        for (auto it = ch.columns.rbegin(); it != ch.columns.rend(); ++it)
            if (it->op == ColumnChange::Op::Drop)
                stmts.push_back(L"ALTER TABLE " + t + L" DROP COLUMN " +
                                QuoteIdent(it->column.name, Dialect::Postgres));
        return true;
    }

    bool GetDatabaseOverview(DbOverview& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT d.datname, pg_catalog.pg_get_userbyid(d.datdba), "
                L"pg_catalog.pg_encoding_to_char(d.encoding), d.datcollate "
                L"FROM pg_database d WHERE d.datistemplate=false ORDER BY d.datname",
                r, err))
            return false;
        out = DbOverview{};
        out.columns = { {L"name", L"名称"}, {L"owner", L"所有者"},
                        {L"encoding", L"编码"}, {L"collation", L"排序规则"} };
        out.rows = std::move(r.rows);
        return true;
    }

    wxString ServerVersion() const override { return version_; }

    // ---- user / privilege administration (logic in db/UserAdminPg.cpp) ----
    bool SupportsUserAdmin() const override { return true; }
    bool ListUsers(std::vector<UserInfo>& o, wxString& e) override
        { return useradmin::PgListUsers(this, o, e); }
    bool GetUserGrants(const wxString& u, const wxString&, UserGrants& o, wxString& e) override
        { return useradmin::PgGetGrants(this, u, o, e); }
    bool SaveUser(const UserSpec& s, bool isNew, wxString& e) override
        { return useradmin::PgSaveUser(this, s, isNew, e); }
    bool DropUser(const wxString& u, const wxString&, wxString& e) override
        { return useradmin::PgDropUser(this, u, e); }
    UserAdminModel GetUserAdminModel() const override { return useradmin::PgModel(); }

private:
    static wxString Esc(const wxString& s)
    {
        wxString e = s;
        e.Replace(L"'", L"''");
        return e;
    }

    // Numeric type OIDs whose text form is a valid unquoted SQL literal.
    // (bool/16 is deliberately excluded — 't'/'f' must be quoted on insert.)
    static bool IsNumericOid(Oid t)
    {
        switch (t) {
        case 20: case 21: case 23: case 26:      // int8/int2/int4/oid
        case 700: case 701: case 1700:           // float4/float8/numeric
            return true;
        default:
            return false;
        }
    }

    // PG returns bytea in text mode as "\x48454C4C4F" (hex, lowercase). Strip the
    // "\x" prefix and upper-case so the Cell carries prefix-less UC hex digits;
    // RenderLiteral re-frames them into the target dialect's binary literal.
    // (Assumes bytea_output='hex', the default since PG 9.0.)
    static wxString ByteaHex(const char* v)
    {
        wxString s = wxString::FromUTF8(v);
        if (s.StartsWith(L"\\x")) s = s.Mid(2);
        return s.Upper();
    }

    PGconn*  conn_ = nullptr;
    wxString version_;
    core::ConnectionProfile prof_;   // saved for ensureDb() reconnects + EffectiveProfile()
    wxString boundDb_;               // the database this libpq session is currently on
};

} // namespace

std::unique_ptr<IConnection> CreatePgConnection()
{
    return std::make_unique<PgConnection>();
}

} // namespace db
