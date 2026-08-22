// mysql_pg_live_exec.cpp — env-gated LIVE EXECUTE/WRITE-PATH integration test.
//
// WHY THIS FILE EXISTS. Data sync is held at Beta because it is the only
// feature in this program that WRITES, and the write path had never been
// observed doing so. The compare suite (mysql_pg_live_test) is 70 green checks
// of READING. db::sync::SyncEngine::Execute, db::sync::ExecuteDataSync and
// IConnection::ExecuteBatch — the three components that actually put bytes into
// a customer's database — had never run against a real server at all. Every
// claim about them ("inserts land", "500 rows per batch", "a failing table
// rolls back", "the delete master switch means no DELETE reaches the target")
// was an argument about source code.
//
// Writing features earn GA on observed behaviour, not on argument. So every
// assertion in this suite and its sibling (mysql_pg_live_exec2.cpp) READS THE
// TARGET BACK and compares its actual contents. `Execute() returned true` is
// never accepted as evidence: "wrote nothing", "wrote once correctly", "wrote
// twice" and "wrote the wrong values" all return true, and only the target's
// own rows tell them apart.
//
// Items covered here: A (the basic write is correct), B (ExecuteBatch:
// parameterized, coerced, NULL-preserving, correct across the 500-row batch
// boundary), F (unconvertible values refused AT THE TARGET), G (identity/
// sequence repair). Items C-E and H live in mysql_pg_live_exec2.cpp, purely to
// keep both TUs under the charter's 1000-line ceiling.
//
// SAFETY: no password/host is hard-coded; everything comes from the same
// environment variables mysql_pg_live_test.cpp documents. Unset → SKIP, exit 0.
// The only databases touched carry the `swiftsql_xexec_` prefix and are dropped
// on the way in AND out:
//   MySQL  swiftsql_xexec_src   (source)
//   MySQL  swiftsql_xexec_my2   (same-engine MySQL→MySQL target)
//   PG     swiftsql_xexec_tgt   (cross-engine target)
//   PG     swiftsql_xexec_pg2   (same-engine PG→PG target)
#include "mysql_pg_live_exec.h"

#include <memory>

using namespace mpexec;

namespace {

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// t_basic — item A. Source has 3 rows; target starts with one row that matches,
// one that differs in a non-key column, one that differs in another, and one
// extra row that exists only on the target. That is one of each category
// (insert / update / delete) plus an untouched row, which is the minimum that
// can distinguish "applied the right change to the right row" from "rewrote the
// whole table".
bool SetupBasicMySql(IConnection& c, const wxString& db)
{
    const wxString q = QuoteIdent(db, Dialect::MySQL);
    return mplive::Exec(c, L"DROP TABLE IF EXISTS " + q + L".t_basic", "drop t_basic") &&
           mplive::Exec(c,
               L"CREATE TABLE " + q + L".t_basic ("
               L"  id INT NOT NULL PRIMARY KEY,"
               L"  name VARCHAR(50) NOT NULL,"
               L"  v INT NOT NULL) ENGINE=InnoDB", "create t_basic") &&
           mplive::Exec(c,
               L"INSERT INTO " + q + L".t_basic (id,name,v) VALUES "
               L"(1,'alpha',10),(2,'beta',20),(3,'gamma',30),(5,'epsilon',50)",
               "seed t_basic");
}

bool SetupBasicPg(IConnection& c)
{
    return mplive::Exec(c, L"DROP TABLE IF EXISTS t_basic", "drop pg t_basic") &&
           mplive::Exec(c,
               L"CREATE TABLE t_basic ("
               L"  id integer NOT NULL PRIMARY KEY,"
               L"  name varchar(50) NOT NULL,"
               L"  v integer NOT NULL)", "create pg t_basic") &&
           // id=2 differs in `name`, id=3 differs in `v`, id=4 is target-only,
           // id=5 is identical on both sides and must not be touched.
           mplive::Exec(c,
               L"INSERT INTO t_basic (id,name,v) VALUES "
               L"(2,'BETA-OLD',20),(3,'gamma',99),(4,'delta',40),(5,'epsilon',50)",
               "seed pg t_basic");
}

// The same fixture rendered for a MySQL target (item A's same-engine leg).
bool SetupBasicMySqlTarget(IConnection& c, const wxString& db)
{
    const wxString q = QuoteIdent(db, Dialect::MySQL);
    return mplive::Exec(c, L"DROP TABLE IF EXISTS " + q + L".t_basic", "drop my2 t_basic") &&
           mplive::Exec(c,
               L"CREATE TABLE " + q + L".t_basic ("
               L"  id INT NOT NULL PRIMARY KEY,"
               L"  name VARCHAR(50) NOT NULL,"
               L"  v INT NOT NULL) ENGINE=InnoDB", "create my2 t_basic") &&
           mplive::Exec(c,
               L"INSERT INTO " + q + L".t_basic (id,name,v) VALUES "
               L"(2,'BETA-OLD',20),(3,'gamma',99),(4,'delta',40),(5,'epsilon',50)",
               "seed my2 t_basic");
}

// The PG→PG leg's SOURCE. Named `t_pgpg` on both sides on purpose.
//
// A first draft of this leg gave the two sides different table names and the
// run failed with `relation "t_basic_src" does not exist` — raised by the
// TARGET's cursor. That is not a defect but it IS worth writing down, because
// the two halves of this feature disagree about which name they use and only
// the caller's discipline hides it: DataSync's merge streams BOTH sides with
// `spec.srcSchema.name` (MergeParams carries ONE `table`), while
// DataSyncExec's TableApplier writes to `spec.tgtSchema.name`. Same-named
// tables are the only configuration in which those agree, and SyncEngine only
// ever builds specs that way — so nothing reachable from the product is
// affected. A future caller that pairs differently-named tables would read one
// table and write another, silently. Keeping the fixture same-named documents
// the constraint rather than papering over it.
bool SetupPgPgSource(IConnection& c)
{
    return mplive::Exec(c, L"DROP TABLE IF EXISTS t_pgpg", "drop pgpg src") &&
           mplive::Exec(c,
               L"CREATE TABLE t_pgpg ("
               L"  id integer NOT NULL PRIMARY KEY,"
               L"  name varchar(50) NOT NULL,"
               L"  v integer NOT NULL)", "create pgpg src") &&
           mplive::Exec(c,
               L"INSERT INTO t_pgpg (id,name,v) VALUES "
               L"(1,'alpha',10),(2,'beta',20),(3,'gamma',30),(5,'epsilon',50)",
               "seed pgpg src");
}

bool SetupPgPgTarget(IConnection& c)
{
    return mplive::Exec(c, L"DROP TABLE IF EXISTS t_pgpg", "drop pgpg tgt") &&
           mplive::Exec(c,
               L"CREATE TABLE t_pgpg ("
               L"  id integer NOT NULL PRIMARY KEY,"
               L"  name varchar(50) NOT NULL,"
               L"  v integer NOT NULL)", "create pgpg tgt") &&
           mplive::Exec(c,
               L"INSERT INTO t_pgpg (id,name,v) VALUES "
               L"(2,'BETA-OLD',20),(3,'gamma',99),(4,'delta',40),(5,'epsilon',50)",
               "seed pgpg tgt");
}

// The canonical read-back for t_basic. Ordered by id so the assertion pins row
// ORDER too, and every column is included so a wrong value in an untested
// column cannot hide.
const wxChar* kBasicReadPg =
    L"SELECT id, name, v FROM t_basic ORDER BY id";

// After a full sync the target must be byte-identical to the source: the extra
// row gone, the missing row inserted, both differing rows corrected, the
// matching row untouched.
const wxChar* kBasicExpected = L"1|alpha|10;2|beta|20;3|gamma|30;5|epsilon|50";

// ---------------------------------------------------------------------------
// A — the basic write actually happens and is correct
// ---------------------------------------------------------------------------

// One table, all three categories, deletes armed. Asserts on the TARGET's rows.
void RunBasicLeg(IConnection& src, IConnection& tgt, const wxString& srcDb,
                 const wxString& tgtDb, const wxString& srcTable,
                 const wxString& readBack, const char* leg)
{
    std::printf("\n  -- item A leg: %s --\n", leg);

    DataDiffSpec spec;
    if (!MakeSpec(src, tgt, srcDb, tgtDb, srcTable, spec)) {
        mplive::ExpectTrue("itemA spec built", false);
        return;
    }
    // What the user would have been shown before pressing Execute.
    const CompareOutcome cmp = Compare1(src, tgt, spec);
    std::printf("  OBS  %s compare: ins=%lld upd=%lld del=%lld blocked=%lld\n",
                leg, cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes,
                cmp.stat.blocked);
    mplive::ExpectTrue("itemA compare ran", cmp.ran);
    mplive::ExpectEq("itemA compare sees 1 insert",  cmp.stat.inserts, 1);
    mplive::ExpectEq("itemA compare sees 2 updates", cmp.stat.updates, 2);
    mplive::ExpectEq("itemA compare sees 1 delete",  cmp.stat.deletes, 1);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(srcTable, Allow{true, true, true}, true)});

    const ExecOutcome x = RunExec(src, tgt, jobs);
    ShowResult(x.result, leg);
    mplive::ExpectTrue("itemA execute returned ok", x.ok);
    if (!x.ok) std::printf("  ERR  %s: %s\n", leg, (const char*)x.err.utf8_str());

    const TableExecReport* rep = ReportOf(x.result, srcTable);
    mplive::ExpectTrue("itemA table reported", rep != nullptr);
    if (rep) {
        mplive::ExpectTrue("itemA table committed", rep->committed);
        mplive::ExpectEq("itemA applied 1 insert",  rep->inserts, 1);
        mplive::ExpectEq("itemA applied 2 updates", rep->updates, 2);
        mplive::ExpectEq("itemA applied 1 delete",  rep->deletes, 1);
    }

    // THE ASSERTION THIS SUITE EXISTS FOR: the target's actual contents.
    const wxString got = Dump(tgt, readBack);
    std::printf("  OBS  %s target now: [%s]\n", leg, (const char*)got.utf8_str());
    ExpectStr("itemA TARGET CONTENTS after sync", got, kBasicExpected);

    // Re-running must be a no-op. A write path that is not idempotent against an
    // already-synced target is writing something it should not (the "wrote
    // twice" failure mode), and it shows up here and nowhere else.
    const CompareOutcome again = Compare1(src, tgt, spec);
    mplive::ExpectEq("itemA re-compare finds no inserts", again.stat.inserts, 0);
    mplive::ExpectEq("itemA re-compare finds no updates", again.stat.updates, 0);
    mplive::ExpectEq("itemA re-compare finds no deletes", again.stat.deletes, 0);
}

void ItemA(IConnection& my, IConnection& pg, IConnection& my2, IConnection& pg2,
           const wxString& myDb, const wxString& pgDb, const wxString& my2Db)
{
    std::printf("\n== item A: the basic write happens, and is correct ==\n");

    // A1 — cross-engine MySQL → PostgreSQL.
    RunBasicLeg(my, pg, myDb, pgDb, L"t_basic", kBasicReadPg, "A1 MySQL->PG");

    // A2 — same-engine MySQL → MySQL.
    RunBasicLeg(my, my2, myDb, my2Db, L"t_basic",
                L"SELECT id, name, v FROM " + QuoteIdent(my2Db, Dialect::MySQL) +
                L".t_basic ORDER BY id", "A2 MySQL->MySQL");

    // A3 — same-engine PostgreSQL → PostgreSQL. `t_pgpg` on the cross-engine
    // target database (a PG server we already own) is the source; `t_pgpg` on
    // the second PG database is the target.
    {
        std::printf("\n  -- item A leg: A3 PG->PG --\n");
        DataDiffSpec spec;
        wxString e1, e2;
        const bool s = pg.GetTableSchema(pgDb, L"t_pgpg", spec.srcSchema, e1);
        const bool t = pg2.GetTableSchema(wxString(), L"t_pgpg", spec.tgtSchema, e2);
        if (!s || !t) {
            std::printf("  ERR  A3 schemas: %s / %s\n",
                        (const char*)e1.utf8_str(), (const char*)e2.utf8_str());
            mplive::ExpectTrue("itemA A3 schemas read", false);
            return;
        }
        spec.srcDb = pgDb;
        spec.tgtDb = wxString();

        const CompareOutcome cmp = Compare1(pg, pg2, spec);
        std::printf("  OBS  A3 compare: ins=%lld upd=%lld del=%lld\n",
                    cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes);
        mplive::ExpectEq("itemA A3 compare sees 1 insert",  cmp.stat.inserts, 1);
        mplive::ExpectEq("itemA A3 compare sees 2 updates", cmp.stat.updates, 2);
        mplive::ExpectEq("itemA A3 compare sees 1 delete",  cmp.stat.deletes, 1);

        std::vector<TableExecJob> jobs;
        jobs.push_back(TableExecJob{
            spec, MakeAllow(L"t_pgpg", Allow{true, true, true}, true)});
        const ExecOutcome x = RunExec(pg, pg2, jobs);
        ShowResult(x.result, "A3 PG->PG");
        mplive::ExpectTrue("itemA A3 execute ok", x.ok);
        if (!x.ok) std::printf("  ERR  A3: %s\n", (const char*)x.err.utf8_str());

        const wxString got = Dump(pg2, L"SELECT id, name, v FROM t_pgpg ORDER BY id");
        std::printf("  OBS  A3 target now: [%s]\n", (const char*)got.utf8_str());
        ExpectStr("itemA A3 TARGET CONTENTS after sync", got, kBasicExpected);
    }
}

// ---------------------------------------------------------------------------
// B — ExecuteBatch: parameterized, batched, 500 rows/batch
// ---------------------------------------------------------------------------
//
// kBatchRows is chosen to cross the 500-row boundary TWICE with a non-multiple
// remainder (1201 = 500 + 500 + 201). A batching off-by-one — a `<` that should
// be `<=`, a flush that drops the final partial batch, a chunk loop that
// re-sends the last row — is invisible in a 3-row test and silently loses or
// duplicates hundreds of rows in production. The exact-count and
// exact-distinct-count assertions below are the only thing that can see it.
constexpr int kBatchRows = 1201;

// Deterministic per-row values, defined once so setup and read-back cannot
// drift apart. id runs 1..kBatchRows.
//   flag : TINYINT(1) 1/0 → PostgreSQL boolean true/false (the coercion that
//          must write a REAL boolean, not the text '1')
//   note : NULL when id%3==0, EMPTY STRING when id%3==1, text otherwise —
//          NULL and '' must stay distinguishable end to end
//   payload : binary, including a 0x00 byte, which is exactly what a
//          text-transport bug truncates at
bool SetupBatchMySql(IConnection& c, const wxString& db)
{
    const wxString q = QuoteIdent(db, Dialect::MySQL);
    if (!mplive::Exec(c, L"DROP TABLE IF EXISTS " + q + L".t_batch", "drop t_batch") ||
        !mplive::Exec(c,
            L"CREATE TABLE " + q + L".t_batch ("
            L"  id INT NOT NULL PRIMARY KEY,"
            L"  flag TINYINT(1) NOT NULL,"
            L"  note VARCHAR(50) NULL,"
            L"  payload VARBINARY(16) NULL) ENGINE=InnoDB", "create t_batch"))
        return false;

    // Seeded in chunks so the seeding itself does not depend on the very batch
    // path under test.
    wxString values;
    int inChunk = 0;
    for (int id = 1; id <= kBatchRows; ++id) {
        if (!values.IsEmpty()) values += L",";
        const wxString note = (id % 3 == 0) ? wxString(L"NULL")
                            : (id % 3 == 1) ? wxString(L"''")
                                            : wxString::Format(L"'n%d'", id);
        // 4 bytes, second one 0x00, last one varies with id.
        values += wxString::Format(L"(%d,%d,%s,0xAA00BB%02X)",
                                   id, (id % 2 == 0) ? 1 : 0, note, id & 0xFF);
        if (++inChunk >= 200 || id == kBatchRows) {
            if (!mplive::Exec(c, L"INSERT INTO " + q +
                              L".t_batch (id,flag,note,payload) VALUES " + values,
                              "seed t_batch"))
                return false;
            values.Clear();
            inChunk = 0;
        }
    }
    return true;
}

bool SetupBatchPg(IConnection& c)
{
    return mplive::Exec(c, L"DROP TABLE IF EXISTS t_batch", "drop pg t_batch") &&
           mplive::Exec(c,
               L"CREATE TABLE t_batch ("
               L"  id integer NOT NULL PRIMARY KEY,"
               L"  flag boolean NOT NULL,"
               L"  note varchar(50) NULL,"
               L"  payload bytea NULL)", "create pg t_batch");
    // Deliberately EMPTY: every row is an insert, so the whole table goes
    // through ExecuteBatch and the batch boundary is crossed twice.
}

void ItemB(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item B: ExecuteBatch — parameterized, batched, coerced ==\n");

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_batch", spec)) {
        mplive::ExpectTrue("itemB spec built", false);
        return;
    }

    const CompareOutcome cmp = Compare1(my, pg, spec);
    std::printf("  OBS  compare: ins=%lld upd=%lld del=%lld truncated=%d\n",
                cmp.stat.inserts, cmp.stat.updates, cmp.stat.deletes,
                (int)cmp.truncated);
    mplive::ExpectEq("itemB compare counts every row as an insert",
                     cmp.stat.inserts, kBatchRows);
    // The compare pass materializes at most 500 rows; 1201 differences must set
    // Truncated(). If this is false the cap is not engaging and the sample is
    // silently the whole table.
    mplive::ExpectTrue("itemB compare marks the sample truncated", cmp.truncated);

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_batch", Allow{true, true, false}, false)});
    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "B");
    mplive::ExpectTrue("itemB execute ok", x.ok);
    if (!x.ok) std::printf("  ERR  B: %s\n", (const char*)x.err.utf8_str());

    const TableExecReport* rep = ReportOf(x.result, L"t_batch");
    if (rep) mplive::ExpectEq("itemB reported inserts", rep->inserts, kBatchRows);

    // ---- every row exactly once, across the batch boundary -----------------
    mplive::ExpectEq("itemB target row count",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_batch"), kBatchRows);
    mplive::ExpectEq("itemB target DISTINCT id count",
                     ScalarInt(pg, L"SELECT COUNT(DISTINCT id) FROM t_batch"), kBatchRows);
    // Sum pins that the ids are the RIGHT ids, not merely the right number of
    // them: a chunk loop that re-sent row 500 would keep the count and break
    // this. 1+2+...+1201 = 1201*1202/2.
    mplive::ExpectEq("itemB target id checksum",
                     ScalarInt(pg, L"SELECT COALESCE(SUM(id),0) FROM t_batch"),
                     (long long)kBatchRows * (kBatchRows + 1) / 2);
    // The rows immediately around each boundary, by identity — an off-by-one
    // loses exactly these and nothing else.
    ExpectStr("itemB rows across batch boundary 1 (500/501)",
              Dump(pg, L"SELECT id FROM t_batch WHERE id IN (499,500,501,502) ORDER BY id"),
              L"499;500;501;502");
    ExpectStr("itemB rows across batch boundary 2 (1000/1001)",
              Dump(pg, L"SELECT id FROM t_batch WHERE id IN (999,1000,1001,1002) ORDER BY id"),
              L"999;1000;1001;1002");
    ExpectStr("itemB final partial batch survives (last row)",
              Dump(pg, wxString::Format(L"SELECT id FROM t_batch WHERE id=%d", kBatchRows)),
              wxString::Format(L"%d", kBatchRows));

    // ---- TINYINT(1) → boolean must be a REAL boolean -----------------------
    // Counted with `IS TRUE` / `IS FALSE`, which only a genuine boolean column
    // can answer. Even ids carry flag=1.
    const long long trues  = ScalarInt(pg, L"SELECT COUNT(*) FROM t_batch WHERE flag IS TRUE");
    const long long falses = ScalarInt(pg, L"SELECT COUNT(*) FROM t_batch WHERE flag IS FALSE");
    std::printf("  OBS  flag: true=%lld false=%lld\n", trues, falses);
    mplive::ExpectEq("itemB TINYINT(1)=1 wrote boolean true",  trues,  kBatchRows / 2);
    mplive::ExpectEq("itemB TINYINT(1)=0 wrote boolean false", falses, kBatchRows - kBatchRows / 2);
    // And the value itself, per row, at the two ends of the table.
    ExpectStr("itemB flag round-trip on specific rows",
              Dump(pg, L"SELECT id, flag FROM t_batch WHERE id IN (1,2,1200,1201) ORDER BY id"),
              L"1|f;2|t;1200|t;1201|f");

    // ---- NULL stays NULL, '' stays '' --------------------------------------
    // Disambiguated in SQL: the driver renders a SQL NULL as the text "NULL",
    // so asking it directly could not tell these two apart.
    const long long nulls  = ScalarInt(pg, L"SELECT COUNT(*) FROM t_batch WHERE note IS NULL");
    const long long empties= ScalarInt(pg, L"SELECT COUNT(*) FROM t_batch WHERE note = ''");
    std::printf("  OBS  note: null=%lld empty=%lld\n", nulls, empties);
    mplive::ExpectEq("itemB NULL notes stayed NULL", nulls, kBatchRows / 3);
    mplive::ExpectEq("itemB empty-string notes stayed empty (not NULL)",
                     empties, (kBatchRows + 2) / 3);
    ExpectStr("itemB NULL vs empty string are distinguishable at the target",
              Dump(pg, L"SELECT id, CASE WHEN note IS NULL THEN '#NULL#' "
                       L"WHEN note = '' THEN '#EMPTY#' ELSE note END "
                       L"FROM t_batch WHERE id IN (1,2,3) ORDER BY id"),
              L"1|#EMPTY#;2|n2;3|#NULL#");

    // ---- binary/blob round-trip, including the 0x00 byte -------------------
    // Compared as hex so the assertion is on BYTES, not on however the client
    // chose to render them.
    ExpectStr("itemB binary payload round-trips byte for byte",
              Dump(pg, L"SELECT id, upper(encode(payload,'hex')) FROM t_batch "
                       L"WHERE id IN (1,2,255,256) ORDER BY id"),
              L"1|AA00BB01;2|AA00BB02;255|AA00BBFF;256|AA00BB00");
    mplive::ExpectEq("itemB no payload was truncated at the 0x00 byte",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_batch WHERE length(payload) <> 4"), 0);

    // ---- and the whole table now compares clean ----------------------------
    const CompareOutcome again = Compare1(my, pg, spec);
    std::printf("  OBS  re-compare: ins=%lld upd=%lld del=%lld\n",
                again.stat.inserts, again.stat.updates, again.stat.deletes);
    mplive::ExpectEq("itemB re-compare finds no differences",
                     again.stat.inserts + again.stat.updates + again.stat.deletes, 0);
}

// ---------------------------------------------------------------------------
// F — unconvertible values are refused AT THE TARGET too
// ---------------------------------------------------------------------------
//
// The compare suite proved these rows are REFUSED at the source (hazards 4 and
// 5). That is a claim about a gate. This is the claim that matters to a
// customer: after Execute, the bad row is not in the target IN ANY FORM — not
// as NULL, not truncated to a valid date, not coerced to a clamped integer.
bool SetupBadMySql(IConnection& c, const wxString& db)
{
    const wxString q = QuoteIdent(db, Dialect::MySQL);
    wxString ig;
    // Zero-dates need a relaxed sql_mode to insert at all; if the server
    // refuses, the zero-date half of this item cannot be set up and says so.
    mplive::TryExec(c, L"SET SESSION sql_mode=''", "relax sql_mode", ig);
    return mplive::Exec(c, L"DROP TABLE IF EXISTS " + q + L".t_bad", "drop t_bad") &&
           mplive::Exec(c,
               L"CREATE TABLE " + q + L".t_bad ("
               L"  id INT NOT NULL PRIMARY KEY,"
               L"  d DATE NULL,"
               L"  n BIGINT UNSIGNED NULL) ENGINE=InnoDB", "create t_bad") &&
           mplive::Exec(c,
               L"INSERT INTO " + q + L".t_bad (id,d,n) VALUES "
               L"(1,'2024-01-15',42),"                    // clean, and BEFORE the bad rows
               L"(2,'0000-00-00',7),"                     // zero-date
               L"(3,'2024-02-20',18446744073709551615)",  // unsigned overflow
               "seed t_bad");
}

bool SetupBadPg(IConnection& c)
{
    return mplive::Exec(c, L"DROP TABLE IF EXISTS t_bad", "drop pg t_bad") &&
           mplive::Exec(c,
               L"CREATE TABLE t_bad ("
               L"  id integer NOT NULL PRIMARY KEY,"
               L"  d date NULL,"
               L"  n bigint NULL)", "create pg t_bad");
}

void ItemF(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item F: unconvertible values refused AT THE TARGET ==\n");

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_bad", spec)) {
        mplive::ExpectTrue("itemF spec built", false);
        return;
    }

    const CompareOutcome cmp = Compare1(my, pg, spec);
    std::printf("  OBS  compare: ins=%lld blocked=%lld executable=%d\n",
                cmp.stat.inserts, cmp.stat.blocked, (int)cmp.executable);
    mplive::ExpectEq("itemF compare blocks 2 rows", cmp.stat.blocked, 2);
    mplive::ExpectTrue("itemF compare marks the table non-executable", !cmp.executable);

    // Execute anyway. Production greys this table out, but the executor must not
    // rely on that: reaching a blocked row at execute time is the documented
    // "source changed between the passes" case, and the documented response is
    // to abort the table with its transaction rolled back.
    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_bad", Allow{true, true, false}, false)});
    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "F");
    std::printf("  OBS  F error: [%s]\n", (const char*)x.err.utf8_str());
    mplive::ExpectTrue("itemF execute FAILS rather than writing a partial table", !x.ok);
    mplive::ExpectTrue("itemF error names the table", x.err.Contains(L"t_bad"));

    const TableExecReport* rep = ReportOf(x.result, L"t_bad");
    if (rep) mplive::ExpectTrue("itemF table not marked committed", !rep->committed);

    // THE ASSERTION: nothing from the bad rows reached the target, in any form.
    std::printf("  OBS  t_bad target contents: [%s]\n",
                (const char*)Dump(pg, L"SELECT id, COALESCE(d::text,'#NULL#'), "
                                      L"COALESCE(n::text,'#NULL#') FROM t_bad ORDER BY id")
                    .utf8_str());
    mplive::ExpectEq("itemF zero-date row absent from target",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_bad WHERE id=2"), 0);
    mplive::ExpectEq("itemF unsigned-overflow row absent from target",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_bad WHERE id=3"), 0);
    mplive::ExpectEq("itemF no row was smuggled in with a NULL substitute",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_bad WHERE d IS NULL OR n IS NULL"), 0);
    // The transaction rolled back, so the CLEAN row that preceded the bad one is
    // gone too. That is the documented behaviour — per-table atomicity — and it
    // is asserted rather than assumed, because "we kept the good rows" would be
    // a silently partial apply.
    mplive::ExpectEq("itemF whole table rolled back (clean row not left behind)",
                     ScalarInt(pg, L"SELECT COUNT(*) FROM t_bad"), 0);
}

// ---------------------------------------------------------------------------
// G — identity/sequence repair
// ---------------------------------------------------------------------------
//
// A latent-bug class: the sync appears to succeed, and the target's NEXT plain
// INSERT fails with a duplicate key because the sequence was never advanced past
// the literal identity values that were written. The only honest test is to
// perform that next INSERT.
bool SetupIdentMySql(IConnection& c, const wxString& db)
{
    const wxString q = QuoteIdent(db, Dialect::MySQL);
    return mplive::Exec(c, L"DROP TABLE IF EXISTS " + q + L".t_ident", "drop t_ident") &&
           mplive::Exec(c,
               L"CREATE TABLE " + q + L".t_ident ("
               L"  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
               L"  label VARCHAR(50) NOT NULL) ENGINE=InnoDB", "create t_ident") &&
           mplive::Exec(c,
               L"INSERT INTO " + q + L".t_ident (id,label) VALUES "
               L"(101,'a'),(102,'b'),(103,'c')", "seed t_ident");
}

bool SetupIdentPg(IConnection& c)
{
    return mplive::Exec(c, L"DROP TABLE IF EXISTS t_ident", "drop pg t_ident") &&
           mplive::Exec(c,
               L"CREATE TABLE t_ident ("
               L"  id serial PRIMARY KEY,"
               L"  label varchar(50) NOT NULL)", "create pg t_ident");
}

void ItemG(IConnection& my, IConnection& pg, const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n== item G: identity/sequence repair ==\n");

    DataDiffSpec spec;
    if (!MakeSpec(my, pg, myDb, pgDb, L"t_ident", spec)) {
        mplive::ExpectTrue("itemG spec built", false);
        return;
    }
    // The repair only runs if the target column is actually recognized as
    // identity-backed; if introspection missed it, the postamble is skipped and
    // the duplicate-key failure below would be blamed on the wrong component.
    const NormColumn* idc = spec.tgtSchema.FindColumn(L"id");
    mplive::ExpectTrue("itemG target id introspected as auto-increment",
                       idc && idc->autoIncrement);

    std::printf("  OBS  pg sequence before sync: last_value=%s\n",
                (const char*)Scalar(pg, L"SELECT last_value FROM t_ident_id_seq").utf8_str());

    std::vector<TableExecJob> jobs;
    jobs.push_back(TableExecJob{spec, MakeAllow(L"t_ident", Allow{true, true, false}, false)});
    const ExecOutcome x = RunExec(my, pg, jobs);
    ShowResult(x.result, "G");
    mplive::ExpectTrue("itemG execute ok", x.ok);

    ExpectStr("itemG explicit identity values landed",
              Dump(pg, L"SELECT id, label FROM t_ident ORDER BY id"),
              L"101|a;102|b;103|c");
    std::printf("  OBS  pg sequence after sync: last_value=%s\n",
                (const char*)Scalar(pg, L"SELECT last_value FROM t_ident_id_seq").utf8_str());

    // THE ASSERTION: the next plain INSERT, with no id supplied, must succeed.
    // Without the postamble the sequence still sits at 1 and this collides.
    {
        QueryResult r; wxString err;
        const bool ok = pg.Execute(L"INSERT INTO t_ident (label) VALUES ('next')", r, err);
        if (!ok) std::printf("  OBS  plain INSERT error: [%s]\n", (const char*)err.utf8_str());
        mplive::ExpectTrue("itemG plain INSERT after sync does NOT collide", ok);
    }
    const wxString after = Dump(pg, L"SELECT id, label FROM t_ident ORDER BY id");
    std::printf("  OBS  t_ident after plain insert: [%s]\n", (const char*)after.utf8_str());
    ExpectStr("itemG generator allocated the next free id",
              after, L"101|a;102|b;103|c;104|next");
}

} // namespace

// ---------------------------------------------------------------------------

int main()
{
    std::printf("== mysql_pg_live_exec (EXECUTE/WRITE path, live) ==\n");

    const wxString myHost = mplive::Env("SWIFTSQL_MYTEST_HOST");
    const wxString myUser = mplive::Env("SWIFTSQL_MYTEST_USER");
    const wxString myPass = mplive::Env("SWIFTSQL_MYTEST_PASS");
    const wxString pgHost = mplive::Env("SWIFTSQL_PGTEST_HOST");
    const wxString pgUser = mplive::Env("SWIFTSQL_PGTEST_USER");
    const wxString pgPass = mplive::Env("SWIFTSQL_PGTEST_PASS");
    if (myHost.IsEmpty() || myUser.IsEmpty() || myPass.IsEmpty() ||
        pgHost.IsEmpty() || pgUser.IsEmpty() || pgPass.IsEmpty()) {
        std::printf("SKIP: no live MySQL+PostgreSQL pair configured\n");
        return 0;
    }

    const int myPort = mplive::EnvInt("SWIFTSQL_MYTEST_PORT", 3306);
    const int pgPort = mplive::EnvInt("SWIFTSQL_PGTEST_PORT", 5432);
    wxString myBoot = mplive::Env("SWIFTSQL_MYTEST_DB"); if (myBoot.IsEmpty()) myBoot = L"mysql";
    wxString pgBoot = mplive::Env("SWIFTSQL_PGTEST_DB"); if (pgBoot.IsEmpty()) pgBoot = L"postgres";

    // Own prefix, distinct from the compare suite's `swiftsql_xsync_*`, so the
    // two suites can never collide if they are ever run concurrently.
    const wxString myDb  = L"swiftsql_xexec_src";
    const wxString my2Db = L"swiftsql_xexec_my2";
    const wxString pgDb  = L"swiftsql_xexec_tgt";
    const wxString pg2Db = L"swiftsql_xexec_pg2";

    const wxString qMyDb  = QuoteIdent(myDb,  Dialect::MySQL);
    const wxString qMy2Db = QuoteIdent(my2Db, Dialect::MySQL);
    const wxString qPgDb  = QuoteIdent(pgDb,  Dialect::Postgres);
    const wxString qPg2Db = QuoteIdent(pg2Db, Dialect::Postgres);

    auto myMaint = CreateConnection(DbType::MySQL);
    auto pgMaint = CreateConnection(DbType::PostgreSQL);
    {
        wxString e1, e2;
        if (!myMaint->Connect(mplive::Profile(DbType::MySQL, myHost, myPort, myUser, myPass, myBoot), e1)) {
            std::printf("  FAIL connect MySQL bootstrap: %s\n", (const char*)e1.utf8_str());
            return 1;
        }
        if (!pgMaint->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort, pgUser, pgPass, pgBoot), e2)) {
            std::printf("  FAIL connect PostgreSQL bootstrap: %s\n", (const char*)e2.utf8_str());
            return 1;
        }
        mplive::Observe("mysql server version", myMaint->ServerVersion());
        mplive::Observe("pg server version", pgMaint->ServerVersion());
    }

    std::unique_ptr<IConnection> my, pg, my2, pg2;
    struct Guard {
        IConnection *myMaint, *pgMaint;
        std::unique_ptr<IConnection> *my, *pg, *my2, *pg2;
        wxString qMyDb, qMy2Db, qPgDb, qPg2Db;
        ~Guard()
        {
            // Disconnect FIRST: PostgreSQL refuses to DROP a database that still
            // has a session attached, so a live connection here would leave the
            // test databases behind on a shared server.
            for (auto* p : { my, pg, my2, pg2 }) if (p && *p) (*p)->Disconnect();
            QueryResult r; wxString e;
            if (myMaint) {
                myMaint->Execute(L"DROP DATABASE IF EXISTS " + qMyDb,  r, e);
                myMaint->Execute(L"DROP DATABASE IF EXISTS " + qMy2Db, r, e);
            }
            if (pgMaint) {
                pgMaint->Execute(L"DROP DATABASE IF EXISTS " + qPgDb,  r, e);
                pgMaint->Execute(L"DROP DATABASE IF EXISTS " + qPg2Db, r, e);
            }
        }
    };

    const bool provisioned =
        mplive::Exec(*myMaint, L"DROP DATABASE IF EXISTS " + qMyDb,  "drop my src") &&
        mplive::Exec(*myMaint, L"DROP DATABASE IF EXISTS " + qMy2Db, "drop my tgt") &&
        mplive::Exec(*myMaint, L"CREATE DATABASE " + qMyDb +
                     L" DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci", "create my src") &&
        mplive::Exec(*myMaint, L"CREATE DATABASE " + qMy2Db +
                     L" DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci", "create my tgt") &&
        mplive::Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + qPgDb,  "drop pg tgt") &&
        mplive::Exec(*pgMaint, L"DROP DATABASE IF EXISTS " + qPg2Db, "drop pg tgt2") &&
        mplive::Exec(*pgMaint, L"CREATE DATABASE " + qPgDb,  "create pg tgt") &&
        mplive::Exec(*pgMaint, L"CREATE DATABASE " + qPg2Db, "create pg tgt2");

    my  = CreateConnection(DbType::MySQL);
    my2 = CreateConnection(DbType::MySQL);
    pg  = CreateConnection(DbType::PostgreSQL);
    pg2 = CreateConnection(DbType::PostgreSQL);
    Guard guard{ myMaint.get(), pgMaint.get(), &my, &pg, &my2, &pg2,
                 qMyDb, qMy2Db, qPgDb, qPg2Db };

    mplive::ExpectTrue("provision four throwaway databases", provisioned);
    if (!provisioned) {
        std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
        return 1;
    }

    {
        wxString e1, e2, e3, e4;
        const bool a = my->Connect(mplive::Profile(DbType::MySQL, myHost, myPort, myUser, myPass, myDb), e1);
        const bool b = my2->Connect(mplive::Profile(DbType::MySQL, myHost, myPort, myUser, myPass, my2Db), e2);
        const bool c = pg->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort, pgUser, pgPass, pgDb), e3);
        const bool d = pg2->Connect(mplive::Profile(DbType::PostgreSQL, pgHost, pgPort, pgUser, pgPass, pg2Db), e4);
        mplive::ExpectTrue("connect all four test databases", a && b && c && d);
        if (!(a && b && c && d)) {
            std::printf("  errs: %s / %s / %s / %s\n",
                        (const char*)e1.utf8_str(), (const char*)e2.utf8_str(),
                        (const char*)e3.utf8_str(), (const char*)e4.utf8_str());
            std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
            return 1;
        }
    }

    const bool setup =
        SetupBasicMySql(*my, myDb) && SetupBasicPg(*pg) &&
        SetupBasicMySqlTarget(*my2, my2Db) &&
        SetupPgPgSource(*pg) && SetupPgPgTarget(*pg2) &&
        SetupBatchMySql(*my, myDb) && SetupBatchPg(*pg) &&
        SetupBadMySql(*my, myDb) && SetupBadPg(*pg) &&
        SetupIdentMySql(*my, myDb) && SetupIdentPg(*pg);
    mplive::ExpectTrue("fixture setup on all four databases", setup);
    if (!setup) {
        std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
        return 1;
    }

    ItemA(*my, *pg, *my2, *pg2, myDb, pgDb, my2Db);
    ItemB(*my, *pg, myDb, pgDb);
    ItemB2_WideBatch(*my, *pg, myDb, pgDb);
    ItemB3_MySqlTargetBinding(*my, *pg, myDb, pgDb);
    ItemC_PartialFailure(*my, *pg, myDb, pgDb);
    ItemC3_DeleteSweepWindow(*my, *pg, myDb, pgDb);
    ItemD3_PopulatedTarget(*my, *pg, myDb, pgDb);
    ItemD_DriftWindow(*my, *pg, myDb, pgDb);
    ItemE_Selection(*my, *pg, myDb, pgDb);
    ItemF(*my, *pg, myDb, pgDb);
    ItemG(*my, *pg, myDb, pgDb);
    ItemH_Scale(*my, *pg, myDb, pgDb);

    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;   // guard drops all four test databases on the way out
}
