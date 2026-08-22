// SyncCrossEngineCompareTests.cpp — the data diff must compare cells the way the
// two ENGINES mean them, not the way their drivers spell them.
//
// The defect this file pins, found during the ADR-015 integration. The merge in
// DataSync.cpp compared source and target cells RAW — before any ColumnBridge
// was applied — even though the whole value-conversion layer (SyncValueMap.h)
// already existed and was used when EMITTING. A MySQL `tinyint(1)` holding 1 and
// a PostgreSQL `boolean` holding `t` are the same logical value; raw, they are
// "1" and "t". Two silent consequences, both covered below:
//
//   1. NON-KEY column -> a false-positive UPDATE storm. Every boolean column in
//      a cross-engine comparison reported a difference on rows that were
//      identical. That directly breaks the release-blocking zero-false-positive
//      bar: a user comparing genuinely identical data saw a screen full of fake
//      updates and would rightly stop trusting the tool.
//   2. KEY column -> identity failure. The same row on both sides did not match
//      as the same key, so instead of one UPDATE (or nothing) the merge emitted
//      DELETE + INSERT — silently rewriting rows that needed no touching.
//
// The fix canonicalizes BOTH sides into a neutral form (SyncCellCompare.h)
// rather than bridging one side into the other's space, because ConvertCell's
// output is SQL-LITERAL spelling ("TRUE") and not the target's read-back
// spelling ("t") — bridging would have left the false positive in place. What
// this file must therefore also prove is that the fix did not buy correctness
// with a desynced merge: the canonicalization is order-preserving on each side,
// so the sorted merge's ordering assumption still holds.
//
// WHAT IS NOT PROVEN HERE. There is no live MySQL or PostgreSQL on this host, so
// the cell TEXT each driver would really produce for a `tinyint(1)` and a
// `boolean` is assumed, not observed: `1`/`0` (CellKind::Numeric) for MySQL and
// `t`/`f` (CellKind::Text) for PostgreSQL, which is libpq's text-format output.
// The canonicalizer also accepts `true`/`false`/`y`/`n`, so a driver that spells
// it differently is covered — but only a live pair can confirm which spelling
// actually arrives. That remains an integration/QA item.
//
// Same dependency-free harness as sync_test.cpp / SyncRowIdentityTests.cpp.
#include "db/DataSync.h"
#include "db/DbDriver.h"
#include "db/SyncCellCompare.h"

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

// `id` int PK + `flag` boolean-ish payload. The MySQL side spells the payload
// `tinyint(1)`, the PostgreSQL side spells it `boolean` — the exact pair the
// defect report names.
static TableSchema MySqlFlagSchema()
{
    TableSchema s;
    s.name = L"accounts";
    s.columns.push_back(Col(L"id",   ColKind::Integer, L"int"));
    s.columns.push_back(Col(L"flag", ColKind::Boolean, L"tinyint(1)"));
    s.primaryKey.push_back(L"id");
    return s;
}

static TableSchema PgFlagSchema()
{
    TableSchema s = MySqlFlagSchema();
    s.columns[1] = Col(L"flag", ColKind::Boolean, L"boolean");
    return s;
}

static DataDiffSpec FlagSpec()
{
    DataDiffSpec s;
    s.srcDb = L"s"; s.tgtDb = L"t";
    s.srcSchema = MySqlFlagSchema();
    s.tgtSchema = PgFlagSchema();
    return s;
}

// Runs the compare pass and reports the three counters.
struct DiffResult {
    bool     ok = false;
    RowStat  stat;
    wxString err;
    bool     executable = true;
    wxString firstBlockingReason;
};

// Goes through BuildRowLayout + StreamRowChanges rather than DiffRowChanges so
// the full RowStat — including `unchanged`, which RowChangeSet does not carry —
// is observable. It is the same merge either way: DiffRowChanges is a thin
// wrapper over exactly these two calls.
static DiffResult RunDiff(StubConn& src, StubConn& tgt, const DataDiffSpec& spec)
{
    std::atomic<bool> stop{false};
    DataSyncOptions opt; opt.insert = opt.update = opt.deleteMissing = true;
    RowLayout layout; RowChangeSet set;
    DiffResult r;
    std::vector<Finding> findings;
    if (!BuildRowLayout(spec, src.GetDialect(), tgt.GetDialect(), layout, findings)) {
        r.ok = false;
        r.executable = false;
        for (const Finding& f : findings)
            if (!MayAutoAlter(f.verdict)) { r.firstBlockingReason = f.reason; break; }
        return r;
    }
    r.ok = StreamRowChanges(src, tgt, spec, layout, opt,
        [&](RowChangeBuilder&& b, const wxString&) -> bool {
            set.Accept(std::move(b));
            return true;
        }, r.stat, r.err, stop);
    r.executable = set.Executable();
    for (const Finding& f : set.Findings())
        if (!MayAutoAlter(f.verdict)) { r.firstBlockingReason = f.reason; break; }
    return r;
}

// ===========================================================================
//  1. THE ZERO-FALSE-POSITIVE BAR — coerced NON-KEY column, identical rows
// ===========================================================================
// Five rows, identical in meaning on both sides, differing only in how each
// engine spells a boolean. Before the fix this reported five UPDATEs; the bar
// says it must report zero differences.
static void TestNonKeyBoolCoercionIsNotADifference()
{
    std::printf("\n[1] tinyint(1) vs boolean in a NON-KEY column: zero differences\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    // MySQL hands back 0/1 as numerics; PostgreSQL hands back f/t as text. Same
    // rows, same meaning, different spelling — and different CellKind too, which
    // the old kind-sensitive equality would have tripped on by itself.
    src.rows_ = { { Num(L"1"), Num(L"1") }, { Num(L"2"), Num(L"0") },
                  { Num(L"3"), Num(L"1") }, { Num(L"4"), Num(L"0") },
                  { Num(L"5"), Num(L"1") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"t") }, { Num(L"2"), Txt(L"f") },
                  { Num(L"3"), Txt(L"t") }, { Num(L"4"), Txt(L"f") },
                  { Num(L"5"), Txt(L"t") } };

    const DiffResult r = RunDiff(src, tgt, FlagSpec());

    ExpectTrue("compare pass ok", r.ok);
    if (!r.ok) std::printf("       err: [%s]\n", (const char*)r.err.utf8_str());
    ExpectEq("ZERO false-positive UPDATEs", r.stat.updates, 0);
    ExpectEq("zero INSERTs",                r.stat.inserts, 0);
    ExpectEq("zero DELETEs",                r.stat.deletes, 0);
    ExpectEq("all five rows unchanged",     r.stat.unchanged, 5);
}

// ===========================================================================
//  2. A GENUINE difference in a coerced column is still ONE UPDATE
// ===========================================================================
// The fix must not achieve zero false positives by going blind. Row 2 really
// differs (MySQL 0 vs PostgreSQL t) and must surface as exactly one UPDATE —
// and NOT as a DELETE + INSERT pair, which is what a key-identity failure would
// have produced.
static void TestRealDifferenceInCoercedColumn()
{
    std::printf("\n[2] MySQL 0 vs PostgreSQL t: one UPDATE, not DELETE+INSERT\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    src.rows_ = { { Num(L"1"), Num(L"1") }, { Num(L"2"), Num(L"0") },
                  { Num(L"3"), Num(L"1") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"t") }, { Num(L"2"), Txt(L"t") },
                  { Num(L"3"), Txt(L"t") } };

    const DiffResult r = RunDiff(src, tgt, FlagSpec());

    ExpectTrue("compare pass ok", r.ok);
    ExpectEq("exactly one UPDATE",   r.stat.updates,   1);
    ExpectEq("no INSERT invented",   r.stat.inserts,   0);
    ExpectEq("no DELETE invented",   r.stat.deletes,   0);
    ExpectEq("the other two rows are unchanged", r.stat.unchanged, 2);
    // The row is genuinely emittable — MySQL 0 -> PG FALSE is a Coerced verdict,
    // not a refusal — so the set must stay executable.
    ExpectTrue("set is executable", r.executable);
}

// ===========================================================================
//  3. THE KEY COLUMN — the identity failure, and the decision taken
// ===========================================================================
// DECISION: coerced keys are SUPPORTED, not refused, whenever the
// canonicalization is provably order-preserving on both sides — which is the
// case for every coercion that exists today (see the CompareOp proofs in
// SyncCellCompare.h). Refusing them would have been the honest fallback, but it
// is not needed here: mapping PostgreSQL's {f,t} onto {0,1} and MySQL's tinyint
// onto itself are both MONOTONE and INJECTIVE, so each server's stream is still
// sorted by the comparator the merge uses.
//
// The composite key below is the realistic shape (a boolean alone can key at
// most two rows): `tenant` int + `active` tinyint(1)/boolean. Both sides hold
// the same four rows. Before the fix, "1" vs "t" never compared equal, so the
// merge walked past every row and produced 4 DELETEs + 4 INSERTs.
static TableSchema MySqlCompositeKeySchema()
{
    TableSchema s;
    s.name = L"memberships";
    s.columns.push_back(Col(L"tenant", ColKind::Integer, L"int"));
    s.columns.push_back(Col(L"active", ColKind::Boolean, L"tinyint(1)"));
    s.columns.push_back(Col(L"note",   ColKind::Varchar, L"varchar(40)"));
    s.primaryKey.push_back(L"tenant");
    s.primaryKey.push_back(L"active");
    return s;
}

static TableSchema PgCompositeKeySchema()
{
    TableSchema s = MySqlCompositeKeySchema();
    s.columns[1] = Col(L"active", ColKind::Boolean, L"boolean");
    return s;
}

static DataDiffSpec CompositeKeySpec()
{
    DataDiffSpec s;
    s.srcDb = L"s"; s.tgtDb = L"t";
    s.srcSchema = MySqlCompositeKeySchema();
    s.tgtSchema = PgCompositeKeySchema();
    return s;
}

static void TestCoercedKeyMatchesInsteadOfDeleteInsert()
{
    std::printf("\n[3] coerced KEY column: rows match, no DELETE+INSERT churn\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    // Both servers sort by the RAW key. MySQL orders active as 0,1; PostgreSQL
    // orders it f,t. The two raw sequences look nothing alike ("0","1" vs
    // "f","t") but they are the SAME logical sequence, which is exactly what the
    // order-preserving canonicalization relies on.
    src.rows_ = { { Num(L"1"), Num(L"0"), Txt(L"a") },
                  { Num(L"1"), Num(L"1"), Txt(L"b") },
                  { Num(L"2"), Num(L"0"), Txt(L"c") },
                  { Num(L"2"), Num(L"1"), Txt(L"d") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"f"), Txt(L"a") },
                  { Num(L"1"), Txt(L"t"), Txt(L"b") },
                  { Num(L"2"), Txt(L"f"), Txt(L"c") },
                  { Num(L"2"), Txt(L"t"), Txt(L"d") } };

    {
        // The decision is asserted, not just described: the key really is
        // coerced (so the test is not vacuous) and the layout ACCEPTS it.
        RowLayout layout;
        std::vector<Finding> findings;
        const bool built = BuildRowLayout(CompositeKeySpec(), Dialect::MySQL,
                                          Dialect::Postgres, layout, findings);
        ExpectTrue("layout accepts the coerced key", built);
        ExpectEq("two PK columns", (long long)layout.pkColumns.size(), 2);
        if (layout.keyBridges.size() == 2)
            ExpectTrue("the key bridge really does coerce (test is not vacuous)",
                       !layout.keyBridges[1].passthrough);
        if (layout.pkIdx.size() == 2)
            ExpectTrue("the coerced key column is classified Boolish",
                       layout.compare.At(layout.pkIdx[1]) == CompareOp::Boolish);
        ExpectTrue("...and Boolish is gated as order-safe",
                   KeyOrderSafe(CompareOp::Boolish));
    }

    const DiffResult r = RunDiff(src, tgt, CompositeKeySpec());

    ExpectTrue("compare pass ok", r.ok);
    if (!r.ok) std::printf("       err: [%s]\n", (const char*)r.err.utf8_str());
    ExpectEq("ZERO invented INSERTs", r.stat.inserts, 0);
    ExpectEq("ZERO invented DELETEs", r.stat.deletes, 0);
    ExpectEq("zero UPDATEs",          r.stat.updates, 0);
    ExpectEq("all four rows matched by key", r.stat.unchanged, 4);
}

// A coerced key where one row's payload genuinely differs: the row must be
// identified by key and reported as ONE update — the merge staying aligned
// through a coerced key is what separates one UPDATE from a DELETE+INSERT pair.
static void TestCoercedKeyStillDetectsRealUpdates()
{
    std::printf("\n[3b] coerced KEY + a real payload change: one UPDATE\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    src.rows_ = { { Num(L"1"), Num(L"0"), Txt(L"a") },
                  { Num(L"1"), Num(L"1"), Txt(L"CHANGED") },
                  { Num(L"2"), Num(L"0"), Txt(L"c") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"f"), Txt(L"a") },
                  { Num(L"1"), Txt(L"t"), Txt(L"b") },
                  { Num(L"2"), Txt(L"f"), Txt(L"c") } };

    const DiffResult r = RunDiff(src, tgt, CompositeKeySpec());

    ExpectTrue("compare pass ok", r.ok);
    ExpectEq("exactly one UPDATE", r.stat.updates, 1);
    ExpectEq("no INSERT invented", r.stat.inserts, 0);
    ExpectEq("no DELETE invented", r.stat.deletes, 0);
    ExpectEq("two rows unchanged", r.stat.unchanged, 2);
}

// A row present only on one side must STILL be reported, through a coerced key.
// A comparator that has gone blind would report zero here, so this is the
// counterweight to test [1].
static void TestCoercedKeyStillDetectsMissingRows()
{
    std::printf("\n[3c] coerced KEY: a genuinely missing row is still found\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    src.rows_ = { { Num(L"1"), Num(L"0"), Txt(L"a") },
                  { Num(L"1"), Num(L"1"), Txt(L"b") },
                  { Num(L"3"), Num(L"1"), Txt(L"only-on-source") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"f"), Txt(L"a") },
                  { Num(L"2"), Txt(L"t"), Txt(L"only-on-target") } };

    const DiffResult r = RunDiff(src, tgt, CompositeKeySpec());

    ExpectTrue("compare pass ok", r.ok);
    ExpectEq("one INSERT (tenant=1,active=1) + one (tenant=3)", r.stat.inserts, 2);
    ExpectEq("one DELETE (tenant=2)", r.stat.deletes, 1);
    ExpectEq("one row matched",       r.stat.unchanged, 1);
    ExpectEq("no UPDATE",             r.stat.updates, 0);
}

// ===========================================================================
//  4. SAME-ENGINE IS UNCHANGED AND FREE
// ===========================================================================
// The structural claim, asserted rather than trusted: when both dialects match,
// BuildBridges makes every bridge passthrough, so every CompareOp is Raw and
// CompareRules::allRaw is true — which is the flag the merge uses to take the
// pre-fix loop with no per-cell dispatch at all. And a same-engine boolean
// column keeps its kind-sensitive raw semantics: "1" (Numeric) vs "1" (Text) is
// still a difference, exactly as before.
static void TestSameEngineIsRawAndFree()
{
    std::printf("\n[4] same-engine: every rule Raw, allRaw set, semantics unchanged\n");

    DataDiffSpec spec;
    spec.srcDb = L"s"; spec.tgtDb = L"t";
    spec.srcSchema = PgFlagSchema();
    spec.tgtSchema = PgFlagSchema();

    RowLayout layout;
    std::vector<Finding> findings;
    ExpectTrue("same-engine layout builds",
               BuildRowLayout(spec, Dialect::Postgres, Dialect::Postgres, layout,
                              findings));
    ExpectTrue("allRaw is set (merge takes the zero-cost path)", layout.compare.allRaw);
    bool everyRuleRaw = !layout.compare.ops.empty();
    for (CompareOp op : layout.compare.ops)
        if (op != CompareOp::Raw) everyRuleRaw = false;
    ExpectTrue("every column rule is Raw", everyRuleRaw);
    bool everyBridgePassthrough = true;
    for (const ColumnBridge& b : layout.valueBridges)
        if (!b.passthrough) everyBridgePassthrough = false;
    ExpectTrue("every bridge is still passthrough", everyBridgePassthrough);

    // Raw equality stays CellKind-sensitive — no same-engine verdict may move.
    ExpectTrue("Raw: Numeric 1 != Text 1 (unchanged)",
               !CellsEqual(Num(L"1"), Txt(L"1"), CompareOp::Raw));
    ExpectTrue("Raw: Numeric 1 == Numeric 1",
               CellsEqual(Num(L"1"), Num(L"1"), CompareOp::Raw));
    // ...and a same-engine diff of identical rows still finds nothing.
    StubConn src, tgt;
    src.dialect_ = tgt.dialect_ = Dialect::Postgres;
    src.rows_ = { { Num(L"1"), Txt(L"t") }, { Num(L"2"), Txt(L"f") } };
    tgt.rows_ = src.rows_;
    const DiffResult r = RunDiff(src, tgt, spec);
    ExpectTrue("same-engine compare ok", r.ok);
    ExpectEq("same-engine: zero differences", r.stat.updates + r.stat.inserts +
                                              r.stat.deletes, 0);
    ExpectEq("same-engine: two rows unchanged", r.stat.unchanged, 2);
}

// ===========================================================================
//  5. THE ORDER PROOFS, asserted directly
// ===========================================================================
// The merge's soundness rests on each CompareOp being a MONOTONE, INJECTIVE map
// of each side's native order. These assertions are the executable form of the
// proofs written into SyncCellCompare.h — if someone weakens a canonicalization,
// this fails before the corruption reaches a user.
static void TestCanonicalizationIsOrderPreserving()
{
    std::printf("\n[5] the canonicalizations are order-preserving and total\n");

    // Boolish, PostgreSQL side: f < t, matching PG's own false < true.
    ExpectTrue("Boolish: f < t", CompareCells(Txt(L"f"), Txt(L"t"), CompareOp::Boolish) < 0);
    // Boolish, MySQL side: 0 < 1, matching MySQL's arithmetic order.
    ExpectTrue("Boolish: 0 < 1", CompareCells(Num(L"0"), Num(L"1"), CompareOp::Boolish) < 0);
    // Cross-side: the two orders are now the SAME order. This is the identity
    // fix — before it, "1" vs "t" compared by code point and the source key was
    // ALWAYS smaller, so every row became an INSERT and every target row a DELETE.
    ExpectTrue("Boolish: MySQL 1 == PG t",  CompareCells(Num(L"1"), Txt(L"t"), CompareOp::Boolish) == 0);
    ExpectTrue("Boolish: MySQL 0 == PG f",  CompareCells(Num(L"0"), Txt(L"f"), CompareOp::Boolish) == 0);
    ExpectTrue("Boolish: MySQL 0 < PG t",   CompareCells(Num(L"0"), Txt(L"t"), CompareOp::Boolish) < 0);
    ExpectTrue("Boolish: MySQL 1 > PG f",   CompareCells(Num(L"1"), Txt(L"f"), CompareOp::Boolish) > 0);
    ExpectTrue("Boolish: alternative spelling true == 1",
               CompareCells(Txt(L"true"), Num(L"1"), CompareOp::Boolish) == 0);

    // A tinyint(1) may legally hold values outside {0,1}. They keep their
    // ARITHMETIC position rather than collapsing onto a boolean, which is what
    // keeps MySQL's own ordering monotone through the canonicalization. (Such a
    // value is still refused for EMISSION by ConvertCell; this only keeps the
    // merge aligned while that happens.)
    ExpectTrue("Boolish: 2 sorts after 1",  CompareCells(Num(L"2"), Num(L"1"), CompareOp::Boolish) > 0);
    ExpectTrue("Boolish: -1 sorts before 0", CompareCells(Num(L"-1"), Num(L"0"), CompareOp::Boolish) < 0);
    ExpectTrue("Boolish: 2 != t",           !CellsEqual(Num(L"2"), Txt(L"t"), CompareOp::Boolish));

    // Numeric: arithmetic, not lexicographic. This is the case raw comparison
    // got wrong whenever the two drivers disagreed about CellKind — code point
    // puts "10" before "9".
    ExpectTrue("Numeric: 9 < 10",  CompareCells(Num(L"9"), Txt(L"10"), CompareOp::Numeric) < 0);
    ExpectTrue("Numeric: 7 == 7 across kinds",
               CellsEqual(Num(L"7"), Txt(L"7"), CompareOp::Numeric));

    // TextOnly: identity on the payload, so code-point order is preserved; only
    // the CellKind label is ignored.
    ExpectTrue("TextOnly: a < b", CompareCells(Txt(L"a"), Txt(L"b"), CompareOp::TextOnly) < 0);
    ExpectTrue("TextOnly: same hex, different kind label, is equal",
               CellsEqual(Cell{ CellKind::Binary, L"DEADBEEF" },
                          Txt(L"DEADBEEF"), CompareOp::TextOnly));

    // NULL semantics are uniform across every op and unchanged from CmpCell:
    // NULL sorts first, two NULLs are equal, NULL never equals a value.
    const Cell nul{ CellKind::Null, wxString() };
    for (CompareOp op : { CompareOp::Raw, CompareOp::Boolish, CompareOp::Numeric,
                          CompareOp::TextOnly }) {
        ExpectTrue("NULL == NULL", CellsEqual(nul, nul, op));
        ExpectTrue("NULL sorts before a value", CompareCells(nul, Num(L"0"), op) < 0);
        ExpectTrue("NULL != a value", !CellsEqual(nul, Num(L"0"), op));
    }
    // NULL and the empty string stay distinct, in every op.
    ExpectTrue("NULL != empty string", !CellsEqual(nul, Txt(wxString()), CompareOp::TextOnly));

    // Totality/transitivity of the rank split: a value with no canonical numeric
    // form ranks after every value that has one, consistently in both directions.
    ExpectTrue("Numeric: unparseable ranks after a number",
               CompareCells(Txt(L"zzz"), Num(L"99"), CompareOp::Numeric) > 0);
    ExpectTrue("Numeric: ...and the reverse agrees",
               CompareCells(Num(L"99"), Txt(L"zzz"), CompareOp::Numeric) < 0);
}

// ===========================================================================
//  6. THE GATE — an unclassifiable coercion is refused as a KEY, not guessed
// ===========================================================================
// The classification switch has no silent default: a BridgeOp added later and
// not given an order proof maps to CompareOp::Unsafe, which KeyOrderSafe rejects
// and BuildRowLayout turns into a blocking Finding. Nothing produces Unsafe
// today, so the mapping table is asserted directly — that is what makes the gate
// fail CLOSED rather than becoming dead code nobody notices is wrong.
static void TestUnsafeOpsAreRefusedAsKeys()
{
    std::printf("\n[6] the key gate: only proven-order-preserving ops may key\n");

    ExpectTrue("Raw may key",      KeyOrderSafe(CompareOp::Raw));
    ExpectTrue("Boolish may key",  KeyOrderSafe(CompareOp::Boolish));
    ExpectTrue("Numeric may key",  KeyOrderSafe(CompareOp::Numeric));
    ExpectTrue("TextOnly may key", KeyOrderSafe(CompareOp::TextOnly));
    ExpectTrue("Unsafe may NOT key — the whole point of the gate",
               !KeyOrderSafe(CompareOp::Unsafe));

    // The BridgeOp -> CompareOp table, pinned. Each row of this is a claim about
    // ordering that SyncCellCompare.h carries the proof for.
    auto opFor = [](BridgeOp op) {
        ColumnBridge b;
        b.passthrough = false;
        b.op          = op;
        return CompareOpFor(b);
    };
    ExpectTrue("BoolToPgBool  -> Boolish",  opFor(BridgeOp::BoolToPgBool)  == CompareOp::Boolish);
    ExpectTrue("PgBoolToInt   -> Boolish",  opFor(BridgeOp::PgBoolToInt)   == CompareOp::Boolish);
    ExpectTrue("IntRange      -> Numeric",  opFor(BridgeOp::IntRange)      == CompareOp::Numeric);
    ExpectTrue("BinaryReframe -> TextOnly", opFor(BridgeOp::BinaryReframe) == CompareOp::TextOnly);
    ExpectTrue("TextValidate  -> TextOnly", opFor(BridgeOp::TextValidate)  == CompareOp::TextOnly);
    // Naive wall-clock temporals ARE canonicalizable (and orderable); temporals
    // with timezone semantics on either side are NOT decidable from cell text at
    // all, and land on the same fail-closed rung as an unclassified coercion.
    ExpectTrue("TemporalGuard       -> Temporal", opFor(BridgeOp::TemporalGuard)       == CompareOp::Temporal);
    ExpectTrue("TemporalTzAmbiguous -> Unsafe",   opFor(BridgeOp::TemporalTzAmbiguous) == CompareOp::Unsafe);
    ExpectTrue("Temporal may key",   KeyOrderSafe(CompareOp::Temporal));

    // A passthrough bridge is Raw whatever its op says — the fast exit that
    // makes same-engine free.
    ColumnBridge pass;
    pass.passthrough = true;
    pass.op          = BridgeOp::BoolToPgBool;
    ExpectTrue("passthrough short-circuits to Raw", CompareOpFor(pass) == CompareOp::Raw);
}

// ===========================================================================
//  7. TEMPORALS — the same defect class as the booleans, one type family over
// ===========================================================================
// Temporal cells were classified CompareOp::TextOnly, i.e. compared as raw text.
// The two engines spell the SAME INSTANT differently — PostgreSQL appends a
// `+00` offset, drivers may use the ISO `T` separator, and the fractional-second
// precision follows the column's declared precision, not the value — so every
// timestamp column in a cross-engine comparison reported a false-positive
// UPDATE, and a temporal PRIMARY KEY additionally desynced the sorted merge.
//
// THE RULING, in two halves:
//   * NAIVE wall-clock on both sides (MySQL DATE/DATETIME/TIME <-> PG date/
//     timestamp/time WITHOUT time zone) -> canonicalized to SyncTemporal.h's
//     TemporalKey, and permitted as a key: the map is monotone and injective per
//     side, so each stream stays sorted under the merge's comparator.
//   * TIMEZONE semantics on EITHER side (MySQL TIMESTAMP, PG timestamptz) ->
//     REFUSED. See section 7d: this is genuinely undecidable from cell text and
//     is failed loudly rather than guessed.
static TableSchema MySqlEventSchema(const wxString& rawTs)
{
    TableSchema s;
    s.name = L"events";
    s.columns.push_back(Col(L"id", ColKind::Integer,   L"int"));
    s.columns.push_back(Col(L"at", ColKind::Timestamp, rawTs));
    s.primaryKey.push_back(L"id");
    return s;
}

// MySQL DATETIME <-> PG `timestamp without time zone`: naive on both sides.
static DataDiffSpec NaiveEventSpec()
{
    DataDiffSpec s;
    s.srcDb = L"s"; s.tgtDb = L"t";
    s.srcSchema = MySqlEventSchema(L"datetime");
    s.tgtSchema = MySqlEventSchema(L"timestamp without time zone");
    return s;
}

// ---- 7a. the zero-false-positive bar, on a NON-KEY temporal column ---------
static void TestTemporalTextFormsAreNotDifferences()
{
    std::printf("\n[7a] same instant spelled three ways: zero differences\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    // Row 1: bare vs `+00` offset suffix.
    // Row 2: space vs `T` separator, AND 1 fractional digit vs 6 — the
    //        DATETIME(6)-vs-timestamp(3) precision mismatch the brief names.
    // Row 3: no fraction at all vs an explicitly zero fraction, plus `Z`.
    src.rows_ = { { Num(L"1"), Txt(L"2024-01-01 00:00:00") },
                  { Num(L"2"), Txt(L"2024-03-05 12:30:45.5") },
                  { Num(L"3"), Txt(L"2024-06-30 23:59:59") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"2024-01-01 00:00:00+00") },
                  { Num(L"2"), Txt(L"2024-03-05T12:30:45.500000") },
                  { Num(L"3"), Txt(L"2024-06-30 23:59:59.000Z") } };

    const DiffResult r = RunDiff(src, tgt, NaiveEventSpec());

    ExpectTrue("compare pass ok", r.ok);
    if (!r.ok) std::printf("       err: [%s]\n", (const char*)r.err.utf8_str());
    ExpectEq("ZERO false-positive UPDATEs", r.stat.updates,   0);
    ExpectEq("zero INSERTs",                r.stat.inserts,   0);
    ExpectEq("zero DELETEs",                r.stat.deletes,   0);
    ExpectEq("all three rows unchanged",    r.stat.unchanged, 3);
}

// ---- 7b. a GENUINELY different instant is still exactly one UPDATE ---------
// The fix must not buy zero false positives by going blind. Two distinct ways to
// differ are covered: a different clock time, and a different FRACTION — the
// latter matters because the canonicalization PADS the fraction rather than
// truncating it to a common precision. Truncating would have made `.123456` and
// `.123` compare equal, silently hiding that the target really is holding a
// different instant (and breaking injectivity, hence key ordering, as well).
static void TestTemporalRealDifferenceIsOneUpdate()
{
    std::printf("\n[7b] a different instant: one UPDATE, not DELETE+INSERT\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    src.rows_ = { { Num(L"1"), Txt(L"2024-01-01 00:00:00") },
                  { Num(L"2"), Txt(L"2024-03-05 12:30:45") },
                  { Num(L"3"), Txt(L"2024-06-30 23:59:59") } };
    tgt.rows_ = { { Num(L"1"), Txt(L"2024-01-01 00:00:00+00") },
                  { Num(L"2"), Txt(L"2024-03-05 12:30:46+00") },   // one second later
                  { Num(L"3"), Txt(L"2024-06-30 23:59:59+00") } };

    const DiffResult r = RunDiff(src, tgt, NaiveEventSpec());

    ExpectTrue("compare pass ok", r.ok);
    ExpectEq("exactly one UPDATE", r.stat.updates,   1);
    ExpectEq("no INSERT invented", r.stat.inserts,   0);
    ExpectEq("no DELETE invented", r.stat.deletes,   0);
    ExpectEq("the other two unchanged", r.stat.unchanged, 2);
    ExpectTrue("set is executable", r.executable);

    // Fractional precision is padded, never truncated: a genuinely different
    // sub-second value stays a difference instead of being rounded away.
    ExpectTrue("differing fraction is a REAL difference",
               !CellsEqual(Txt(L"2024-01-01 00:00:00.123456"),
                           Txt(L"2024-01-01 00:00:00.123"), CompareOp::Temporal));
    ExpectTrue("...and it orders the shorter one first",
               CompareCells(Txt(L"2024-01-01 00:00:00.123"),
                            Txt(L"2024-01-01 00:00:00.123456"), CompareOp::Temporal) < 0);
}

// ---- 7c. a temporal KEY column: SUPPORTED, because the map is monotone ------
// DECISION: a naive temporal key is supported rather than refused. The
// obligation is that each side's stream stay sorted under the merge's
// comparator. It does: lexicographic order on (date, secs, frac) IS
// chronological order, which is how both engines order a naive temporal; and
// although the map is deliberately many-to-one ACROSS engines (that is the whole
// fix), within ONE stream it is injective, because a server renders a given
// column with ONE format for every row.
//
// Before the fix, "2024-01-01 00:00:00" never matched "2024-01-01 00:00:00+00",
// so the merge walked past every row and produced 3 DELETEs + 3 INSERTs.
static DataDiffSpec TemporalKeySpec(const wxString& srcRaw, const wxString& tgtRaw)
{
    TableSchema s;
    s.name = L"readings";
    s.columns.push_back(Col(L"at",  ColKind::Timestamp, srcRaw));
    s.columns.push_back(Col(L"val", ColKind::Integer,   L"int"));
    s.primaryKey.push_back(L"at");

    TableSchema t = s;
    t.columns[0] = Col(L"at", ColKind::Timestamp, tgtRaw);

    DataDiffSpec spec;
    spec.srcDb = L"s"; spec.tgtDb = L"t";
    spec.srcSchema = s;
    spec.tgtSchema = t;
    return spec;
}

static void TestTemporalKeyMatchesInsteadOfChurning()
{
    std::printf("\n[7c] naive temporal KEY: rows match, no DELETE+INSERT churn\n");

    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;

    // Each side is given its rows in ITS OWN server's sort order. The two raw
    // text sequences differ, but both are chronological — which is exactly the
    // property the canonicalization has to preserve for the merge to stay sound.
    src.rows_ = { { Txt(L"2024-01-01 00:00:00"),    Num(L"10") },
                  { Txt(L"2024-01-01 06:00:00.5"),  Num(L"20") },
                  { Txt(L"2024-02-29 23:59:59"),    Num(L"30") } };
    tgt.rows_ = { { Txt(L"2024-01-01T00:00:00+00"),      Num(L"10") },
                  { Txt(L"2024-01-01T06:00:00.500000Z"), Num(L"20") },
                  { Txt(L"2024-02-29T23:59:59+00:00"),   Num(L"30") } };

    const DiffResult r = RunDiff(src, tgt, TemporalKeySpec(L"datetime", L"timestamp without time zone"));

    ExpectTrue("temporal key is ACCEPTED, not refused", r.ok);
    if (!r.ok) std::printf("       blocked: [%s]\n", (const char*)r.firstBlockingReason.utf8_str());
    ExpectEq("no DELETE churn",     r.stat.deletes,   0);
    ExpectEq("no INSERT churn",     r.stat.inserts,   0);
    ExpectEq("no false UPDATEs",    r.stat.updates,   0);
    ExpectEq("all three rows matched by key", r.stat.unchanged, 3);
}

// ---- 7d. THE UNDECIDABLE CASE — refused loudly, never guessed --------------
// MySQL TIMESTAMP is stored UTC and rendered through the SESSION timezone with
// NO offset suffix; PG timestamptz is stored UTC and rendered through ITS
// session timezone WITH one. So `2024-01-01 00:00:00` (MySQL) and
// `2024-01-01 00:00:00+00` (PG) are the same instant if and only if MySQL's
// session was UTC — and that fact appears NOWHERE in the cell text. Identical
// text can be different instants; different text can be the same instant.
//
// This is not a parser limitation that a better parser would fix: the
// information is absent. So the column is classified CompareOp::Unsafe —
// refused outright as a key, and carrying an explicit Finding on a non-key
// column so a possible false positive is announced rather than silent.
static void TestTimezoneAmbiguityIsRefusedNotGuessed()
{
    std::printf("\n[7d] tz-bearing temporal: refused as a key, flagged otherwise\n");

    // The classification itself, from BuildBridges' type reasoning.
    TableSchema my = MySqlEventSchema(L"timestamp");                      // tz-aware
    TableSchema pg = MySqlEventSchema(L"timestamp with time zone");       // tz-aware
    std::vector<ColumnBridge> bs;
    std::vector<Finding>      fs;
    ExpectTrue("BuildBridges still succeeds (non-blocking)",
               BuildBridges(my, Dialect::MySQL, pg, Dialect::Postgres, bs, fs));
    ExpectTrue("tz temporal arms TemporalTzAmbiguous",
               bs.size() == 2 && bs[1].op == BridgeOp::TemporalTzAmbiguous && !bs[1].passthrough);
    ExpectTrue("...which compares Unsafe", CompareOpFor(bs[1]) == CompareOp::Unsafe);
    ExpectTrue("...and may NEVER key",     !KeyOrderSafe(CompareOpFor(bs[1])));

    // The refusal is LOUD: a Finding explains why the column cannot be decided.
    bool warned = false;
    for (const Finding& f : fs)
        if (f.column == L"at") warned = true;
    ExpectTrue("a Finding announces the undecidable column", warned);

    // A tz-bearing temporal used as a PRIMARY KEY blocks the whole table's data
    // diff rather than merging two streams we cannot order consistently.
    StubConn src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;
    src.rows_ = { { Txt(L"2024-01-01 00:00:00"), Num(L"10") } };
    tgt.rows_ = { { Txt(L"2024-01-01 00:00:00+00"), Num(L"10") } };

    const DiffResult r = RunDiff(src, tgt, TemporalKeySpec(L"timestamp", L"timestamp with time zone"));
    ExpectTrue("tz temporal KEY is REFUSED, not merged", !r.ok);
    ExpectTrue("the refusal carries a blocking reason", !r.firstBlockingReason.empty());
    std::printf("       verdict: [%s]\n", (const char*)r.firstBlockingReason.utf8_str());
}

// ---- 7e. MySQL's zero date must remain Unrepresentable for EMISSION --------
// The comparison canonicalization gives `0000-00-00` its own rank so that it
// sorts first (as MySQL sorts it) and equals only another zero date. That is a
// COMPARISON concern; it must not have made the value look emittable. Emission
// goes through ConvertCell, which is a different function — pinned here so a
// future change to one cannot quietly relax the other.
static void TestZeroDateStillUnrepresentable()
{
    std::printf("\n[7e] MySQL zero date: comparable, still NOT emittable\n");

    TableSchema my = MySqlEventSchema(L"datetime");
    TableSchema pg = MySqlEventSchema(L"timestamp without time zone");
    std::vector<ColumnBridge> bs;
    std::vector<Finding>      fs;
    ExpectTrue("BuildBridges ok", BuildBridges(my, Dialect::MySQL, pg, Dialect::Postgres, bs, fs));

    const ConvertedCell cc = ConvertCell(Txt(L"0000-00-00 00:00:00"), bs[1]);
    ExpectTrue("zero date is STILL Unrepresentable for emission",
               cc.verdict == ValueVerdict::Unrepresentable);
    ExpectTrue("zero date is not silently coerced to NULL",
               !(cc.verdict == ValueVerdict::Coerced && cc.cell.kind == CellKind::Null));

    // Comparison side: equal to itself, distinct from every real date, and
    // sorting FIRST — which is MySQL's own ordering for the zero date, so a
    // zero-date key does not desync the merge either.
    ExpectTrue("zero date equals zero date",
               CellsEqual(Txt(L"0000-00-00 00:00:00"), Txt(L"0000-00-00 00:00:00"), CompareOp::Temporal));
    ExpectTrue("zero date != a real date",
               !CellsEqual(Txt(L"0000-00-00 00:00:00"), Txt(L"2024-01-01 00:00:00"), CompareOp::Temporal));
    ExpectTrue("zero date sorts BEFORE every real date, as MySQL sorts it",
               CompareCells(Txt(L"0000-00-00 00:00:00"), Txt(L"1970-01-01 00:00:00"), CompareOp::Temporal) < 0);
    ExpectTrue("...and the reverse agrees",
               CompareCells(Txt(L"1970-01-01 00:00:00"), Txt(L"0000-00-00 00:00:00"), CompareOp::Temporal) > 0);
}

// ---- 7f. the order proof, and what the parser REFUSES ----------------------
static void TestTemporalOrderAndRefusals()
{
    std::printf("\n[7f] temporal ordering is chronological, total and transitive\n");

    // Monotone: canonical order agrees with chronological order across every
    // field boundary, including the ones text order gets wrong.
    ExpectTrue("year orders",
               CompareCells(Txt(L"2023-12-31 23:59:59"), Txt(L"2024-01-01 00:00:00"), CompareOp::Temporal) < 0);
    ExpectTrue("time-of-day orders",
               CompareCells(Txt(L"2024-01-01 09:00:00"), Txt(L"2024-01-01 10:00:00"), CompareOp::Temporal) < 0);
    // Text order would put "2024-01-01T..." AFTER "2024-01-01 ..." (T > space)
    // regardless of the instants; the canonical order does not care.
    ExpectTrue("separator does not leak into the order",
               CompareCells(Txt(L"2024-01-01T00:00:00"), Txt(L"2024-01-02 00:00:00"), CompareOp::Temporal) < 0);
    // A bare MySQL TIME may be negative and may exceed 24 hours; both order
    // arithmetically rather than by code point.
    ExpectTrue("negative TIME sorts before positive",
               CompareCells(Txt(L"-12:00:00"), Txt(L"01:00:00"), CompareOp::Temporal) < 0);
    ExpectTrue("TIME past 24h orders arithmetically",
               CompareCells(Txt(L"09:00:00"), Txt(L"100:00:00"), CompareOp::Temporal) < 0);

    // REFUSED, deliberately: a NON-ZERO offset is real data, not spelling.
    // Normalizing it would need calendar arithmetic whose agreement with the
    // server's ORDER BY we cannot verify from here, so such a value is ranked
    // after every canonicalizable one and compared by text — it can cost a false
    // positive, never a false negative and never a desync.
    ExpectTrue("a non-zero offset is NOT quietly shifted to UTC",
               !CellsEqual(Txt(L"2024-01-01 08:00:00+08:00"),
                           Txt(L"2024-01-01 00:00:00"), CompareOp::Temporal));
    ExpectTrue("a zero offset IS spelling, and is normalized away",
               CellsEqual(Txt(L"2024-01-01 00:00:00+00:00"),
                          Txt(L"2024-01-01 00:00:00"), CompareOp::Temporal));

    // Totality/transitivity of the rank split, the same contract the numeric ops
    // carry: a value with no canonical form ranks consistently in both
    // directions, so the merge's comparator stays a total order.
    ExpectTrue("unparseable ranks after a real timestamp",
               CompareCells(Txt(L"not-a-date"), Txt(L"2024-01-01 00:00:00"), CompareOp::Temporal) > 0);
    ExpectTrue("...and the reverse agrees",
               CompareCells(Txt(L"2024-01-01 00:00:00"), Txt(L"not-a-date"), CompareOp::Temporal) < 0);
    ExpectTrue("two unparseables fall back to code point, consistently",
               CompareCells(Txt(L"zzz"), Txt(L"aaa"), CompareOp::Temporal) > 0);
    ExpectTrue("NULL still sorts before every temporal",
               CompareCells(Cell{ CellKind::Null, wxString() },
                            Txt(L"0000-00-00"), CompareOp::Temporal) < 0);
}

// ---- 7g. same-engine temporals stay RAW and free ---------------------------
static void TestSameEngineTemporalStaysRaw()
{
    std::printf("\n[7g] same-engine temporal: still allRaw, still zero cost\n");

    TableSchema s = MySqlEventSchema(L"datetime");
    s.columns.push_back(Col(L"ts", ColKind::Timestamp, L"timestamp"));  // tz-aware
    s.columns.push_back(Col(L"d",  ColKind::Date,      L"date"));

    std::vector<ColumnBridge> bs;
    std::vector<Finding>      fs;
    ExpectTrue("same-engine BuildBridges ok",
               BuildBridges(s, Dialect::MySQL, s, Dialect::MySQL, bs, fs));
    ExpectTrue("same-engine temporal bridges stay passthrough Identity",
               bs.size() == 4 && bs[1].passthrough && bs[1].op == BridgeOp::Identity &&
               bs[2].passthrough && bs[3].passthrough);
    ExpectTrue("same-engine temporals raise NO findings", fs.empty());

    CompareRules rules;
    BuildCompareRules(bs, rules);
    ExpectTrue("allRaw still true — the per-row short-circuit survives", rules.allRaw);
    for (size_t i = 0; i < bs.size(); ++i)
        ExpectTrue("every same-engine rule is Raw", rules.At(i) == CompareOp::Raw);

    // And the raw comparator itself is untouched: a same-engine temporal is
    // still compared byte-for-byte the way it always was, so no existing verdict
    // can have moved.
    ExpectTrue("Raw temporal compare is unchanged (text forms still differ)",
               !CellsEqual(Txt(L"2024-01-01 00:00:00"),
                           Txt(L"2024-01-01T00:00:00"), CompareOp::Raw));
}

int main()
{
    std::printf("=== SyncCrossEngineCompareTests ===\n");
    TestNonKeyBoolCoercionIsNotADifference();
    TestRealDifferenceInCoercedColumn();
    TestCoercedKeyMatchesInsteadOfDeleteInsert();
    TestCoercedKeyStillDetectsRealUpdates();
    TestCoercedKeyStillDetectsMissingRows();
    TestSameEngineIsRawAndFree();
    TestCanonicalizationIsOrderPreserving();
    TestUnsafeOpsAreRefusedAsKeys();
    TestTemporalTextFormsAreNotDifferences();
    TestTemporalRealDifferenceIsOneUpdate();
    TestTemporalKeyMatchesInsteadOfChurning();
    TestTimezoneAmbiguityIsRefusedNotGuessed();
    TestZeroDateStillUnrepresentable();
    TestTemporalOrderAndRefusals();
    TestSameEngineTemporalStaysRaw();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
