// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MySqlDriver.cpp — MySQL + OceanBase connection over the MySQL wire protocol
// (libmariadb). OceanBase's MySQL mode speaks this protocol natively, so both
// engines share this implementation.
#include "db/DbDriver.h"
#include "db/MySqlSql.h"    // detail:: pure SQL-rendering helpers
#include "db/MySqlStream.h" // mysqlstream:: byte-order ORDER BY + batch INSERT
#include "db/UserAdmin.h"   // useradmin:: user/privilege helpers (impl in UserAdmin.cpp)

#include <mysql.h>
#include <wx/stopwatch.h>
#include <cstdio>

namespace db {
namespace {

wxString FromDb(const char* s)
{
    return s ? wxString::FromUTF8(s) : wxString(L"NULL");
}

// RAII: free the result set when the fetch/dump loop leaves scope — even via an
// exception from FromDb/push_back/emit/sink. For use_result sets mysql_free_result
// also drains any rows still on the wire, so the connection stays usable; that is
// exactly what the manual free at the end of each loop did on the normal path.
struct MysqlResGuard {
    MYSQL_RES* res;
    explicit MysqlResGuard(MYSQL_RES* r) : res(r) {}
    ~MysqlResGuard() { if (res) mysql_free_result(res); }
    MysqlResGuard(const MysqlResGuard&) = delete;
    MysqlResGuard& operator=(const MysqlResGuard&) = delete;
};

class MySqlConnection : public IConnection {
public:
    ~MySqlConnection() override { Disconnect(); }

    Dialect GetDialect() const override { return Dialect::MySQL; }

    // target_ already holds the profile passed to Connect() (tunnel-rewritten).
    const core::ConnectionProfile& EffectiveProfile() const override { return target_; }

    bool Connect(const core::ConnectionProfile& p, wxString& err) override
    {
        Disconnect();
        target_ = p;              // remember for Cancel() (KILL QUERY) + EffectiveProfile()
        conn_ = mysql_init(nullptr);
        if (!conn_) { err = L"mysql_init 失败"; return false; }

        unsigned int timeout = p.connectTimeout > 0
                                   ? static_cast<unsigned int>(p.connectTimeout) : 8;
        mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

        // The wxString must outlive the buffer: wxString::utf8_str() returns a
        // NON-owning wxScopedCharBuffer that points into the string's internal
        // conversion cache, so binding it from a temporary (the ternary result)
        // leaves `cs` dangling at the semicolon — a use-after-free mysql_options
        // then reads. Name the string so it lives to the mysql_options call.
        const wxString csName = p.charset.IsEmpty() ? wxString(L"utf8mb4") : p.charset;
        const wxScopedCharBuffer cs = csName.utf8_str();
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, cs.data());

        // SSL: enforce encryption / verify server cert per the connection's SSL
        // tab. Default (verify off) matches the stock mysql client and avoids
        // CERT_E_UNTRUSTEDROOT against MySQL 8.0's self-signed certs.
        char verifyCert = p.sslVerifyServerCert ? 1 : 0;
        char enforceSsl = p.sslEnabled ? 1 : 0;
        mysql_options(conn_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verifyCert);
        mysql_options(conn_, MYSQL_OPT_SSL_ENFORCE, &enforceSsl);
        if (!p.sslCaCert.IsEmpty() || !p.sslClientCert.IsEmpty() ||
            !p.sslCipher.IsEmpty()) {
            caBuf_   = p.sslCaCert.utf8_str();
            certBuf_ = p.sslClientCert.utf8_str();
            keyBuf_  = p.sslClientKey.utf8_str();
            cipherBuf_ = p.sslCipher.utf8_str();
            mysql_ssl_set(conn_,
                          p.sslClientKey.IsEmpty()  ? nullptr : keyBuf_.data(),
                          p.sslClientCert.IsEmpty() ? nullptr : certBuf_.data(),
                          p.sslCaCert.IsEmpty()     ? nullptr : caBuf_.data(),
                          nullptr,
                          p.sslCipher.IsEmpty()     ? nullptr : cipherBuf_.data());
        }

        // read/write timeout, compression, auto-reconnect
        if (p.readWriteTimeout > 0) {
            unsigned int rw = static_cast<unsigned int>(p.readWriteTimeout);
            mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &rw);
            mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &rw);
        }
        if (p.compression)
            mysql_options(conn_, MYSQL_OPT_COMPRESS, nullptr);
        if (p.autoReconnect) {
            char rc = 1;
            mysql_options(conn_, MYSQL_OPT_RECONNECT, &rc);
        }
        // startup SQL: session time zone, then user init commands
        if (!p.timezone.IsEmpty()) {
            tzBuf_ = (L"SET time_zone='" + p.timezone + L"'").utf8_str();
            mysql_options(conn_, MYSQL_INIT_COMMAND, tzBuf_.data());
        }
        if (!p.initCommands.IsEmpty()) {
            initBuf_ = p.initCommands.utf8_str();
            mysql_options(conn_, MYSQL_INIT_COMMAND, initBuf_.data());
        }

        const wxScopedCharBuffer sockBuf = p.socketFile.utf8_str();
        if (!mysql_real_connect(conn_,
                                p.host.utf8_str(),
                                p.user.utf8_str(),
                                p.password.utf8_str(),
                                p.database.IsEmpty() ? nullptr : p.database.utf8_str().data(),
                                static_cast<unsigned int>(p.port),
                                p.socketFile.IsEmpty() ? nullptr : sockBuf.data(), 0)) {
            err = wxString::FromUTF8(mysql_error(conn_));
            mysql_close(conn_);
            conn_ = nullptr;
            return false;
        }
        version_ = wxString::FromUTF8(mysql_get_server_info(conn_));
        return true;
    }

    void Disconnect() override
    {
        if (conn_) { mysql_close(conn_); conn_ = nullptr; }
    }

    bool IsConnected() const override { return conn_ != nullptr; }

    // A dropped-connection client error — CR_SERVER_GONE_ERROR (2006) /
    // CR_SERVER_LOST (2013): the socket is dead. Close the handle so IsConnected()
    // reports false; otherwise callers keep querying a dead conn_ (conn_ is still
    // non-null) and the client library can crash. Read mysql_error() BEFORE calling.
    void DropIfLost(unsigned merr)
    {
        if ((merr == 2006 || merr == 2013) && conn_) { mysql_close(conn_); conn_ = nullptr; }
    }

    // Cancel the running query via a separate connection: KILL QUERY <id>.
    void Cancel() override
    {
        if (!conn_) return;
        // Read the thread id live: with MYSQL_OPT_RECONNECT an intervening
        // auto-reconnect hands the session a new id, so a cached one (captured at
        // Connect) would KILL QUERY the wrong — possibly someone else's — session.
        const unsigned long tid = mysql_thread_id(conn_);
        if (tid == 0) return;
        MYSQL* k = mysql_init(nullptr);
        if (!k) return;
        unsigned int to = 5;
        mysql_options(k, MYSQL_OPT_CONNECT_TIMEOUT, &to);
        char verify = target_.sslVerifyServerCert ? 1 : 0;   // match main conn's SSL
        mysql_options(k, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
        const wxScopedCharBuffer sock = target_.socketFile.utf8_str();
        if (mysql_real_connect(k, target_.host.utf8_str(), target_.user.utf8_str(),
                               target_.password.utf8_str(), nullptr,
                               static_cast<unsigned int>(target_.port),
                               target_.socketFile.IsEmpty() ? nullptr : sock.data(), 0)) {
            char q[64];
            snprintf(q, sizeof q, "KILL QUERY %lu", tid);
            mysql_query(k, q);
        }
        mysql_close(k);
    }

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        if (!conn_) { err = L"未连接"; return false; }
        out = QueryResult();

        wxStopWatch sw;
        const wxScopedCharBuffer q = sql.utf8_str();
        if (mysql_real_query(conn_, q.data(), q.length()) != 0) {
            err = wxString::FromUTF8(mysql_error(conn_));
            DropIfLost(mysql_errno(conn_));   // dead socket → mark disconnected (no crash)
            return false;
        }

        MYSQL_RES* res = mysql_store_result(conn_);
        if (res) {
            MysqlResGuard resGuard(res);   // frees at block exit (was manual free)
            out.isSelect = true;
            const unsigned int nf = mysql_num_fields(res);
            MYSQL_FIELD* fields = mysql_fetch_fields(res);
            for (unsigned int i = 0; i < nf; ++i)
                out.columns.push_back(wxString::FromUTF8(fields[i].name));

            while (MYSQL_ROW row = mysql_fetch_row(res)) {
                std::vector<wxString> r;
                r.reserve(nf);
                for (unsigned int i = 0; i < nf; ++i)
                    r.push_back(FromDb(row[i]));
                out.rows.push_back(std::move(r));
            }
            out.affected = out.rows.size();
        } else if (mysql_field_count(conn_) == 0) {
            out.isSelect = false;
            out.affected = mysql_affected_rows(conn_);
        } else {
            err = wxString::FromUTF8(mysql_error(conn_));
            return false;
        }
        out.elapsedMs = sw.Time();
        return true;
    }

    bool ListDatabases(std::vector<wxString>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SHOW DATABASES", r, err)) return false;
        for (const auto& row : r.rows)
            if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool UseDatabase(const wxString& db, wxString& err) override
    {
        if (db.IsEmpty()) return true;
        QueryResult r;
        return Execute(L"USE " + QuoteIdent(db, Dialect::MySQL), r, err);
    }

    bool ListTables(const wxString& database,
                    std::vector<TableInfo>& out, wxString& err) override
    {
        // information_schema row counts are estimates — good enough for the
        // sidebar, and cheap even on huge schemas.
        wxString esc = database;
        esc.Replace(L"'", L"''");
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT table_name, IFNULL(table_rows, 0) FROM information_schema.tables "
                L"WHERE table_schema = '%s' AND table_type = 'BASE TABLE' ORDER BY table_name",
                esc), r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 2) continue;
            TableInfo t;
            t.name = row[0];
            t.approxRows = row[1] == L"0" ? wxString(L"—") : row[1];
            out.push_back(t);
        }
        return true;
    }

    // Rich per-table metadata for the database-overview grid — one query pulls
    // size, rows, auto-increment, last-modified, comment and column count from
    // information_schema. The correlated COLUMNS count is fine here because the
    // UI loads this off the main thread with a loading indicator.
    bool GetTableList(const wxString& database,
                      std::vector<TableMeta>& out, wxString& err) override
    {
        const wxString db = Esc(database);
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT t.TABLE_NAME, IFNULL(t.TABLE_COMMENT,''), IFNULL(t.ENGINE,''), "
                L"IFNULL(t.TABLE_ROWS,0), IFNULL(t.DATA_LENGTH,0)+IFNULL(t.INDEX_LENGTH,0), "
                L"IFNULL(t.AUTO_INCREMENT,-1), "
                L"IFNULL(DATE_FORMAT(IFNULL(t.UPDATE_TIME,t.CREATE_TIME),'%%Y-%%m-%%d %%H:%%i'),''), "
                L"(SELECT COUNT(*) FROM information_schema.COLUMNS c "
                L" WHERE c.TABLE_SCHEMA=t.TABLE_SCHEMA AND c.TABLE_NAME=t.TABLE_NAME) "
                L"FROM information_schema.TABLES t "
                L"WHERE t.TABLE_SCHEMA='%s' AND t.TABLE_TYPE='BASE TABLE' "
                L"ORDER BY t.TABLE_NAME", db), r, err))
            return false;
        out.clear();
        for (const auto& row : r.rows) {
            if (row.size() < 8) continue;
            TableMeta m;
            m.name    = row[0];
            m.comment = row[1];
            m.engine  = row[2];
            row[3].ToLongLong(&m.rows);
            row[4].ToLongLong(&m.sizeBytes);
            row[5].ToLongLong(&m.autoIncrement);
            m.updatedAt = row[6];
            long long cols = -1; row[7].ToLongLong(&cols); m.columns = static_cast<int>(cols);
            out.push_back(m);
        }
        return true;
    }

    // Database-level summary: default charset/collation from SCHEMATA + table
    // count and aggregate size from TABLES.
    bool GetDatabaseInfo(const wxString& database, DbInfo& out, wxString& err) override
    {
        const wxString db = Esc(database);
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT IFNULL(s.DEFAULT_CHARACTER_SET_NAME,''), "
                L"IFNULL(s.DEFAULT_COLLATION_NAME,''), "
                L"(SELECT COUNT(*) FROM information_schema.TABLES t "
                L"  WHERE t.TABLE_SCHEMA=s.SCHEMA_NAME AND t.TABLE_TYPE='BASE TABLE'), "
                L"(SELECT IFNULL(SUM(IFNULL(t.DATA_LENGTH,0)+IFNULL(t.INDEX_LENGTH,0)),0) "
                L"  FROM information_schema.TABLES t WHERE t.TABLE_SCHEMA=s.SCHEMA_NAME) "
                L"FROM information_schema.SCHEMATA s WHERE s.SCHEMA_NAME='%s'", db), r, err))
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

    // Full single-table info for the open-table info panel — one row from
    // information_schema.TABLES (engine / sizes / row format / collation / dates).
    bool GetTableDetail(const wxString& database, const wxString& table,
                        TableDetail& out, wxString& err) override
    {
        const wxString db = Esc(database), tb = Esc(table);
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT IFNULL(ENGINE,''), IFNULL(TABLE_ROWS,0), IFNULL(DATA_LENGTH,0), "
                L"IFNULL(INDEX_LENGTH,0), IFNULL(MAX_DATA_LENGTH,0), IFNULL(DATA_FREE,0), "
                L"IFNULL(AUTO_INCREMENT,-1), IFNULL(ROW_FORMAT,''), IFNULL(TABLE_COLLATION,''), "
                L"IFNULL(CREATE_OPTIONS,''), IFNULL(TABLE_COMMENT,''), "
                L"IFNULL(DATE_FORMAT(CREATE_TIME,'%%Y-%%m-%%d %%H:%%i:%%s'),''), "
                L"IFNULL(DATE_FORMAT(UPDATE_TIME,'%%Y-%%m-%%d %%H:%%i:%%s'),''), "
                L"IFNULL(DATE_FORMAT(CHECK_TIME,'%%Y-%%m-%%d %%H:%%i:%%s'),'') "
                L"FROM information_schema.TABLES WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s'",
                db, tb), r, err))
            return false;
        out = TableDetail{};
        out.name = table;
        if (!r.rows.empty() && r.rows[0].size() >= 14) {
            const auto& x = r.rows[0];
            out.engine = x[0];
            x[1].ToLongLong(&out.rows);
            x[2].ToLongLong(&out.dataLength);
            x[3].ToLongLong(&out.indexLength);
            x[4].ToLongLong(&out.maxDataLength);
            x[5].ToLongLong(&out.dataFree);
            x[6].ToLongLong(&out.autoIncrement);
            out.rowFormat = x[7]; out.collation = x[8]; out.createOptions = x[9];
            out.comment = x[10]; out.createdAt = x[11]; out.updatedAt = x[12];
            out.checkTime = x[13];
        }
        return true;
    }

    // ---- schema introspection ----
    bool GetColumns(const wxString& database, const wxString& table,
                    std::vector<ColumnInfo>& out, wxString& err) override
    {
        const wxString db = Esc(database), tb = Esc(table);
        QueryResult r;
        // Cols 0-6 are the legacy set (name/type/length/notNull/default/key);
        // 7-13 are the ADR-014 extended introspection fields (best-effort back-
        // fill). COLUMN_TYPE (11) carries the full type incl. " unsigned"; EXTRA
        // (12) carries "auto_increment" and "VIRTUAL/STORED GENERATED".
        if (!Execute(wxString::Format(
                L"SELECT ordinal_position, column_name, data_type, "
                L"COALESCE(character_maximum_length, "
                L"  CONCAT_WS(',', numeric_precision, NULLIF(numeric_scale,0)), '') AS len, "
                L"is_nullable, COALESCE(column_default, ''), column_key, "
                L"COALESCE(numeric_scale, ''), COALESCE(column_comment, ''), "
                L"COALESCE(character_set_name, ''), COALESCE(collation_name, ''), "
                L"COALESCE(column_type, ''), COALESCE(extra, ''), "
                L"COALESCE(generation_expression, '') "
                L"FROM information_schema.columns "
                L"WHERE table_schema='%s' AND table_name='%s' "
                L"ORDER BY ordinal_position", db, tb), r, err))
            return false;

        // MySQL's column_key gives PRI/UNI directly; FK needs key_column_usage.
        std::vector<ForeignKey> fks;
        wxString ignore;
        GetForeignKeys(database, table, fks, ignore);

        for (const auto& row : r.rows) {
            if (row.size() < 7) continue;
            ColumnInfo c;
            long ord = 0; row[0].ToLong(&ord); c.ordinal = static_cast<int>(ord);
            c.name = row[1];
            c.type = row[2];
            c.length = (row[3] == L"NULL") ? wxString() : row[3];
            c.notNull = (row[4] == L"NO");
            c.defaultVal = (row[5] == L"NULL") ? wxString() : row[5];
            if (row[6] == L"PRI") c.key = L"PK";
            else if (row[6] == L"UNI") c.key = L"UQ";
            for (const auto& fk : fks)
                if (fk.fromColumn == c.name && c.key != L"PK") { c.key = L"FK"; break; }
            // ---- extended introspection (best-effort) ----
            if (row.size() >= 14) {
                c.scale     = (row[7] == L"NULL") ? wxString() : row[7];
                c.comment   = row[8];
                c.charset   = row[9];
                c.collation = row[10];
                const wxString colType = row[11].Lower();
                c.unsignedFlag = colType.Contains(L"unsigned");
                const wxString extra = row[12].Lower();
                c.autoIncrement = extra.Contains(L"auto_increment");
                if (extra.Contains(L"generated")) {
                    c.generatedExpr   = row[13];   // GENERATION_EXPRESSION
                    c.generatedStored = extra.Contains(L"stored generated");
                }
            }
            out.push_back(std::move(c));
        }
        return true;
    }

    bool GetForeignKeys(const wxString& database, const wxString& table,
                        std::vector<ForeignKey>& out, wxString& err) override
    {
        const wxString db = Esc(database);
        wxString where = wxString::Format(
            L"table_schema='%s' AND referenced_table_name IS NOT NULL", db);
        if (!table.IsEmpty())
            where += wxString::Format(L" AND table_name='%s'", Esc(table));
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT table_name, column_name, referenced_table_name, "
                L"referenced_column_name FROM information_schema.key_column_usage "
                L"WHERE %s ORDER BY table_name", where), r, err))
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
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT index_name, GROUP_CONCAT(column_name ORDER BY seq_in_index), "
                L"MAX(non_unique)=0 AS is_unique "
                L"FROM information_schema.statistics "
                L"WHERE table_schema='%s' AND table_name='%s' "
                L"GROUP BY index_name ORDER BY index_name",
                Esc(database), Esc(table)), r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            out.push_back({ row[0], row[1], row[2] == L"1" });
        }
        return true;
    }

    bool GetCreateDdl(const wxString& database, const wxString& table,
                      wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(wxString::Format(L"SHOW CREATE TABLE `%s`.`%s`",
                                      database, table), r, err))
            return false;
        if (!r.rows.empty() && r.rows[0].size() >= 2)
            ddl = r.rows[0][1];
        return true;
    }

    bool ListViews(const wxString& database, std::vector<wxString>& out,
                   wxString& err) override
    {
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT table_name FROM information_schema.views "
                L"WHERE table_schema='%s' ORDER BY table_name", Esc(database)),
                r, err))
            return false;
        for (const auto& row : r.rows)
            if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool GetViewDdl(const wxString& database, const wxString& view,
                    wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(wxString::Format(L"SHOW CREATE VIEW `%s`.`%s`",
                                      database, view), r, err))
            return false;
        // columns: View, Create View, character_set_client, collation_connection
        if (!r.rows.empty() && r.rows[0].size() >= 2)
            ddl = r.rows[0][1];
        return true;
    }

    bool ListRoutines(const wxString& database, std::vector<RoutineInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        // routine_type FUNCTION sorts before PROCEDURE (F<P) → functions emit first.
        if (!Execute(wxString::Format(
                L"SELECT routine_name, routine_type FROM information_schema.routines "
                L"WHERE routine_schema='%s' ORDER BY routine_type, routine_name",
                Esc(database)), r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetRoutineDdl(const wxString& database, const RoutineInfo& rt,
                       wxString& ddl, wxString& err) override
    {
        const wxString kw = (rt.type == L"FUNCTION") ? L"FUNCTION" : L"PROCEDURE";
        QueryResult r;
        if (!Execute(wxString::Format(L"SHOW CREATE %s `%s`.`%s`", kw,
                                      database, rt.name), r, err))
            return false;
        // columns: <Name>, sql_mode, Create <Proc/Func>, … → DDL is column 2.
        if (!r.rows.empty() && r.rows[0].size() >= 3)
            ddl = r.rows[0][2];
        return true;
    }

    bool ListTriggers(const wxString& database, std::vector<TriggerInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT trigger_name, event_object_table "
                L"FROM information_schema.triggers "
                L"WHERE trigger_schema='%s' ORDER BY trigger_name",
                Esc(database)), r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetTriggerDdl(const wxString& database, const TriggerInfo& tg,
                       wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(wxString::Format(L"SHOW CREATE TRIGGER `%s`.`%s`",
                                      database, tg.name), r, err))
            return false;
        // columns: Trigger, sql_mode, SQL Original Statement, … → DDL is column 2.
        if (!r.rows.empty() && r.rows[0].size() >= 3)
            ddl = r.rows[0][2];
        return true;
    }

    bool GetCreateDatabaseCaps(DbCreateCaps& out, wxString& err) override
    {
        out = DbCreateCaps{};
        out.supported = true;

        DbCreateOption cs;
        cs.id = L"charset"; cs.label = L"字符集"; cs.defaultVal = L"utf8mb4";
        {
            QueryResult r;
            if (!Execute(L"SHOW CHARACTER SET", r, err)) return false;
            for (const auto& row : r.rows)
                if (!row.empty()) cs.choices.push_back(row[0]);
        }
        out.options.push_back(cs);

        DbCreateOption co;
        co.id = L"collation"; co.label = L"排序规则"; co.groupSource = L"charset";
        {
            QueryResult r;
            // columns: Collation, Charset, Id, Default, Compiled, Sortlen
            if (!Execute(L"SHOW COLLATION", r, err)) return false;
            for (const auto& row : r.rows)
                if (row.size() >= 2) {
                    co.choices.push_back(row[0]);
                    co.choiceGroup.push_back(row[1]);
                }
        }
        out.options.push_back(co);
        return true;
    }

    wxString BuildCreateDatabaseSql(const DbCreateRequest& req) const override
    {
        wxString sql = L"CREATE DATABASE " + QuoteIdent(req.name, Dialect::MySQL);
        const wxString cs = req.Value(L"charset");
        const wxString co = req.Value(L"collation");
        // charset/collation are keyword-position identifiers — whitelist to
        // [A-Za-z0-9_] as defence in depth (they come from a fixed list anyway).
        if (!cs.IsEmpty() && IsSafeKeyword(cs)) sql += L" CHARACTER SET " + cs;
        if (!co.IsEmpty() && IsSafeKeyword(co)) sql += L" COLLATE " + co;
        return sql;
    }

    bool DumpTableData(const wxString& database, const wxString& table,
                       const std::function<void(const wxString&)>& emit,
                       wxString& err) override
    {
        if (!conn_) { err = L"未连接"; return false; }
        const wxString stmt =
            wxString::Format(L"SELECT * FROM `%s`.`%s`", database, table);
        const wxScopedCharBuffer q = stmt.utf8_str();
        if (mysql_real_query(conn_, q.data(), q.length()) != 0) {
            err = wxString::FromUTF8(mysql_error(conn_));
            DropIfLost(mysql_errno(conn_));   // dead socket → mark disconnected (no crash)
            return false;
        }
        // use_result streams rows from the server one at a time — unlike
        // store_result it never buffers the whole table in the client.
        MYSQL_RES* res = mysql_use_result(conn_);
        if (!res) {
            if (mysql_field_count(conn_) == 0) return true;   // nothing to dump
            err = wxString::FromUTF8(mysql_error(conn_));
            return false;
        }
        MysqlResGuard resGuard(res);   // frees (and drains) at scope exit
        const unsigned int nf = mysql_num_fields(res);
        MYSQL_FIELD* fields = mysql_fetch_fields(res);

        // Column-name list is identical for every row — build it once.
        wxString cols;
        for (unsigned int i = 0; i < nf; ++i)
            cols += (i ? L", `" : L"`") + wxString::FromUTF8(fields[i].name) + L"`";
        const wxString prefix =
            wxString::Format(L"INSERT INTO `%s` (%s) VALUES (", table, cols);

        while (MYSQL_ROW row = mysql_fetch_row(res)) {
            const unsigned long* len = mysql_fetch_lengths(res);
            wxString vals;
            for (unsigned int i = 0; i < nf; ++i) {
                if (i) vals += L", ";
                if (row[i] == nullptr)               vals += L"NULL";                 // real NULL
                else if (fields[i].flags & NUM_FLAG) vals += wxString::FromUTF8(row[i], len[i]); // numeric → bare
                else if (IsBinaryField(fields[i]))   vals += mysqlstream::HexLiteral(row[i], len[i]);         // blob → 0x…
                else vals += L"'" + Esc(wxString::FromUTF8(row[i], len[i])) + L"'";   // text → quoted
            }
            emit(prefix + vals + L");\n");
        }
        // With use_result a fetch/transport error only shows up after the loop.
        // Read the error before resGuard frees (and drains) the set at scope exit.
        const unsigned int merr = mysql_errno(conn_);
        if (merr) { err = wxString::FromUTF8(mysql_error(conn_)); DropIfLost(merr); return false; }
        return true;
    }

    // ---- cross-database synchronization primitives ----

    // Normalized structure for the diff engine: one information_schema.columns
    // pass yields precise ColKind + rawType (column_type) + auto_increment +
    // char/numeric length & scale; PK order, indexes, FKs and table options are
    // filled from targeted follow-up queries.
    bool GetTableSchema(const wxString& database, const QualifiedName& table,
                        TableSchema& out, wxString& err) override
    {
        out = TableSchema{};
        out.name = QualifiedName(table.Name());   // no schema layer on MySQL
        const wxString db = Esc(database), tb = Esc(table.Name());

        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT column_name, data_type, column_type, is_nullable, "
                L"CASE WHEN column_default IS NULL THEN 0 ELSE 1 END, "
                L"IFNULL(column_default,''), IFNULL(extra,''), "
                L"IFNULL(character_maximum_length,-1), IFNULL(numeric_precision,-1), "
                L"IFNULL(numeric_scale,-1), ordinal_position, IFNULL(column_key,'') "
                L"FROM information_schema.columns "
                L"WHERE table_schema='%s' AND table_name='%s' "
                L"ORDER BY ordinal_position", db, tb), r, err))
            return false;
        for (const auto& row : r.rows) {
            if (row.size() < 12) continue;
            NormColumn nc;
            nc.name          = row[0];
            nc.kind          = detail::MapColKind(row[1], row[2]);
            nc.rawType       = row[2];
            nc.notNull       = (row[3] == L"NO");
            nc.hasDefault    = (row[4] == L"1");
            nc.defaultExpr   = nc.hasDefault ? row[5] : wxString();
            nc.autoIncrement = row[6].Lower().Contains(L"auto_increment");
            long long charLen = -1, prec = -1; long scale = -1, ord = 0;
            row[7].ToLongLong(&charLen);
            row[8].ToLongLong(&prec);
            row[9].ToLong(&scale);
            row[10].ToLong(&ord);
            if (charLen >= 0) nc.length = charLen;
            else if (prec >= 0) {
                nc.length = prec;
                if (nc.kind == ColKind::Decimal && scale >= 0) nc.scale = (int)scale;
            }
            nc.ordinal = (int)ord;
            out.columns.push_back(std::move(nc));
        }

        wxString ig;
        GetPrimaryKey(database, table.Name(), out.primaryKey, ig);   // ordered PK

        std::vector<IndexInfo> idxs;
        if (GetIndexes(database, table.Name(), idxs, ig))
            for (const auto& ii : idxs) {
                NormIndex ni;
                ni.name    = ii.name;
                ni.unique  = ii.unique;
                ni.primary = (ii.name == L"PRIMARY");
                ni.columns = detail::SplitCsv(ii.columns);
                out.indexes.push_back(std::move(ni));
            }

        QueryResult fr;
        if (Execute(wxString::Format(
                L"SELECT k.constraint_name, "
                L"GROUP_CONCAT(k.column_name ORDER BY k.ordinal_position), "
                L"k.referenced_table_name, "
                L"GROUP_CONCAT(k.referenced_column_name ORDER BY k.ordinal_position), "
                L"IFNULL(rc.delete_rule,''), IFNULL(rc.update_rule,'') "
                L"FROM information_schema.key_column_usage k "
                L"LEFT JOIN information_schema.referential_constraints rc "
                L"  ON rc.constraint_schema=k.table_schema "
                L"  AND rc.constraint_name=k.constraint_name "
                L"WHERE k.table_schema='%s' AND k.table_name='%s' "
                L"AND k.referenced_table_name IS NOT NULL "
                L"GROUP BY k.constraint_name, k.referenced_table_name, "
                L"rc.delete_rule, rc.update_rule "
                L"ORDER BY k.constraint_name", db, tb), fr, ig))
            for (const auto& row : fr.rows) {
                if (row.size() < 6) continue;
                NormForeignKey nf;
                nf.name       = row[0];
                nf.columns    = detail::SplitCsv(row[1]);
                nf.refTable   = row[2];
                nf.refColumns = detail::SplitCsv(row[3]);
                nf.onDelete   = row[4];
                nf.onUpdate   = row[5];
                out.foreignKeys.push_back(std::move(nf));
            }

        QueryResult tr;
        if (Execute(wxString::Format(
                L"SELECT IFNULL(ENGINE,''), IFNULL(TABLE_COLLATION,''), "
                L"IFNULL(TABLE_COMMENT,'') FROM information_schema.tables "
                L"WHERE table_schema='%s' AND table_name='%s'", db, tb), tr, ig))
            if (!tr.rows.empty() && tr.rows[0].size() >= 3) {
                out.engine    = tr.rows[0][0];
                out.collation = tr.rows[0][1];
                out.charset   = out.collation.BeforeFirst(L'_');
                out.comment   = tr.rows[0][2];
            }
        return true;
    }

    // Ordered primary-key columns straight from statistics (seq_in_index).
    bool GetPrimaryKey(const wxString& database, const wxString& table,
                       std::vector<wxString>& pkColumns, wxString& err) override
    {
        QueryResult r;
        if (!Execute(wxString::Format(
                L"SELECT column_name FROM information_schema.statistics "
                L"WHERE table_schema='%s' AND table_name='%s' "
                L"AND index_name='PRIMARY' ORDER BY seq_in_index",
                Esc(database), Esc(table)), r, err))
            return false;
        pkColumns.clear();
        for (const auto& row : r.rows)
            if (!row.empty()) pkColumns.push_back(row[0]);
        return true;
    }

    // Keyed, chunked structured row stream. use_result keeps memory flat; each
    // row becomes typed Cells (NULL / numeric-bare / prefix-less uppercase hex /
    // UTF-8 text) mirroring DumpTableData's per-field classification.
    bool StreamRows(const wxString& database, const QualifiedName& table,
                    const StreamOptions& opt, const RowSink& sink,
                    wxString& err) override
    {
        if (!conn_) { err = L"未连接"; return false; }

        wxString proj;
        if (opt.columns.empty()) proj = L"*";
        else
            for (size_t i = 0; i < opt.columns.size(); ++i)
                proj += (i ? L", " : L"") +
                        QuoteIdent(opt.columns[i], Dialect::MySQL);

        wxString sql = wxString::Format(L"SELECT %s FROM `%s`.`%s`",
                                        proj, database, table.Name());
        // Single-column cursor paging (composite-PK paging lands with the diff
        // engine in milestone C); compares against the first orderBy column.
        if (!opt.keyLowExcl.IsEmpty() && !opt.orderBy.empty())
            sql += wxString::Format(L" WHERE %s > '%s'",
                                    QuoteIdent(opt.orderBy[0], Dialect::MySQL),
                                    Esc(opt.keyLowExcl));
        sql += mysqlstream::OrderByClause(opt);   // honors opt.binaryOrder (T2)
        if (opt.limit >= 0)
            sql += wxString::Format(L" LIMIT %lld", opt.limit);

        const wxScopedCharBuffer q = sql.utf8_str();
        if (mysql_real_query(conn_, q.data(), q.length()) != 0) {
            err = wxString::FromUTF8(mysql_error(conn_));
            DropIfLost(mysql_errno(conn_));   // dead socket → mark disconnected (no crash)
            return false;
        }
        MYSQL_RES* res = mysql_use_result(conn_);
        if (!res) {
            if (mysql_field_count(conn_) == 0) return true;
            err = wxString::FromUTF8(mysql_error(conn_));
            return false;
        }
        MysqlResGuard resGuard(res);   // frees (and drains) at scope exit
        const unsigned int nf = mysql_num_fields(res);
        MYSQL_FIELD* fields = mysql_fetch_fields(res);

        bool stopped = false;
        while (MYSQL_ROW row = mysql_fetch_row(res)) {
            const unsigned long* len = mysql_fetch_lengths(res);
            std::vector<Cell> cells;
            cells.reserve(nf);
            for (unsigned int i = 0; i < nf; ++i) {
                Cell cell;
                if (row[i] == nullptr)               cell.kind = CellKind::Null;
                else if (fields[i].flags & NUM_FLAG) {
                    cell.kind = CellKind::Numeric;
                    cell.text = wxString::FromUTF8(row[i], len[i]);
                } else if (IsBinaryField(fields[i])) {
                    cell.kind = CellKind::Binary;
                    cell.text = mysqlstream::HexDigits(row[i], len[i]);   // no 0x prefix
                } else {
                    cell.kind = CellKind::Text;
                    cell.text = wxString::FromUTF8(row[i], len[i]);
                }
                cells.push_back(std::move(cell));
            }
            if (!sink(cells)) { stopped = true; break; }   // cancellation
        }
        // resGuard's mysql_free_result drains any rows still on the wire at scope
        // exit (fine after an early stop). Read the error before it does so.
        const unsigned int merr = stopped ? 0 : mysql_errno(conn_);
        if (merr) { err = wxString::FromUTF8(mysql_error(conn_)); DropIfLost(merr); return false; }
        return true;
    }

    bool ExecuteBatch(const wxString& sql, const std::vector<std::vector<Cell>>& rows,
                      wxString& err) override
    { return mysqlstream::ExecuteBatch(conn_, sql, rows, err); }

    // Abstract change set → MySQL statements (no trailing ';'). Order: create /
    // drop whole table short-circuit; else ADD/MODIFY columns, then index/FK
    // adds, then DROP-class in reverse (FK → index → column) so dependents go
    // before their targets.
    bool RenderSchemaChange(const SchemaChangeSet& ch,
                            std::vector<wxString>& stmts, wxString& err) override
    {
        (void)err;
        const wxString t = QuoteIdent(ch.table, Dialect::MySQL);

        if (ch.dropTable) {
            stmts.push_back(L"DROP TABLE IF EXISTS " + t);
            return true;
        }
        if (ch.createTable) {
            stmts.push_back(detail::RenderCreateTable(ch.createSchema));
            return true;
        }

        for (const auto& cc : ch.columns) {
            if (cc.op == ColumnChange::Op::Add) {
                wxString s = L"ALTER TABLE " + t + L" ADD COLUMN " +
                             detail::ColumnDef(cc.column);
                if (!cc.afterColumn.IsEmpty())
                    s += L" AFTER " + QuoteIdent(cc.afterColumn, Dialect::MySQL);
                stmts.push_back(s);
            } else if (cc.op == ColumnChange::Op::Modify) {
                stmts.push_back(L"ALTER TABLE " + t + L" MODIFY COLUMN " +
                                detail::ColumnDef(cc.column));
            }
        }
        for (const auto& ic : ch.indexes)
            if (ic.op == IndexChange::Op::Add) {
                if (ic.index.primary)
                    stmts.push_back(L"ALTER TABLE " + t + L" ADD PRIMARY KEY (" +
                                    detail::JoinIdents(ic.index.columns) + L")");
                else
                    stmts.push_back(L"ALTER TABLE " + t + L" ADD " +
                                    detail::IndexDef(ic.index));
            }
        for (const auto& fc : ch.foreignKeys)
            if (fc.op == FkChange::Op::Add)
                stmts.push_back(L"ALTER TABLE " + t + L" ADD " + detail::FkDef(fc.fk));

        for (auto it = ch.foreignKeys.rbegin(); it != ch.foreignKeys.rend(); ++it)
            if (it->op == FkChange::Op::Drop)
                stmts.push_back(L"ALTER TABLE " + t + L" DROP FOREIGN KEY " +
                                QuoteIdent(it->fk.name, Dialect::MySQL));
        for (auto it = ch.indexes.rbegin(); it != ch.indexes.rend(); ++it)
            if (it->op == IndexChange::Op::Drop)
                stmts.push_back(it->index.primary
                    ? (L"ALTER TABLE " + t + L" DROP PRIMARY KEY")
                    : (L"ALTER TABLE " + t + L" DROP INDEX " +
                       QuoteIdent(it->index.name, Dialect::MySQL)));
        for (auto it = ch.columns.rbegin(); it != ch.columns.rend(); ++it)
            if (it->op == ColumnChange::Op::Drop)
                stmts.push_back(L"ALTER TABLE " + t + L" DROP COLUMN " +
                                QuoteIdent(it->column.name, Dialect::MySQL));
        return true;
    }

    bool GetDatabaseOverview(DbOverview& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(
                L"SELECT s.SCHEMA_NAME, s.DEFAULT_CHARACTER_SET_NAME, "
                L"s.DEFAULT_COLLATION_NAME, "
                L"(SELECT COUNT(*) FROM information_schema.TABLES t "
                L"  WHERE t.TABLE_SCHEMA=s.SCHEMA_NAME) "
                L"FROM information_schema.SCHEMATA s ORDER BY s.SCHEMA_NAME", r, err))
            return false;
        out = DbOverview{};
        out.columns = { {L"name", L"数据库"}, {L"charset", L"字符集"},
                        {L"collation", L"排序规则"}, {L"tables", L"表数"} };
        out.rows = std::move(r.rows);
        return true;
    }

    wxString ServerVersion() const override { return version_; }

    // ---- user / privilege administration (thin — logic in db/UserAdmin.cpp) ----
    bool SupportsUserAdmin() const override { return true; }
    bool ListUsers(std::vector<UserInfo>& o, wxString& e) override
        { return useradmin::MyListUsers(this, o, e); }
    bool GetUserGrants(const wxString& u, const wxString& h, UserGrants& o, wxString& e) override
        { return useradmin::MyGetGrants(this, u, h, o, e); }
    bool SaveUser(const UserSpec& s, bool isNew, wxString& e) override
        { return useradmin::MySaveUser(this, s, isNew, e); }
    bool DropUser(const wxString& u, const wxString& h, wxString& e) override
        { return useradmin::MyDropUser(this, u, h, e); }
    UserAdminModel GetUserAdminModel() const override { return useradmin::MySqlModel(); }

private:
    // Escape a string literal for use inside single quotes.
    static wxString Esc(const wxString& s)
    {
        wxString e = s;
        e.Replace(L"\\", L"\\\\");
        e.Replace(L"'", L"''");
        return e;
    }

    // A binary column (BLOB/BINARY/VARBINARY/GEOMETRY) carries the binary charset
    // (63); real text columns carry a genuine character-set number. These bytes
    // aren't valid UTF-8, so they're dumped as an 0x… literal, not a quoted string.
    static bool IsBinaryField(const MYSQL_FIELD& f)
    {
        if (f.charsetnr != 63) return false;
        switch (f.type) {
        case MYSQL_TYPE_TINY_BLOB:  case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:  case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_STRING:     case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_GEOMETRY:
            return true;
        default:
            return false;
        }
    }

    // True if `s` is a bare keyword/identifier (letters, digits, underscore) — a
    // charset/collation name spliced into DDL unquoted must satisfy this.
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

    MYSQL*   conn_ = nullptr;
    wxString version_;
    // kept alive because mysql_ssl_set()/MYSQL_INIT_COMMAND store the pointers
    // until mysql_real_connect()
    wxScopedCharBuffer caBuf_, certBuf_, keyBuf_, cipherBuf_, tzBuf_, initBuf_;
    core::ConnectionProfile target_;   // for KILL QUERY on Cancel()
};

} // namespace

std::unique_ptr<IConnection> CreateMySqlConnection()
{
    return std::make_unique<MySqlConnection>();
}

} // namespace db
