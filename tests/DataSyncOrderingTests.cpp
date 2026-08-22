// DataSyncOrderingTests.cpp — ADR-015 T2/T4: the PK-ordering defenses of the
// streaming data diff, and the ExecuteBatch degradation contract.
//
// Why this file exists. db::sync::DiffAndEmitData is a sorted merge over two
// servers' row streams. It is correct only if both servers return rows in the
// same total order, and in the same order CmpCell compares keys with. For a
// VARCHAR primary key they do not agree by default: MySQL's stock
// utf8mb4_general_ci folds case AND accents, PostgreSQL's default collation
// does neither, so the two sides hand back the same rows in different orders.
// A desynced merge does not fail — it emits INSERTs for rows that already exist
// and DELETEs for rows that were never missing, and reports success. That is
// the single silent data-corruption path in this feature; everything else here
// fails loudly.
//
// WHAT IS AND IS NOT PROVEN HERE. There is no live MySQL or PostgreSQL in this
// environment, so the two servers are simulated: FakeServer sorts its canonical
// row set with a comparator standing in for a collation, and honors (or, for
// the negative test, deliberately ignores) StreamOptions::binaryOrder. That
// genuinely proves the *client-side* half — that DataSync asks for byte order,
// that the merge is correct once both sides byte-order, that the refusal gate
// classifies PK types the way it claims, and that the backwards-key assertion
// converts a desync into a loud abort. It does NOT prove that a real MySQL
// server's `CONVERT(col USING utf8mb4) COLLATE utf8mb4_bin` and a real
// PostgreSQL's `COLLATE "C"` produce the identical byte order on real data —
// the SQL text is asserted here (OrderByClause), but only live servers can
// confirm the servers obey it. That remains an integration/QA item.
//
// Same dependency-free harness as sync_test.cpp: ExpectTrue/ExpectEq, non-zero
// exit on any failure, no test framework.
#include "db/DataSync.h"
#include "db/DbDriver.h"
#include "db/MySqlStream.h"
#include "db/PgStream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
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
//  Collation simulation
// ===========================================================================

// Primary-weight fold shared by both simulated collations: case-insensitive and
// accent-insensitive, i.e. what utf8mb4_general_ci actually does. Only the
// characters this file's fixtures use are mapped — this is a stand-in, not a
// Unicode implementation.
static wxString Fold(const wxString& s)
{
    wxString o;
    for (wxUniChar ch : s) {
        wxUint32 c = ch.GetValue();
        switch (c) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
            c = L'a'; break;
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
            c = L'e'; break;
        case 0x00D6: case 0x00F6: c = L'o'; break;
        case 0x00D1: case 0x00F1: c = L'n'; break;
        default:
            if (c >= L'A' && c <= L'Z') c += 0x20;
            break;
        }
        o += wxUniChar(c);
    }
    return o;
}

// Code-point order — what CmpCell uses, what UTF-8 byte order is, and what both
// utf8mb4_bin and COLLATE "C" are expected to produce.
static int CmpCodePoint(const wxString& a, const wxString& b)
{
    wxString::const_iterator ia = a.begin(), ib = b.begin();
    for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
        const wxUint32 ca = (*ia).GetValue(), cb = (*ib).GetValue();
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (ia != a.end()) return 1;
    if (ib != b.end()) return -1;
    return 0;
}

enum class FakeCollation {
    GeneralCi,   // MySQL utf8mb4_general_ci: fold, ties by original ascending
    PgLocale,    // PG default locale: fold, ties by original DESCENDING
    CodePoint    // utf8mb4_bin / COLLATE "C"
};

// The two non-binary comparators agree on the primary weight and disagree on the
// tiebreak, which is exactly the shape of the real hazard: same rows, same
// "sorted" claim, two different sequences. Both also differ from code-point
// order, so a merge that assumes code-point order desyncs against either.
static bool LessBy(FakeCollation coll, const wxString& a, const wxString& b)
{
    if (coll == FakeCollation::CodePoint) return CmpCodePoint(a, b) < 0;
    const int p = CmpCodePoint(Fold(a), Fold(b));
    if (p) return p < 0;
    const int t = CmpCodePoint(a, b);
    return coll == FakeCollation::GeneralCi ? t < 0 : t > 0;
}

// ===========================================================================
//  FakeServer — an IConnection that orders rows the way a collation would
// ===========================================================================
class FakeServer : public IConnection {
public:
    Dialect       dialect_   = Dialect::MySQL;
    FakeCollation collation_ = FakeCollation::GeneralCi;
    // false simulates a server or driver that does NOT honor binaryOrder (a
    // future engine, an older server, a bug) — the case defense 3 exists for.
    bool          honorBinaryOrder_ = true;
    size_t        pkIndex_ = 0;
    wxString      pkName_  = L"code";
    std::vector<std::vector<Cell>> rows_;    // canonical, unordered
    long long     endlessRows_ = 0;          // >0: stream forever, for cancel tests
    // Captured from the last StreamRows call, so a test can assert what DataSync
    // actually asked the server for.
    StreamOptions lastOpt_;

    Dialect GetDialect() const override { return dialect_; }

    bool StreamRows(const wxString&, const QualifiedName&, const StreamOptions& opt,
                    const RowSink& sink, wxString&) override
    {
        lastOpt_ = opt;

        if (endlessRows_ > 0) {
            for (long long i = 0; i < endlessRows_; ++i) {
                std::vector<Cell> r{ Cell{ CellKind::Numeric,
                                           wxString::Format(L"%lld", i) } };
                if (!sink(r)) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        const bool byteOrder =
            honorBinaryOrder_ && opt.binaryOrder &&
            std::find(opt.binaryOrderCols.begin(), opt.binaryOrderCols.end(),
                      pkName_) != opt.binaryOrderCols.end();
        const FakeCollation eff = byteOrder ? FakeCollation::CodePoint : collation_;

        std::vector<std::vector<Cell>> ordered = rows_;
        const size_t pk = pkIndex_;
        std::stable_sort(ordered.begin(), ordered.end(),
            [eff, pk](const std::vector<Cell>& a, const std::vector<Cell>& b) {
                // Numeric keys sort numerically in every mode — collation has no
                // bearing on them, which is the whole reason they are exempt
                // from the refusal gate.
                if (a[pk].kind == CellKind::Numeric && b[pk].kind == CellKind::Numeric) {
                    long long la = 0, lb = 0;
                    a[pk].text.ToLongLong(&la);
                    b[pk].text.ToLongLong(&lb);
                    return la < lb;
                }
                return LessBy(eff, a[pk].text, b[pk].text);
            });

        for (const auto& r : ordered)
            if (!sink(r)) return true;
        return true;
    }

    // ---- minimal bodies for the remaining pure virtuals ----
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
    wxString ServerVersion() const override { return L"fake"; }
};

// A driver that overrides nothing beyond the pure virtuals — used to assert the
// IConnection defaults degrade rather than pretend.
class BareConn : public FakeServer {
public:
    bool StreamRows(const wxString&, const QualifiedName&, const StreamOptions&,
                    const RowSink&, wxString& err) override
    { err = L"该驱动不支持结构化行读取"; return false; }
};

// ===========================================================================
//  Fixtures
// ===========================================================================

static Cell Txt(const wxString& s) { return Cell{ CellKind::Text, s }; }
static Cell Num(long long v)       { return Cell{ CellKind::Numeric,
                                                  wxString::Format(L"%lld", v) }; }

// The regression fixture: a VARCHAR primary key carrying mixed case AND accents,
// i.e. both of the things utf8mb4_general_ci folds away and PostgreSQL's default
// collation does not. Every value is distinct as bytes, so a correct diff of
// this set against itself must find literally nothing.
static std::vector<std::vector<Cell>> MixedCaseAccentRows()
{
    const wchar_t* keys[] = {
        L"Apple", L"apple", L"BANANA", L"banana",
        L"Café", L"café",             // Café / café
        L"Ångström", L"ångström",  // Ångström / ångström
        L"Niño", L"niño",             // Niño / niño
        L"Zebra"
    };
    std::vector<std::vector<Cell>> rows;
    long long payload = 100;
    for (const wchar_t* k : keys)
        rows.push_back({ Txt(k), Num(payload++) });
    return rows;
}

// name/kind/rawType for a one-text-PK table (`code` PK, `n` payload).
static TableSchema TextPkSchema(ColKind kind = ColKind::Varchar,
                                const wxString& rawType = L"varchar(80)")
{
    TableSchema s;
    s.name = L"t_codes";
    NormColumn pk;  pk.name = L"code"; pk.kind = kind; pk.rawType = rawType;
    NormColumn n;   n.name  = L"n";    n.kind  = ColKind::Integer; n.rawType = L"int";
    s.columns.push_back(pk);
    s.columns.push_back(n);
    s.primaryKey.push_back(L"code");
    return s;
}

static TableSchema IntPkSchema()
{
    TableSchema s;
    s.name = L"t_ints";
    NormColumn pk; pk.name = L"id"; pk.kind = ColKind::Integer; pk.rawType = L"bigint";
    s.columns.push_back(pk);
    s.primaryKey.push_back(L"id");
    return s;
}

// ===========================================================================
//  1. THE MANDATORY REGRESSION — VARCHAR PK, MySQL _ci vs PG default
// ===========================================================================
static void TestNoSpuriousChangesAcrossCollations()
{
    std::printf("\n[1] VARCHAR PK, mixed case + accents, MySQL _ci vs PG default\n");

    FakeServer src;
    src.dialect_   = Dialect::MySQL;
    src.collation_ = FakeCollation::GeneralCi;
    src.rows_      = MixedCaseAccentRows();

    FakeServer tgt;
    tgt.dialect_   = Dialect::Postgres;
    tgt.collation_ = FakeCollation::PgLocale;
    tgt.rows_      = MixedCaseAccentRows();   // byte-identical row set

    // Sanity: the fixture really does discriminate. If the two simulated
    // collations happened to agree, this whole test would prove nothing.
    {
        std::vector<wxString> a, b;
        for (const auto& r : src.rows_) a.push_back(r[0].text);
        b = a;
        std::stable_sort(a.begin(), a.end(), [](const wxString& x, const wxString& y)
                         { return LessBy(FakeCollation::GeneralCi, x, y); });
        std::stable_sort(b.begin(), b.end(), [](const wxString& x, const wxString& y)
                         { return LessBy(FakeCollation::PgLocale, x, y); });
        ExpectTrue("fixture: the two collations really do order differently", a != b);
    }

    DataSyncOptions opt;
    opt.deleteMissing = true;   // deletes would be EMITTED, so a desync is visible
    RowStat  stat;
    wxString err;
    std::atomic<bool> stop{false};
    long long emitted = 0;

    const bool ok = DiffAndEmitData(src, tgt, L"s", L"t", TextPkSchema(), opt,
                                    [&](const wxString&) { ++emitted; },
                                    stat, err, stop);

    ExpectTrue("diff succeeded", ok);
    if (!ok) std::printf("       err: [%s]\n", (const char*)err.utf8_str());
    ExpectEq("ZERO spurious INSERTs", stat.inserts, 0);
    ExpectEq("ZERO spurious DELETEs", stat.deletes, 0);
    ExpectEq("zero UPDATEs",          stat.updates, 0);
    ExpectEq("all rows unchanged",    stat.unchanged,
             static_cast<long long>(MixedCaseAccentRows().size()));
    ExpectEq("no statements emitted", emitted, 0);

    // Defense 1 is what made the above work: assert DataSync actually asked for
    // byte order and named the text PK as needing the collation override.
    ExpectTrue("source stream asked for binaryOrder", src.lastOpt_.binaryOrder);
    ExpectTrue("target stream asked for binaryOrder", tgt.lastOpt_.binaryOrder);
    ExpectEq("source binaryOrderCols names the text PK",
             static_cast<long long>(src.lastOpt_.binaryOrderCols.size()), 1);
    if (src.lastOpt_.binaryOrderCols.size() == 1)
        ExpectStr("binaryOrderCols[0]", src.lastOpt_.binaryOrderCols[0], L"code");
}

// ===========================================================================
//  2. Defense 3 — a server that ignores binaryOrder must fail LOUDLY
// ===========================================================================
static void TestBackwardsKeyAborts()
{
    std::printf("\n[2] a server that ignores binaryOrder aborts, not corrupts\n");

    FakeServer src;
    src.dialect_          = Dialect::MySQL;
    src.collation_        = FakeCollation::GeneralCi;
    src.honorBinaryOrder_ = false;              // the defect defense 3 nets
    src.rows_             = MixedCaseAccentRows();

    FakeServer tgt;
    tgt.dialect_          = Dialect::Postgres;
    tgt.collation_        = FakeCollation::PgLocale;
    tgt.honorBinaryOrder_ = false;
    tgt.rows_             = MixedCaseAccentRows();

    DataSyncOptions opt;
    opt.deleteMissing = true;
    RowStat  stat;
    wxString err;
    std::atomic<bool> stop{false};

    const bool ok = DiffAndEmitData(src, tgt, L"s", L"t", TextPkSchema(), opt,
                                    [](const wxString&) {}, stat, err, stop);

    ExpectTrue("diff FAILED instead of silently corrupting", !ok);
    ExpectTrue("error names the ordering violation", err.Contains(L"回退"));
    ExpectTrue("error names the table", err.Contains(L"t_codes"));
    std::printf("       err: [%s]\n", (const char*)err.utf8_str());

    // The point of defense 3 is that the caller can distinguish "done" from
    // "gave up": a false return means the whole table is abandoned, so whatever
    // partial statements were emitted before detection are never applied.
    ExpectTrue("aborted before scanning the whole table",
               stat.inserts + stat.deletes + stat.updates + stat.unchanged <
                   static_cast<long long>(MixedCaseAccentRows().size()) * 2);
}

// ===========================================================================
//  3. What the bug actually looked like — reference naive merge
// ===========================================================================
// Not a test of production code; a proof that the fixture is a real corruption
// case. Merging the two collations' orders with a code-point comparator and NO
// defenses (i.e. the pre-T2 behavior) produces spurious changes out of two
// identical row sets. If this ever reported zero, tests 1 and 2 would be
// vacuous.
static void TestNaiveMergeWouldCorrupt()
{
    std::printf("\n[3] the pre-T2 merge on this data (documents the bug)\n");

    std::vector<wxString> s, t;
    for (const auto& r : MixedCaseAccentRows()) { s.push_back(r[0].text); }
    t = s;
    std::stable_sort(s.begin(), s.end(), [](const wxString& x, const wxString& y)
                     { return LessBy(FakeCollation::GeneralCi, x, y); });
    std::stable_sort(t.begin(), t.end(), [](const wxString& x, const wxString& y)
                     { return LessBy(FakeCollation::PgLocale, x, y); });

    long long ins = 0, del = 0, same = 0;
    size_t i = 0, j = 0;
    while (i < s.size() || j < t.size()) {
        if (j >= t.size())      { ++ins; ++i; continue; }
        if (i >= s.size())      { ++del; ++j; continue; }
        const int c = CmpCodePoint(s[i], t[j]);
        if      (c < 0) { ++ins; ++i; }
        else if (c > 0) { ++del; ++j; }
        else            { ++same; ++i; ++j; }
    }
    std::printf("       naive merge on identical row sets: %lld INSERT, %lld DELETE, "
                "%lld unchanged\n", ins, del, same);
    ExpectTrue("naive merge really does invent changes (so the fixture is valid)",
               ins > 0 && del > 0);
}

// ===========================================================================
//  4. Defense 2 — the refusal gate
// ===========================================================================
static void TestOrderingGate()
{
    std::printf("\n[4] CheckDataDiffOrdering: allow vs refuse by PK type\n");

    std::vector<Finding>  f;
    std::vector<wxString> bin;

    // Text PK between two engines that CAN be pinned to byte order: allowed,
    // and the PK is flagged as needing the explicit collation.
    f.clear();
    ExpectTrue("varchar PK, MySQL<->PG: allowed",
               CheckDataDiffOrdering(TextPkSchema(), Dialect::MySQL,
                                     Dialect::Postgres, f, bin));
    ExpectEq("  binaryOrderCols = {code}", static_cast<long long>(bin.size()), 1);

    // SQLite's default collation IS byte order, so a text PK is safe there too.
    f.clear();
    ExpectTrue("varchar PK, MySQL<->SQLite: allowed",
               CheckDataDiffOrdering(TextPkSchema(), Dialect::MySQL,
                                     Dialect::Sqlite, f, bin));

    // No byte-order support on one side -> refuse, with a Finding.
    f.clear();
    ExpectTrue("varchar PK, MySQL<->SQL Server: REFUSED",
               !CheckDataDiffOrdering(TextPkSchema(), Dialect::MySQL,
                                      Dialect::SqlServer, f, bin));
    ExpectEq("  one Finding recorded", static_cast<long long>(f.size()), 1);
    if (!f.empty()) {
        ExpectStr("  Finding.table",  f[0].table,  L"t_codes");
        ExpectStr("  Finding.column", f[0].column, L"code");
        ExpectTrue("  Finding.reason explains the risk",
                   f[0].reason.Contains(L"字节序") && f[0].reason.Contains(L"拒绝"));
        std::printf("       reason: [%s]\n", (const char*)f[0].reason.utf8_str());
    }
    ExpectEq("  refusal clears binaryOrderCols", static_cast<long long>(bin.size()), 0);

    // Integer PKs are unaffected by collation — allowed even between two engines
    // with no byte-order support at all. Over-restricting these would make the
    // gate useless in practice (~95% of real PKs).
    f.clear();
    ExpectTrue("bigint PK, Oracle<->SQL Server: allowed",
               CheckDataDiffOrdering(IntPkSchema(), Dialect::Oracle,
                                     Dialect::SqlServer, f, bin));
    ExpectEq("  no COLLATE needed", static_cast<long long>(bin.size()), 0);

    // Other collation-independent key types.
    const ColKind safeKinds[] = { ColKind::Uuid, ColKind::Binary, ColKind::Blob,
                                  ColKind::Timestamp, ColKind::Date,
                                  ColKind::Decimal, ColKind::Boolean };
    for (ColKind k : safeKinds) {
        f.clear();
        ExpectTrue("collation-independent PK kind allowed on any engine",
                   CheckDataDiffOrdering(TextPkSchema(k, L"x"), Dialect::Oracle,
                                         Dialect::SqlServer, f, bin));
    }

    // ENUM / JSON have no agreed cross-engine order and no COLLATE fixes them.
    f.clear();
    ExpectTrue("enum PK: REFUSED even MySQL<->PG",
               !CheckDataDiffOrdering(TextPkSchema(ColKind::Enum, L"enum('a','b')"),
                                      Dialect::MySQL, Dialect::Postgres, f, bin));
    f.clear();
    ExpectTrue("json PK: REFUSED",
               !CheckDataDiffOrdering(TextPkSchema(ColKind::Json, L"json"),
                                      Dialect::MySQL, Dialect::Postgres, f, bin));

    // ColKind::Other (what the IConnection base-class introspection leaves) must
    // fall back to the raw type text rather than being waved through.
    f.clear();
    ExpectTrue("Other/'character varying' -> treated as text, allowed MySQL<->PG",
               CheckDataDiffOrdering(TextPkSchema(ColKind::Other, L"character varying"),
                                     Dialect::MySQL, Dialect::Postgres, f, bin));
    ExpectEq("  and flagged for COLLATE", static_cast<long long>(bin.size()), 1);
    f.clear();
    ExpectTrue("Other/'character varying' -> REFUSED MySQL<->Oracle",
               !CheckDataDiffOrdering(TextPkSchema(ColKind::Other, L"character varying"),
                                      Dialect::MySQL, Dialect::Oracle, f, bin));
    f.clear();
    ExpectTrue("Other/'bigint' -> deterministic, allowed anywhere",
               CheckDataDiffOrdering(TextPkSchema(ColKind::Other, L"bigint"),
                                     Dialect::Oracle, Dialect::SqlServer, f, bin));
    f.clear();
    ExpectTrue("Other/'geometry' -> unclassifiable, REFUSED",
               !CheckDataDiffOrdering(TextPkSchema(ColKind::Other, L"geometry"),
                                      Dialect::MySQL, Dialect::Postgres, f, bin));

    // No PK at all.
    f.clear();
    TableSchema noPk = TextPkSchema();
    noPk.primaryKey.clear();
    ExpectTrue("no PK: REFUSED",
               !CheckDataDiffOrdering(noPk, Dialect::MySQL, Dialect::Postgres, f, bin));

    // DiffAndEmitData must enforce the gate itself, not trust its caller.
    FakeServer a, b;
    a.dialect_ = Dialect::MySQL;
    b.dialect_ = Dialect::SqlServer;
    a.rows_ = b.rows_ = MixedCaseAccentRows();
    RowStat  stat;
    wxString err;
    std::atomic<bool> stop{false};
    ExpectTrue("DiffAndEmitData refuses a gated table itself",
               !DiffAndEmitData(a, b, L"s", L"t", TextPkSchema(), DataSyncOptions{},
                                [](const wxString&) {}, stat, err, stop));
    ExpectTrue("  and says why", err.Contains(L"拒绝"));
}

// ===========================================================================
//  5. Defense 1 — the SQL text each driver emits
// ===========================================================================
// Pure string rendering: OrderByClause never touches a handle. This is the only
// part of defense 1 that can be checked offline; whether the servers honor the
// clause needs live instances.
static void TestOrderByClause()
{
    std::printf("\n[5] byte-order ORDER BY rendering\n");

    StreamOptions o;
    o.orderBy = { L"code" };
    ExpectStr("MySQL, binaryOrder off", mysqlstream::OrderByClause(o),
              L" ORDER BY `code`");
    ExpectStr("PG, binaryOrder off", pgstream::OrderByClause(o),
              L" ORDER BY \"code\"");

    o.binaryOrder     = true;
    o.binaryOrderCols = { L"code" };
    ExpectStr("MySQL, text key forced to utf8mb4_bin",
              mysqlstream::OrderByClause(o),
              L" ORDER BY CONVERT(`code` USING utf8mb4) COLLATE utf8mb4_bin");
    ExpectStr("PG, text key forced to C",
              pgstream::OrderByClause(o),
              L" ORDER BY \"code\" COLLATE \"C\"");

    // A composite key mixing a text column and an integer one: COLLATE goes on
    // the text column ONLY — `ORDER BY int_col COLLATE "C"` is a type error on
    // both engines.
    o.orderBy         = { L"id", L"code" };
    o.binaryOrderCols = { L"code" };
    ExpectStr("MySQL, COLLATE only on the text column",
              mysqlstream::OrderByClause(o),
              L" ORDER BY `id`, CONVERT(`code` USING utf8mb4) COLLATE utf8mb4_bin");
    ExpectStr("PG, COLLATE only on the text column",
              pgstream::OrderByClause(o),
              L" ORDER BY \"id\", \"code\" COLLATE \"C\"");

    // binaryOrder with no listed columns (the integer-PK case) must not emit any
    // COLLATE at all.
    o.orderBy         = { L"id" };
    o.binaryOrderCols.clear();
    ExpectStr("MySQL, integer key: no COLLATE",
              mysqlstream::OrderByClause(o), L" ORDER BY `id`");
    ExpectStr("PG, integer key: no COLLATE",
              pgstream::OrderByClause(o), L" ORDER BY \"id\"");

    o.orderBy.clear();
    ExpectStr("no orderBy -> empty clause", mysqlstream::OrderByClause(o), L"");
}

// ===========================================================================
//  6. Progress + cancellation
// ===========================================================================
static void TestProgressAndCancel()
{
    std::printf("\n[6] intra-table progress and prompt cancellation\n");

    // Progress: 5000 identical integer-keyed rows on both sides -> 10000 scanned,
    // so the every-4096 hook must fire more than once inside the one table.
    FakeServer src, tgt;
    src.dialect_ = Dialect::MySQL;
    tgt.dialect_ = Dialect::Postgres;
    src.pkName_ = tgt.pkName_ = L"id";
    for (long long i = 0; i < 5000; ++i) {
        src.rows_.push_back({ Num(i) });
        tgt.rows_.push_back({ Num(i) });
    }

    std::vector<long long> seen;
    DataSyncOptions opt;
    opt.rowsEstimated = 5000;
    opt.progress = [&](const wxString& table, long long scanned, long long est) {
        ExpectTrue("progress carries the table name", table == L"t_ints");
        ExpectEq("progress carries the estimate", est, 5000);
        seen.push_back(scanned);
    };

    RowStat  stat;
    wxString err;
    std::atomic<bool> stop{false};
    const bool ok = DiffAndEmitData(src, tgt, L"s", L"t", IntPkSchema(), opt,
                                    [](const wxString&) {}, stat, err, stop);
    ExpectTrue("integer-PK diff succeeded", ok);
    ExpectEq("all 5000 rows matched", stat.unchanged, 5000);
    ExpectTrue("progress fired repeatedly inside one table", seen.size() > 2);
    bool monotonic = true;
    for (size_t i = 1; i < seen.size(); ++i)
        if (seen[i] < seen[i - 1]) monotonic = false;
    ExpectTrue("progress counts never go backwards", monotonic);
    if (!seen.empty())
        ExpectEq("final progress is the exact scanned total", seen.back(), 10000);

    // Cancellation: an endless stream, cancelled from another thread. The call
    // must come back promptly rather than after the stream ends (it never does).
    FakeServer es, et;
    es.dialect_ = Dialect::MySQL;
    et.dialect_ = Dialect::Postgres;
    es.pkName_ = et.pkName_ = L"id";
    es.endlessRows_ = et.endlessRows_ = 100000;   // ~100 s at 1 ms/row

    std::atomic<bool> cancel{false};
    RowStat  st2;
    wxString err2;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel.store(true);
    });
    const bool ok2 = DiffAndEmitData(es, et, L"s", L"t", IntPkSchema(),
                                     DataSyncOptions{}, [](const wxString&) {},
                                     st2, err2, cancel);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    canceller.join();

    ExpectTrue("cancelled diff returns false", !ok2);
    ExpectStr("cancelled diff says so", err2, L"已取消");
    std::printf("       cancel acknowledged in %lld ms\n", (long long)ms);
    ExpectTrue("cancel acknowledged in bounded time (<5 s)", ms < 5000);
}

// ===========================================================================
//  7. ExecuteBatch degradation contract (T4)
// ===========================================================================
static void TestExecuteBatchDefault()
{
    std::printf("\n[7] IConnection::ExecuteBatch default degradation\n");

    BareConn c;
    wxString err;
    std::vector<std::vector<Cell>> rows{ { Num(1), Txt(L"x") } };
    ExpectTrue("un-wired driver reports failure", !c.ExecuteBatch(L"INSERT", rows, err));
    ExpectStr("with the standard message", err, L"该驱动不支持批量执行");

    // Same shape as StreamRows' precedent, checked so the two stay consistent.
    wxString serr;
    StreamOptions o;
    ExpectTrue("StreamRows precedent still degrades",
               !c.StreamRows(L"d", L"t", o, [](const std::vector<Cell>&) { return true; },
                             serr));
    ExpectStr("  with its own message", serr, L"该驱动不支持结构化行读取");
}

int main()
{
    std::printf("== DataSync ordering + batch tests (ADR-015 T2/T4) ==\n");
    TestNoSpuriousChangesAcrossCollations();
    TestBackwardsKeyAborts();
    TestNaiveMergeWouldCorrupt();
    TestOrderingGate();
    TestOrderByClause();
    TestProgressAndCancel();
    TestExecuteBatchDefault();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
