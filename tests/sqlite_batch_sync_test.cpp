// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sqlite_batch_sync_test.cpp — live proof that SQLite can be an INSERT target
// for data sync, i.e. that SqliteConnection::ExecuteBatch actually puts bytes
// into a target file.
//
// Zero-config and self-contained, exactly like sqlite_live_test and
// sqlite_clone_sweep_test: SQLite is an embedded file engine, so this runs
// unconditionally in CI against throwaway database files in the system temp dir.
//
// WHY THIS FILE EXISTS
// --------------------
// Until src/db/SqliteStream.cpp landed, SqliteConnection had no ExecuteBatch
// override and inherited IConnection's default, which refuses with
// "该驱动不支持批量执行". SQLite could be a sync SOURCE (it has StreamRows) and
// could take UPDATE/DELETE (those go through TableApplier::Statement, one
// rendered statement per row), but the very first buffered INSERT aborted the
// whole sweep — so a SQLite target could never be populated at all. That is the
// gap this suite pins shut. Every case below therefore drives a REAL
// SQLite->SQLite db::sync::ExecuteDataSync into an EMPTY target and then reads
// the target back through a fresh connection.
//
// WHAT EACH CASE DEFENDS (none of these is decorative)
// ---------------------------------------------------
//  [1] Batch boundary. DataSyncExec hands ExecuteBatch 500 rows at a time and
//      the driver chunks again internally. A batching off-by-one is invisible at
//      3 rows and catastrophic in production, so this case syncs 1200 rows —
//      crossing the 500-row boundary twice — and verifies the rows ON the
//      boundaries (499..502, 999..1002) individually, not just COUNT(*).
//
//  [2] NULL vs empty string vs binary. This is where the equivalent MySQL bug
//      lived: MySqlStream.cpp used to set MYSQL_BIND::buffer = nullptr for empty
//      values, and libmariadb reads a null buffer as SQL NULL no matter what
//      is_null says — 234 of 700 empty strings arrived as NULL against a live
//      MySQL 8.0.36 target, and because the diff then saw a real difference on
//      every run the sync never converged. sqlite3_bind_text(st, i, nullptr, ...)
//      has exactly the same semantics: it binds NULL, not ''. So this case
//      distinguishes NULL / '' / X'' / a blob containing embedded 0x00 in the
//      TARGET (typeof + length + hex), and then re-runs the sweep and asserts it
//      finds NOTHING to do — convergence is the property the MySQL bug broke,
//      and a value-mangling bind cannot pass it.
//
//  [3] Wide table. SQLite caps parameters per statement at
//      SQLITE_LIMIT_VARIABLE_NUMBER (999 historically, 32766 since 3.32), and
//      the driver lowers its effective batch to stay under it. With 70 columns a
//      500-row batch would need 35000 parameters, so WITHOUT the lowering
//      sqlite3_prepare_v2 fails outright with "too many SQL variables". The case
//      asserts up front that 70 * 500 really does exceed this build's limit, so
//      it can never quietly degrade into a test of nothing.
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

// ---------------------------------------------------------------------------
// throwaway-file plumbing (same shape as sqlite_clone_sweep_test)
// ---------------------------------------------------------------------------

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

// This build's per-statement parameter ceiling, read from the library rather
// than assumed — case [3]'s whole premise is that 70 * 500 exceeds it, and that
// premise has to be checked against the sqlite actually linked in.
static int VariableLimit()
{
    sqlite3* raw = nullptr;
    if (sqlite3_open(":memory:", &raw) != SQLITE_OK) { sqlite3_close(raw); return -1; }
    const int lim = sqlite3_limit(raw, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
    sqlite3_close(raw);
    return lim;
}

static core::ConnectionProfile FileProfile(const wxString& file)
{
    core::ConnectionProfile p;
    p.type     = db::DbType::Sqlite;
    p.database = file;
    return p;
}

static void Nuke(const wxString& path)
{
    if (!path.IsEmpty() && wxFileExists(path)) wxRemoveFile(path);
}

// Removes the throwaway files unconditionally, pass or fail.
struct FileGuard {
    wxString a, b;
    ~FileGuard()
    {
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

static long long ScalarOf(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return -1;
    if (r.rows.empty() || r.rows[0].empty()) return -1;
    long long v = 0;
    return r.rows[0][0].ToLongLong(&v) ? v : -1;
}

// Scalar as text. SqliteConnection renders a SQL NULL as the literal "NULL"
// (FromCol), which is fine here: every NULL/'' question below is asked through
// typeof()/length()/hex(), never by eyeballing the raw value.
static wxString TextOf(IConnection& c, const wxString& sql)
{
    QueryResult r; wxString err;
    if (!c.Execute(sql, r, err)) return L"<query-failed:" + err + L">";
    if (r.rows.empty() || r.rows[0].empty()) return L"<no-row>";
    return r.rows[0][0];
}

// ---------------------------------------------------------------------------
// One sweep, end to end, through the production path (default = real
// db::CloneConnection target-read clone, same as sqlite_clone_sweep_test).
// ---------------------------------------------------------------------------

struct SweepOutcome {
    bool      ok = false;
    bool      committed = false;
    long long inserts = 0, updates = 0, deletes = 0;
    wxString  err;
};

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

// Open a src/tgt pair of freshly bootstrapped throwaway files.
struct Pair {
    std::unique_ptr<IConnection> src, tgt;
    bool ok = false;
};

static Pair OpenPair(const wxString& srcFile, const wxString& tgtFile)
{
    Pair p;
    wxString err;
    Nuke(srcFile); Nuke(tgtFile);
    if (!Bootstrap(srcFile, err) || !Bootstrap(tgtFile, err)) {
        std::printf("  ERR  bootstrap: %s\n", (const char*)err.utf8_str());
        return p;
    }
    p.src = CreateConnection(db::DbType::Sqlite);
    p.tgt = CreateConnection(db::DbType::Sqlite);
    if (!p.src || !p.tgt ||
        !p.src->Connect(FileProfile(srcFile), err) ||
        !p.tgt->Connect(FileProfile(tgtFile), err)) {
        std::printf("  ERR  connect: %s\n", (const char*)err.utf8_str());
        return p;
    }
    p.ok = true;
    return p;
}

// ---------------------------------------------------------------------------
// [1] BATCH BOUNDARY — 1200 rows into an empty target, boundaries verified
// ---------------------------------------------------------------------------
//
// 1200 crosses DataSyncExec's 500-row buffer twice (500 | 500 | 200). The rows
// that a chunking off-by-one would drop, duplicate or transpose are the ones
// straddling each flush, so those are checked one at a time. COUNT(*) alone
// would not catch a swap, and would not catch "row 500 written twice while row
// 501 was dropped".
static void CaseBatchBoundary()
{
    const int kRows = 1200;
    std::printf("\n[1] batch boundary — %d rows, SQLite -> SQLite INSERT\n", kRows);

    const wxString srcFile = TempPath(L"swiftsql_batch_src.db");
    const wxString tgtFile = TempPath(L"swiftsql_batch_tgt.db");
    FileGuard guard{ srcFile, tgtFile };

    Pair p = OpenPair(srcFile, tgtFile);
    if (!p.ok) { ExpectTrue("open src+tgt", false); return; }

    const wxChar* kDdl =
        L"CREATE TABLE t_batch (id INTEGER PRIMARY KEY, v TEXT NOT NULL, n INTEGER)";
    if (!Exec(*p.src, kDdl, "create src") || !Exec(*p.tgt, kDdl, "create tgt")) {
        ExpectTrue("create both tables", false);
        return;
    }

    // Seed the SOURCE only. The target stays empty, so every row is an INSERT
    // and the sweep exercises nothing but the ExecuteBatch path.
    Exec(*p.src, L"BEGIN", "src begin");
    for (int i = 1; i <= kRows; ++i)
        Exec(*p.src, wxString::Format(L"INSERT INTO t_batch VALUES (%d,'row-%d',%d)",
                                      i, i, i * 7), "seed src");
    Exec(*p.src, L"COMMIT", "src commit");

    // Non-vacuity: the target really is empty going in, so a "committed" report
    // that wrote nothing cannot pass the assertions below.
    ExpectEq("target empty before sweep", ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_batch"), 0);

    const SweepOutcome x = RunSweep(*p.src, *p.tgt, L"t_batch");
    std::printf("  OBS  ok=%d committed=%d ins=%lld upd=%lld del=%lld\n",
                (int)x.ok, (int)x.committed, x.inserts, x.updates, x.deletes);
    if (!x.err.IsEmpty())
        std::printf("  OBS  error text: \"%s\"\n", (const char*)x.err.utf8_str());

    ExpectTrue("sweep returned ok", x.ok);
    ExpectTrue("table committed", x.committed);
    ExpectEq("reported inserts == source rows", x.inserts, kRows);
    ExpectEq("reported updates == 0", x.updates, 0);
    ExpectEq("reported deletes == 0", x.deletes, 0);

    // The rows actually landed.
    ExpectEq("target row count", ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_batch"), kRows);
    // No gaps and no duplicates anywhere: min/max/distinct pin the whole range,
    // and the payload check pins that every row kept ITS OWN values (a
    // transposition would leave the count right and this wrong).
    ExpectEq("min id", ScalarOf(*p.tgt, L"SELECT MIN(id) FROM t_batch"), 1);
    ExpectEq("max id", ScalarOf(*p.tgt, L"SELECT MAX(id) FROM t_batch"), kRows);
    ExpectEq("distinct ids", ScalarOf(*p.tgt, L"SELECT COUNT(DISTINCT id) FROM t_batch"), kRows);
    ExpectEq("every row's payload matches its own id",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_batch "
                              L"WHERE v = 'row-' || id AND n = id * 7"), kRows);

    // The boundary rows, individually — the whole reason for 1200 rows.
    for (int id : { 1, 499, 500, 501, 502, 999, 1000, 1001, 1002, 1200 }) {
        const wxString got =
            TextOf(*p.tgt, wxString::Format(L"SELECT v FROM t_batch WHERE id=%d", id));
        ExpectStr(wxString::Format(L"boundary row id=%d", id).utf8_str().data(),
                  got, wxString::Format(L"row-%d", id));
    }

    // Integer affinity survived the bind: values go out as bound TEXT and SQLite
    // applies the column's INTEGER affinity, so the target must store integers,
    // not the strings "7", "14", ... A stored-as-text id would still compare
    // equal above but sorts and joins differently forever after.
    ExpectEq("id stored with INTEGER affinity",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_batch WHERE typeof(id)='integer'"), kRows);
    ExpectEq("n stored with INTEGER affinity",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_batch WHERE typeof(n)='integer'"), kRows);

    // Convergence: a second sweep must find NOTHING left to do.
    const SweepOutcome y = RunSweep(*p.src, *p.tgt, L"t_batch");
    std::printf("  OBS  re-sweep ok=%d ins=%lld upd=%lld del=%lld\n",
                (int)y.ok, y.inserts, y.updates, y.deletes);
    ExpectTrue("re-sweep ok", y.ok);
    ExpectEq("re-sweep inserts 0", y.inserts, 0);
    ExpectEq("re-sweep updates 0", y.updates, 0);
    ExpectEq("re-sweep deletes 0", y.deletes, 0);
}

// ---------------------------------------------------------------------------
// [2] NULL vs EMPTY STRING vs BINARY — the MySQL-bug shape
// ---------------------------------------------------------------------------
static void CaseNullEmptyBinary()
{
    std::printf("\n[2] NULL vs '' vs blob — the bind-fidelity case\n");

    const wxString srcFile = TempPath(L"swiftsql_batchtypes_src.db");
    const wxString tgtFile = TempPath(L"swiftsql_batchtypes_tgt.db");
    FileGuard guard{ srcFile, tgtFile };

    Pair p = OpenPair(srcFile, tgtFile);
    if (!p.ok) { ExpectTrue("open src+tgt", false); return; }

    // t and b are NULLABLE on purpose: if an empty value were mangled into NULL
    // the INSERT would still SUCCEED here, and the corruption would be silent —
    // which is exactly how the MySQL bug survived. A NOT NULL column would have
    // failed the batch loudly and made this case much weaker.
    const wxChar* kDdl =
        L"CREATE TABLE t_types (id INTEGER PRIMARY KEY, t TEXT, b BLOB, r REAL)";
    if (!Exec(*p.src, kDdl, "create src") || !Exec(*p.tgt, kDdl, "create tgt")) {
        ExpectTrue("create both tables", false);
        return;
    }

    // id=1 NULL text / NULL blob
    // id=2 EMPTY text / EMPTY blob        <- the two the MySQL bug destroyed
    // id=3 ordinary text / ordinary blob
    // id=4 blob with an EMBEDDED 0x00 and a trailing 0x00 (a strlen-based bind
    //      truncates this to zero bytes; a hex round-trip that drops leading
    //      zeros mangles it)
    // id=5 text that is the four characters N,U,L,L — must NOT come back as a
    //      SQL NULL, which is the stringly-typed version of the same bug
    // id=6 text with a quote, a backslash and CJK — the escaping class that
    //      parameter binding exists to eliminate
    const wxChar* kSeed[] = {
        L"INSERT INTO t_types VALUES (1, NULL, NULL, NULL)",
        L"INSERT INTO t_types VALUES (2, '', X'', 0.0)",
        L"INSERT INTO t_types VALUES (3, 'hello', X'DEADBEEF', 1.5)",
        L"INSERT INTO t_types VALUES (4, 'nul-blob', X'00FF00010200', -2.25)",
        L"INSERT INTO t_types VALUES (5, 'NULL', X'0A', 3.0)",
        L"INSERT INTO t_types VALUES (6, 'he''s \\ 中文', X'C3A9', 42.0)",
    };
    Exec(*p.src, L"BEGIN", "src begin");
    for (const wxChar* s : kSeed) Exec(*p.src, s, "seed src");
    Exec(*p.src, L"COMMIT", "src commit");

    ExpectEq("target empty before sweep", ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types"), 0);

    const SweepOutcome x = RunSweep(*p.src, *p.tgt, L"t_types");
    std::printf("  OBS  ok=%d committed=%d ins=%lld\n", (int)x.ok, (int)x.committed, x.inserts);
    if (!x.err.IsEmpty())
        std::printf("  OBS  error text: \"%s\"\n", (const char*)x.err.utf8_str());
    ExpectTrue("sweep returned ok", x.ok);
    ExpectEq("inserted all 6 rows", x.inserts, 6);

    // ---- NULL stays NULL ---------------------------------------------------
    ExpectStr("id=1 t is SQL NULL",  TextOf(*p.tgt, L"SELECT typeof(t) FROM t_types WHERE id=1"), L"null");
    ExpectStr("id=1 b is SQL NULL",  TextOf(*p.tgt, L"SELECT typeof(b) FROM t_types WHERE id=1"), L"null");
    ExpectStr("id=1 r is SQL NULL",  TextOf(*p.tgt, L"SELECT typeof(r) FROM t_types WHERE id=1"), L"null");

    // ---- EMPTY stays EMPTY, and is NOT NULL --------------------------------
    // This pair of assertions is the whole point of the case. sqlite3_bind_text
    // with a null pointer binds SQL NULL, so a "if empty then nullptr" bind
    // would flip typeof from 'text' to 'null' right here.
    ExpectStr("id=2 t is TEXT (not null)", TextOf(*p.tgt, L"SELECT typeof(t) FROM t_types WHERE id=2"), L"text");
    ExpectEq ("id=2 t has length 0",       ScalarOf(*p.tgt, L"SELECT length(t) FROM t_types WHERE id=2"), 0);
    ExpectEq ("id=2 t IS NOT NULL",        ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE id=2 AND t IS NOT NULL"), 1);
    ExpectEq ("id=2 t = ''",               ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE id=2 AND t = ''"), 1);
    ExpectStr("id=2 b is BLOB (not null)", TextOf(*p.tgt, L"SELECT typeof(b) FROM t_types WHERE id=2"), L"blob");
    ExpectEq ("id=2 b has length 0",       ScalarOf(*p.tgt, L"SELECT length(b) FROM t_types WHERE id=2"), 0);
    ExpectEq ("id=2 b IS NOT NULL",        ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE id=2 AND b IS NOT NULL"), 1);

    // NULL and '' are DISTINGUISHABLE in the target — one of each, not two of
    // one. If the empty row had been mangled this count would read 2.
    ExpectEq("exactly one NULL t across the table",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE t IS NULL"), 1);
    ExpectEq("exactly one NULL b across the table",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE b IS NULL"), 1);

    // ---- binary is byte-exact ---------------------------------------------
    ExpectStr("id=3 blob byte-exact", TextOf(*p.tgt, L"SELECT hex(b) FROM t_types WHERE id=3"), L"DEADBEEF");
    ExpectStr("id=4 blob byte-exact (embedded + trailing 0x00)",
              TextOf(*p.tgt, L"SELECT hex(b) FROM t_types WHERE id=4"), L"00FF00010200");
    ExpectEq ("id=4 blob length is 6 (no strlen truncation)",
              ScalarOf(*p.tgt, L"SELECT length(b) FROM t_types WHERE id=4"), 6);
    ExpectStr("id=6 blob byte-exact", TextOf(*p.tgt, L"SELECT hex(b) FROM t_types WHERE id=6"), L"C3A9");
    // ...and a blob is stored as a BLOB, not as the hex TEXT that Cell carries.
    ExpectEq("all non-null b are typeof blob",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE b IS NOT NULL AND typeof(b)='blob'"), 5);

    // ---- text fidelity -----------------------------------------------------
    ExpectStr("id=3 text", TextOf(*p.tgt, L"SELECT t FROM t_types WHERE id=3"), L"hello");
    ExpectStr("id=5 the literal string 'NULL' is text, not SQL NULL",
              TextOf(*p.tgt, L"SELECT typeof(t) FROM t_types WHERE id=5"), L"text");
    ExpectEq ("id=5 text is the 4 characters NULL",
              ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE id=5 AND t='NULL'"), 1);
    ExpectEq ("id=6 quote/backslash/CJK survived binding",
              ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE id=6 AND t='he''s \\ 中文'"), 1);

    // ---- REAL affinity -----------------------------------------------------
    ExpectEq("non-null r stored with REAL affinity",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE r IS NOT NULL AND typeof(r)='real'"), 5);
    ExpectEq("id=4 negative real round-tripped",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_types WHERE id=4 AND r = -2.25"), 1);

    // ---- convergence -------------------------------------------------------
    // The MySQL bug's real-world symptom was non-convergence: the diff saw a
    // difference every run and re-issued the same phantom UPDATEs forever. Any
    // value that did not round-trip byte-for-byte shows up here as work.
    const SweepOutcome y = RunSweep(*p.src, *p.tgt, L"t_types");
    std::printf("  OBS  re-sweep ok=%d ins=%lld upd=%lld del=%lld\n",
                (int)y.ok, y.inserts, y.updates, y.deletes);
    ExpectTrue("re-sweep ok", y.ok);
    ExpectEq("re-sweep finds no inserts", y.inserts, 0);
    ExpectEq("re-sweep finds no updates (sync converged)", y.updates, 0);
    ExpectEq("re-sweep finds no deletes", y.deletes, 0);
}

// ---------------------------------------------------------------------------
// [3] WIDE TABLE — forces the variable-limit path to lower the batch
// ---------------------------------------------------------------------------
static void CaseWideTable()
{
    const int kCols = 70;    // + the id column = 71 bound parameters per row
    const int kRows = 600;   // > 500, so DataSyncExec flushes twice as well

    const int lim = VariableLimit();
    std::printf("\n[3] wide table — %d cols x %d rows "
                "(SQLITE_LIMIT_VARIABLE_NUMBER = %d)\n", kCols + 1, kRows, lim);

    // NON-VACUITY GATE. This case only tests the lowering path if a full
    // 500-row batch would genuinely overrun the limit. If a future sqlite
    // raises the ceiling past 35500, this fails loudly and demands new numbers
    // rather than silently becoming a plain wide-table test.
    ExpectTrue("500-row batch of this width really would exceed the limit "
               "(so the lowering path is under test)",
               lim > 0 && (kCols + 1) * 500 > lim);
    std::printf("  OBS  a 500-row batch would need %d parameters\n", (kCols + 1) * 500);

    const wxString srcFile = TempPath(L"swiftsql_batchwide_src.db");
    const wxString tgtFile = TempPath(L"swiftsql_batchwide_tgt.db");
    FileGuard guard{ srcFile, tgtFile };

    Pair p = OpenPair(srcFile, tgtFile);
    if (!p.ok) { ExpectTrue("open src+tgt", false); return; }

    wxString ddl = L"CREATE TABLE t_wide (id INTEGER PRIMARY KEY";
    for (int c = 0; c < kCols; ++c) ddl += wxString::Format(L", c%d TEXT", c);
    ddl += L")";
    if (!Exec(*p.src, ddl, "create src") || !Exec(*p.tgt, ddl, "create tgt")) {
        ExpectTrue("create both tables", false);
        return;
    }

    Exec(*p.src, L"BEGIN", "src begin");
    for (int i = 1; i <= kRows; ++i) {
        wxString ins = wxString::Format(L"INSERT INTO t_wide VALUES (%d", i);
        for (int c = 0; c < kCols; ++c) ins += wxString::Format(L", 'v%d-%d'", i, c);
        ins += L")";
        Exec(*p.src, ins, "seed src");
    }
    Exec(*p.src, L"COMMIT", "src commit");

    ExpectEq("target empty before sweep", ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_wide"), 0);

    const SweepOutcome x = RunSweep(*p.src, *p.tgt, L"t_wide");
    std::printf("  OBS  ok=%d committed=%d ins=%lld\n", (int)x.ok, (int)x.committed, x.inserts);
    if (!x.err.IsEmpty())
        std::printf("  OBS  error text: \"%s\"\n", (const char*)x.err.utf8_str());

    // WITHOUT the lowering this is where it dies: sqlite3_prepare_v2 rejects the
    // statement with "too many SQL variables" and the sweep reports failure.
    ExpectTrue("sweep returned ok (batch was lowered under the variable limit)", x.ok);
    ExpectTrue("table committed", x.committed);
    ExpectEq("inserted every row", x.inserts, kRows);
    ExpectEq("target row count", ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_wide"), kRows);
    ExpectEq("distinct ids", ScalarOf(*p.tgt, L"SELECT COUNT(DISTINCT id) FROM t_wide"), kRows);

    // Row content around the LOWERED chunk boundary. The effective chunk is
    // floor(limit / 71) — 461 for a 32766 limit — so these bracket the internal
    // split as well as the 500-row DataSyncExec flush.
    const int chunk = lim / (kCols + 1);
    std::printf("  OBS  effective chunk would be %d rows\n", chunk);
    for (int id : { 1, chunk - 1, chunk, chunk + 1, 499, 500, 501, kRows }) {
        if (id < 1 || id > kRows) continue;
        const wxString got = TextOf(*p.tgt,
            wxString::Format(L"SELECT c0 || '|' || c69 FROM t_wide WHERE id=%d", id));
        ExpectStr(wxString::Format(L"wide row id=%d first+last column", id).utf8_str().data(),
                  got, wxString::Format(L"v%d-0|v%d-69", id, id));
    }

    // Nothing transposed anywhere across all 70 columns of all 600 rows.
    ExpectEq("every wide row's first column matches its id",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_wide WHERE c0 = 'v' || id || '-0'"), kRows);
    ExpectEq("every wide row's last column matches its id",
             ScalarOf(*p.tgt, L"SELECT COUNT(*) FROM t_wide WHERE c69 = 'v' || id || '-69'"), kRows);

    const SweepOutcome y = RunSweep(*p.src, *p.tgt, L"t_wide");
    std::printf("  OBS  re-sweep ok=%d ins=%lld upd=%lld del=%lld\n",
                (int)y.ok, y.inserts, y.updates, y.deletes);
    ExpectTrue("re-sweep ok", y.ok);
    ExpectEq("re-sweep finds nothing to do",
             y.inserts + y.updates + y.deletes, 0);
}

int main()
{
    std::printf("== sqlite_batch_sync_test (libsqlite %s) ==\n", sqlite3_libversion());

    CaseBatchBoundary();
    CaseNullEmptyBinary();
    CaseWideTable();

    std::printf("\n== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
