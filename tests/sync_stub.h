// sync_stub.h — shared harness for the offline cross-database synchronization
// test suite (sync_schema_test / sync_data_test / sync_exec_test /
// sync_plan_test).
//
// These four TUs were one file, sync_test.cpp, until it reached the charter's
// 1000-line ceiling. The split is by SUBJECT, not by line count — structural
// diff, data compare, data write, plan orchestration — which is the same
// compare/write vocabulary the live suites already use. What they share is the
// stuff below, and it lives here exactly once so a stub's behaviour can never
// drift between the suite that reads it and the suite that writes through it.
//
// Everything here is offline: DiffSchema is a pure function over TableSchema
// value objects, and StubConn is a hand-written IConnection serving canned
// schema + rows, so no DB client library is exercised by any consumer.
//
// Same dependency-free assert loop as sqlscript_test.cpp / tableio_test.cpp
// (ExpectTrue/ExpectStr/ExpectEq, non-zero exit on any failure, no doctest /
// Catch2). The counters are `inline` so each test binary owns one copy and
// prints its own total — mirroring mysql_pg_live.h.
#pragma once

#include "db/SchemaDiff.h"
#include "db/DataSync.h"
#include "db/DbDriver.h"
#include "db/SyncCellCompare.h"   // CmpCell — the merge's own key comparator

#include <cstdio>
#include <map>
#include <utility>
#include <vector>
#include <wx/string.h>

namespace synctest {

// ---- check counters --------------------------------------------------------
inline int g_checks = 0;
inline int g_fails  = 0;

inline wxString Vis(const wxString& s)
{
    wxString o = s;
    o.Replace(L"\r", L"\\r");
    o.Replace(L"\n", L"\\n");
    return o;
}

inline void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

inline void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)Vis(want).utf8_str(),
                    (const char*)Vis(got).utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

inline void ExpectEq(const char* name, long long got, long long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%lld got=%lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// Every suite's main() ends the same way; keeping it here means the exit-code
// contract (non-zero on any failure) cannot be got wrong in a new sibling TU.
inline int Report(const char* suite)
{
    std::printf("== %s: %d checks, %d failed ==\n", suite, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

// ---- small builders for TableSchema value objects --------------------------

inline db::NormColumn Col(const wxString& name, db::ColKind kind,
                          const wxString& raw, long long length = -1,
                          int scale = -1)
{
    db::NormColumn c;
    c.name    = name;
    c.kind    = kind;
    c.rawType = raw;
    c.length  = length;
    c.scale   = scale;
    return c;
}

// The fixture behind almost every data test: two columns, `id` as the PK.
inline db::TableSchema TwoColSchema()
{
    db::TableSchema s;
    s.name = L"t";
    s.columns.push_back(Col(L"id",   db::ColKind::Integer, L"int"));
    s.columns.push_back(Col(L"name", db::ColKind::Varchar, L"varchar(50)", 50));
    s.primaryKey = { L"id" };
    return s;
}

inline db::sync::DataDiffSpec TwoColSpec()
{
    db::sync::DataDiffSpec s;
    s.srcDb     = L"s";
    s.tgtDb     = L"t";
    s.srcSchema = TwoColSchema();
    s.tgtSchema = TwoColSchema();
    return s;
}

// Count changes of a given op in a change set.
inline int CountCol(const db::SchemaChangeSet& ch,
                    db::ColumnChange::Op op)
{
    int n = 0;
    for (const auto& c : ch.columns) if (c.op == op) ++n;
    return n;
}
inline int CountIdx(const db::SchemaChangeSet& ch,
                    db::IndexChange::Op op)
{
    int n = 0;
    for (const auto& c : ch.indexes) if (c.op == op) ++n;
    return n;
}
inline int CountFk(const db::SchemaChangeSet& ch, db::FkChange::Op op)
{
    int n = 0;
    for (const auto& c : ch.foreignKeys) if (c.op == op) ++n;
    return n;
}
inline bool HasWarning(const std::vector<wxString>& ws, const wxString& needle)
{
    for (const auto& w : ws) if (w.Contains(needle)) return true;
    return false;
}
// A4: SchemaDiffResult.warnings / SchemaDelta.findings are now typed Finding,
// not free text — this overload checks Finding.reason the same way.
inline bool HasWarning(const std::vector<db::sync::Finding>& fs,
                       const wxString& needle)
{
    for (const auto& f : fs) if (f.reason.Contains(needle)) return true;
    return false;
}

// ---- Cell helpers ----------------------------------------------------------
inline db::Cell Num(const wxString& t) { return db::Cell{ db::CellKind::Numeric, t }; }
inline db::Cell Txt(const wxString& t) { return db::Cell{ db::CellKind::Text, t }; }

// ===========================================================================
//  Stub IConnection — no wire protocol, just canned schema + row streams.
// ===========================================================================
class StubConn : public db::IConnection {
public:
    db::Dialect dialect_ = db::Dialect::Postgres;
    std::vector<db::TableInfo>              tableList_;   // drives ListTables
    std::map<wxString, db::TableSchema>     schemas_;     // drives GetTableSchema
    std::vector<std::vector<db::Cell>>      rows_;        // drives StreamRows

    // ---- the interesting overrides ----
    db::Dialect GetDialect() const override { return dialect_; }

    bool ListTables(const wxString&, std::vector<db::TableInfo>& out,
                    wxString&) override
    {
        out = tableList_;
        return true;
    }

    bool GetTableSchema(const wxString&, const db::QualifiedName& table,
                        db::TableSchema& out, wxString&) override
    {
        // schemas_ is keyed by bare name (these stubs model a single-namespace
        // engine); the interface now hands an identity, so ask by its name half.
        auto it = schemas_.find(table.Name());
        out = (it != schemas_.end()) ? it->second : db::TableSchema{};
        if (it == schemas_.end()) out.name = table;
        return true;
    }

    bool RenderSchemaChange(const db::SchemaChangeSet& ch,
                            std::vector<wxString>& stmts, wxString&) override
    {
        // A recognizable single statement per change set; enough for the
        // orchestration tests (ordering / preamble) which don't inspect DDL text.
        if (ch.createTable) stmts.push_back(L"CREATE TABLE " + ch.table);
        else                stmts.push_back(L"ALTER TABLE " + ch.table);
        return true;
    }

    // Captured from the last StreamRows call, so a test CAN assert what the
    // diff actually asked the server for (binaryOrder, binaryOrderCols, the
    // projection). Mirrors DataSyncOrderingTests' FakeServer::lastOpt_.
    db::StreamOptions lastOpt_;

    // ---- WHAT THIS STUB DOES NOT DO, enforced rather than merely stated ----
    //
    // StubConn serves `rows_` VERBATIM, in whatever order the test author wrote
    // them. It does not sort, it does not page, and it models no collation. So
    // it structurally CANNOT express the defect class that ordering is about:
    // two servers returning the same rows in DIFFERENT orders. That is this
    // project's worst historical bug — a desynced sorted merge that emitted 10
    // spurious INSERTs and 10 DELETEs from 11 identical rows, reported success,
    // and corrupted the target. Reproducing it needs a connection that actually
    // models a collation and can be told to IGNORE binaryOrder: that is
    // DataSyncOrderingTests.cpp's FakeServer. Ordering tests belong there. Do
    // not reimplement FakeServer here.
    //
    // A comment saying so would be archived and forgotten, and an ordering test
    // written against this stub would come back GREEN with zero coverage. So the
    // stub instead CHECKS the precondition it silently relied on: the rows it is
    // about to serve must already be sorted by the key the caller asked to order
    // by, compared with CmpCell — the very comparator the merge compares with.
    // Violate it and the suite fails and the call fails; you cannot walk into
    // the blind spot quietly.
    bool StreamRows(const wxString&, const db::QualifiedName&,
                    const db::StreamOptions& opt, const db::RowSink& sink,
                    wxString& err) override
    {
        lastOpt_ = opt;
        if (!CheckServableOrder(opt, err)) return false;
        for (const auto& r : rows_)
            if (!sink(r)) return true;   // sink asked to stop (cancellation)
        return true;
    }

    // ---- minimal bodies for the remaining pure virtuals ----
    bool Connect(const core::ConnectionProfile&, wxString&) override { return true; }
    void Disconnect() override {}
    bool IsConnected() const override { return true; }
    bool Execute(const wxString&, db::QueryResult&, wxString&) override { return true; }
    bool ListDatabases(std::vector<wxString>&, wxString&) override { return true; }
    bool GetColumns(const wxString&, const wxString&, std::vector<db::ColumnInfo>&,
                    wxString&) override { return true; }
    bool GetForeignKeys(const wxString&, const wxString&,
                        std::vector<db::ForeignKey>&, wxString&) override { return true; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<db::IndexInfo>&,
                    wxString&) override { return true; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&, wxString&)
        override { return true; }
    wxString ServerVersion() const override { return L"stub"; }

private:
    // Resolve each orderBy column onto its cell index. `opt.columns` IS the
    // projection the rows are served in, so it maps names to positions exactly;
    // an empty projection means "all columns, schema order", which for these
    // hand-written fixtures is the order rows_ is already written in, so the
    // leading cells are the key.
    static std::vector<size_t> KeyIndices(const db::StreamOptions& opt)
    {
        std::vector<size_t> idx;
        for (const wxString& k : opt.orderBy) {
            size_t at = idx.size();                 // positional fallback
            for (size_t i = 0; i < opt.columns.size(); ++i)
                if (opt.columns[i] == k) { at = i; break; }
            idx.push_back(at);
        }
        return idx;
    }

    bool CheckServableOrder(const db::StreamOptions& opt, wxString& err)
    {
        // Paging this stub does not implement. Serving every row while the
        // caller believes it asked for one chunk would fake a working pager.
        if (!opt.keyLowExcl.IsEmpty() || opt.limit >= 0) {
            ExpectTrue("StubConn: caller requested paging (keyLowExcl/limit) "
                       "that this stub does not implement", false);
            err = L"StubConn ignores StreamOptions paging; it cannot answer a "
                  L"chunked read.";
            return false;
        }

        const std::vector<size_t> key = KeyIndices(opt);
        if (key.empty()) return true;   // no particular order requested

        for (size_t r = 1; r < rows_.size(); ++r) {
            int c = 0;
            for (size_t k : key) {
                if (k >= rows_[r].size() || k >= rows_[r - 1].size()) break;
                c = db::sync::CmpCell(rows_[r - 1][k], rows_[r][k]);
                if (c != 0) break;
            }
            if (c > 0) {
                ExpectTrue("StubConn: canned rows_ must already be sorted by the "
                           "requested key — this stub models no collation and "
                           "cannot express an ordering defect; write ordering "
                           "tests against FakeServer in DataSyncOrderingTests.cpp",
                           false);
                err = wxString::Format(
                    L"StubConn: rows_[%zu] sorts before rows_[%zu] under the "
                    L"requested key order.", r, r - 1);
                return false;
            }
        }
        return true;
    }
};

// Records everything the executor sends, so a test can assert on what did NOT
// go out as well as what did — which is the whole point for the delete gate.
class RecordingConn : public StubConn {
public:
    std::vector<wxString>                    executed;   // single statements
    std::vector<std::pair<wxString, size_t>> batches;    // (head, row count)
    size_t                                   batchRows = 0;

    bool Execute(const wxString& sql, db::QueryResult&, wxString&) override
    {
        executed.push_back(sql);
        return true;
    }

    bool ExecuteBatch(const wxString& sql,
                      const std::vector<std::vector<db::Cell>>& rows,
                      wxString&) override
    {
        batches.push_back({ sql, rows.size() });
        batchRows += rows.size();
        return true;
    }

    // True when no statement sent begins with `verb`.
    bool NoneStartWith(const wxString& verb) const
    {
        for (const wxString& s : executed)
            if (s.Upper().StartsWith(verb)) return false;
        return true;
    }
};

}   // namespace synctest
