// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SqliteDriver.cpp — embedded, file-based engine via the SQLite C API.
// Unlike the client/server drivers there is no host/port/user/password: the
// "connection" is a database file path, carried in ConnectionProfile::database.
#include "db/DbDriver.h"
#include "db/SqliteStream.h"   // parameterized batch INSERT (this file is at the
                               // 1000-line ceiling; see that header's banner)

#include <sqlite3.h>
#include <wx/filename.h>
#include <wx/stopwatch.h>

#include <mutex>

namespace db {
namespace {

wxString FromCol(sqlite3_stmt* st, int i)
{
    if (sqlite3_column_type(st, i) == SQLITE_NULL) return wxString(L"NULL");
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? wxString::FromUTF8(reinterpret_cast<const char*>(t)) : wxString(L"NULL");
}

// RAII: finalize the prepared statement when it leaves scope — even via an
// exception from FromCol/push_back/emit. Runs at scope exit where the manual
// sqlite3_finalize used to, so normal-path semantics are unchanged.
struct SqliteStmtGuard {
    sqlite3_stmt* st;
    explicit SqliteStmtGuard(sqlite3_stmt* s) : st(s) {}
    ~SqliteStmtGuard() { if (st) sqlite3_finalize(st); }
    SqliteStmtGuard(const SqliteStmtGuard&) = delete;
    SqliteStmtGuard& operator=(const SqliteStmtGuard&) = delete;
};

// ---- connection-free DDL rendering helpers (for RenderSchemaChange) --------
// SQLite-specific but handle-free: they turn normalized schema value objects
// (SchemaModel.h) into SQLite DDL text. Kept in this TU (the driver is well under
// the 1000-line charter after these), mirroring the PgSql/MySqlSql split only in
// spirit. Nothing here touches a sqlite3* handle.

// Double-quote + comma-join a column-name list (SQLite identifiers).
wxString JoinIdentsSqlite(const std::vector<wxString>& cols)
{
    wxString s;
    for (size_t i = 0; i < cols.size(); ++i)
        s += (i ? L", " : L"") + QuoteIdent(cols[i], Dialect::Sqlite);
    return s;
}

// Column-set equality (ordered) — used to skip a PK's implicit unique index.
bool SameColsSqlite(const std::vector<wxString>& a, const std::vector<wxString>& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
    return true;
}

// ColKind → a SQLite type-affinity keyword (cross-engine createTable fallback,
// used only when the source column carries no rawType). SQLite has dynamic typing
// with five affinities; this maps to them. Honest gate: coarse — width/scale are
// dropped and there is no native boolean/date/time type (they degrade to
// INTEGER/TEXT).
wxString SqliteTypeFromKind(ColKind k)
{
    switch (k) {
    case ColKind::Integer:
    case ColKind::Boolean:   return L"INTEGER";
    case ColKind::Decimal:   return L"NUMERIC";
    case ColKind::Float:     return L"REAL";
    case ColKind::Binary:
    case ColKind::Blob:      return L"BLOB";
    default:                 return L"TEXT";   // Char/Varchar/Text/Date/Time/…/Other
    }
}

// Column type text. Same-engine SQLite (rawType present): used verbatim — SQLite
// stores the declared type string and derives affinity from it, so "VARCHAR(80)"
// round-trips. Cross-engine (rawType empty): mapped from kind.
wxString SqliteTypeText(const NormColumn& c)
{
    return c.rawType.IsEmpty() ? SqliteTypeFromKind(c.kind) : c.rawType;
}

// `"col" <type> [NOT NULL] [DEFAULT …]` for a CREATE TABLE column list. PRIMARY
// KEY is emitted at table level (see RenderCreateTableSqlite), not inline, so a
// composite PK renders the same way as a single-column one.
wxString SqliteColumnDef(const NormColumn& c)
{
    wxString s = QuoteIdent(c.name, Dialect::Sqlite) + L" " + SqliteTypeText(c);
    if (c.notNull)    s += L" NOT NULL";
    if (c.hasDefault) s += L" DEFAULT " + c.defaultExpr;
    return s;
}

// CREATE [UNIQUE] INDEX "idx" ON "t" ("c1","c2").
wxString RenderCreateIndexSqlite(const wxString& table, const NormIndex& idx)
{
    wxString s = idx.unique ? L"CREATE UNIQUE INDEX " : L"CREATE INDEX ";
    s += QuoteIdent(idx.name, Dialect::Sqlite) + L" ON " +
         QuoteIdent(table, Dialect::Sqlite) + L" (" +
         JoinIdentsSqlite(idx.columns) + L")";
    return s;
}

// Inline FOREIGN KEY clause for a CREATE TABLE column list. SQLite can declare
// foreign keys ONLY at table-creation time (there is no ALTER TABLE ADD
// CONSTRAINT), so this is the sole place an FK is rendered — RenderSchemaChange
// skips every standalone FkChange with a warning.
wxString RenderFkInlineSqlite(const NormForeignKey& fk)
{
    wxString s = L"FOREIGN KEY (" + JoinIdentsSqlite(fk.columns) + L") REFERENCES " +
                 QuoteIdent(fk.refTable, Dialect::Sqlite) + L" (" +
                 JoinIdentsSqlite(fk.refColumns) + L")";
    if (!fk.onDelete.IsEmpty()) s += L" ON DELETE " + fk.onDelete;
    if (!fk.onUpdate.IsEmpty()) s += L" ON UPDATE " + fk.onUpdate;
    return s;
}

// Whole new table: CREATE TABLE (columns + table-level PRIMARY KEY + inline FKs)
// pushed first, then each secondary index as its own CREATE INDEX. The PK's
// implicit unique index is skipped.
void RenderCreateTableSqlite(const TableSchema& s, std::vector<wxString>& stmts)
{
    const wxString t = QuoteIdent(s.name.Name(), Dialect::Sqlite);
    std::vector<wxString> defs;
    for (const auto& c : s.columns) defs.push_back(L"  " + SqliteColumnDef(c));
    if (!s.primaryKey.empty())
        defs.push_back(L"  PRIMARY KEY (" + JoinIdentsSqlite(s.primaryKey) + L")");
    for (const auto& fk : s.foreignKeys)
        defs.push_back(L"  " + RenderFkInlineSqlite(fk));

    wxString sql = L"CREATE TABLE " + t + L" (\n";
    for (size_t i = 0; i < defs.size(); ++i)
        sql += defs[i] + (i + 1 < defs.size() ? L",\n" : L"\n");
    sql += L")";
    stmts.push_back(sql);

    for (const auto& idx : s.indexes) {
        if (idx.primary) continue;
        if (idx.unique && SameColsSqlite(idx.columns, s.primaryKey)) continue;
        stmts.push_back(RenderCreateIndexSqlite(s.name.Name(), idx));
    }
}

class SqliteConnection : public IConnection {
public:
    ~SqliteConnection() override { Disconnect(); }

    Dialect GetDialect() const override { return Dialect::Sqlite; }

    // Sqlite is a local file — no SSH tunnel concept — so this is just the profile
    // passed to Connect() below, verbatim.
    const core::ConnectionProfile& EffectiveProfile() const override { return profile_; }

    bool Connect(const core::ConnectionProfile& p, wxString& err) override
    {
        Disconnect();
        profile_ = p;
        if (p.database.IsEmpty()) { err = L"未指定数据库文件"; return false; }
        // READWRITE only (no CREATE): opening a non-existent path is an error in
        // V1 rather than silently creating an empty database.
        const int rc = sqlite3_open_v2(p.database.utf8_str(), &db_,
                                       SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK) {
            err = db_ ? wxString::FromUTF8(sqlite3_errmsg(db_))
                      : wxString(L"无法打开数据库文件");
            sqlite3_close_v2(db_);
            db_ = nullptr;
            return false;
        }
        sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
        return true;
    }

    void Disconnect() override
    {
        // Take the same mutex Cancel() holds before releasing db_, so Cancel()
        // can never sqlite3_interrupt a handle we are about to sqlite3_close_v2
        // (theoretical UAF if the two race). The lock is held only for the swap —
        // Execute()'s long prepare/step/finalize stays outside it, so a running
        // query is still interruptible.
        std::lock_guard<std::mutex> lk(dbMx_);
        if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
    }

    bool IsConnected() const override { return db_ != nullptr; }

    // sqlite3_interrupt is safe to call from another thread while a query runs.
    void Cancel() override
    {
        std::lock_guard<std::mutex> lk(dbMx_);
        if (db_) sqlite3_interrupt(db_);
    }

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        if (!db_) { err = L"未连接"; return false; }
        out = QueryResult();

        wxStopWatch sw;
        sqlite3_stmt* st = nullptr;
        const wxScopedCharBuffer q = sql.utf8_str();
        if (sqlite3_prepare_v2(db_, q.data(), static_cast<int>(q.length()),
                               &st, nullptr) != SQLITE_OK) {
            err = wxString::FromUTF8(sqlite3_errmsg(db_));
            return false;
        }
        SqliteStmtGuard stGuard(st);   // finalizes on every exit (was manual)

        // A comment- or whitespace-only statement prepares successfully to a NULL
        // handle (nothing to run). Treat it as a clean no-op — this is exactly the
        // shape of the "-- SQLite 不支持…" honest-skip lines RenderSchemaChange
        // emits, so a sync plan that carries one stays executable.
        if (!st) {
            out.isSelect = false;
            out.affected = 0;
            out.elapsedMs = sw.Time();
            return true;
        }

        const int ncol = sqlite3_column_count(st);
        if (ncol > 0) {
            out.isSelect = true;
            for (int i = 0; i < ncol; ++i)
                out.columns.push_back(wxString::FromUTF8(sqlite3_column_name(st, i)));
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                std::vector<wxString> row;
                row.reserve(ncol);
                for (int i = 0; i < ncol; ++i) row.push_back(FromCol(st, i));
                out.rows.push_back(std::move(row));
            }
            if (rc != SQLITE_DONE) {
                err = wxString::FromUTF8(sqlite3_errmsg(db_));
                return false;
            }
            out.affected = out.rows.size();
        } else {
            const int rc = sqlite3_step(st);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                err = wxString::FromUTF8(sqlite3_errmsg(db_));
                return false;
            }
            out.isSelect = false;
            out.affected = static_cast<unsigned long long>(sqlite3_changes(db_));
        }
        out.elapsedMs = sw.Time();
        return true;
    }

    // One file = one database. Present it under its file name (or "main").
    bool ListDatabases(std::vector<wxString>& out, wxString& err) override
    {
        (void)err;
        const wxString name = wxFileName(path_()).GetFullName();
        out.push_back(name.IsEmpty() ? wxString(L"main") : name);
        return true;
    }

    bool ListTables(const wxString& /*database*/, std::vector<TableInfo>& out,
                    wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT name FROM sqlite_master WHERE type='table' "
                     L"AND name NOT LIKE 'sqlite_%' ORDER BY name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (!row.empty()) out.push_back({row[0], L"—"});   // no row estimate in V1
        return true;
    }

    // Rich per-table metadata for the database-overview grid. SQLite has no cheap
    // estimates or per-table size, so rows come from a real COUNT(*) and size is
    // left unknown. comment/updatedAt/engine are all N/A for SQLite.
    bool GetTableList(const wxString& /*database*/,
                      std::vector<TableMeta>& out, wxString& err) override
    {
        // One pass for names + AUTOINCREMENT flag. We test the stored CREATE SQL
        // rather than sqlite_sequence: that table only exists once some AUTOINCREMENT
        // table has been created, so referencing it on a plain DB would error.
        QueryResult tl;
        if (!Execute(
                L"SELECT name, "
                L"CASE WHEN sql LIKE '%AUTOINCREMENT%' THEN 1 ELSE -1 END "
                L"FROM sqlite_master WHERE type='table' "
                L"AND name NOT LIKE 'sqlite_%' ORDER BY name", tl, err))
            return false;

        out.clear();
        for (const auto& row : tl.rows) {
            if (row.empty()) continue;
            TableMeta m;
            m.name = row[0];
            if (row.size() >= 2) row[1].ToLongLong(&m.autoIncrement);

            // Column count via the table-valued pragma (name is a string literal).
            QueryResult cc; wxString e2;
            if (Execute(L"SELECT COUNT(*) FROM pragma_table_info('" + Esc(m.name) + L"')",
                        cc, e2) && !cc.rows.empty() && !cc.rows[0].empty()) {
                long long n = -1; cc.rows[0][0].ToLongLong(&n);
                m.columns = static_cast<int>(n);
            }
            // Real row count — SQLite exposes no estimate. Identifier safely quoted.
            QueryResult rc; wxString e3;
            if (Execute(L"SELECT COUNT(*) FROM " + QuoteIdent(m.name, Dialect::Sqlite),
                        rc, e3) && !rc.rows.empty() && !rc.rows[0].empty())
                rc.rows[0][0].ToLongLong(&m.rows);
            out.push_back(m);
        }
        return true;
    }

    // Database-level summary: encoding from PRAGMA, table count from sqlite_master,
    // whole-file size from page_count*page_size, and the file path as extra note.
    bool GetDatabaseInfo(const wxString& database, DbInfo& out, wxString& err) override
    {
        out = DbInfo{};
        out.name = database;

        QueryResult enc; wxString e1;
        if (Execute(L"PRAGMA encoding", enc, e1) &&
            !enc.rows.empty() && !enc.rows[0].empty())
            out.charset = enc.rows[0][0];

        QueryResult tc; wxString e2;
        if (Execute(L"SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                    L"AND name NOT LIKE 'sqlite_%'", tc, e2) &&
            !tc.rows.empty() && !tc.rows[0].empty())
            tc.rows[0][0].ToLongLong(&out.tableCount);

        long long pages = -1, pageSize = -1;
        QueryResult pc; wxString e3;
        if (Execute(L"PRAGMA page_count", pc, e3) &&
            !pc.rows.empty() && !pc.rows[0].empty())
            pc.rows[0][0].ToLongLong(&pages);
        QueryResult ps; wxString e4;
        if (Execute(L"PRAGMA page_size", ps, e4) &&
            !ps.rows.empty() && !ps.rows[0].empty())
            ps.rows[0][0].ToLongLong(&pageSize);
        if (pages >= 0 && pageSize >= 0) out.sizeBytes = pages * pageSize;

        const wxString p = path_();
        if (!p.IsEmpty()) out.extra = p;
        (void)err;
        return true;
    }

    bool GetColumns(const wxString& /*db*/, const wxString& table,
                    std::vector<ColumnInfo>& out, wxString& err) override
    {
        QueryResult r;
        // table_xinfo adds a 7th "hidden" column that distinguishes generated
        // columns (2=VIRTUAL, 3=STORED) from ordinary (0) and truly-hidden (1);
        // it's the only pragma that surfaces generated columns. Fall back to
        // table_info on ancient SQLite (<3.26) that lacks table_xinfo.
        bool xinfo = Execute(
            L"PRAGMA table_xinfo(" + QuoteIdent(table, Dialect::Sqlite) + L")", r, err);
        if (!xinfo) {
            err.clear();
            if (!Execute(L"PRAGMA table_info(" + QuoteIdent(table, Dialect::Sqlite) + L")",
                         r, err))
                return false;
        }
        // SQLite pragmas do not expose the generation expression; parse it from the
        // CREATE TABLE SQL (best-effort — empty when it can't be isolated).
        wxString ddl; wxString ignore;
        {
            QueryResult d;
            if (Execute(L"SELECT sql FROM sqlite_master WHERE type='table' AND name='" +
                        Esc(table) + L"'", d, ignore) &&
                !d.rows.empty() && !d.rows[0].empty())
                ddl = d.rows[0][0];
        }

        // columns: cid, name, type, notnull, dflt_value, pk [, hidden]
        for (const auto& row : r.rows) {
            if (row.size() < 6) continue;
            const wxString hidden = (row.size() >= 7) ? row[6] : wxString(L"0");
            if (hidden == L"1") continue;   // truly-hidden: table_info omits these
            ColumnInfo c;
            long cid = 0; row[0].ToLong(&cid); c.ordinal = static_cast<int>(cid) + 1;
            c.name = row[1];
            c.type = row[2];
            c.length = LenFromType(row[2]);
            c.notNull = (row[3] == L"1");
            c.defaultVal = (row[4] == L"NULL") ? wxString() : row[4];
            if (row[5] != L"0") c.key = L"PK";
            // ---- extended introspection (best-effort) ----
            // SQLite has no per-column comment/charset/collation catalog — left empty.
            if (hidden == L"2" || hidden == L"3") {
                c.generatedStored = (hidden == L"3");
                c.generatedExpr = GeneratedExprFromDdl(ddl, c.name);
            }
            out.push_back(c);
        }
        return true;
    }

    bool GetForeignKeys(const wxString& /*db*/, const wxString& table,
                        std::vector<ForeignKey>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"PRAGMA foreign_key_list(" +
                     QuoteIdent(table, Dialect::Sqlite) + L")", r, err))
            return false;
        // columns: id, seq, table, from, to, on_update, on_delete, match
        for (const auto& row : r.rows) {
            if (row.size() < 5) continue;
            ForeignKey fk;
            fk.fromTable = table;  fk.fromColumn = row[3];
            fk.toTable   = row[2]; fk.toColumn   = row[4];
            out.push_back(fk);
        }
        return true;
    }

    bool GetIndexes(const wxString& /*db*/, const wxString& table,
                    std::vector<IndexInfo>& out, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"PRAGMA index_list(" + QuoteIdent(table, Dialect::Sqlite) + L")",
                     r, err))
            return false;
        // columns: seq, name, unique, origin, partial
        for (const auto& row : r.rows) {
            if (row.size() < 3) continue;
            IndexInfo idx;
            idx.name = row[1];
            idx.unique = (row[2] == L"1");
            QueryResult ci;
            wxString e2;
            if (Execute(L"PRAGMA index_info(" + QuoteIdent(idx.name, Dialect::Sqlite) +
                        L")", ci, e2)) {
                // columns: seqno, cid, name
                for (const auto& cr : ci.rows)
                    if (cr.size() >= 3) {
                        if (!idx.columns.IsEmpty()) idx.columns += L", ";
                        idx.columns += cr[2];
                    }
            }
            out.push_back(idx);
        }
        return true;
    }

    bool GetCreateDdl(const wxString& /*db*/, const wxString& table,
                      wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT sql FROM sqlite_master WHERE type='table' AND name='" +
                     Esc(table) + L"'", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty()) ddl = r.rows[0][0];
        return true;
    }

    bool ListViews(const wxString& /*db*/, std::vector<wxString>& out,
                   wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT name FROM sqlite_master WHERE type='view' ORDER BY name",
                     r, err))
            return false;
        for (const auto& row : r.rows)
            if (!row.empty()) out.push_back(row[0]);
        return true;
    }

    bool GetViewDdl(const wxString& /*db*/, const wxString& view,
                    wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT sql FROM sqlite_master WHERE type='view' AND name='" +
                     Esc(view) + L"'", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty()) ddl = r.rows[0][0];
        return true;
    }

    bool ListTriggers(const wxString& /*db*/, std::vector<TriggerInfo>& out,
                      wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT name, tbl_name FROM sqlite_master WHERE type='trigger' "
                     L"ORDER BY name", r, err))
            return false;
        for (const auto& row : r.rows)
            if (row.size() >= 2) out.push_back({row[0], row[1]});
        return true;
    }

    bool GetTriggerDdl(const wxString& /*db*/, const TriggerInfo& tg,
                       wxString& ddl, wxString& err) override
    {
        QueryResult r;
        if (!Execute(L"SELECT sql FROM sqlite_master WHERE type='trigger' AND name='" +
                     Esc(tg.name) + L"'", r, err))
            return false;
        if (!r.rows.empty() && !r.rows[0].empty()) ddl = r.rows[0][0];
        return true;
    }

    bool DumpTableData(const wxString& /*db*/, const wxString& table,
                       const std::function<void(const wxString&)>& emit,
                       wxString& err) override
    {
        if (!db_) { err = L"未连接"; return false; }
        const wxString q = QuoteIdent(table, Dialect::Sqlite);
        sqlite3_stmt* st = nullptr;
        const wxString stmt = L"SELECT * FROM " + q;
        const wxScopedCharBuffer sql = stmt.utf8_str();
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.length()),
                               &st, nullptr) != SQLITE_OK) {
            err = wxString::FromUTF8(sqlite3_errmsg(db_));
            return false;
        }
        SqliteStmtGuard stGuard(st);   // finalizes on every exit (was manual)
        const int nf = sqlite3_column_count(st);
        wxString cols;
        for (int i = 0; i < nf; ++i)
            cols += (i ? L", " : L"") +
                    QuoteIdent(wxString::FromUTF8(sqlite3_column_name(st, i)),
                               Dialect::Sqlite);
        const wxString prefix = L"INSERT INTO " + q + L" (" + cols + L") VALUES (";

        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            wxString vals;
            for (int i = 0; i < nf; ++i) {
                if (i) vals += L", ";
                switch (sqlite3_column_type(st, i)) {
                case SQLITE_NULL:    vals += L"NULL"; break;
                case SQLITE_INTEGER:
                case SQLITE_FLOAT:
                    vals += wxString::FromUTF8(
                        reinterpret_cast<const char*>(sqlite3_column_text(st, i)));
                    break;
                case SQLITE_BLOB:    vals += BlobHex(st, i); break;
                default:             // SQLITE_TEXT
                    vals += L"'" + Esc(FromCol(st, i)) + L"'";
                    break;
                }
            }
            emit(prefix + vals + L");\n");
        }
        // rc captured before stGuard finalizes st at scope exit.
        if (rc != SQLITE_DONE) { err = wxString::FromUTF8(sqlite3_errmsg(db_)); return false; }
        return true;
    }

    // ---- cross-database synchronization primitives ----

    // Keyed, chunked structured row stream over the sqlite3 step loop: one row is
    // materialized into typed Cells at a time (SQLite already pulls a single row
    // per sqlite3_step), so client memory stays flat regardless of table size —
    // the same discipline as DumpTableData. Cells classify by sqlite3_column_type:
    // NULL → Null; INTEGER/FLOAT → Numeric (bare text, no quotes); BLOB →
    // Binary (prefix-less UC hex); TEXT → Text (UTF-8). RenderLiteral then frames
    // each cell into the target dialect so the row round-trips.
    bool StreamRows(const wxString& /*database*/, const QualifiedName& table,
                    const StreamOptions& opt, const RowSink& sink,
                    wxString& err) override
    {
        if (!db_) { err = L"未连接"; return false; }

        wxString proj;
        if (opt.columns.empty()) proj = L"*";
        else
            for (size_t i = 0; i < opt.columns.size(); ++i)
                proj += (i ? L", " : L"") +
                        QuoteIdent(opt.columns[i], Dialect::Sqlite);

        wxString sql = L"SELECT " + proj + L" FROM " +
                       QuoteIdent(table.Name(), Dialect::Sqlite);
        // Single-column cursor paging (composite-PK paging is handled upstream by
        // DataSync's dual-stream merge); compares against the first orderBy column.
        // Bound as a parameter (?1) — injection-safe, and SQLite applies the
        // column's type affinity so a text cursor still compares correctly against
        // an INTEGER key.
        const bool paged = !opt.keyLowExcl.IsEmpty() && !opt.orderBy.empty();
        if (paged)
            sql += L" WHERE " + QuoteIdent(opt.orderBy[0], Dialect::Sqlite) + L" > ?";
        if (!opt.orderBy.empty()) {
            sql += L" ORDER BY ";
            for (size_t i = 0; i < opt.orderBy.size(); ++i)
                sql += (i ? L", " : L"") +
                       QuoteIdent(opt.orderBy[i], Dialect::Sqlite);
        }
        if (opt.limit >= 0)
            sql += wxString::Format(L" LIMIT %lld", opt.limit);

        sqlite3_stmt* st = nullptr;
        const wxScopedCharBuffer q = sql.utf8_str();
        if (sqlite3_prepare_v2(db_, q.data(), static_cast<int>(q.length()),
                               &st, nullptr) != SQLITE_OK) {
            err = wxString::FromUTF8(sqlite3_errmsg(db_));
            return false;
        }
        SqliteStmtGuard stGuard(st);   // finalizes on every exit, incl. exceptions
        if (paged) {
            const wxScopedCharBuffer cur = opt.keyLowExcl.utf8_str();
            sqlite3_bind_text(st, 1, cur.data(), static_cast<int>(cur.length()),
                              SQLITE_TRANSIENT);
        }

        const int nf = sqlite3_column_count(st);
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            std::vector<Cell> cells;
            cells.reserve(nf);
            for (int c = 0; c < nf; ++c) {
                Cell cell;
                switch (sqlite3_column_type(st, c)) {
                case SQLITE_NULL:
                    cell.kind = CellKind::Null;
                    break;
                case SQLITE_INTEGER:
                case SQLITE_FLOAT: {
                    cell.kind = CellKind::Numeric;      // bare value, no quotes
                    const unsigned char* t = sqlite3_column_text(st, c);
                    cell.text = t ? wxString::FromUTF8(
                        reinterpret_cast<const char*>(t)) : wxString();
                    break;
                }
                case SQLITE_BLOB:
                    cell.kind = CellKind::Binary;       // prefix-less UC hex
                    cell.text = BlobHexBare(st, c);
                    break;
                default: {                              // SQLITE_TEXT
                    cell.kind = CellKind::Text;
                    const unsigned char* t = sqlite3_column_text(st, c);
                    cell.text = t ? wxString::FromUTF8(
                        reinterpret_cast<const char*>(t)) : wxString();
                    break;
                }
                }
                cells.push_back(std::move(cell));
            }
            if (!sink(cells)) return true;   // early stop (cancellation) — not an error
        }
        // rc captured before stGuard finalizes st at scope exit.
        if (rc != SQLITE_DONE) {
            err = wxString::FromUTF8(sqlite3_errmsg(db_));
            return false;
        }
        return true;
    }

    // Parameterized batch INSERT — the apply half of data sync. Delegates to
    // SqliteStream.cpp (see that header): without this override SQLite inherited
    // IConnection's refusal and could never be an INSERT target at all.
    bool ExecuteBatch(const wxString& sql, const std::vector<std::vector<Cell>>& rows,
                      wxString& err) override
    { return sqlitestream::ExecuteBatch(db_, sql, rows, err); }

    // Abstract change set → SQLite statements (no trailing ';'; SyncEngine
    // Execute()s each one). SQLite's ALTER TABLE is deliberately minimal — it
    // supports only ADD COLUMN, DROP COLUMN (3.35+), and RENAME. So this renderer
    // is HONEST about what it cannot do rather than emitting invalid SQL:
    //   • createTable / dropTable short-circuit (FKs inlined at create time);
    //   • ColumnChange Add     → ALTER TABLE … ADD COLUMN (NOT-NULL-without-DEFAULT
    //                            is illegal → degraded to nullable + a comment);
    //   • ColumnChange Modify  → SKIPPED (no ALTER COLUMN) + a "-- SQLite …" note;
    //   • ColumnChange Drop    → ALTER TABLE … DROP COLUMN (requires 3.35+);
    //   • IndexChange Add/Drop → CREATE/DROP INDEX (schema-level objects);
    //   • FkChange  Add/Drop   → SKIPPED (FKs are create-time only) + a note.
    // Order mirrors the other drivers: ADD/MODIFY columns, then index/FK adds,
    // then DROP-class in reverse (FK → index → column) so dependents precede
    // targets. The skip-notes are pushed as `-- …` statements — harmless no-ops
    // when Execute()'d (see the NULL-stmt guard in Execute), and visible if the
    // plan is dumped as a script.
    bool RenderSchemaChange(const SchemaChangeSet& ch,
                            std::vector<wxString>& stmts, wxString& err) override
    {
        (void)err;
        const wxString t = QuoteIdent(ch.table, Dialect::Sqlite);

        if (ch.dropTable) {
            stmts.push_back(L"DROP TABLE IF EXISTS " + t);
            return true;
        }
        if (ch.createTable) {
            RenderCreateTableSqlite(ch.createSchema, stmts);
            return true;
        }

        for (const auto& cc : ch.columns) {
            if (cc.op == ColumnChange::Op::Add) {
                const NormColumn& c = cc.column;
                if (c.notNull && !c.hasDefault) {
                    // SQLite forbids ADD COLUMN NOT NULL without a DEFAULT (the
                    // existing rows would have no value). Degrade to nullable and
                    // flag it, rather than emit an illegal statement.
                    stmts.push_back(
                        L"-- SQLite: 列 \"" + c.name +
                        L"\" 为 NOT NULL 但无默认值，ADD COLUMN 不能加 NOT NULL，"
                        L"已降级为可空（需要时请手工补默认值/回填）");
                    NormColumn relaxed = c;
                    relaxed.notNull = false;
                    stmts.push_back(L"ALTER TABLE " + t + L" ADD COLUMN " +
                                    SqliteColumnDef(relaxed));
                } else {
                    stmts.push_back(L"ALTER TABLE " + t + L" ADD COLUMN " +
                                    SqliteColumnDef(c));
                }
            } else if (cc.op == ColumnChange::Op::Modify) {
                // SQLite's ALTER TABLE cannot change a column's type/constraints;
                // it needs a full table rebuild (create-new + copy + drop +
                // rename). Honest gate: skip + note rather than a bogus ALTER.
                stmts.push_back(
                    L"-- SQLite: 不支持 ALTER COLUMN，列 \"" + cc.column.name +
                    L"\" 的类型/约束修改已跳过（需整表重建 create+copy+drop+rename）");
            }
        }

        for (const auto& ic : ch.indexes)
            if (ic.op == IndexChange::Op::Add)
                stmts.push_back(RenderCreateIndexSqlite(ch.table, ic.index));

        for (const auto& fc : ch.foreignKeys)
            if (fc.op == FkChange::Op::Add)
                stmts.push_back(
                    L"-- SQLite: 不支持 ALTER 添加外键，约束 \"" +
                    (fc.fk.name.IsEmpty() ? wxString(L"(未命名)") : fc.fk.name) +
                    L"\" 已跳过（外键只能在建表时定义）");

        // DROP-class in reverse (FK → index → column).
        for (auto it = ch.foreignKeys.rbegin(); it != ch.foreignKeys.rend(); ++it)
            if (it->op == FkChange::Op::Drop)
                stmts.push_back(
                    L"-- SQLite: 不支持 ALTER 删除外键，约束 \"" +
                    (it->fk.name.IsEmpty() ? wxString(L"(未命名)") : it->fk.name) +
                    L"\" 已跳过（需整表重建）");
        for (auto it = ch.indexes.rbegin(); it != ch.indexes.rend(); ++it)
            if (it->op == IndexChange::Op::Drop)
                stmts.push_back(L"DROP INDEX IF EXISTS " +
                                QuoteIdent(it->index.name, Dialect::Sqlite));
        for (auto it = ch.columns.rbegin(); it != ch.columns.rend(); ++it)
            if (it->op == ColumnChange::Op::Drop)
                // DROP COLUMN requires SQLite 3.35+ (2021-03). Older engines will
                // reject it — a documented minimum-version requirement.
                stmts.push_back(L"ALTER TABLE " + t + L" DROP COLUMN " +
                                QuoteIdent(it->column.name, Dialect::Sqlite));
        return true;
    }

    bool GetDatabaseOverview(DbOverview& out, wxString& err) override
    {
        // PRAGMA database_list → seq, name, file (main + any ATTACHed).
        QueryResult r;
        if (!Execute(L"PRAGMA database_list", r, err)) return false;
        out = DbOverview{};
        out.columns = { {L"name", L"名称"}, {L"file", L"文件路径"} };
        for (const auto& row : r.rows)
            if (row.size() >= 3)
                out.rows.push_back({ row[1],
                    row[2].IsEmpty() ? wxString(L"(内存)") : row[2] });
        return true;
    }

    wxString ServerVersion() const override
    {
        return wxString::FromUTF8(sqlite3_libversion());
    }

private:
    // Escape a string literal for '…' (SQLite doubles single quotes).
    static wxString Esc(const wxString& s)
    {
        wxString e = s;
        e.Replace(L"'", L"''");
        return e;
    }

    // Best-effort: pull column `col`'s generation expression out of the CREATE
    // TABLE SQL. Splits the column-def list at depth-0 commas (so each column /
    // table-constraint is isolated), finds the segment whose leading identifier is
    // `col`, then extracts the balanced-paren expression after " AS ". Returns ""
    // when it can't be isolated confidently — never guesses.
    static wxString GeneratedExprFromDdl(const wxString& ddl, const wxString& col)
    {
        if (ddl.IsEmpty()) return wxString();
        // Locate the outer '(' opening the column-def list and its match.
        int open = ddl.Find(L'(');
        if (open == wxNOT_FOUND) return wxString();
        int depth = 0; int close = -1;
        for (int i = open; i < (int)ddl.length(); ++i) {
            const wxChar ch = ddl[i];
            if (ch == L'(') ++depth;
            else if (ch == L')') { if (--depth == 0) { close = i; break; } }
        }
        if (close < 0) return wxString();
        const wxString body = ddl.Mid(open + 1, close - open - 1);

        // Split body at depth-0 commas into segments.
        std::vector<wxString> segs;
        int d = 0; size_t start = 0;
        for (size_t i = 0; i < body.length(); ++i) {
            const wxChar ch = body[i];
            if (ch == L'(') ++d;
            else if (ch == L')') --d;
            else if (ch == L',' && d == 0) {
                segs.push_back(body.Mid(start, i - start));
                start = i + 1;
            }
        }
        segs.push_back(body.Mid(start));

        for (wxString seg : segs) {
            seg.Trim(false);
            // Leading identifier: quoted ("x" / `x` / [x]) or a bare word.
            wxString name;
            if (!seg.IsEmpty() && (seg[0] == L'"' || seg[0] == L'`' || seg[0] == L'[')) {
                const wxChar cl = (seg[0] == L'[') ? L']' : seg[0];
                size_t j = 1;
                for (; j < seg.length() && seg[j] != cl; ++j) name += seg[j];
            } else {
                size_t j = 0;
                for (; j < seg.length() &&
                       !wxIsspace(seg[j]) && seg[j] != L'(' && seg[j] != L','; ++j)
                    name += seg[j];
            }
            if (name != col) continue;
            // Find " AS " (case-insensitive) at depth 0, then a balanced '(' expr.
            const wxString up = seg.Upper();
            int as = wxNOT_FOUND, dd = 0;
            for (int i = 0; i + 1 < (int)up.length(); ++i) {
                const wxChar ch = up[i];
                if (ch == L'(') ++dd;
                else if (ch == L')') --dd;
                else if (dd == 0 && (i == 0 || wxIsspace(up[i - 1])) &&
                         up[i] == L'A' && up[i + 1] == L'S' &&
                         (i + 2 >= (int)up.length() || wxIsspace(up[i + 2]) ||
                          up[i + 2] == L'(')) { as = i; break; }
            }
            if (as == wxNOT_FOUND) return wxString();
            int p = seg.find(L'(', as);
            if (p == (int)wxString::npos) return wxString();
            int ed = 0; int endp = -1;
            for (int i = p; i < (int)seg.length(); ++i) {
                if (seg[i] == L'(') ++ed;
                else if (seg[i] == L')') { if (--ed == 0) { endp = i; break; } }
            }
            if (endp < 0) return wxString();
            wxString expr = seg.Mid(p + 1, endp - p - 1);
            expr.Trim(true).Trim(false);
            return expr;
        }
        return wxString();
    }

    // Length inside a declared type, e.g. "VARCHAR(80)" → "80", "NUMERIC(10,2)" → "10,2".
    static wxString LenFromType(const wxString& type)
    {
        const int lp = type.Find(L'(');
        const int rp = type.Find(L')');
        if (lp != wxNOT_FOUND && rp != wxNOT_FOUND && rp > lp + 1)
            return type.Mid(lp + 1, rp - lp - 1);
        return wxString();
    }

    // BLOB bytes → X'DEADBEEF' literal; empty blob → X''.
    static wxString BlobHex(sqlite3_stmt* st, int i)
    {
        const int n = sqlite3_column_bytes(st, i);
        const unsigned char* p =
            static_cast<const unsigned char*>(sqlite3_column_blob(st, i));
        static const wchar_t* kHex = L"0123456789ABCDEF";
        wxString h = L"X'";
        for (int k = 0; k < n && p; ++k) { h += kHex[p[k] >> 4]; h += kHex[p[k] & 0xF]; }
        return h + L"'";
    }

    // BLOB bytes → prefix-less UPPERCASE hex ("DEADBEEF"), the framing-free form
    // Cell/RenderLiteral expects (RenderLiteral adds the dialect wrapper). Empty
    // blob → "". Mirrors PgDriver::ByteaHex / MySqlDriver's binary path.
    static wxString BlobHexBare(sqlite3_stmt* st, int i)
    {
        const int n = sqlite3_column_bytes(st, i);
        const unsigned char* p =
            static_cast<const unsigned char*>(sqlite3_column_blob(st, i));
        static const wchar_t* kHex = L"0123456789ABCDEF";
        wxString h;
        for (int k = 0; k < n && p; ++k) { h += kHex[p[k] >> 4]; h += kHex[p[k] & 0xF]; }
        return h;
    }

    // Path of the open database (via sqlite3_db_filename for "main").
    wxString path_() const
    {
        const char* f = db_ ? sqlite3_db_filename(db_, "main") : nullptr;
        return f ? wxString::FromUTF8(f) : wxString();
    }

    sqlite3* db_ = nullptr;
    std::mutex dbMx_;   // serializes Cancel() vs Disconnect() releasing db_
    core::ConnectionProfile profile_;   // for EffectiveProfile() (no tunnel concept — always the raw profile)
};

} // namespace

std::unique_ptr<IConnection> CreateSqliteConnection()
{
    return std::make_unique<SqliteConnection>();
}

} // namespace db
