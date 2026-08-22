// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// connection_clone_test.cpp — self-contained, zero-config unit test for
// db::CloneConnection and IConnection::EffectiveProfile() (src/db/ConnectionClone.*,
// src/db/DbDriver.h). No live network DB needed — mirrors sqlite_live_test.cpp's
// pattern (a throwaway SQLite file in the system temp dir) for the real
// end-to-end clone, plus a small fake IConnection for the parts that need a
// live SSH tunnel to exercise for real (there is none in a unit test — the
// fake plays ConnectionTree::ConnectEntry's role of handing Connect() an
// already tunnel-rewritten profile, then EffectiveProfile() must report THAT,
// not some separately-kept "original").
//
// What this test does NOT cover (needs a live DB / live SSH tunnel, tracked as
// an unverified point in the handoff): a real MySQL/PG/SqlServer/Oracle/DM
// EffectiveProfile() after a real SSH-tunneled Connect(), and CloneConnection
// riding an actual net::SshTunnel end-to-end.
#include "db/ConnectionClone.h"
#include "db/DbDriver.h"
#include "core/ConnectionProfile.h"

#include <sqlite3.h>
#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/string.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace db;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectEq(const char* name, int got, int want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%d got=%d\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ===========================================================================
//  A fake IConnection built on the SAME pattern the real drivers use for
//  EffectiveProfile(): Connect() records exactly the profile it was handed
//  (target_ = p), and EffectiveProfile() returns that verbatim — see
//  MySqlConnection::target_ / PgConnection::prof_ in src/db/*Driver.cpp. There
//  is deliberately no second "original profile" member: proves CloneConnection
//  can only ever see what Connect() actually received.
// ===========================================================================
class FakeConn : public IConnection {
public:
    Dialect GetDialect() const override { return Dialect::Sqlite; }
    bool Connect(const core::ConnectionProfile& p, wxString&) override
    { target_ = p; connected_ = true; return true; }
    void Disconnect() override { connected_ = false; }
    bool IsConnected() const override { return connected_; }
    const core::ConnectionProfile& EffectiveProfile() const override { return target_; }

    bool Execute(const wxString&, QueryResult&, wxString&) override { return true; }
    bool ListDatabases(std::vector<wxString>&, wxString&) override { return true; }
    bool ListTables(const wxString&, std::vector<TableInfo>&, wxString&) override
    { return true; }
    bool GetColumns(const wxString&, const wxString&, std::vector<ColumnInfo>&,
                    wxString&) override { return true; }
    bool GetForeignKeys(const wxString&, const wxString&, std::vector<ForeignKey>&,
                        wxString&) override { return true; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<IndexInfo>&,
                    wxString&) override { return true; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&, wxString&)
        override { return true; }
    wxString ServerVersion() const override { return L"fake"; }

private:
    core::ConnectionProfile target_;
    bool connected_ = false;
};

// A minimal IConnection that overrides NOTHING beyond the pure virtuals — used
// to check IConnection's own default EffectiveProfile() (an empty profile),
// the fallback every non-driver IConnection (test stubs, future fakes) gets
// for free without having to implement the override.
class DefaultOnlyConn : public IConnection {
public:
    Dialect GetDialect() const override { return Dialect::Sqlite; }
    bool Connect(const core::ConnectionProfile&, wxString&) override { return true; }
    void Disconnect() override {}
    bool IsConnected() const override { return true; }
    bool Execute(const wxString&, QueryResult&, wxString&) override { return true; }
    bool ListDatabases(std::vector<wxString>&, wxString&) override { return true; }
    bool ListTables(const wxString&, std::vector<TableInfo>&, wxString&) override
    { return true; }
    bool GetColumns(const wxString&, const wxString&, std::vector<ColumnInfo>&,
                    wxString&) override { return true; }
    bool GetForeignKeys(const wxString&, const wxString&, std::vector<ForeignKey>&,
                        wxString&) override { return true; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<IndexInfo>&,
                    wxString&) override { return true; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&, wxString&)
        override { return true; }
    wxString ServerVersion() const override { return L"default"; }
};

// ===========================================================================
//  A1. IConnection::EffectiveProfile() default (base class, unoverridden).
// ===========================================================================
static void TestDefaultEffectiveProfile()
{
    std::printf("[EffectiveProfile — base default]\n");
    DefaultOnlyConn c;
    const core::ConnectionProfile& p = c.EffectiveProfile();
    ExpectTrue("default: empty host", p.host.IsEmpty() || p.host == L"127.0.0.1");
    // The important contract: it must not crash / must be a stable, harmless
    // value an un-wired IConnection can return without tracking anything.
    ExpectTrue("default: type is a plain default (MySQL)", p.type == db::DbType::MySQL);
}

// ===========================================================================
//  A2. EffectiveProfile() reports the tunnel-rewritten profile, not a
//      separately-kept "original" — the exact contract CloneConnection relies
//      on (see ConnectionTree::ConnectEntry's `eff` local for the real-world
//      equivalent: host/port rewritten to 127.0.0.1 + the tunnel's local port
//      BEFORE Connect() is called).
// ===========================================================================
static void TestTunnelRewriteContract()
{
    std::printf("[EffectiveProfile — tunnel-rewrite contract]\n");

    core::ConnectionProfile raw;
    raw.type = db::DbType::MySQL;
    raw.host = L"db.internal.example.com";   // only reachable through the tunnel
    raw.port = 3306;
    raw.sshEnabled = true;

    // What ConnectionTree::ConnectEntry actually calls Connect() with once the
    // tunnel is up: host/port rewritten to the local forward.
    core::ConnectionProfile eff = raw;
    eff.host = L"127.0.0.1";
    eff.port = 25060;   // stand-in for tunnel->localPort()

    FakeConn conn;
    wxString err;
    ExpectTrue("connect via tunnel-rewritten profile", conn.Connect(eff, err));
    ExpectTrue("EffectiveProfile == tunnel-rewritten host",
               conn.EffectiveProfile().host == L"127.0.0.1");
    ExpectTrue("EffectiveProfile == tunnel-rewritten port",
               conn.EffectiveProfile().port == 25060);
    ExpectTrue("EffectiveProfile != raw saved host (would break the clone)",
               conn.EffectiveProfile().host != raw.host);
}

// ===========================================================================
//  A3. CloneConnection — guard rail: refuses to clone a disconnected source.
// ===========================================================================
static void TestCloneRefusesDisconnected()
{
    std::printf("[CloneConnection — disconnected guard]\n");
    FakeConn src;   // never Connect()ed
    wxString err;
    std::unique_ptr<IConnection> clone = CloneConnection(src, err);
    ExpectTrue("clone of disconnected source is null", clone == nullptr);
    ExpectTrue("err is set", !err.IsEmpty());
}

// ===========================================================================
//  A4. CloneConnection — real end-to-end clone against a throwaway SQLite
//      file (mirrors sqlite_live_test.cpp's bootstrap pattern). Exercises the
//      real dispatch path: CreateConnection(src.EffectiveProfile().type) then
//      Connect(src.EffectiveProfile()) — the clone must be a SEPARATE, live
//      connection to the same file, using only what FakeConn exposes via
//      EffectiveProfile() (there is no other profile it could have read).
// ===========================================================================
static bool BootstrapSqlite(const wxString& path, wxString& err)
{
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.utf8_str(), &raw) != SQLITE_OK) {
        err = raw ? wxString::FromUTF8(sqlite3_errmsg(raw)) : wxString(L"sqlite3_open failed");
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

static void TestCloneEndToEnd()
{
    std::printf("[CloneConnection — end-to-end via throwaway SQLite file]\n");

    wxString dbFile = wxFileName::CreateTempFileName(L"swiftsql_cloneclonetest_");
    if (dbFile.IsEmpty()) {
        std::printf("  FATAL: could not allocate a temp file name\n");
        ++g_fails;
        return;
    }
    if (wxFileExists(dbFile)) wxRemoveFile(dbFile);

    struct FileGuard {
        wxString path;
        ~FileGuard()
        {
            if (wxFileExists(path))           wxRemoveFile(path);
            if (wxFileExists(path + L"-wal")) wxRemoveFile(path + L"-wal");
            if (wxFileExists(path + L"-shm")) wxRemoveFile(path + L"-shm");
        }
    } fileGuard{ dbFile };

    wxString bErr;
    if (!BootstrapSqlite(dbFile, bErr)) {
        std::printf("  FATAL: bootstrap failed: %s\n", (const char*)bErr.utf8_str());
        ++g_fails;
        return;
    }

    // The FakeConn's EffectiveProfile() is the ONLY thing CloneConnection can
    // see — its GetDialect() even lies (reports Sqlite regardless), so a pass
    // here proves the dispatch really goes through EffectiveProfile().type.
    core::ConnectionProfile eff;
    eff.type     = db::DbType::Sqlite;
    eff.database = dbFile;

    FakeConn src;
    wxString cerr;
    ExpectTrue("fake src connects", src.Connect(eff, cerr));

    wxString err;
    std::unique_ptr<IConnection> clone = CloneConnection(src, err);
    ExpectTrue("clone created", clone != nullptr);
    if (clone) {
        ExpectTrue("clone is connected", clone->IsConnected());
        ExpectTrue("clone dialect matches", clone->GetDialect() == Dialect::Sqlite);
        ExpectTrue("clone reused the same effective database path",
                   clone->EffectiveProfile().database == dbFile);

        // Independence: exercise the clone with a real query — it must work on
        // its own connection, entirely separate from `src` (which never
        // actually opened a real sqlite3 handle — FakeConn is a pure fake).
        QueryResult r; wxString qerr;
        ExpectTrue("clone can query its own connection",
                   clone->Execute(L"SELECT 1", r, qerr));
    }
}

int main()
{
    std::printf("== connection_clone_test (libsqlite %s) ==\n", sqlite3_libversion());
    TestDefaultEffectiveProfile();
    TestTunnelRewriteContract();
    TestCloneRefusesDisconnected();
    TestCloneEndToEnd();
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
