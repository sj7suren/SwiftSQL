// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sqlite_clone_sweep_test.cpp — PROBE/REGRESSION for the target-read clone on
// SQLite.
//
// Zero-config and self-contained, exactly like sqlite_live_test: SQLite is an
// embedded file engine, so this runs unconditionally in CI against throwaway
// database files in the system temp dir.
//
// WHY THIS FILE EXISTS
// --------------------
// RunTableSweep (src/db/DataSyncExec.cpp) opens a SECOND connection to the
// target, via db::CloneConnection, and reads the target through it while the
// writes go through the original. On MySQL/PostgreSQL that is load-bearing: one
// client-library connection used from two threads (the RowCursor's producer
// thread draining a result set, this thread issuing writes) is undefined
// behavior, observed live as "Commands out of sync" and as a segfault.
//
// SQLite is not a client/server engine. A "connection" is a file handle, and
// two handles on one file contend through the OS file-locking ladder
// (SHARED -> RESERVED -> PENDING -> EXCLUSIVE) rather than through a wire
// protocol. That is a DIFFERENT hazard, and it is the one this file pins.
//
// DRIVER FACTS (read off src/db/SqliteDriver.cpp, not assumed):
//   * Connect() opens with sqlite3_open_v2(..., SQLITE_OPEN_READWRITE, nullptr)
//     — no CREATE, no shared cache, no custom VFS.
//   * The ONLY pragma issued at open is `PRAGMA foreign_keys=ON`. In
//     particular there is NO `journal_mode=WAL` and NO sqlite3_busy_timeout(),
//     so the busy handler is SQLite's default: NONE. A lock conflict fails
//     IMMEDIATELY with SQLITE_BUSY ("database is locked") — it does not wait.
//   * StreamRows() is a genuine incremental cursor (one sqlite3_step per row)
//     and DataSync's RowCursor requests opt.limit = -1, i.e. ONE unchunked
//     statement for the whole table. So the read handle holds its SHARED lock
//     for the ENTIRE scan, concurrently with the writes.
//
// So the question this file answers empirically is: does the writer, holding
// RESERVED while a reader holds SHARED, ever need to escalate to EXCLUSIVE
// before the reader lets go?
#include "db/DataSyncExec.h"
#include "db/DbDriver.h"
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

static bool Exec(IConnection& c, const wxString& sql, const char* label)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) {
        std::printf("  ERR  %s: %s\n", label, (const char*)err.utf8_str());
        return false;
    }
    return true;
}

// Bootstrap a valid database file with the raw C API (default flags include
// CREATE); the driver itself opens READWRITE-only and cannot create one.
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

static core::ConnectionProfile FileProfile(const wxString& file)
{
    core::ConnectionProfile p;
    p.type     = db::DbType::Sqlite;
    p.database = file;
    return p;
}

// Delete a file if present; used by the RAII guard below.
static void Nuke(const wxString& path)
{
    if (!path.IsEmpty() && wxFileExists(path)) wxRemoveFile(path);
}

// Removes the throwaway files unconditionally, pass or fail.
struct FileGuard {
    wxString a, b;
    ~FileGuard()
    {
        // Journal siblings too: a crashed run can leave -journal/-wal behind.
        for (const wxString& p : { a, b }) {
            Nuke(p);
            Nuke(p + L"-journal");
            Nuke(p + L"-wal");
            Nuke(p + L"-shm");
        }
    }
};

static wxString TempPath(const wxChar* stem)
{
    wxFileName fn(wxFileName::GetTempDir(), stem);
    return fn.GetFullPath();
}

// A payload wide enough that N rows of it exceed SQLite's default page cache
// (cache_size = -2000, i.e. 2 MiB). Cache pressure is precisely what forces a
// mid-transaction spill, which is the only thing that makes a writer holding
// RESERVED try to escalate to EXCLUSIVE before it commits.
static wxString Payload(int i, int width)
{
    wxString s;
    s.Alloc(width + 16);
    s << i << L"-";
    while ((int)s.length() < width) s << L"x";
    return s;
}

// ---------------------------------------------------------------------------
// One sweep, end to end, through the production path.
// ---------------------------------------------------------------------------

struct SweepOutcome {
    bool     ok = false;
    bool     committed = false;
    long long inserts = 0, updates = 0, deletes = 0;
    wxString err;
};

// Build the spec + job and run ExecuteDataSync with the DEFAULT opener — i.e.
// production, i.e. db::CloneConnection. That default is the whole point: an
// injected opener would test a stub instead of the real second file handle.
static SweepOutcome RunSweep(IConnection& src, IConnection& tgt,
                             const wxString& table)
{
    SweepOutcome out;

    DataDiffSpec spec;
    wxString e1, e2;
    if (!src.GetTableSchema(wxString(), table, spec.srcSchema, e1) ||
        !tgt.GetTableSchema(wxString(), table, spec.tgtSchema, e2)) {
        out.err = e1.IsEmpty() ? e2 : e1;
        return out;
    }

    TableDataSpec allow(spec.SourceTable().Key());
    allow.EnableInserts();
    allow.EnableUpdates();
    if (auto tok = AuthorizeDeletes(true)) allow.EnableDeletes(*tok);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, allow});

    DataSyncOptions   base;
    DataExecResult    result;
    std::atomic<bool> stop{false};
    wxString          err;

    out.ok = ExecuteDataSync(src, tgt, jobs, base, result, err, stop,
                             DataExecProgress());
    out.err = err;
    for (const TableExecReport& r : result.tables) {
        if (r.table != spec.SourceTable().Key()) continue;
        out.committed = r.committed;
        out.inserts   = r.inserts;
        out.updates   = r.updates;
        out.deletes   = r.deletes;
    }
    return out;
}

// Count rows, and count rows whose value already matches the source's, so a
// "committed" report that wrote nothing cannot pass.
static long long ScalarOf(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return -1;
    if (r.rows.empty() || r.rows[0].empty()) return -1;
    long long v = 0;
    return r.rows[0][0].ToLongLong(&v) ? v : -1;
}

// ---------------------------------------------------------------------------
// The scenario: a target that ALREADY HOLDS ROWS, and a diff whose FIRST
// classified row is a write. That is the exact shape that broke the server
// engines — the target read stream is still mid-flight when the first write is
// issued — and it is the shape that puts a SQLite writer's RESERVED lock in
// contention with the reader's SHARED lock.
// ---------------------------------------------------------------------------
static void RunCase(const wxChar* label, int rows, int width)
{
    std::printf("\n[SQLite target-read clone: %ls  rows=%d width=%d]\n",
                label, rows, width);

    const wxString srcFile = TempPath(L"swiftsql_clone_src.db");
    const wxString tgtFile = TempPath(L"swiftsql_clone_tgt.db");
    FileGuard guard{ srcFile, tgtFile };
    Nuke(srcFile); Nuke(tgtFile);

    wxString err;
    if (!Bootstrap(srcFile, err) || !Bootstrap(tgtFile, err)) {
        ExpectTrue("bootstrap throwaway db files", false);
        return;
    }

    std::unique_ptr<IConnection> src = CreateConnection(db::DbType::Sqlite);
    std::unique_ptr<IConnection> tgt = CreateConnection(db::DbType::Sqlite);
    if (!src || !tgt ||
        !src->Connect(FileProfile(srcFile), err) ||
        !tgt->Connect(FileProfile(tgtFile), err)) {
        std::printf("  ERR  connect: %s\n", (const char*)err.utf8_str());
        ExpectTrue("connect both throwaway databases", false);
        return;
    }

    const wxChar* kDdl =
        L"CREATE TABLE t_sweep (id INTEGER PRIMARY KEY, v TEXT NOT NULL)";
    if (!Exec(*src, kDdl, "create src") || !Exec(*tgt, kDdl, "create tgt")) {
        ExpectTrue("create both tables", false);
        return;
    }

    // Seed. Source rows 1..rows carry the NEW payload; the target carries the
    // OLD payload for the SAME keys, so every key is an UPDATE and the very
    // first key the merge classifies (id=1) is already a write — the target read
    // stream is still mid-flight when that first write goes out. The target also
    // holds one extra key (rows+1) so the DELETE sweep has work.
    //
    // DELIBERATELY NO INSERT IN THIS SCENARIO. SqliteConnection does not
    // override IConnection::ExecuteBatch, so it inherits the base default that
    // refuses with "该驱动不支持批量执行" — SQLite cannot be an INSERT target at
    // all today. That is a real, pre-existing gap (see this file's report), but
    // it is NOT the clone hazard, and letting it fire here would abort the sweep
    // at the first buffered insert and mask the locking question this file
    // exists to answer. UPDATE and DELETE go through TableApplier::Statement(),
    // one statement per row, which needs no batch support — and is the path that
    // actually stresses the lock ladder, since it issues thousands of writes
    // while the read cursor on the second handle stays open.
    Exec(*src, L"BEGIN", "src begin");
    Exec(*tgt, L"BEGIN", "tgt begin");
    for (int i = 1; i <= rows; ++i) {
        Exec(*src, wxString::Format(L"INSERT INTO t_sweep VALUES (%d,'%s')",
                                    i, Payload(i, width)), "seed src");
        Exec(*tgt, wxString::Format(L"INSERT INTO t_sweep VALUES (%d,'OLD-%s')",
                                    i, Payload(i, width)), "seed tgt");
    }
    Exec(*tgt, wxString::Format(L"INSERT INTO t_sweep VALUES (%d,'%s')",
                                rows + 1, Payload(rows + 1, width)),
         "seed tgt extra");
    Exec(*src, L"COMMIT", "src commit");
    Exec(*tgt, L"COMMIT", "tgt commit");

    // Sanity: the target really does already hold rows. If this were 0 the whole
    // scenario would collapse into the create-and-fill case, which skips the
    // clone entirely and would make every assertion below vacuous.
    const long long before = ScalarOf(*tgt, L"SELECT COUNT(*) FROM t_sweep");
    std::printf("  OBS  target rows before sweep: %lld\n", before);
    ExpectEq("target is non-empty before the sweep (scenario is not vacuous)",
             before, rows + 1);
    // And it holds the OLD payload everywhere, so every one of the `rows`
    // updates below is a REAL write rather than a no-op the merge would skip.
    ExpectEq("every target row is stale before the sweep",
             ScalarOf(*tgt, L"SELECT COUNT(*) FROM t_sweep WHERE v LIKE 'OLD-%'"),
             rows);

    const SweepOutcome x = RunSweep(*src, *tgt, L"t_sweep");
    std::printf("  OBS  ExecuteDataSync ok=%d committed=%d ins=%lld upd=%lld del=%lld\n",
                (int)x.ok, (int)x.committed, x.inserts, x.updates, x.deletes);
    if (!x.err.IsEmpty())
        std::printf("  OBS  error text: \"%s\"\n", (const char*)x.err.utf8_str());

    ExpectTrue("sweep returned ok", x.ok);
    ExpectTrue("table committed", x.committed);
    ExpectEq("applied N updates", x.updates, rows);
    ExpectEq("applied 1 delete",  x.deletes, 1);
    ExpectEq("applied 0 inserts (scenario has none)", x.inserts, 0);

    // The data actually landed: the extra target row is gone and NO row still
    // carries the OLD payload. Without these two the test would pass on a sweep
    // that reported success and wrote nothing.
    const long long after = ScalarOf(*tgt, L"SELECT COUNT(*) FROM t_sweep");
    const long long stale =
        ScalarOf(*tgt, L"SELECT COUNT(*) FROM t_sweep WHERE v LIKE 'OLD-%'");
    std::printf("  OBS  target rows after sweep: %lld (stale OLD rows: %lld)\n",
                after, stale);
    ExpectEq("target row count matches source", after, rows);
    ExpectEq("no stale OLD rows remain", stale, 0);
}

// ---------------------------------------------------------------------------
// NEGATIVE CONTROL — proves the two cases above are not vacuous.
// ---------------------------------------------------------------------------
//
// The passing cases assert that a sweep against a rows-already-present SQLite
// target commits. That assertion is only worth something if a commit CAN fail
// here — otherwise it would pass against any implementation, including one that
// held the target read cursor open across the COMMIT.
//
// So this case pins the failure mode directly. It parks an INDEPENDENT third
// handle on the target file with a live, un-finalized SELECT cursor — i.e.
// exactly the state `tgtRead` would be in if the target read outlived the scan
// — and then runs the same sweep. Measured against the raw sqlite3 API with the
// driver's own open flags: a writer holding RESERVED may issue unlimited DML
// while another handle holds SHARED (20000 updates spilling well past the 2 MiB
// page cache all succeeded), but the COMMIT must escalate to EXCLUSIVE, and
// with no busy timeout configured that fails INSTANTLY with SQLITE_BUSY
// ("database is locked") rather than waiting.
//
// If this case ever stops failing, the assertions above have stopped meaning
// anything and this file needs rewriting, not deleting.
static void RunNegativeControl()
{
    std::printf("\n[negative control: a reader that outlives the scan]\n");

    const wxString srcFile = TempPath(L"swiftsql_clone_nc_src.db");
    const wxString tgtFile = TempPath(L"swiftsql_clone_nc_tgt.db");
    FileGuard guard{ srcFile, tgtFile };
    Nuke(srcFile); Nuke(tgtFile);

    wxString err;
    if (!Bootstrap(srcFile, err) || !Bootstrap(tgtFile, err)) {
        ExpectTrue("negative control bootstrap", false);
        return;
    }

    std::unique_ptr<IConnection> src = CreateConnection(db::DbType::Sqlite);
    std::unique_ptr<IConnection> tgt = CreateConnection(db::DbType::Sqlite);
    if (!src || !tgt ||
        !src->Connect(FileProfile(srcFile), err) ||
        !tgt->Connect(FileProfile(tgtFile), err)) {
        ExpectTrue("negative control connect", false);
        return;
    }

    const wxChar* kDdl =
        L"CREATE TABLE t_sweep (id INTEGER PRIMARY KEY, v TEXT NOT NULL)";
    Exec(*src, kDdl, "create src"); Exec(*tgt, kDdl, "create tgt");
    for (int i = 1; i <= 20; ++i) {
        Exec(*src, wxString::Format(L"INSERT INTO t_sweep VALUES (%d,'%s')",
                                    i, Payload(i, 32)), "seed src");
        Exec(*tgt, wxString::Format(L"INSERT INTO t_sweep VALUES (%d,'OLD-%s')",
                                    i, Payload(i, 32)), "seed tgt");
    }

    // The squatter: a separate handle, opened with the SAME flags the driver
    // uses, holding a stepped-but-unfinalized cursor => a SHARED lock that never
    // goes away on its own.
    sqlite3* squat = nullptr;
    const int orc = sqlite3_open_v2(tgtFile.utf8_str(), &squat,
                                    SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_stmt* st = nullptr;
    bool parked = false;
    if (orc == SQLITE_OK &&
        sqlite3_prepare_v2(squat, "SELECT id, v FROM t_sweep ORDER BY id",
                           -1, &st, nullptr) == SQLITE_OK) {
        parked = (sqlite3_step(st) == SQLITE_ROW);
    }
    ExpectTrue("negative control parked a live reader on the target", parked);

    const SweepOutcome x = RunSweep(*src, *tgt, L"t_sweep");
    std::printf("  OBS  with a lingering reader: ok=%d committed=%d err=\"%s\"\n",
                (int)x.ok, (int)x.committed, (const char*)x.err.utf8_str());

    // THE POINT: this must fail, and it must fail at the lock, not somewhere
    // incidental. `database is locked` is SQLite's SQLITE_BUSY text.
    ExpectTrue("a reader outliving the scan DOES break the sweep", !x.ok);
    ExpectTrue("...and it breaks with SQLITE_BUSY (database is locked)",
               x.err.Lower().Contains(L"database is locked"));
    ExpectTrue("...and the table is reported as NOT committed", !x.committed);

    // Nothing landed: the transaction rolled back, so the target is untouched.
    const long long stale =
        ScalarOf(*tgt, L"SELECT COUNT(*) FROM t_sweep WHERE v LIKE 'OLD-%'");
    std::printf("  OBS  target still holds %lld stale rows (rollback intact)\n", stale);
    ExpectEq("the failed sweep left the target unchanged", stale, 20);

    if (st) sqlite3_finalize(st);
    if (squat) sqlite3_close(squat);

    // And once the squatter is gone, the very same sweep succeeds — proving the
    // lock was the whole cause and nothing else in the scenario was broken.
    const SweepOutcome y = RunSweep(*src, *tgt, L"t_sweep");
    std::printf("  OBS  after releasing the reader: ok=%d committed=%d upd=%lld\n",
                (int)y.ok, (int)y.committed, y.updates);
    ExpectTrue("the identical sweep succeeds once the reader is released", y.ok);
    ExpectEq("...and applies every update", y.updates, 20);
    ExpectEq("...leaving no stale rows",
             ScalarOf(*tgt, L"SELECT COUNT(*) FROM t_sweep WHERE v LIKE 'OLD-%'"), 0);
}

int main()
{
    std::printf("=== sqlite_clone_sweep_test ===\n");
    std::printf("Driver opens SQLITE_OPEN_READWRITE, no WAL, no busy timeout:\n"
                "a lock conflict therefore fails IMMEDIATELY, it does not wait.\n");

    // Small: everything the writer dirties fits in the default page cache, so
    // it never needs to escalate past RESERVED while the reader holds SHARED.
    RunCase(L"small (fits in page cache)", 50, 32);

    // Large: several MiB of dirty pages against a 2 MiB default cache, which
    // forces a mid-transaction spill to the rollback journal. That spill is the
    // moment the writer must escalate, and it happens while the read cursor is
    // still open on the second handle.
    RunCase(L"large (forces mid-transaction cache spill)", 20000, 200);

    // Without this the two cases above would pass against an implementation
    // that never released the target read cursor. See its comment.
    RunNegativeControl();

    std::printf("\n=== %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
