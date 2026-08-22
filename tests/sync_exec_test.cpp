// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sync_exec_test.cpp — offline unit tests for the WRITE pass:
// db::sync::ExecuteDataSync and the delete gate it makes structural
// (AuthorizeDeletes / TableDataSpec).
//
// Its sibling sync_data_test.cpp asserts on what the compare pass OBSERVED;
// this one asserts on what was SENT, through the RecordingConn in sync_stub.h.
// That is the point of it being its own TU and its own ctest target: these are
// the only assertions in the offline suite about statements that would reach a
// customer's database, and several of them are NEGATIVE — that no DELETE and no
// UPDATE went out at all. A negative claim needs a connection that remembers
// everything, so a new test here is one that inspects `executed`/`batches`.
//
// Covers: AuthorizeDeletes as the sole producer of a DeleteAuthorization,
// inserts-only never constructing a DELETE, an authorized delete targeting
// exactly the extra row, row exclusion by identity rather than position, and
// inserts leaving as parameterized batches rather than rendered literals.
#include "sync_stub.h"
#include "negative_control.h"

#include "db/DataSyncExec.h"

#include <atomic>
#include <memory>
#include <optional>

using namespace db;
using namespace db::sync;
using namespace synctest;

// ===========================================================================
//  A5. T10 — the executor, and the delete gate it makes structural.
// ===========================================================================
static void TestDataSyncExec()
{
    std::printf("[ExecuteDataSync — streaming apply + category gate]\n");
    std::atomic<bool> stop{false};

    auto makeSrc = [] {
        RecordingConn* c = new RecordingConn();
        c->dialect_ = Dialect::Postgres;
        c->rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"b") },
                     { Num(L"3"), Txt(L"c") } };
        return c;
    };
    auto makeTgt = [] {
        RecordingConn* c = new RecordingConn();
        c->dialect_ = Dialect::Postgres;
        c->rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"B") },
                     { Num(L"4"), Txt(L"d") } };
        return c;
    };

    // THE TARGET READ RUNS ON ITS OWN CONNECTION.
    //
    // ExecuteDataSync reads the target through a SECOND connection, never the
    // one it writes through — db::IConnection is not thread-safe and the merge
    // drains the target on a background thread while this thread writes (see
    // DataSyncExec.cpp's RunTableSweep for the collision that forces it).
    // Production gets that connection from db::CloneConnection, which dials a
    // real server and therefore cannot clone a fake. So this test supplies the
    // second connection itself: a fresh RecordingConn carrying the SAME rows,
    // which is exactly what a second connection to the same target would see.
    //
    // Writes still go to `tgt`, so every assertion below still inspects the
    // connection the executor actually wrote through.
    auto openRead = [&makeTgt](const IConnection&, wxString&) {
        return std::unique_ptr<IConnection>(makeTgt());
    };

    // ---- the master switch is the only producer of a DeleteAuthorization ----
    ExpectTrue("gate: no token while the master switch is off",
               !AuthorizeDeletes(false).has_value());
    ExpectTrue("gate: token available once it is on",
               AuthorizeDeletes(true).has_value());

    // ---- inserts only: the unchecked DELETE is never even constructed ------
    {
        RecordingConn* src = makeSrc(); RecordingConn* tgt = makeTgt();
        TableExecJob job;
        job.spec = TwoColSpec();
        job.allow = TableDataSpec(L"t");
        job.allow.EnableInserts();          // updates + deletes left off

        DataExecResult res; wxString err;
        const bool ok = ExecuteDataSync(*src, *tgt, { job }, DataSyncOptions{},
                                        res, err, stop, nullptr, openRead);
        ExpectTrue("exec(ins): ok", ok);
        ExpectEq("exec(ins): one table reported", (long long)res.tables.size(), 1);
        ExpectEq("exec(ins): exactly the one insert applied", res.tables[0].inserts, 1);
        ExpectEq("exec(ins): no updates applied", res.tables[0].updates, 0);
        ExpectEq("exec(ins): no deletes applied", res.tables[0].deletes, 0);
        ExpectTrue("exec(ins): committed", res.tables[0].committed && res.AllCommitted());
        // The safety property, asserted on the wire and not on an intention:
        // nothing resembling a DELETE or an UPDATE was ever sent.
        //
        // NoneStartWith() is vacuously true on an EMPTY statement log, so on its
        // own it would also pass against an executor that never opened its mouth
        // — the negative claim has to rest on a connection that demonstrably
        // received traffic. The transaction bracket is that proof: the executor
        // wraps every sweep in BEGIN/COMMIT through Execute(), so a non-empty
        // log is what makes "no DELETE among them" mean something.
        ExpectTrue("exec(ins): the target was actually talked to",
                   !tgt->executed.empty());
        ExpectTrue("exec(ins): the write was transaction-bracketed",
                   !tgt->NoneStartWith(L"BEGIN") && !tgt->NoneStartWith(L"COMMIT"));
        //
        // The two claims below are NEGATIVE, so each carries a negative control
        // (tests/negative_control.h): the same predicate is re-run against a log
        // that DID receive the forbidden verb and must come out false. Without
        // that, a NoneStartWith() that had been broken to return true
        // unconditionally would satisfy both claims forever.
        {
            RecordingConn broken;
            db::QueryResult qr; wxString ignored;
            broken.Execute(L"DELETE FROM t WHERE id=1", qr, ignored);
            broken.Execute(L"UPDATE t SET a=1", qr, ignored);
            EXPECT_NEGATIVE("exec(ins): NO DELETE reached the target",
                            tgt->NoneStartWith(L"DELETE"),
                            broken.NoneStartWith(L"DELETE"));
            EXPECT_NEGATIVE("exec(ins): NO UPDATE reached the target",
                            tgt->NoneStartWith(L"UPDATE"),
                            broken.NoneStartWith(L"UPDATE"));
        }
        // Inserts go out parameterized and batched, never as rendered literals.
        ExpectEq("exec(ins): one batch", (long long)tgt->batches.size(), 1);
        ExpectEq("exec(ins): carrying one row", (long long)tgt->batchRows, 1);
        ExpectTrue("exec(ins): batch head is a VALUES-terminated INSERT",
                   !tgt->batches.empty() &&
                   tgt->batches[0].first.StartsWith(L"INSERT INTO") &&
                   tgt->batches[0].first.EndsWith(L"VALUES"));
        delete src; delete tgt;
    }

    // ---- deletes authorized: now, and only now, a DELETE goes out ----------
    {
        RecordingConn* src = makeSrc(); RecordingConn* tgt = makeTgt();
        std::optional<DeleteAuthorization> token = AuthorizeDeletes(true);
        TableExecJob job;
        job.spec = TwoColSpec();
        job.allow = TableDataSpec(L"t");
        job.allow.EnableDeletes(*token);    // inserts/updates left off

        DataExecResult res; wxString err;
        const bool ok = ExecuteDataSync(*src, *tgt, { job }, DataSyncOptions{},
                                        res, err, stop, nullptr, openRead);
        ExpectTrue("exec(del): ok", ok);
        ExpectEq("exec(del): the one extra target row deleted", res.tables[0].deletes, 1);
        ExpectEq("exec(del): nothing inserted", res.tables[0].inserts, 0);
        ExpectEq("exec(del): no insert batch issued", (long long)tgt->batches.size(), 0);
        bool sawDelete = false;
        for (const wxString& s : tgt->executed)
            if (s.StartsWith(L"DELETE FROM \"t\" WHERE \"id\" = 4")) sawDelete = true;
        ExpectTrue("exec(del): DELETE targets exactly the extra row", sawDelete);
        delete src; delete tgt;
    }

    // ---- a row the user unchecked is skipped by identity, not by position --
    {
        RecordingConn* src = makeSrc(); RecordingConn* tgt = makeTgt();
        TableExecJob job;
        job.spec = TwoColSpec();
        job.allow = TableDataSpec(L"t");
        job.allow.EnableInserts();
        job.allow.ExcludeRow(EncodeRowKey({ L"id" }, { Num(L"3") }));

        DataExecResult res; wxString err;
        ExecuteDataSync(*src, *tgt, { job }, DataSyncOptions{}, res, err, stop,
                        nullptr, openRead);
        ExpectEq("exec(excl): the excluded row was not applied", res.tables[0].inserts, 0);
        ExpectEq("exec(excl): and is counted as excluded", res.tables[0].excluded, 1);
        ExpectEq("exec(excl): no batch issued at all", (long long)tgt->batches.size(), 0);
        delete src; delete tgt;
    }
}

int main()
{
    std::printf("== sync_exec_test ==\n");
    TestDataSyncExec();
    return Report("sync_exec_test");
}
