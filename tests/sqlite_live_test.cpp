// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sqlite_live_test.cpp — self-contained, zero-config live integration test for
// the SQLite driver (SqliteConnection). Unlike pg_live_test this needs NO env
// vars, NO server and NO network: SQLite is an embedded, file-based engine, so
// the whole test runs against a throwaway database file in the system temp dir.
// CI can run it unconditionally.
//
// DRIVER FACT (verified against src/db/SqliteDriver.cpp): Connect() opens with
// SQLITE_OPEN_READWRITE *only* — no SQLITE_OPEN_CREATE. Opening a non-existent
// path is therefore an error, not a silent create. So this test first bootstraps
// a valid empty database file with the raw sqlite3 C API (sqlite3_open uses
// READWRITE|CREATE by default) and forces the header to disk with a PRAGMA,
// then hands the finished file to SqliteConnection::Connect. The test target
// links unofficial::sqlite3::sqlite3 for exactly this bootstrap (mirrors how
// src/db links the same lib PRIVATE).
//
// The temp file is removed unconditionally on exit via an RAII guard, pass or
// fail. Nothing outside that one file is touched.
#include "db/DbDriver.h"
#include "db/SchemaDiff.h"
#include "db/DataSync.h"
#include "core/ConnectionProfile.h"

#include <sqlite3.h>
#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/string.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

using namespace db;
using namespace db::sync;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectEq(const char* name, long long got, long long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%lld got=%lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// String equality with a diagnostic dump on mismatch (UTF-8 to be console-safe).
static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=\"%s\" got=\"%s\"\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// Run one statement through the driver; on failure print the error, return false.
static bool Exec(IConnection& c, const wxString& sql, const char* label)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) {
        std::printf("  ERR  %s: %s\n", label, (const char*)err.utf8_str());
        return false;
    }
    return true;
}

// Bootstrap: create a valid, non-empty SQLite database file at `path` using the
// raw C API (default open flags = READWRITE|CREATE). The PRAGMA forces the 100-
// byte header to disk so the file exists and is a real database — required
// because SqliteConnection::Connect opens READWRITE-only and a lazily-opened
// zero-byte file might never hit the filesystem.
static bool Bootstrap(const wxString& path, wxString& err)
{
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.utf8_str(), &raw) != SQLITE_OK) {
        err = raw ? wxString::FromUTF8(sqlite3_errmsg(raw))
                  : wxString(L"sqlite3_open failed");
        sqlite3_close(raw);
        return false;
    }
    char* e = nullptr;
    const int rc = sqlite3_exec(raw, "PRAGMA user_version=1;", nullptr, nullptr, &e);
    if (rc != SQLITE_OK) {
        err = e ? wxString::FromUTF8(e) : wxString(L"bootstrap PRAGMA failed");
        sqlite3_free(e);
        sqlite3_close(raw);
        return false;
    }
    sqlite3_close(raw);
    return true;
}

// SQLite profile: file path in `database`, no host/port/user/password.
static core::ConnectionProfile FileProfile(const wxString& file)
{
    core::ConnectionProfile p;
    p.type     = db::DbType::Sqlite;
    p.database = file;
    return p;
}

int main()
{
    std::printf("== sqlite_live_test (libsqlite %s) ==\n", sqlite3_libversion());

    // ---- unique temp file path (do NOT pre-create; bootstrap creates it) ----
    // wxFileName::CreateTempFileName creates a 0-byte file and returns its path;
    // we take that unique path, delete the placeholder, then let Bootstrap build
    // a real database there. Guarantees no collision with a concurrent run.
    wxString dbFile = wxFileName::CreateTempFileName(L"swiftsql_sqlitetest_");
    if (dbFile.IsEmpty()) {
        std::printf("  FATAL: could not allocate a temp file name\n");
        return 1;
    }
    if (wxFileExists(dbFile)) wxRemoveFile(dbFile);

    // RAII: remove the temp DB file unconditionally on the way out (and any
    // -wal/-shm side files SQLite may have created), pass or fail.
    struct FileGuard {
        wxString path;
        ~FileGuard()
        {
            if (wxFileExists(path))            wxRemoveFile(path);
            if (wxFileExists(path + L"-wal"))  wxRemoveFile(path + L"-wal");
            if (wxFileExists(path + L"-shm"))  wxRemoveFile(path + L"-shm");
        }
    } fileGuard{ dbFile };

    {
        wxString bErr;
        if (!Bootstrap(dbFile, bErr)) {
            std::printf("  FATAL: bootstrap of %s failed: %s\n",
                        (const char*)dbFile.utf8_str(), (const char*)bErr.utf8_str());
            return 1;
        }
    }
    ExpectTrue("bootstrap created db file", wxFileExists(dbFile));

    // ---- (optional) honesty gate: READWRITE-only means opening a missing file
    //      must fail with an error, not silently create it ----
    {
        auto miss = CreateConnection(db::DbType::Sqlite);
        core::ConnectionProfile mp = FileProfile(dbFile + L"_does_not_exist.db");
        wxString err;
        bool ok = miss->Connect(mp, err);
        ExpectTrue("connect(nonexistent path) fails (READWRITE, no CREATE)", !ok);
        ExpectTrue("connect(nonexistent path) returns an error message", !err.IsEmpty());
        // and the driver must NOT have created the phantom file
        ExpectTrue("nonexistent path not silently created",
                   !wxFileExists(dbFile + L"_does_not_exist.db"));
        if (wxFileExists(dbFile + L"_does_not_exist.db"))
            wxRemoveFile(dbFile + L"_does_not_exist.db");
    }

    // ---- 1. connect ----
    auto conn = CreateConnection(db::DbType::Sqlite);
    ExpectTrue("factory returned a connection", conn != nullptr);
    if (!conn) { std::printf("== %d checks, %d failed ==\n", g_checks, g_fails); return 1; }

    {
        core::ConnectionProfile p = FileProfile(dbFile);
        wxString err;
        bool ok = conn->Connect(p, err);
        ExpectTrue("Connect() to bootstrapped file", ok);
        if (!ok) {
            std::printf("  connect err: %s\n", (const char*)err.utf8_str());
            std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
            return 1;
        }
        ExpectTrue("IsConnected()", conn->IsConnected());
        ExpectTrue("GetDialect()==Sqlite", conn->GetDialect() == Dialect::Sqlite);
    }

    // ---- 2. DDL + data via Execute() ----
    bool setup = true;
    setup &= Exec(*conn,
        L"CREATE TABLE t_widget("
        L"id INTEGER PRIMARY KEY, name TEXT NOT NULL, note TEXT)", "create table");
    setup &= Exec(*conn, L"CREATE INDEX idx_name ON t_widget(name)", "create index");
    ExpectTrue("DDL setup ok", setup);

    // INSERTs — one row deliberately has note = NULL. Assert affected==1 each.
    if (setup) {
        struct Ins { const wchar_t* sql; };
        const wxString rows[] = {
            L"INSERT INTO t_widget(id,name,note) VALUES (1,'alpha','n1')",
            L"INSERT INTO t_widget(id,name,note) VALUES (2,'beta','n2')",
            L"INSERT INTO t_widget(id,name,note) VALUES (3,'gamma',NULL)",
        };
        for (int i = 0; i < 3; ++i) {
            QueryResult r; wxString err;
            bool ok = conn->Execute(rows[i], r, err);
            ExpectTrue("INSERT ok", ok);
            if (ok) {
                ExpectTrue("INSERT is not a SELECT", !r.isSelect);
                ExpectEq("INSERT affected == 1", (long long)r.affected, 1);
            } else {
                std::printf("  insert err: %s\n", (const char*)err.utf8_str());
            }
        }
    }

    // ---- 3. introspection ----
    if (setup) {
        // ListTables — must include t_widget.
        {
            std::vector<TableInfo> tables; wxString err;
            bool ok = conn->ListTables(wxString(), tables, err);
            ExpectTrue("ListTables ok", ok);
            bool found = false;
            for (const auto& t : tables) if (t.name == L"t_widget") found = true;
            ExpectTrue("ListTables contains t_widget", found);
        }

        // GetColumns — id/name/note with type, notNull, PK marker.
        {
            std::vector<ColumnInfo> cols; wxString err;
            bool ok = conn->GetColumns(wxString(), L"t_widget", cols, err);
            ExpectTrue("GetColumns ok", ok);
            ExpectEq("GetColumns count == 3", (long long)cols.size(), 3);
            if (cols.size() == 3) {
                ExpectStr("col[0].name == id",   cols[0].name, L"id");
                ExpectStr("col[0].type == INTEGER", cols[0].type, L"INTEGER");
                ExpectStr("col[0].key == PK",    cols[0].key,  L"PK");
                // INTEGER PRIMARY KEY is rowid alias; pragma reports notnull=0
                // unless explicitly declared NOT NULL, so id.notNull is false.
                ExpectTrue("col[0](id).notNull == false", !cols[0].notNull);

                ExpectStr("col[1].name == name", cols[1].name, L"name");
                ExpectStr("col[1].type == TEXT", cols[1].type, L"TEXT");
                ExpectTrue("col[1](name).notNull == true", cols[1].notNull);
                ExpectStr("col[1](name).key not PK", cols[1].key, wxString());

                ExpectStr("col[2].name == note", cols[2].name, L"note");
                ExpectStr("col[2].type == TEXT", cols[2].type, L"TEXT");
                ExpectTrue("col[2](note).notNull == false", !cols[2].notNull);
            }
        }

        // GetPrimaryKey — [id] (base-class default derives from key=="PK").
        {
            std::vector<wxString> pk; wxString err;
            bool ok = conn->GetPrimaryKey(wxString(), L"t_widget", pk, err);
            ExpectTrue("GetPrimaryKey ok", ok);
            ExpectEq("GetPrimaryKey size == 1", (long long)pk.size(), 1);
            if (pk.size() == 1) ExpectStr("GetPrimaryKey[0] == id", pk[0], L"id");
        }

        // GetIndexes — must include idx_name (non-unique).
        {
            std::vector<IndexInfo> idx; wxString err;
            bool ok = conn->GetIndexes(wxString(), L"t_widget", idx, err);
            ExpectTrue("GetIndexes ok", ok);
            const IndexInfo* named = nullptr;
            for (const auto& i : idx) if (i.name == L"idx_name") named = &i;
            ExpectTrue("GetIndexes contains idx_name", named != nullptr);
            if (named) {
                ExpectStr("idx_name columns == name", named->columns, L"name");
                ExpectTrue("idx_name is not unique", !named->unique);
            }
        }

        // GetTableSchema — assembled via base default (GetColumns + GetPrimaryKey
        // + GetIndexes). Assert column count, PK, and that the index came through.
        {
            TableSchema ts; wxString err;
            bool ok = conn->GetTableSchema(wxString(), L"t_widget", ts, err);
            ExpectTrue("GetTableSchema ok", ok);
            ExpectEq("GetTableSchema columns == 3", (long long)ts.columns.size(), 3);
            ExpectEq("GetTableSchema primaryKey size == 1",
                     (long long)ts.primaryKey.size(), 1);
            if (ts.primaryKey.size() == 1)
                ExpectStr("GetTableSchema PK == id", ts.primaryKey[0], L"id");
            bool hasIdx = false;
            for (const auto& ni : ts.indexes) if (ni.name == L"idx_name") hasIdx = true;
            ExpectTrue("GetTableSchema has idx_name", hasIdx);
        }
    }

    // ---- 4. query result shape + values + NULL handling ----
    if (setup) {
        QueryResult r; wxString err;
        bool ok = conn->Execute(
            L"SELECT id,name,note FROM t_widget ORDER BY id", r, err);
        ExpectTrue("SELECT ok", ok);
        if (ok) {
            ExpectTrue("SELECT isSelect", r.isSelect);
            ExpectEq("SELECT column count == 3", (long long)r.columns.size(), 3);
            if (r.columns.size() == 3) {
                ExpectStr("SELECT col0 == id",   r.columns[0], L"id");
                ExpectStr("SELECT col1 == name", r.columns[1], L"name");
                ExpectStr("SELECT col2 == note", r.columns[2], L"note");
            }
            ExpectEq("SELECT row count == 3", (long long)r.rows.size(), 3);
            if (r.rows.size() == 3) {
                ExpectStr("row0 id",   r.rows[0][0], L"1");
                ExpectStr("row0 name", r.rows[0][1], L"alpha");
                ExpectStr("row0 note", r.rows[0][2], L"n1");
                ExpectStr("row1 name", r.rows[1][1], L"beta");
                ExpectStr("row2 id",   r.rows[2][0], L"3");
                ExpectStr("row2 name", r.rows[2][1], L"gamma");
                // NULL note → driver's NULL marker ("NULL"), NOT empty string.
                ExpectStr("row2 note is NULL marker", r.rows[2][2], L"NULL");
            }
        } else {
            std::printf("  select err: %s\n", (const char*)err.utf8_str());
        }
    }

    // ---- 5. structure sync: DiffSchema → RenderSchemaChange (SQLite DDL) ----
    // Hand-build a source/target schema pair whose diff exercises every arm of the
    // SQLite renderer: an ADD column, a type-Modify column (must be HONESTLY
    // skipped — SQLite has no ALTER COLUMN), an ADD index, and an ADD foreign key
    // (must be HONESTLY skipped — SQLite FKs are create-time only). Same-dialect
    // diff → exact rawType compare. This is pure rendering: no DDL is executed,
    // so the real t_widget structure is untouched.
    if (setup) {
        // matches a real statement only — comment (`-- …`) lines are ignored, so a
        // note that merely *mentions* "ALTER COLUMN" doesn't count as one emitted.
        auto anyStmt = [](const std::vector<wxString>& v, const wxString& sub) {
            for (const auto& s : v)
                if (!s.StartsWith(L"--") && s.Contains(sub)) return true;
            return false;
        };
        auto anyLine = [](const std::vector<wxString>& v, const wxString& sub) {
            for (const auto& s : v) if (s.Contains(sub)) return true;
            return false;
        };
        auto mkCol = [](const wxString& n, const wxString& raw, bool nn = false) {
            NormColumn c; c.name = n; c.rawType = raw; c.notNull = nn; return c;
        };

        TableSchema ss, ts;
        ss.name = ts.name = L"t_widget";
        ss.primaryKey = ts.primaryKey = { L"id" };
        ss.columns = { mkCol(L"id", L"INTEGER"), mkCol(L"name", L"TEXT", true),
                       mkCol(L"price", L"REAL"), mkCol(L"extra", L"TEXT") };
        ts.columns = { mkCol(L"id", L"INTEGER"), mkCol(L"name", L"TEXT", true),
                       mkCol(L"price", L"INTEGER") };   // price rawType differs → Modify
        { NormIndex ix; ix.name = L"idx_name"; ix.columns = { L"name" };
          ss.indexes.push_back(ix); }                  // source-only → Add index
        { NormForeignKey fk; fk.name = L"fk_owner"; fk.columns = { L"extra" };
          fk.refTable = L"users"; fk.refColumns = { L"id" };
          ss.foreignKeys.push_back(fk); }              // source-only → Add FK (skipped)

        SchemaDiffResult diff = DiffSchema(ss, ts, Dialect::Sqlite, Dialect::Sqlite);
        std::vector<wxString> stmts; wxString err;
        bool ok = conn->RenderSchemaChange(diff.changes, stmts, err);
        ExpectTrue("RenderSchemaChange ok", ok);

        // Added column → ADD COLUMN with quoted name + type.
        ExpectTrue("DDL: ADD COLUMN \"extra\" TEXT",
                   anyStmt(stmts, L"ADD COLUMN \"extra\" TEXT"));
        // Added index → CREATE INDEX with quoted table + column.
        ExpectTrue("DDL: CREATE INDEX \"idx_name\"",
                   anyStmt(stmts, L"CREATE INDEX \"idx_name\""));
        ExpectTrue("DDL: index ON \"t_widget\" (\"name\")",
                   anyStmt(stmts, L"ON \"t_widget\" (\"name\")"));
        // Modified column → honestly skipped: NO real ALTER COLUMN, plus a note.
        ExpectTrue("DDL: no bogus ALTER COLUMN statement",
                   !anyStmt(stmts, L"ALTER COLUMN"));
        ExpectTrue("DDL: modify skipped with warning note",
                   anyLine(stmts, L"不支持 ALTER COLUMN"));
        // Added FK → honestly skipped: NO real FOREIGN KEY clause, plus a note.
        ExpectTrue("DDL: no bogus ALTER ADD FOREIGN KEY",
                   !anyStmt(stmts, L"FOREIGN KEY"));
        ExpectTrue("DDL: FK add skipped with warning note",
                   anyLine(stmts, L"不支持 ALTER 添加外键"));
    }

    // ---- 6. data sync: real cross-connection diff via StreamRows ----
    // DataSync reads schema.name from BOTH connections, so a genuine src≠tgt diff
    // needs two connections. SQLite is one-file-per-connection, so we spin up a
    // second throwaway db file + connection whose t_widget differs from the first
    // (§2) by exactly one insert (id=3) / one update (id=2) / one delete (id=4) /
    // one unchanged row (id=1).
    if (setup) {
        wxString dbFile2 = wxFileName::CreateTempFileName(L"swiftsql_sqlitetest2_");
        ExpectTrue("temp file 2 allocated", !dbFile2.IsEmpty());
        if (wxFileExists(dbFile2)) wxRemoveFile(dbFile2);

        // Declared BEFORE conn2 so it is destroyed AFTER it: conn2's destructor
        // disconnects (closes the handle) before this guard removes the file —
        // avoiding a Windows "file in use" on teardown.
        struct FileGuard2 {
            wxString path;
            ~FileGuard2()
            {
                if (wxFileExists(path))           wxRemoveFile(path);
                if (wxFileExists(path + L"-wal")) wxRemoveFile(path + L"-wal");
                if (wxFileExists(path + L"-shm")) wxRemoveFile(path + L"-shm");
            }
        } fileGuard2{ dbFile2 };

        wxString b2Err;
        bool boot2 = !dbFile2.IsEmpty() && Bootstrap(dbFile2, b2Err);
        ExpectTrue("bootstrap second db file", boot2);

        auto conn2 = CreateConnection(db::DbType::Sqlite);
        bool conn2Ok = false;
        if (boot2 && conn2) {
            core::ConnectionProfile p2 = FileProfile(dbFile2);
            wxString e2;
            conn2Ok = conn2->Connect(p2, e2);
            ExpectTrue("Connect() second db", conn2Ok);
            if (!conn2Ok) std::printf("  connect2 err: %s\n", (const char*)e2.utf8_str());
        }

        if (conn2Ok) {
            bool s2 = true;
            s2 &= Exec(*conn2,
                L"CREATE TABLE t_widget("
                L"id INTEGER PRIMARY KEY, name TEXT NOT NULL, note TEXT)",
                "create tgt table");
            s2 &= Exec(*conn2,
                L"INSERT INTO t_widget(id,name,note) VALUES "
                L"(1,'alpha','n1'),(2,'BETA','changed'),(4,'delta','n4')",
                "insert tgt rows");
            ExpectTrue("target-table setup ok", s2);

            if (s2) {
                TableSchema common;
                common.name = L"t_widget";
                // Types matter, and not only cosmetically: ADR-015 T2's ordering
                // gate (db::sync::CheckDataDiffOrdering) decides whether a PK can
                // be ordered identically by both engines from the column's
                // ColKind/rawType, and REFUSES a key it cannot classify. A
                // typeless NormColumn is exactly such a key, so the fixture
                // declares what the real introspection would report for
                // "id INTEGER PRIMARY KEY, name TEXT, note TEXT".
                auto addc = [&](const wxString& n, ColKind k, const wxString& raw) {
                    NormColumn c; c.name = n; c.kind = k; c.rawType = raw;
                    common.columns.push_back(c);
                };
                addc(L"id",   ColKind::Integer, L"INTEGER");
                addc(L"name", ColKind::Text,    L"TEXT");
                addc(L"note", ColKind::Text,    L"TEXT");
                common.primaryKey = { L"id" };

                DataSyncOptions opt;
                opt.insert = opt.update = opt.deleteMissing = true;
                std::vector<wxString> emitted; RowStat stat; wxString err;
                std::atomic<bool> stop{false};
                // src = conn (t_widget from §2), tgt = conn2 (t_widget above).
                bool ok = DiffAndEmitData(*conn, *conn2, wxString(), wxString(),
                                          common, opt,
                                          [&](const wxString& s) { emitted.push_back(s); },
                                          stat, err, stop);
                ExpectTrue("DiffAndEmitData ok", ok);
                if (!ok) std::printf("  datasync err: %s\n", (const char*)err.utf8_str());

                ExpectEq("data: inserts (id=3)",   stat.inserts,   1);
                ExpectEq("data: updates (id=2)",   stat.updates,   1);
                ExpectEq("data: deletes (id=4)",   stat.deletes,   1);
                ExpectEq("data: unchanged (id=1)", stat.unchanged, 1);

                auto anyEmit = [&](const wxString& sub) {
                    for (const auto& s : emitted) if (s.Contains(sub)) return true;
                    return false;
                };
                // Target dialect = SQLite → double-quoted idents, bare numerics.
                ExpectTrue("emit: INSERT into \"t_widget\"",
                           anyEmit(L"INSERT INTO \"t_widget\""));
                ExpectTrue("emit: INSERT carries id=3 'gamma'", anyEmit(L"'gamma'"));
                // NULL note (source id=3) preserved as a real NULL, not 'NULL'.
                ExpectTrue("emit: INSERT preserves NULL note", anyEmit(L"'gamma', NULL"));
                ExpectTrue("emit: UPDATE \"t_widget\" SET", anyEmit(L"UPDATE \"t_widget\" SET"));
                ExpectTrue("emit: UPDATE targets \"id\" = 2", anyEmit(L"\"id\" = 2"));
                ExpectTrue("emit: DELETE \"id\" = 4",
                           anyEmit(L"DELETE FROM \"t_widget\" WHERE \"id\" = 4"));
            }
        }
        // conn2 (declared after fileGuard2) is disconnected by its destructor here,
        // then fileGuard2 removes the second db file.
    }

    // ---- teardown ----
    conn->Disconnect();
    ExpectTrue("Disconnect() clears IsConnected", !conn->IsConnected());

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;   // FileGuard removes the temp db file on the way out
}
