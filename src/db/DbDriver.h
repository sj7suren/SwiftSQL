// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DbDriver.h — real database connectivity layer.
// Blocking calls by design; the UI runs them off the main thread and marshals
// results back with CallAfter. One implementation per wire protocol:
//   MySqlConnection  → MySQL + OceanBase (MySQL protocol, libmariadb)
//   PgConnection     → PostgreSQL (libpq)
#pragma once

#include <wx/string.h>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include "core/ConnectionProfile.h"
#include "core/DbTypes.h"
#include "db/SchemaModel.h"   // TableSchema / SchemaChangeSet value objects
#include "db/SyncTypes.h"     // Cell / StreamOptions / RowSink
#include "db/UserAdmin.h"     // UserInfo / UserSpec / UserGrants (user-admin virtuals)

namespace db {

struct QueryResult {
    std::vector<wxString>              columns;
    std::vector<std::vector<wxString>> rows;      // cell text; NULL → "NULL"
    unsigned long long                 affected = 0;
    long                               elapsedMs = 0;
    bool                               isSelect = false;
};

struct TableInfo {
    wxString name;
    wxString approxRows;   // human-readable estimate ("48,120", "—")

    // The namespace this table lives in, on engines that have one — PostgreSQL's
    // nspname. Empty on MySQL/SQLite/SQL Server, where the database IS the
    // namespace and a table name is already unique within it (QualifiedName.h
    // explains why that asymmetry is carried honestly rather than smoothed over).
    //
    // This field is why a table picker can now tell `public.orders` from
    // `archive.orders`. Before it existed, PostgreSQL's ListTables returned the
    // string "orders" twice, and nothing above it could recover which was which.
    wxString schema;

    // The identity, assembled from the two halves above. Callers that need a
    // stable key (a selection map, a plan unit, a sync scope entry) must use
    // THIS, never `name` — `name` alone is ambiguous on PostgreSQL.
    QualifiedName Qualified() const { return QualifiedName(schema, name); }
};

// Rich per-table metadata for the database-overview table list. Unknown numeric
// fields are -1 (the UI renders them "—"); unknown text fields stay empty. One
// metadata query per database fills a whole vector of these.
struct TableMeta {
    wxString  name;
    wxString  comment;         // table COMMENT ("" if none)
    wxString  engine;          // storage engine ("InnoDB"…); "" if N/A
    long long rows = -1;       // approx row count
    long long sizeBytes = -1;  // data + index size, bytes
    long long autoIncrement = -1;  // next AUTO_INCREMENT; -1 = no auto-inc column
    int       columns = -1;    // column count
    wxString  updatedAt;       // last-modified timestamp text ("" if unknown)
};

// Database-level summary for the overview info panel.
struct DbInfo {
    wxString  name;
    wxString  charset;         // default character set ("" if N/A)
    wxString  collation;       // default collation ("" if N/A)
    long long tableCount = -1; // base tables
    long long sizeBytes = -1;  // total data + index across tables
    wxString  extra;           // any engine-specific note ("" if none)
};

// Full single-table info for the open-table view's right-hand panel (Navicat-
// style). Numeric unknowns are -1, text unknowns "". Engines fill what they can.
struct TableDetail {
    wxString  name;
    wxString  comment;
    wxString  engine;
    wxString  rowFormat;       // "Dynamic", "Compact"… (MySQL)
    wxString  collation;
    wxString  createOptions;
    long long rows = -1;
    long long dataLength = -1;     // bytes
    long long indexLength = -1;    // bytes
    long long maxDataLength = -1;  // bytes
    long long dataFree = -1;       // bytes
    long long autoIncrement = -1;  // -1 = none
    wxString  createdAt;       // "YYYY-MM-DD HH:MM:SS" or ""
    wxString  updatedAt;
    wxString  checkTime;
};

// One column of a table's structure — the INTROSPECTION DTO returned by
// GetColumns. The extended fields below are read best-effort from each engine's
// catalog and feed the field editor's "back-fill" (so opening an existing column
// shows its real comment / charset / unsigned / generated etc.). This is NOT the
// editing model — TableDesignView maps ColumnInfo → db::ColumnModel for editing.
// Engines fill what they can; unset stays empty/false and the UI shows blank.
struct ColumnInfo {
    int      ordinal = 0;
    wxString name;
    wxString type;         // base type: "varchar", "int8", "timestamptz"…
    wxString length;       // size / precision: "80", "10" (may still be "10,2" — UI splits)
    bool     notNull = false;
    wxString defaultVal;   // "" when none
    wxString key;          // "PK", "FK", "UQ", or ""
    // ---- extended introspection (best-effort per engine; feed back-fill) ----
    wxString scale;            // decimal scale ("2" of decimal(10,2)); "" if N/A
    wxString comment;          // column comment / description
    wxString charset;          // character set (MySQL/SQL Server); "" if N/A
    wxString collation;        // collation; "" if N/A
    bool     unsignedFlag = false;    // MySQL UNSIGNED
    bool     autoIncrement = false;   // AUTO_INCREMENT / IDENTITY / serial
    wxString generatedExpr;    // generated/computed column expression; "" = not generated
    bool     generatedStored = false; // STORED vs VIRTUAL (when generatedExpr set)
    wxString keyLength;        // MySQL index prefix length for this column; "" if none
};

// A foreign-key edge (for Table Design 外键 tab and the ER diagram).
struct ForeignKey {
    wxString fromTable, fromColumn;
    wxString toTable,   toColumn;
};

// An index on a table (for Table Design 索引 tab).
struct IndexInfo {
    wxString name;
    wxString columns;      // comma-joined column list
    bool     unique = false;
};

// One entity for the ER diagram: table name + a few columns with key markers.
struct Entity {
    wxString                name;
    std::vector<ColumnInfo> columns;
};

// A stored routine (for dump export). type is "PROCEDURE" or "FUNCTION".
struct RoutineInfo {
    wxString name;
    wxString type;
};

// A trigger (for dump export). table is the table it fires on (PG needs it for DROP).
struct TriggerInfo {
    wxString name;
    wxString table;
};

// SQL dialect — drives DDL generation (identifier quoting, ALTER syntax).
// SqlServer and Oracle fall into the "else" (double-quote) branch everywhere a
// dialect is consumed, matching Postgres — no exhaustive switch depends on this
// enum. Oracle covers both Oracle Database and 达梦 DM (DM is Oracle-compatible;
// its driver reuses the Oracle introspection SQL, so it reports Dialect::Oracle).
enum class Dialect { MySQL, Postgres, Sqlite, SqlServer, Oracle };

// Quote a SQL identifier for the dialect (MySQL `x`, others "x"), doubling any
// embedded quote char. Used wherever a name is spliced into DDL.
inline wxString QuoteIdent(const wxString& id, Dialect d)
{
    const wxString q = (d == Dialect::MySQL) ? L"`" : L"\"";
    wxString e = id;
    e.Replace(q, q + q);
    return q + e + q;
}

// Render one table's identity as a dialect-legal, fully-qualified SQL name.
//
// SINGLE DEFINITION ON PURPOSE. DataSync.cpp (which renders DML for the preview
// path) and DataSyncExec.cpp (which renders the statements that actually run)
// each used to carry their own private copy of this logic. Two copies of "how do
// we spell the target table" is exactly the seam a wrong-table write slips
// through, so there is now one.
//
// The two engines are qualified along DIFFERENT axes, which is the asymmetry
// QualifiedName.h exists to keep honest:
//   MySQL     `db`.`tbl`      — the DATABASE is the namespace; MySQL DML can
//                               name another database directly, and MySQL has no
//                               schema layer, so QualifiedName::Schema() is
//                               ignored here rather than invented.
//   PostgreSQL "schema"."tbl" — the SCHEMA is the namespace; PG cannot reference
//                               another database in an identifier at all, so
//                               `db` is irrelevant and the connection's current
//                               database is used. An unqualified name is left
//                               bare, resolving through search_path exactly as
//                               before.
//   others     "tbl"          — single namespace; bare, as before.
inline wxString QualifiedTableSql(const wxString& db, const QualifiedName& table,
                                  Dialect d)
{
    if (d == Dialect::MySQL)
        return QuoteIdent(db, d) + L"." + QuoteIdent(table.Name(), d);
    if (table.HasSchema())
        return QuoteIdent(table.Schema(), d) + L"." + QuoteIdent(table.Name(), d);
    return QuoteIdent(table.Name(), d);
}

// Dialect-aware row-window clause for a paged/chunked SELECT (leading space
// included; no trailing ';'). MySQL/PG/SQLite use LIMIT…OFFSET; Oracle 12c+
// (and DM in Oracle mode) use the SQL:2008 OFFSET…FETCH form. SQL Server keeps
// the legacy LIMIT arm here — its paged browse is a separate, pre-existing path.
inline wxString LimitOffsetClause(Dialect d, long long limit, long long offset)
{
    if (d == Dialect::Oracle) {
        // OFFSET…FETCH is standard on Oracle 12.1+; tolerated without ORDER BY
        // (non-deterministic order, same caveat as LIMIT without ORDER BY).
        if (offset > 0)
            return wxString::Format(L" OFFSET %lld ROWS FETCH NEXT %lld ROWS ONLY",
                                    offset, limit);
        return wxString::Format(L" FETCH FIRST %lld ROWS ONLY", limit);
    }
    return wxString::Format(L" LIMIT %lld OFFSET %lld", limit, offset);
}

// ---- CREATE DATABASE options (drive the "new database" dialog) ----
// One configurable option an engine exposes for CREATE DATABASE.
struct DbCreateOption {
    enum class Kind { Choice, Int, FreeText };
    wxString              id;           // "charset","collation","encoding","owner"…
    wxString              label;        // localized (tr)
    Kind                  kind = Kind::Choice;
    std::vector<wxString> choices;      // candidate values (Choice / FreeText hints)
    std::vector<wxString> choiceGroup;  // parallel to choices; the value of `groupSource` each belongs to
    wxString              groupSource;  // id of the option this one filters by (collation→charset)
    wxString              defaultVal;   // preselected value ("" = none)
    bool                  optional = true;
};

// What an engine supports for CREATE DATABASE. supported=false → bare name only.
struct DbCreateCaps {
    bool                        supported = false;
    std::vector<DbCreateOption> options;
};

// A filled-in create request: name + chosen (option id → value) pairs ("" = omit).
struct DbCreateRequest {
    wxString                                    name;
    std::vector<std::pair<wxString, wxString>>  values;

    wxString Value(const wxString& id) const
    {
        for (const auto& kv : values) if (kv.first == id) return kv.second;
        return wxString();
    }
};

// ---- database overview (drives the connection-overview grid) ----
struct DbListColumn { wxString id; wxString label; };
struct DbOverview {
    std::vector<DbListColumn>           columns;
    std::vector<std::vector<wxString>>  rows;   // row cells parallel to columns
};

// ---- server monitor (drives the session/process list in ServerMonitorDialog) ----
// One live server session mapped onto a fixed, cross-engine column set. Each driver
// fills what its catalog exposes and leaves the rest empty; the dialog renders every
// field as-is. `id` is the token KillProcess() consumes (MySQL process id, PG pid,
// SQL Server spid, Oracle "sid,serial#", DM sess_id) — never shown as editable.
struct ProcessInfo {
    wxString server;    // instance/host label (usually filled by the UI from the conn name)
    wxString id;        // kill token — engine-native session/process identifier
    wxString user;      // login / db user
    wxString host;      // client host / address
    wxString db;        // current database / schema
    wxString command;   // command type / backend type ("Query", "Sleep", …)
    wxString time;      // elapsed/idle seconds (as text)
    wxString state;     // session state ("active", "idle", "Sleep", …)
    wxString info;      // current SQL text (may be empty)
};

class IConnection {
public:
    virtual ~IConnection() = default;

    virtual Dialect GetDialect() const = 0;

    virtual bool Connect(const core::ConnectionProfile& p, wxString& err) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    // The profile actually dialed — the same host/port passed to Connect(), NOT
    // necessarily the caller's saved profile. An SSH-tunneled connection's caller
    // (ConnectionTree::ConnectEntry) rewrites host/port to 127.0.0.1 + the tunnel's
    // local port *before* calling Connect(); this exposes whatever a driver
    // captured from that call, so a second connection can be dialed to the same
    // real endpoint (see db::CloneConnection in db/ConnectionClone.h — the single
    // chokepoint for that). Cloning off the raw saved profile would skip the
    // tunnel and connect to the wrong host, or fail outright. Default: an empty
    // profile, for IConnection implementations that never track it (e.g. test
    // stubs); every real driver overrides with the profile it captured in Connect().
    virtual const core::ConnectionProfile& EffectiveProfile() const
    {
        static const core::ConnectionProfile kEmpty;
        return kEmpty;
    }

    virtual bool Execute(const wxString& sql, QueryResult& out, wxString& err) = 0;

    // Request cancellation of an in-flight query from another thread. Safe to
    // call while Execute() is blocked on the worker thread. Default: no-op.
    virtual void Cancel() {}

    virtual bool ListDatabases(std::vector<wxString>& out, wxString& err) = 0;

    // Make `db` the session's current database for subsequent unqualified SQL.
    // MySQL / SQL Server issue USE; PostgreSQL reconnects (one DB per session);
    // SQLite / Oracle are no-ops (single-file / schema-based). Default: no-op.
    virtual bool UseDatabase(const wxString& db, wxString& err) { (void)db; (void)err; return true; }

    virtual bool ListTables(const wxString& database,
                            std::vector<TableInfo>& out, wxString& err) = 0;

    // ---- database overview (table list + db info) ----
    // Rich per-table metadata for the database-overview grid. Default: derive
    // just the names from ListTables, leaving the rest unknown ("—"), so any
    // engine degrades gracefully; real drivers override with one metadata query.
    virtual bool GetTableList(const wxString& database,
                              std::vector<TableMeta>& out, wxString& err)
    {
        std::vector<TableInfo> t;
        if (!ListTables(database, t, err)) return false;
        out.clear();
        for (const auto& ti : t) { TableMeta m; m.name = ti.name; out.push_back(m); }
        return true;
    }
    // Database-level summary for the overview info panel. Default: just the name.
    virtual bool GetDatabaseInfo(const wxString& database, DbInfo& out, wxString& err)
    {
        (void)err;
        out = DbInfo{};
        out.name = database;
        return true;
    }

    // Full single-table info for the open-table info panel. Default: degrade to
    // the fields GetTableList already exposes (real drivers override with one
    // targeted metadata query).
    virtual bool GetTableDetail(const wxString& database, const wxString& table,
                                TableDetail& out, wxString& err)
    {
        out = TableDetail{};
        out.name = table;
        std::vector<TableMeta> metas;
        if (!GetTableList(database, metas, err)) return true;   // name-only, best effort
        for (const auto& m : metas)
            if (m.name == table) {
                out.comment = m.comment; out.engine = m.engine;
                out.rows = m.rows; out.dataLength = m.sizeBytes;
                out.autoIncrement = m.autoIncrement; out.updatedAt = m.updatedAt;
                break;
            }
        return true;
    }

    // ---- schema introspection (Table Design / ER views) ----
    virtual bool GetColumns(const wxString& database, const wxString& table,
                            std::vector<ColumnInfo>& out, wxString& err) = 0;
    virtual bool GetForeignKeys(const wxString& database, const wxString& table,
                                std::vector<ForeignKey>& out, wxString& err) = 0;
    virtual bool GetIndexes(const wxString& database, const wxString& table,
                            std::vector<IndexInfo>& out, wxString& err) = 0;
    virtual bool GetCreateDdl(const wxString& database, const wxString& table,
                              wxString& ddl, wxString& err) = 0;

    // ---- views ----
    // List view names in `database`. Default: none — engines without view
    // support simply contribute no views to a dump (not an error).
    virtual bool ListViews(const wxString& database, std::vector<wxString>& out,
                           wxString& err)
    {
        (void)database; (void)out; (void)err;
        return true;
    }
    // The CREATE (OR REPLACE) VIEW statement for one view. Default: unsupported.
    virtual bool GetViewDdl(const wxString& database, const wxString& view,
                            wxString& ddl, wxString& err)
    {
        (void)database; (void)view; (void)ddl;
        err = L"该驱动不支持视图导出";
        return false;
    }

    // ---- routines & triggers ----
    // Each returns *bare* DDL (no DELIMITER wrapping, no added terminator) — the
    // dump orchestrator adds the dialect-specific wrapping, matching GetViewDdl.
    // Defaults: none / unsupported, so an un-wired engine contributes no such
    // sections to a dump.
    virtual bool ListRoutines(const wxString& database,
                              std::vector<RoutineInfo>& out, wxString& err)
    {
        (void)database; (void)out; (void)err;
        return true;
    }
    virtual bool GetRoutineDdl(const wxString& database, const RoutineInfo& r,
                               wxString& ddl, wxString& err)
    {
        (void)database; (void)r; (void)ddl;
        err = L"该驱动不支持存储程序导出";
        return false;
    }
    virtual bool ListTriggers(const wxString& database,
                              std::vector<TriggerInfo>& out, wxString& err)
    {
        (void)database; (void)out; (void)err;
        return true;
    }
    virtual bool GetTriggerDdl(const wxString& database, const TriggerInfo& t,
                               wxString& ddl, wxString& err)
    {
        (void)database; (void)t; (void)ddl;
        err = L"该驱动不支持触发器导出";
        return false;
    }

    // ---- CREATE DATABASE ----
    // Report the create-database options (and candidate value lists) this engine
    // supports; may query the server. Default: unsupported → the dialog degrades
    // to a bare name. Returns false only on a query error.
    virtual bool GetCreateDatabaseCaps(DbCreateCaps& out, wxString& err)
    {
        (void)err;
        out = DbCreateCaps{};
        return true;
    }
    // Assemble one safe CREATE DATABASE statement (no trailing ';', so a single
    // Execute autocommits — avoiding PG's transaction-block trap). Identifier and
    // literal escaping happen here, where the dialect is known. Default: bare name.
    virtual wxString BuildCreateDatabaseSql(const DbCreateRequest& req) const
    {
        return L"CREATE DATABASE " + QuoteIdent(req.name, GetDialect());
    }

    // Per-engine database list for the connection-overview grid (columns + rows).
    // Default: a single 名称 column from ListDatabases, so any engine degrades
    // gracefully; real drivers override with dialect-specific metadata SQL.
    virtual bool GetDatabaseOverview(DbOverview& out, wxString& err)
    {
        std::vector<wxString> dbs;
        if (!ListDatabases(dbs, err)) return false;
        out = DbOverview{};
        out.columns.push_back({ L"name", L"名称" });
        for (const auto& d : dbs) out.rows.push_back({ d });
        return true;
    }

    // ---- data export ----
    // Stream `table`'s rows as INSERT statements, calling `emit` once per row.
    // Rows are pulled from the server one at a time (not buffered client-side),
    // so memory stays flat regardless of table size. Unlike the stringified
    // QueryResult path this preserves real NULLs, emits numeric columns
    // unquoted, and hex-encodes binary columns, so the dump round-trips.
    // Default: unsupported.
    virtual bool DumpTableData(const wxString& database, const wxString& table,
                               const std::function<void(const wxString&)>& emit,
                               wxString& err)
    {
        (void)database; (void)table; (void)emit;
        err = L"该驱动不支持数据导出";
        return false;
    }

    // ---- cross-database synchronization primitives ----
    // (see docs/design/cross-db-sync.md §4.2). All four carry a default
    // degradation so adding them doesn't break un-wired drivers: GetTableSchema
    // and GetPrimaryKey best-effort off the existing introspection calls (good
    // enough for a same-engine rawType diff); RenderSchemaChange / StreamRows
    // honestly report "unsupported" until a driver overrides them.

    // Normalized structure for the diff engine. Default: assemble from
    // GetColumns + GetPrimaryKey (+ best-effort GetIndexes / GetForeignKeys).
    // kind falls to Other and length is taken from the pre-comma part of
    // ColumnInfo.length; rawType carries the engine-native type so a same-engine
    // diff still works. Returns best-effort true even if the extras fail.
    //
    // `table` is SCHEMA-QUALIFIED, and a driver with a schema layer must read
    // the schema it was ASKED for. The PostgreSQL reader used to hard-code
    // `table_schema='public'` and returned true-with-zero-columns for a table in
    // any other schema — indistinguishable from DiffSchema's "table does not
    // exist" convention, i.e. a silent misread, observed live before this
    // signature changed. This default (name-only engines) is unaffected.
    virtual bool GetTableSchema(const wxString& db, const QualifiedName& table,
                                TableSchema& out, wxString& err)
    {
        out = TableSchema{};
        out.name = table;
        std::vector<ColumnInfo> cols;
        if (!GetColumns(db, table.Name(), cols, err)) return false;
        for (const auto& ci : cols) {
            NormColumn nc;
            nc.name       = ci.name;
            nc.kind       = ColKind::Other;
            nc.rawType    = ci.type;
            nc.notNull    = ci.notNull;
            nc.hasDefault = !ci.defaultVal.IsEmpty();
            nc.defaultExpr= ci.defaultVal;
            nc.ordinal    = ci.ordinal;
            if (!ci.length.IsEmpty()) {          // "10,2" → 10; "" → -1
                long long v = -1;
                if (ci.length.BeforeFirst(L',').ToLongLong(&v)) nc.length = v;
            }
            out.columns.push_back(std::move(nc));
        }
        wxString ignore;
        GetPrimaryKey(db, table.Name(), out.primaryKey, ignore);   // best effort

        std::vector<IndexInfo> idxs;
        if (GetIndexes(db, table.Name(), idxs, ignore))
            for (const auto& ii : idxs) {
                NormIndex ni;
                ni.name    = ii.name;
                ni.unique  = ii.unique;
                ni.primary = (ii.name == L"PRIMARY");
                wxString rest = ii.columns;          // comma-joined → vector
                while (true) {
                    wxString part = rest.BeforeFirst(L',');
                    part.Trim(true).Trim(false);
                    if (!part.IsEmpty()) ni.columns.push_back(part);
                    if (!rest.Contains(L",")) break;
                    rest = rest.AfterFirst(L',');
                }
                out.indexes.push_back(std::move(ni));
            }

        std::vector<ForeignKey> fks;
        if (GetForeignKeys(db, table.Name(), fks, ignore))
            for (const auto& fk : fks) {
                NormForeignKey nf;
                nf.columns.push_back(fk.fromColumn);
                nf.refTable = fk.toTable;
                nf.refColumns.push_back(fk.toColumn);
                out.foreignKeys.push_back(std::move(nf));
            }
        return true;
    }

    // Ordered primary-key columns. Default: derive from GetColumns key=="PK"
    // (table order, not guaranteed PK sequence — drivers override for order).
    virtual bool GetPrimaryKey(const wxString& db, const wxString& table,
                               std::vector<wxString>& pkColumns, wxString& err)
    {
        std::vector<ColumnInfo> cols;
        if (!GetColumns(db, table, cols, err)) return false;
        pkColumns.clear();
        for (const auto& c : cols)
            if (c.key == L"PK") pkColumns.push_back(c.name);
        return true;
    }

    // Abstract change set → this dialect's ALTER/CREATE/DROP (no trailing ';').
    // Default: unsupported — structure sync needs a driver override.
    virtual bool RenderSchemaChange(const SchemaChangeSet& changes,
                                    std::vector<wxString>& stmts, wxString& err)
    {
        (void)changes; (void)stmts;
        err = L"该驱动不支持结构同步";
        return false;
    }

    // Keyed, chunked structured row stream (use_result streaming, flat memory).
    // Default: unsupported — data sync needs a driver override.
    // `table` is SCHEMA-QUALIFIED. A driver with a schema layer (PostgreSQL)
    // MUST honour the schema half and must not fall back to search_path: doing
    // so read whichever same-named table the session happened to resolve, which
    // is the read side of the wrong-table defect this signature closes. Drivers
    // without one (MySQL/SQLite) ignore it — see QualifiedName.h.
    virtual bool StreamRows(const wxString& db, const QualifiedName& table,
                            const StreamOptions& opt, const RowSink& sink,
                            wxString& err)
    {
        (void)db; (void)table; (void)opt; (void)sink;
        err = L"该驱动不支持结构化行读取";
        return false;
    }

    // Parameterized, batched row writer — the apply half of data sync (ADR-015
    // T4). Rendering hundreds of thousands of literal INSERTs and executing them
    // one at a time is both slow (one round trip per row) and unsafe: every row
    // then depends on RenderLiteral's escaping being right for that engine, so a
    // single escaping bug becomes a data-integrity incident instead of a syntax
    // error. Here no value is ever spliced into SQL text — Cells are bound as
    // parameters — and rows go out ~500 per round trip.
    //
    // `sql` is the statement HEAD up to and including the VALUES keyword, with
    // NO value tuples, e.g. "INSERT INTO `app`.`t` (`id`, `name`) VALUES". The
    // driver appends its own native placeholder tuples (MySQL `(?, ?)`,
    // PostgreSQL `($1, $2)`), one per row, so the caller never has to know a
    // dialect's placeholder syntax. Every row in `rows` must have the same
    // arity; an empty `rows` is a no-op success. Binary cells carry prefix-less
    // uppercase hex (StreamRows' convention) and are bound as raw bytes.
    //
    // Default: unsupported, mirroring StreamRows above so an un-wired driver
    // degrades honestly instead of silently doing something else.
    virtual bool ExecuteBatch(const wxString& sql,
                              const std::vector<std::vector<Cell>>& rows,
                              wxString& err)
    {
        (void)sql; (void)rows;
        err = L"该驱动不支持批量执行";
        return false;
    }

    // ---- user / privilege administration ----
    // Whether this driver implements user management. Only MySQL does today; the
    // UI shows a "not supported" notice when this is false, so PG/Oracle/SQL
    // Server/SQLite degrade gracefully (SQLite has no user concept at all). Real
    // support is staged per dialect — we never emit invented syntax here.
    virtual bool SupportsUserAdmin() const { return false; }
    virtual bool ListUsers(std::vector<UserInfo>& out, wxString& err)
    { (void)out; err = L"当前数据库暂不支持用户管理"; return false; }
    virtual bool GetUserGrants(const wxString& user, const wxString& host,
                               UserGrants& out, wxString& err)
    { (void)user; (void)host; (void)out; err = L"当前数据库暂不支持用户管理"; return false; }
    virtual bool SaveUser(const UserSpec& spec, bool isNew, wxString& err)
    { (void)spec; (void)isNew; err = L"当前数据库暂不支持用户管理"; return false; }
    virtual bool DropUser(const wxString& user, const wxString& host, wxString& err)
    { (void)user; (void)host; err = L"当前数据库暂不支持用户管理"; return false; }

    // Account shape + privilege catalogs that drive UserEditDialog's rendering.
    // Default: unsupported (supported=false). Real drivers return a filled model so
    // the dialog adapts per dialect (host field, auth plugin, server/db privilege
    // sets, which tabs to show) without hardcoding any one engine's shape.
    virtual UserAdminModel GetUserAdminModel() const { return UserAdminModel{}; }
    // Role tokens the "隶属成员" tab offers as grantable to an account. Default:
    // derive from ListUsers (every account is a potential role — true for MySQL 8
    // and PostgreSQL where roles ARE accounts); Oracle overrides with DBA_ROLES.
    // Tokens are "name@host" when the model has a host, else plain names — matching
    // the tokens GetUserGrants reports in UserGrants::roles.
    virtual bool ListGrantableRoles(std::vector<wxString>& out, wxString& err)
    {
        std::vector<UserInfo> users;
        if (!ListUsers(users, err)) return false;
        const bool hasHost = GetUserAdminModel().hasHost;
        out.clear();
        for (const auto& u : users)
            out.push_back(hasHost ? (u.name + L"@" + u.host) : u.name);
        return true;
    }

    // Server-monitor session listing / kill is NOT a per-driver virtual: it lives in
    // db/ProcessMonitor.{h,cpp} as free functions over Execute()+GetDialect(), so the
    // ceiling-height driver TUs stay untouched and every dialect's session SQL sits in
    // one readable place (mirrors how UserAdminOracle.cpp externalizes that concern).

    virtual wxString ServerVersion() const = 0;
};

// Factory: picks the wire protocol for the engine.
std::unique_ptr<IConnection> CreateConnection(DbType type);

} // namespace db
