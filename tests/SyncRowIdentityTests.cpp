// SyncRowIdentityTests.cpp — the two things a db::sync::RowChange must carry
// besides the values it will write: what was there BEFORE, and WHICH row it is.
//
// Both were gaps found while wiring ADR-015's compare screen, and both are of
// the silent-and-data-affecting class this feature exists to eliminate:
//
//   1. PRIOR VALUES. The user's requirement is "click into a table and see what
//      differs". For an UPDATE that comparison IS before-vs-after, but the merge
//      threw the target row away the moment it had classified the row, so the
//      drill-in could only ever render the new value. Covered here: prior values
//      arrive, are aligned with Values(), are NOT run through value conversion
//      (they are target-native display cells), and — the memory contract —
//      exist ONLY for rows that entered the <=500-row sample.
//
//   2. THE ROW KEY. StreamRowChanges encodes the row's identity over the RAW,
//      pre-bridge key cells and hands it to the sink; RowChange::Key() holds
//      POST-conversion cells. Re-deriving the identity from Key() therefore
//      reproduces the original encoding only while every key bridge is
//      passthrough. The headline test below builds a genuinely coerced
//      cross-engine key (integer -> PostgreSQL boolean, which rewrites 1 into
//      TRUE) and pins that the retained RowKey() still matches the sink's, while
//      the re-derived one does NOT — i.e. it pins the bug that would otherwise
//      make a user's row-level exclusion silently miss and sync the row anyway.
//
// Offline by construction: a canned-row IConnection stub, no server, no GUI.
// Same dependency-free harness as sync_test.cpp.
#include "db/DataSync.h"
#include "db/DbDriver.h"

#include <atomic>
#include <cstdio>
#include <vector>
#include <wx/string.h>

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
        std::printf("  FAIL %s\n    want: %lld\n    got : %lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ===========================================================================
//  Stub IConnection — canned rows, no wire protocol.
// ===========================================================================
class StubConn : public IConnection {
public:
    Dialect                        dialect_ = Dialect::Postgres;
    std::vector<std::vector<Cell>> rows_;

    Dialect GetDialect() const override { return dialect_; }

    bool StreamRows(const wxString&, const QualifiedName&, const StreamOptions&,
                    const RowSink& sink, wxString&) override
    {
        for (const auto& r : rows_)
            if (!sink(r)) return true;
        return true;
    }

    bool Connect(const core::ConnectionProfile&, wxString&) override { return true; }
    void Disconnect() override {}
    bool IsConnected() const override { return true; }
    bool Execute(const wxString&, QueryResult&, wxString&) override { return true; }
    bool ListDatabases(std::vector<wxString>&, wxString&) override { return true; }
    bool ListTables(const wxString&, std::vector<TableInfo>&, wxString&) override { return true; }
    bool GetColumns(const wxString&, const wxString&, std::vector<ColumnInfo>&,
                    wxString&) override { return true; }
    bool GetForeignKeys(const wxString&, const wxString&, std::vector<ForeignKey>&,
                        wxString&) override { return true; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<IndexInfo>&,
                    wxString&) override { return true; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&, wxString&)
        override { return true; }
    wxString ServerVersion() const override { return L"stub"; }
};

static Cell Num(const wxString& t) { return Cell{ CellKind::Numeric, t }; }
static Cell Txt(const wxString& t) { return Cell{ CellKind::Text, t }; }

static NormColumn Col(const wxString& name, ColKind kind, const wxString& raw)
{
    NormColumn c; c.name = name; c.kind = kind; c.rawType = raw;
    return c;
}

// `id` int PK + `v` text payload, identical on both sides.
static TableSchema TwoColSchema()
{
    TableSchema s;
    s.name = L"orders";
    s.columns.push_back(Col(L"id", ColKind::Integer, L"int"));
    s.columns.push_back(Col(L"v",  ColKind::Varchar, L"varchar(40)"));
    s.primaryKey.push_back(L"id");
    return s;
}

static DataDiffSpec TwoColSpec()
{
    DataDiffSpec s;
    s.srcDb = L"s"; s.tgtDb = L"t";
    s.srcSchema = TwoColSchema();
    s.tgtSchema = TwoColSchema();
    return s;
}

static const RowChange* FindOp(const RowChangeSet& set, RowChange::Op op)
{
    for (const RowChange& r : set.Sample()) if (r.Operation() == op) return &r;
    return nullptr;
}

// ===========================================================================
//  1. Prior values reach the sample, aligned and unconverted
// ===========================================================================
static void TestPriorValues()
{
    std::printf("\n[1] UPDATE rows carry the target's BEFORE values\n");
    std::atomic<bool> stop{false};

    StubConn src, tgt;
    src.dialect_ = tgt.dialect_ = Dialect::Postgres;
    src.rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"b") },
                  { Num(L"3"), Txt(L"c") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"a") }, { Num(L"2"), Txt(L"B") },
                  { Num(L"4"), Txt(L"d") } };

    DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
    RowLayout layout; RowChangeSet set; wxString err;
    ExpectTrue("compare pass ok",
               DiffRowChanges(src, tgt, TwoColSpec(), opt, layout, set, err, stop));
    ExpectEq("one update detected", set.Stat().updates, 1);

    const RowChange* up = FindOp(set, RowChange::Op::Update);
    ExpectTrue("update row is in the sample", up != nullptr);
    if (up) {
        // The whole point of the fix: a real before/after per column.
        ExpectTrue("update HAS prior values", up->HasPriorValues());
        ExpectEq("prior values are aligned with Values()",
                 (long long)up->PriorValues().size(), (long long)up->Values().size());
        ExpectEq("...and with layout.columns",
                 (long long)up->PriorValues().size(), (long long)layout.columns.size());
        ExpectStr("before: v was B", up->PriorValues()[1].text, L"B");
        ExpectStr("after : v becomes b", up->Values()[1].text, L"b");
        ExpectStr("row key is the canonical encoding", up->RowKey(), L"id=2");
    }

    // An INSERT has no "before" (the row does not exist in the target yet) and a
    // DELETE's Values() are empty, so a before/after pair would be meaningless.
    // Both stay honestly empty rather than being filled with something plausible.
    const RowChange* ins = FindOp(set, RowChange::Op::Insert);
    const RowChange* del = FindOp(set, RowChange::Op::Delete);
    ExpectTrue("insert carries NO prior values", ins && !ins->HasPriorValues());
    ExpectTrue("delete carries NO prior values", del && !del->HasPriorValues());
}

// ===========================================================================
//  2. The memory contract: prior values exist for SAMPLED rows only
// ===========================================================================
static void TestPriorValuesAreSampleOnly()
{
    std::printf("\n[2] prior values are materialized for the sample only\n");
    std::atomic<bool> stop{false};

    StubConn src, tgt;
    src.dialect_ = tgt.dialect_ = Dialect::Postgres;
    // 600 rows present on both sides, every one of them differing => 600
    // UPDATEs, i.e. 100 more than the sample cap.
    for (int i = 0; i < 600; ++i) {
        const wxString id = wxString::Format(L"%06d", i);
        src.rows_.push_back({ Num(id), Txt(L"new") });
        tgt.rows_.push_back({ Num(id), Txt(L"old") });
    }

    DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
    RowLayout layout; RowChangeSet set; wxString err;
    ExpectTrue("compare pass ok",
               DiffRowChanges(src, tgt, TwoColSpec(), opt, layout, set, err, stop));

    ExpectEq("all 600 updates COUNTED", set.Stat().updates, 600);
    ExpectEq("only kSampleCap rows retained", (long long)set.Sample().size(),
             (long long)RowChangeSet::kSampleCap);
    ExpectTrue("truncated flag set", set.Truncated());

    // The observable form of "the full scan retains nothing": total prior cells
    // held anywhere == sample size x column count, never row count x column
    // count. Rows past the cap were counted and dropped, carrying nothing.
    long long priorCells = 0, withPrior = 0;
    for (const RowChange& r : set.Sample()) {
        priorCells += (long long)r.PriorValues().size();
        if (r.HasPriorValues()) ++withPrior;
    }
    ExpectEq("every sampled update has prior values", withPrior,
             (long long)RowChangeSet::kSampleCap);
    ExpectEq("prior cells retained == sample x columns", priorCells,
             (long long)(RowChangeSet::kSampleCap * layout.columns.size()));
}

// ===========================================================================
//  3. The execute pass pays nothing for display data
// ===========================================================================
static void TestExecutePathKeepsNoPriorValues()
{
    std::printf("\n[3] a builder that nobody sampled copies no prior row\n");

    // This is exactly the shape of DataSyncExec's sink: it calls Build() itself
    // and never goes through RowChangeSet::Accept, so KeepPriorValues is never
    // turned on and the attached target row is never copied. If the default ever
    // flips, a 500k-row apply starts allocating a vector per row for data no one
    // reads, and this check fails.
    ColumnBridge b0; b0.srcIdx = 0; b0.column = L"id"; b0.passthrough = true;
    ColumnBridge b1; b1.srcIdx = 1; b1.column = L"v";  b1.passthrough = true;

    const std::vector<Cell> targetRow{ Num(L"7"), Txt(L"old") };

    RowChangeBuilder b(RowChange::Op::Update, L"orders", L"id=7");
    b.AttachPriorValues(&targetRow);
    b.AddKeyCell(Num(L"7"), b0);
    b.AddCell(Num(L"7"), b0);
    b.AddCell(Txt(L"new"), b1);

    std::optional<RowChange> row = b.Build();
    ExpectTrue("row builds", row.has_value());
    if (row) {
        ExpectTrue("streaming consumer retains NO prior values",
                   !row->HasPriorValues());
        // The identity is still carried — it costs one already-existing string.
        ExpectStr("...but still carries its row key", row->RowKey(), L"id=7");
    }
}

// ===========================================================================
//  4. THE HEADLINE: the retained key survives a NON-PASSTHROUGH key bridge
// ===========================================================================
//
// A cross-engine key that is genuinely rewritten on the way to the target:
// source `flag` is an integer, target `flag` is a PostgreSQL boolean, so
// BuildBridges arms BridgeOp::BoolToPgBool and ConvertCell turns the cell 1 into
// the literal TRUE. (tinyint(1) -> boolean is the real-world shape; a plain
// `int` source is used so the fixture does not depend on how any one dialect
// profile special-cases tinyint(1).)
//
// The encoding the sink emitted is "flag=1". Re-deriving it from
// RowChange::Key() would give "flag=TRUE" — a different string, so a row-level
// exclusion recorded by the UI would not match and the row would sync anyway.
static void TestRowKeySurvivesCoercedKeyBridge()
{
    std::printf("\n[4] retained row key vs a coerced cross-engine key\n");
    std::atomic<bool> stop{false};

    TableSchema srcSchema;
    srcSchema.name = L"flags";
    srcSchema.columns.push_back(Col(L"flag", ColKind::Integer, L"int"));
    srcSchema.columns.push_back(Col(L"n",    ColKind::Integer, L"int"));
    srcSchema.primaryKey.push_back(L"flag");

    TableSchema tgtSchema = srcSchema;
    tgtSchema.columns[0] = Col(L"flag", ColKind::Boolean, L"boolean");

    DataDiffSpec spec;
    spec.srcDb = L"s"; spec.tgtDb = L"t";
    spec.srcSchema = srcSchema;
    spec.tgtSchema = tgtSchema;

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;
    // Same key on both sides (the merge compares raw cells positionally, so the
    // fixture keeps the key cell identical) and a differing payload => one
    // UPDATE whose KEY goes through the coercing bridge.
    src.rows_ = { { Num(L"1"), Num(L"5") } };
    tgt.rows_ = { { Num(L"1"), Num(L"9") } };

    RowLayout layout;
    std::vector<Finding> findings;
    ExpectTrue("layout builds cross-engine",
               BuildRowLayout(spec, src.GetDialect(), tgt.GetDialect(), layout,
                              findings));
    ExpectEq("one PK column", (long long)layout.pkColumns.size(), 1);
    // If this ever becomes passthrough the test below stops proving anything,
    // so it is asserted rather than assumed.
    ExpectTrue("the key bridge really does transform the key",
               !layout.keyBridges.empty() && !layout.keyBridges[0].passthrough);

    DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
    RowChangeSet set;
    std::vector<wxString> sinkKeys;
    RowStat stat; wxString err;

    const bool ok = StreamRowChanges(src, tgt, spec, layout, opt,
        [&](RowChangeBuilder&& b, const wxString& rowKey) -> bool {
            sinkKeys.push_back(rowKey);
            set.Accept(std::move(b));
            return true;
        }, stat, err, stop);

    ExpectTrue("stream ok", ok);
    ExpectEq("one update", stat.updates, 1);
    ExpectEq("sink fired once", (long long)sinkKeys.size(), 1);
    ExpectEq("one row sampled", (long long)set.Sample().size(), 1);
    if (sinkKeys.empty() || set.Sample().empty()) return;

    const RowChange& r = set.Sample().front();

    // (a) The bridge really did rewrite the key cell.
    ExpectStr("converted key cell is the PG boolean literal",
              r.Key()[0].text, L"TRUE");
    ExpectTrue("row verdict is Coerced", r.Verdict() == ValueVerdict::Coerced);

    // (b) The sink's encoding is over the RAW cells.
    ExpectStr("sink emitted the raw-cell encoding", sinkKeys[0], L"flag=1");

    // (c) THE FIX: the retained key IS the sink's, byte for byte.
    ExpectStr("RowChange::RowKey() == the sink's rowKey", r.RowKey(), sinkKeys[0]);

    std::vector<Cell> rawKey{ Num(L"1") };
    ExpectStr("...and == EncodeRowKey over the raw key cells",
              r.RowKey(), EncodeRowKey(layout.pkColumns, rawKey));

    // (d) THE BUG, pinned: re-deriving the key from the post-conversion cells —
    // which is the only thing a consumer could have done before this fix —
    // produces a DIFFERENT string. Any exclusion matched that way misses.
    const wxString rederived = EncodeRowKey(layout.pkColumns, r.Key());
    ExpectStr("re-derived key is the wrong string", rederived, L"flag=TRUE");
    ExpectTrue("re-derived key does NOT match the authoritative one",
               rederived != r.RowKey());

    // (e) Prior values are the TARGET's own cells, untouched by the src->tgt
    // bridge: the target holds 1, not the TRUE the source value was rewritten
    // into. Converting them would be converting a value that was never on the
    // source side.
    ExpectTrue("update carries prior values", r.HasPriorValues());
    if (r.HasPriorValues()) {
        ExpectStr("prior key cell is target-native, NOT converted",
                  r.PriorValues()[0].text, L"1");
        ExpectStr("prior payload is the target's current value",
                  r.PriorValues()[1].text, L"9");
        ExpectStr("the value being written is the source's",
                  r.Values()[1].text, L"5");
    }
}

int main()
{
    std::printf("== RowChange identity + prior-value tests (ADR-015) ==\n");
    TestPriorValues();
    TestPriorValuesAreSampleOnly();
    TestExecutePathKeepsNoPriorValues();
    TestRowKeySurvivesCoercedKeyBridge();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
