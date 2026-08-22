// sync_data_test.cpp — offline unit tests for the COMPARE pass over row data:
// db::sync::DiffAndEmitData (the PK-ordered streaming diff that emits SQL text)
// and db::sync::DiffRowChanges (the structured pass that emits RowChange values
// instead). Both are driven through the stub IConnection in sync_stub.h, so no
// driver, no wire, no server.
//
// This suite READS ONLY. Nothing here applies anything to a target — the write
// pass (ExecuteDataSync and the delete gate) is sync_exec_test.cpp, mirroring
// the compare/write split the live suites already use (mysql_pg_live_test vs
// mysql_pg_live_exec_test). Which side a new test belongs on is decided by that
// same question: does it assert on what was OBSERVED, or on what was SENT?
//
// Covers: the four row categories and their statement text, the insert/update/
// delete option gates, the missing-PK refusal in both flavours (hard error for
// the statement path, structural verdict for the structured one), the
// materialization cap vs. exact counts, and the same-engine Exact verdict.
#include "sync_stub.h"

#include "db/DataSync.h"

#include <atomic>

using namespace db;
using namespace db::sync;
using namespace synctest;

// ===========================================================================
//  A2. DiffAndEmitData — streaming data diff through the stub connection.
// ===========================================================================
static void TestDataSync()
{
    std::printf("[DiffAndEmitData — data]\n");

    const TableSchema schema = TwoColSchema();
    std::atomic<bool> stop{false};

    // src: id1(a) id2(b) id3(c) ; tgt: id1(a) id2(B) id4(d)
    auto makeSrc = [] {
        StubConn* c = new StubConn();
        c->dialect_ = Dialect::Postgres;
        c->rows_ = { { Num(L"1"), Txt(L"a") },
                     { Num(L"2"), Txt(L"b") },
                     { Num(L"3"), Txt(L"c") } };
        return c;
    };
    auto makeTgt = [] {
        StubConn* c = new StubConn();
        c->dialect_ = Dialect::Postgres;
        c->rows_ = { { Num(L"1"), Txt(L"a") },
                     { Num(L"2"), Txt(L"B") },
                     { Num(L"4"), Txt(L"d") } };
        return c;
    };

    // ---- all ops enabled (deleteMissing on) ----
    {
        StubConn* src = makeSrc(); StubConn* tgt = makeTgt();
        DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
        std::vector<wxString> emitted;
        RowStat stat; wxString err;
        bool ok = DiffAndEmitData(*src, *tgt, L"s", L"t", schema, opt,
                                  [&](const wxString& s) { emitted.push_back(s); },
                                  stat, err, stop);
        ExpectTrue("data: ok", ok);
        ExpectEq("data: inserts",   stat.inserts,   1);
        ExpectEq("data: updates",   stat.updates,   1);
        ExpectEq("data: deletes",   stat.deletes,   1);
        ExpectEq("data: unchanged", stat.unchanged, 1);
        ExpectEq("data: 3 stmts emitted", (long long)emitted.size(), 3);
        if (emitted.size() == 3) {
            ExpectStr("data: UPDATE text", emitted[0],
                      L"UPDATE \"t\" SET \"name\" = 'b' WHERE \"id\" = 2");
            ExpectStr("data: INSERT text", emitted[1],
                      L"INSERT INTO \"t\" (\"id\", \"name\") VALUES (3, 'c')");
            ExpectStr("data: DELETE text", emitted[2],
                      L"DELETE FROM \"t\" WHERE \"id\" = 4");
        }
        delete src; delete tgt;
    }

    // ---- defaults: deleteMissing OFF → DELETE detected but not emitted ----
    {
        StubConn* src = makeSrc(); StubConn* tgt = makeTgt();
        DataSyncOptions opt;   // insert/update on, deleteMissing off
        std::vector<wxString> emitted;
        RowStat stat; wxString err;
        bool ok = DiffAndEmitData(*src, *tgt, L"s", L"t", schema, opt,
                                  [&](const wxString& s) { emitted.push_back(s); },
                                  stat, err, stop);
        ExpectTrue("data(nodel): ok", ok);
        ExpectEq("data(nodel): delete still detected", stat.deletes, 1);
        ExpectEq("data(nodel): 2 stmts emitted", (long long)emitted.size(), 2);
        delete src; delete tgt;
    }

    // ---- insert disabled → INSERT counted but suppressed ----
    {
        StubConn* src = makeSrc(); StubConn* tgt = makeTgt();
        DataSyncOptions opt; opt.insert = false; opt.update = true;
        std::vector<wxString> emitted;
        RowStat stat; wxString err;
        DiffAndEmitData(*src, *tgt, L"s", L"t", schema, opt,
                        [&](const wxString& s) { emitted.push_back(s); },
                        stat, err, stop);
        ExpectEq("data(noins): insert counted", stat.inserts, 1);
        // Pin the count BEFORE the property. Looping over `emitted` to prove no
        // INSERT is in it executes zero assertions when `emitted` is empty — and
        // an empty emit list is exactly what a broken diff produces, so the
        // green result would be strongest precisely when the code is most
        // broken. With insert off and update on, one UPDATE must remain.
        ExpectEq("data(noins): exactly the UPDATE survives",
                 (long long)emitted.size(), 1);
        int stmtsInspected = 0;
        for (const auto& s : emitted) {
            ++stmtsInspected;
            ExpectTrue("data(noins): no INSERT emitted", !s.StartsWith(L"INSERT"));
        }
        ExpectEq("data(noins): the no-INSERT claim was actually tested",
                 stmtsInspected, 1);
        delete src; delete tgt;
    }

    // ---- no primary key → hard error ----
    {
        StubConn* src = makeSrc(); StubConn* tgt = makeTgt();
        TableSchema noPk = schema; noPk.primaryKey.clear();
        DataSyncOptions opt;
        RowStat stat; wxString err;
        bool ok = DiffAndEmitData(*src, *tgt, L"s", L"t", noPk, opt,
                                  [](const wxString&) {}, stat, err, stop);
        ExpectTrue("data(nopk): returns false", !ok);
        ExpectTrue("data(nopk): err set", !err.IsEmpty());
        delete src; delete tgt;
    }

    // ---- identical tables → all unchanged, nothing emitted ----
    {
        StubConn* src = makeSrc();
        StubConn* tgt = new StubConn();
        tgt->dialect_ = Dialect::Postgres;
        tgt->rows_ = src->rows_;   // identical
        DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
        std::vector<wxString> emitted;
        RowStat stat; wxString err;
        DiffAndEmitData(*src, *tgt, L"s", L"t", schema, opt,
                        [&](const wxString& s) { emitted.push_back(s); },
                        stat, err, stop);
        ExpectEq("data(same): unchanged=3", stat.unchanged, 3);
        ExpectEq("data(same): nothing emitted", (long long)emitted.size(), 0);
        delete src; delete tgt;
    }
}

// ===========================================================================
//  A4. T9 — the STRUCTURED compare pass (DiffRowChanges).
// ===========================================================================
static void TestDiffRowChanges()
{
    std::printf("[DiffRowChanges — structured compare pass]\n");
    std::atomic<bool> stop{false};

    // Same fixture as the statement path: src 1(a) 2(b) 3(c), tgt 1(a) 2(B) 4(d).
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::Postgres;
        src.rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"b") },
                      { Num(L"3"), Txt(L"c") } };
        tgt.rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"B") },
                      { Num(L"4"), Txt(L"d") } };

        DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
        RowLayout layout; RowChangeSet set; wxString err;
        const bool ok = DiffRowChanges(src, tgt, TwoColSpec(), opt, layout, set,
                                       err, stop);
        ExpectTrue("rows: ok", ok);
        ExpectEq("rows: inserts", set.Stat().inserts, 1);
        ExpectEq("rows: updates", set.Stat().updates, 1);
        ExpectEq("rows: deletes", set.Stat().deletes, 1);
        ExpectEq("rows: blocked", set.Stat().blocked, 0);
        ExpectTrue("rows: executable", set.Executable());
        ExpectTrue("rows: not truncated", !set.Truncated());
        ExpectEq("rows: sample holds all three", (long long)set.Sample().size(), 3);

        // Same-engine ⇒ every bridge is an identity passthrough, so every row's
        // aggregate verdict must be Exact (nothing was rewritten).
        // The accumulator starts true, so an empty sample would carry it
        // through untouched — same vacuity as an assertion inside the loop,
        // just wearing a flag. Count what was folded in and pin that too.
        bool allExact = true;
        int  verdictsInspected = 0;
        for (const RowChange& r : set.Sample()) {
            ++verdictsInspected;
            if (r.Verdict() != ValueVerdict::Exact) allExact = false;
        }
        ExpectEq("rows: all three verdicts were actually inspected",
                 verdictsInspected, 3);
        ExpectTrue("rows: same-engine rows are Exact", allExact);

        // A DELETE carries only its key — never a value payload. Count the
        // deletes inspected: `for`+`if` asserts nothing at all if no Delete row
        // was ever materialized, which is the one case worth catching.
        int deletesInspected = 0;
        for (const RowChange& r : set.Sample())
            if (r.Operation() == RowChange::Op::Delete) {
                ++deletesInspected;
                ExpectTrue("rows: delete carries key only, no values",
                           r.Values().empty() && r.Key().size() == 1);
            }
        ExpectEq("rows: exactly one delete was inspected", deletesInspected, 1);

        ExpectEq("rows: layout has both columns", (long long)layout.columns.size(), 2);
        ExpectEq("rows: layout has one PK column", (long long)layout.pkColumns.size(), 1);
    }

    // ---- THE MEMORY FIX: counts are exact, materialization is capped --------
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::Postgres;
        for (int i = 0; i < 600; ++i)
            src.rows_.push_back({ Num(wxString::Format(L"%06d", i)), Txt(L"x") });
        // tgt.rows_ empty → all 600 are inserts.

        DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
        RowLayout layout; RowChangeSet set; wxString err;
        const bool ok = DiffRowChanges(src, tgt, TwoColSpec(), opt, layout, set,
                                       err, stop);
        ExpectTrue("cap: ok", ok);
        ExpectEq("cap: all 600 differences COUNTED", set.Stat().inserts, 600);
        ExpectEq("cap: only 500 MATERIALIZED",
                 (long long)set.Sample().size(),
                 (long long)RowChangeSet::kSampleCap);
        ExpectTrue("cap: truncated flag set", set.Truncated());
    }

    // ---- no PK: refused at layout time, as a verdict rather than an error ---
    {
        StubConn src, tgt;
        src.dialect_ = tgt.dialect_ = Dialect::Postgres;
        DataDiffSpec spec = TwoColSpec();
        spec.srcSchema.primaryKey.clear();
        spec.tgtSchema.primaryKey.clear();

        RowLayout layout; RowChangeSet set; wxString err;
        const bool ok = DiffRowChanges(src, tgt, spec, DataSyncOptions{}, layout,
                                       set, err, stop);
        ExpectTrue("nopk: not an engine error", ok && err.IsEmpty());
        ExpectTrue("nopk: set is NOT executable", !set.Executable());
        ExpectTrue("nopk: reason recorded structurally", !set.Findings().empty());
    }
}

int main()
{
    std::printf("== sync_data_test ==\n");
    TestDataSync();
    TestDiffRowChanges();
    return Report("sync_data_test");
}
