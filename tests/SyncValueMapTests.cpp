// SyncValueMapTests.cpp — standalone unit tests for ADR-015 T1
// (db::sync::SyncValueMap: ValueVerdict/MayEmit, BuildBridges, ConvertCell) and
// T3 (db::sync::RowChange/RowChangeBuilder/RowChangeSet + RenderRowDml).
//
// Every conversion ruling from the ADR gets a named test here, plus the two
// structural properties the design turns on:
//   * an Unrepresentable cell cannot produce a RowChange (Build() -> nullopt);
//   * a blocked row makes the whole RowChangeSet non-executable, permanently.
//
// Same dependency-free harness as SyncTypeMapTests.cpp: ExpectTrue/ExpectStr
// assert loop, non-zero exit on any failure. Pure value types + pure functions —
// no connection, no GUI — so this links only swiftsql::db and runs headless.
#include "db/RowChange.h"
#include "db/SyncSeqFix.h"
#include "db/SyncValueMap.h"
#include "db/DbDriver.h"

#include <cstdio>
#include <climits>
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

static const char* VerdictName(ValueVerdict v)
{
    switch (v) {
    case ValueVerdict::Exact:   return "Exact";
    case ValueVerdict::Coerced: return "Coerced";
    default:                    return "Unrepresentable";
    }
}

static void ExpectVerdict(const char* name, ValueVerdict got, ValueVerdict want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: %s\n    got : %s\n", name, VerdictName(want), VerdictName(got));
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---- fixtures -------------------------------------------------------------

static NormColumn MyCol(const wxString& name, ColKind k, const wxString& rawType,
                        long long len = -1)
{
    NormColumn c; c.name = name; c.kind = k; c.rawType = rawType; c.length = len;
    return c;
}

static NormColumn PgCol(const wxString& name, ColKind k, const wxString& dataType,
                        long long len = -1)
{
    NormColumn c; c.name = name; c.kind = k; c.rawType = dataType; c.length = len;
    return c;
}

static Cell Num(const wxString& t) { Cell c; c.kind = CellKind::Numeric; c.text = t; return c; }
static Cell Txt(const wxString& t) { Cell c; c.kind = CellKind::Text;    c.text = t; return c; }
static Cell Bin(const wxString& t) { Cell c; c.kind = CellKind::Binary;  c.text = t; return c; }
static Cell Nul()                  { return Cell(); }

// Build a one-column bridge set for a MySQL->PG (or PG->MySQL) column pair.
static ColumnBridge OneBridge(const NormColumn& s, Dialect sd,
                              const NormColumn& t, Dialect td, bool* ok = nullptr)
{
    TableSchema src, tgt;
    src.name = L"t"; tgt.name = L"t";
    src.columns.push_back(s);
    tgt.columns.push_back(t);
    std::vector<ColumnBridge> bs;
    std::vector<Finding> fs;
    const bool built = BuildBridges(src, sd, tgt, td, bs, fs);
    if (ok) *ok = built;
    if (bs.empty()) return ColumnBridge();
    return bs[0];
}

// ---- T1: the verdict ladder ----------------------------------------------

static void TestMayEmit()
{
    ExpectTrue("MayEmit(Exact)", MayEmit(ValueVerdict::Exact));
    ExpectTrue("MayEmit(Coerced)", MayEmit(ValueVerdict::Coerced));
    ExpectTrue("!MayEmit(Unrepresentable)", !MayEmit(ValueVerdict::Unrepresentable));
    // The value ladder and the schema ladder must agree (also static_asserted).
    ExpectTrue("ToTypeVerdict keeps the ladders aligned",
               MayAutoAlter(ToTypeVerdict(ValueVerdict::Coerced)) &&
               !MayAutoAlter(ToTypeVerdict(ValueVerdict::Unrepresentable)));
}

// ---- ruling: same-engine is free -----------------------------------------

static void TestSameEngineIsIdentity()
{
    TableSchema src, tgt;
    src.name = L"orders";
    tgt.name = L"orders";
    src.columns.push_back(MyCol(L"id",   ColKind::Integer, L"bigint unsigned"));
    src.columns.push_back(MyCol(L"note", ColKind::Text,    L"text"));
    src.columns.push_back(MyCol(L"when", ColKind::Date,    L"date"));
    tgt.columns = src.columns;

    std::vector<ColumnBridge> bs;
    std::vector<Finding> fs;
    ExpectTrue("same-engine BuildBridges succeeds", BuildBridges(src, Dialect::MySQL, tgt, Dialect::MySQL, bs, fs));
    ExpectTrue("same-engine yields one bridge per column", bs.size() == 3);
    bool allPass = true;
    for (const auto& b : bs) allPass = allPass && b.passthrough && b.op == BridgeOp::Identity;
    ExpectTrue("same-engine bridges are all passthrough Identity", allPass);
    ExpectTrue("same-engine produces no findings", fs.empty());

    // Even a MySQL zero date crosses untouched between two MySQL servers: the
    // target can store it, so there is nothing to refuse.
    const ConvertedCell cc = ConvertCell(Txt(L"0000-00-00"), bs[2]);
    ExpectVerdict("same-engine zero date stays Exact", cc.verdict, ValueVerdict::Exact);
    ExpectStr("same-engine zero date value untouched", cc.cell.text, L"0000-00-00");
}

// ---- ruling: MySQL zero-date -> Unrepresentable (NOT NULL) ---------------

static void TestZeroDate()
{
    const ColumnBridge b = OneBridge(MyCol(L"d", ColKind::Date, L"date"), Dialect::MySQL,
                                     PgCol(L"d", ColKind::Date, L"date"), Dialect::Postgres);
    ExpectTrue("cross-engine date arms TemporalGuard", b.op == BridgeOp::TemporalGuard && !b.passthrough);

    const ConvertedCell zero = ConvertCell(Txt(L"0000-00-00"), b);
    ExpectVerdict("MySQL 0000-00-00 -> Unrepresentable", zero.verdict, ValueVerdict::Unrepresentable);
    ExpectTrue("zero date is NOT silently coerced to NULL",
               !(zero.verdict == ValueVerdict::Coerced && zero.cell.kind == CellKind::Null));

    const ColumnBridge bt = OneBridge(MyCol(L"ts", ColKind::Timestamp, L"datetime"), Dialect::MySQL,
                                      PgCol(L"ts", ColKind::Timestamp, L"timestamp without time zone"), Dialect::Postgres);
    ExpectVerdict("MySQL 0000-00-00 00:00:00 -> Unrepresentable",
                  ConvertCell(Txt(L"0000-00-00 00:00:00"), bt).verdict, ValueVerdict::Unrepresentable);
    ExpectVerdict("a real datetime crosses Exact",
                  ConvertCell(Txt(L"2026-07-20 09:30:00"), bt).verdict, ValueVerdict::Exact);
}

// ---- ruling: TINYINT(1) 0/1 -> PG boolean FALSE/TRUE (Coerced) -----------

static void TestBoolCoercion()
{
    const ColumnBridge b = OneBridge(MyCol(L"flag", ColKind::Boolean, L"tinyint(1)"), Dialect::MySQL,
                                     PgCol(L"flag", ColKind::Boolean, L"boolean"), Dialect::Postgres);
    ExpectTrue("tinyint(1)->boolean arms BoolToPgBool", b.op == BridgeOp::BoolToPgBool);

    const ConvertedCell one = ConvertCell(Num(L"1"), b);
    ExpectVerdict("tinyint(1)=1 -> Coerced", one.verdict, ValueVerdict::Coerced);
    ExpectStr("tinyint(1)=1 -> TRUE", one.cell.text, L"TRUE");
    const ConvertedCell zero = ConvertCell(Num(L"0"), b);
    ExpectVerdict("tinyint(1)=0 -> Coerced", zero.verdict, ValueVerdict::Coerced);
    ExpectStr("tinyint(1)=0 -> FALSE", zero.cell.text, L"FALSE");

    // Rendered bare (no quotes) — a quoted 'TRUE' would be a type error in PG
    // for a boolean column in many contexts.
    ExpectStr("PG boolean renders unquoted", RenderLiteral(one.cell, Dialect::Postgres), L"TRUE");

    // MySQL stores 0..255 in a tinyint(1); anything but 0/1 is not a boolean.
    ExpectVerdict("tinyint(1)=7 -> Unrepresentable",
                  ConvertCell(Num(L"7"), b).verdict, ValueVerdict::Unrepresentable);

    // Reverse direction.
    const ColumnBridge r = OneBridge(PgCol(L"flag", ColKind::Boolean, L"boolean"), Dialect::Postgres,
                                     MyCol(L"flag", ColKind::Boolean, L"tinyint(1)"), Dialect::MySQL);
    ExpectTrue("boolean->tinyint(1) arms PgBoolToInt", r.op == BridgeOp::PgBoolToInt);
    ExpectStr("PG 't' -> 1", ConvertCell(Txt(L"t"), r).cell.text, L"1");
    ExpectStr("PG 'false' -> 0", ConvertCell(Txt(L"false"), r).cell.text, L"0");
}

// ---- ruling: unsigned overflow is a PER-VALUE decision --------------------

static void TestUnsignedRangePerValue()
{
    bool ok = false;
    const ColumnBridge b = OneBridge(MyCol(L"n", ColKind::Integer, L"bigint unsigned"), Dialect::MySQL,
                                     PgCol(L"n", ColKind::Integer, L"bigint"), Dialect::Postgres, &ok);
    // The crucial property: the COLUMN is not rejected. SyncTypeMap rates this
    // pairing Unmappable at the type level (PG has no unsigned), but blocking
    // every such table upfront would needlessly stop tables whose values all fit.
    ExpectTrue("unsigned column is NOT rejected upfront", ok);
    ExpectTrue("unsigned->signed arms IntRange", b.op == BridgeOp::IntRange && !b.passthrough);

    ExpectVerdict("value that fits -> Exact", ConvertCell(Num(L"42"), b).verdict, ValueVerdict::Exact);
    ExpectVerdict("LLONG_MAX fits bigint -> Exact",
                  ConvertCell(Num(L"9223372036854775807"), b).verdict, ValueVerdict::Exact);
    ExpectVerdict("2^63 overflows PG bigint -> Unrepresentable",
                  ConvertCell(Num(L"9223372036854775808"), b).verdict, ValueVerdict::Unrepresentable);
    ExpectVerdict("2^64-1 overflows PG bigint -> Unrepresentable",
                  ConvertCell(Num(L"18446744073709551615"), b).verdict, ValueVerdict::Unrepresentable);

    // Narrowing within the signed world is policed the same way.
    const ColumnBridge n = OneBridge(MyCol(L"n", ColKind::Integer, L"bigint"), Dialect::MySQL,
                                     PgCol(L"n", ColKind::Integer, L"smallint"), Dialect::Postgres);
    ExpectVerdict("300 fits smallint -> Exact", ConvertCell(Num(L"300"), n).verdict, ValueVerdict::Exact);
    ExpectVerdict("70000 overflows smallint -> Unrepresentable",
                  ConvertCell(Num(L"70000"), n).verdict, ValueVerdict::Unrepresentable);

    // A same-width pair needs no per-value check at all.
    const ColumnBridge w = OneBridge(MyCol(L"n", ColKind::Integer, L"int"), Dialect::MySQL,
                                     PgCol(L"n", ColKind::Integer, L"bigint"), Dialect::Postgres);
    ExpectTrue("widening int->bigint stays passthrough", w.passthrough);
}

// ---- ruling: binary/BLOB reframing ---------------------------------------

static void TestBinaryReframe()
{
    const ColumnBridge b = OneBridge(MyCol(L"payload", ColKind::Blob, L"blob"), Dialect::MySQL,
                                     PgCol(L"payload", ColKind::Blob, L"bytea"), Dialect::Postgres);
    ExpectTrue("blob->bytea arms BinaryReframe", b.op == BridgeOp::BinaryReframe);

    const ConvertedCell cc = ConvertCell(Bin(L"DEADBEEF"), b);
    ExpectVerdict("binary -> Coerced", cc.verdict, ValueVerdict::Coerced);
    ExpectStr("hex payload is unchanged", cc.cell.text, L"DEADBEEF");
    // The framing change is what makes it Coerced: MySQL 0x… vs PG '\x…'.
    ExpectStr("renders as PG bytea literal", RenderLiteral(cc.cell, Dialect::Postgres), L"'\\xDEADBEEF'");
    ExpectStr("same payload renders as MySQL hex", RenderLiteral(cc.cell, Dialect::MySQL), L"0xDEADBEEF");
}

// ---- ruling: NULL vs empty string are preserved exactly -------------------

static void TestNullVsEmpty()
{
    const ColumnBridge t = OneBridge(MyCol(L"s", ColKind::Varchar, L"varchar(80)", 80), Dialect::MySQL,
                                     PgCol(L"s", ColKind::Varchar, L"character varying", 80), Dialect::Postgres);
    const ConvertedCell nul = ConvertCell(Nul(), t);
    ExpectTrue("NULL stays NULL", nul.cell.kind == CellKind::Null);
    ExpectVerdict("NULL is Exact", nul.verdict, ValueVerdict::Exact);

    const ConvertedCell empty = ConvertCell(Txt(L""), t);
    ExpectTrue("empty string stays Text (not NULL)", empty.cell.kind == CellKind::Text);
    ExpectVerdict("empty string is Exact", empty.verdict, ValueVerdict::Exact);
    ExpectStr("empty string renders as ''", RenderLiteral(empty.cell, Dialect::Postgres), L"''");
    ExpectStr("NULL renders as NULL", RenderLiteral(nul.cell, Dialect::Postgres), L"NULL");

    // Also true on a bridge that would otherwise rewrite the value.
    const ColumnBridge b = OneBridge(MyCol(L"flag", ColKind::Boolean, L"tinyint(1)"), Dialect::MySQL,
                                     PgCol(L"flag", ColKind::Boolean, L"boolean"), Dialect::Postgres);
    ExpectTrue("NULL through a coercing bridge stays NULL",
               ConvertCell(Nul(), b).cell.kind == CellKind::Null);
}

// ---- ruling: invalid UTF-8 -> Unrepresentable ----------------------------

static void TestInvalidUtf8()
{
    const ColumnBridge t = OneBridge(MyCol(L"s", ColKind::Text, L"text"), Dialect::MySQL,
                                     PgCol(L"s", ColKind::Text, L"text"), Dialect::Postgres);
    ExpectTrue("cross-engine text arms TextValidate", t.op == BridgeOp::TextValidate);

    ExpectVerdict("clean text -> Exact", ConvertCell(Txt(L"hello 世界"), t).verdict, ValueVerdict::Exact);

    wxString bad;
    bad << L"ab" << wxUniChar(0xFFFD) << L"cd";   // decoder's replacement marker
    ExpectVerdict("U+FFFD (failed decode) -> Unrepresentable",
                  ConvertCell(Txt(bad), t).verdict, ValueVerdict::Unrepresentable);

    wxString withNul;
    withNul << L"a" << wxUniChar(0) << L"b";      // PG text cannot hold a NUL byte
    ExpectVerdict("embedded NUL -> Unrepresentable",
                  ConvertCell(Txt(withNul), t).verdict, ValueVerdict::Unrepresentable);
}

// ---- BuildBridges structural cases ---------------------------------------

static void TestBuildBridgesRejections()
{
    // Kinds from different families: no value-level mapping exists.
    bool ok = true;
    OneBridge(MyCol(L"n", ColKind::Integer, L"int"), Dialect::MySQL,
              PgCol(L"n", ColKind::Varchar, L"character varying", 10), Dialect::Postgres, &ok);
    ExpectTrue("integer -> varchar is refused at build time", !ok);

    // Target column missing.
    {
        TableSchema src, tgt;
        src.name = L"t"; tgt.name = L"t";
        src.columns.push_back(MyCol(L"a", ColKind::Integer, L"int"));
        src.columns.push_back(MyCol(L"gone", ColKind::Integer, L"int"));
        tgt.columns.push_back(PgCol(L"a", ColKind::Integer, L"integer"));
        std::vector<ColumnBridge> bs; std::vector<Finding> fs;
        ExpectTrue("missing target column fails the build",
                   !BuildBridges(src, Dialect::MySQL, tgt, Dialect::Postgres, bs, fs));
        ExpectTrue("missing target column is reported as a Finding", !fs.empty());
        ExpectTrue("the surviving column still got a bridge", bs.size() == 1);
    }

    // Target NOT NULL column with no source counterpart and no default.
    {
        TableSchema src, tgt;
        src.name = L"t"; tgt.name = L"t";
        src.columns.push_back(MyCol(L"a", ColKind::Integer, L"int"));
        tgt.columns.push_back(PgCol(L"a", ColKind::Integer, L"integer"));
        NormColumn req = PgCol(L"req", ColKind::Integer, L"integer");
        req.notNull = true;
        tgt.columns.push_back(req);
        std::vector<ColumnBridge> bs; std::vector<Finding> fs;
        ExpectTrue("target NOT NULL column with no source fails the build",
                   !BuildBridges(src, Dialect::MySQL, tgt, Dialect::Postgres, bs, fs));
    }
}

// ---- T3: the structural gate ---------------------------------------------

static void TestUnrepresentableCannotBecomeRowChange()
{
    const ColumnBridge id = OneBridge(MyCol(L"id", ColKind::Integer, L"int"), Dialect::MySQL,
                                      PgCol(L"id", ColKind::Integer, L"integer"), Dialect::Postgres);
    const ColumnBridge d  = OneBridge(MyCol(L"d", ColKind::Date, L"date"), Dialect::MySQL,
                                      PgCol(L"d", ColKind::Date, L"date"), Dialect::Postgres);

    // A row whose date cell is a MySQL zero date.
    RowChangeBuilder b(RowChange::Op::Insert, L"orders", L"id=7");
    ExpectTrue("good cell is accepted", b.AddCell(Num(L"7"), id));
    ExpectTrue("zero-date cell is refused", !b.AddCell(Txt(L"0000-00-00"), d));
    ExpectTrue("builder is blocked", b.Blocked());
    ExpectTrue("refusal produced a Finding", b.Findings().size() == 1);

    std::optional<RowChange> row = b.Build();
    // THE property: there is no RowChange for this row. Not an empty one, not a
    // partial one — none. The only expression that produces a RowChange returned
    // nullopt, so no downstream code can be handed this row to execute.
    ExpectTrue("Build() yields nullopt for a blocked row", !row.has_value());

    // And once blocked, adding good cells cannot un-block it.
    ExpectTrue("later good cells do not un-block", !b.AddCell(Num(L"1"), id) && b.Blocked());

    // A partially-converted row is impossible: the good cell that was accepted
    // before the failure never reaches anyone.
    RowChangeSet set;
    RowChangeBuilder b2(RowChange::Op::Insert, L"orders", L"id=8");
    b2.AddCell(Num(L"8"), id);
    b2.AddCell(Txt(L"0000-00-00"), d);
    ExpectTrue("RowChangeSet::Accept refuses the blocked row", !set.Accept(std::move(b2)));
    ExpectTrue("blocked row is counted, not lost", set.Stat().blocked == 1);
    ExpectTrue("no partial row entered the sample", set.Sample().empty());
    ExpectTrue("the set is not executable", !set.Executable());
    ExpectTrue("the refusal is in the findings", !set.Findings().empty());
}

static void TestRowChangeSetHappyPath()
{
    const ColumnBridge id = OneBridge(MyCol(L"id", ColKind::Integer, L"int"), Dialect::MySQL,
                                      PgCol(L"id", ColKind::Integer, L"integer"), Dialect::Postgres);
    const ColumnBridge fl = OneBridge(MyCol(L"flag", ColKind::Boolean, L"tinyint(1)"), Dialect::MySQL,
                                      PgCol(L"flag", ColKind::Boolean, L"boolean"), Dialect::Postgres);

    RowChangeSet set;
    ExpectTrue("fresh set is executable", set.Executable());

    RowChangeBuilder b(RowChange::Op::Insert, L"orders", L"id=1");
    b.AddCell(Num(L"1"), id);
    b.AddCell(Num(L"1"), fl);
    b.AddKeyCell(Num(L"1"), id);
    ExpectTrue("clean row is accepted", set.Accept(std::move(b)));
    ExpectTrue("insert counted", set.Stat().inserts == 1 && set.Stat().Total() == 1);
    ExpectTrue("row landed in the sample", set.Sample().size() == 1);
    ExpectTrue("row verdict is Coerced (bool rewrite)",
               set.Sample()[0].Verdict() == ValueVerdict::Coerced);
    ExpectTrue("set stays executable", set.Executable());

    RowRenderSpec spec;
    spec.qualifiedTable = L"\"public\".\"orders\"";
    spec.columns   = { L"id", L"flag" };
    spec.pkColumns = { L"id" };
    spec.dialect   = Dialect::Postgres;
    ExpectStr("INSERT renders with the coerced boolean",
              RenderRowDml(set.Sample()[0], spec),
              L"INSERT INTO \"public\".\"orders\" (\"id\", \"flag\") VALUES (1, TRUE)");
}

static void TestSampleCap()
{
    const ColumnBridge id = OneBridge(MyCol(L"id", ColKind::Integer, L"int"), Dialect::MySQL,
                                      PgCol(L"id", ColKind::Integer, L"integer"), Dialect::Postgres);
    RowChangeSet set;
    const long long n = static_cast<long long>(RowChangeSet::kSampleCap) + 25;
    for (long long i = 0; i < n; ++i) {
        RowChangeBuilder b(RowChange::Op::Insert, L"t", wxString::Format(L"id=%lld", i));
        b.AddCell(Num(wxString::Format(L"%lld", i)), id);
        b.AddKeyCell(Num(wxString::Format(L"%lld", i)), id);
        set.Accept(std::move(b));
    }
    ExpectTrue("all rows counted", set.Stat().inserts == n);
    ExpectTrue("sample capped at kSampleCap", set.Sample().size() == RowChangeSet::kSampleCap);
    ExpectTrue("truncation is flagged", set.Truncated());
    ExpectTrue("capping does not make the set unexecutable", set.Executable());
}

static void TestKeylessUpdateRefused()
{
    const ColumnBridge id = OneBridge(MyCol(L"id", ColKind::Integer, L"int"), Dialect::MySQL,
                                      PgCol(L"id", ColKind::Integer, L"integer"), Dialect::Postgres);
    RowChangeBuilder b(RowChange::Op::Update, L"t", L"?");
    b.AddCell(Num(L"5"), id);          // value but no key
    ExpectTrue("UPDATE with no key cannot be built", !b.Build().has_value());

    RowChangeBuilder d(RowChange::Op::Delete, L"t", L"?");
    ExpectTrue("DELETE with no key cannot be built", !d.Build().has_value());
}

static void TestUpdateAndDeleteRendering()
{
    const ColumnBridge id = OneBridge(MyCol(L"id", ColKind::Integer, L"int"), Dialect::MySQL,
                                      PgCol(L"id", ColKind::Integer, L"integer"), Dialect::Postgres);
    const ColumnBridge s  = OneBridge(MyCol(L"s", ColKind::Text, L"text"), Dialect::MySQL,
                                      PgCol(L"s", ColKind::Text, L"text"), Dialect::Postgres);

    RowRenderSpec spec;
    spec.qualifiedTable = L"\"t\"";
    spec.columns   = { L"id", L"s" };
    spec.pkColumns = { L"id" };
    spec.dialect   = Dialect::Postgres;

    RowChangeBuilder u(RowChange::Op::Update, L"t", L"id=3");
    u.AddCell(Num(L"3"), id);
    u.AddCell(Txt(L"o'brien"), s);
    u.AddKeyCell(Num(L"3"), id);
    std::optional<RowChange> ur = u.Build();
    ExpectTrue("update row builds", ur.has_value());
    ExpectStr("UPDATE skips the PK in SET and escapes the quote",
              RenderRowDml(*ur, spec), L"UPDATE \"t\" SET \"s\" = 'o''brien' WHERE \"id\" = 3");

    RowChangeBuilder del(RowChange::Op::Delete, L"t", L"id=4");
    del.AddKeyCell(Num(L"4"), id);
    std::optional<RowChange> dr = del.Build();
    ExpectTrue("delete row builds", dr.has_value());
    ExpectStr("DELETE renders with a WHERE", RenderRowDml(*dr, spec), L"DELETE FROM \"t\" WHERE \"id\" = 4");
}

static void TestFindingBlocksSet()
{
    RowChangeSet set;
    Finding f;
    f.table = L"t"; f.column = L"c"; f.verdict = TypeVerdict::Unmappable; f.reason = L"x";
    set.AddFinding(f, ValueVerdict::Unrepresentable);
    ExpectTrue("a blocking Finding alone makes the set non-executable", !set.Executable());

    RowChangeSet set2;
    Finding ok;
    ok.table = L"t"; ok.column = L"c"; ok.verdict = TypeVerdict::Equivalent; ok.reason = L"note";
    set2.AddFinding(ok, ValueVerdict::Coerced);
    ExpectTrue("a Coerced Finding does not block", set2.Executable());
}

// ---- T5: identity/sequence repair postamble ------------------------------

static void TestSequenceFixes()
{
    TableSchema t;
    t.name = L"orders";
    NormColumn id = MyCol(L"id", ColKind::Integer, L"bigint");
    id.autoIncrement = true;
    t.columns.push_back(id);
    t.columns.push_back(MyCol(L"note", ColKind::Text, L"text"));

    std::vector<SeqFix> fixes;
    wxString why;

    // PostgreSQL: self-computing, no caller-supplied max needed.
    ExpectTrue("PG seq fix builds without a known max",
               BuildSequenceFixes(t, Dialect::Postgres, L"\"public\".\"orders\"", -1, fixes, why));
    ExpectTrue("one fix for one identity column", fixes.size() == 1);
    ExpectTrue("PG fix is self-computing", fixes[0].selfComputing);
    ExpectStr("PG setval statement",
              fixes[0].statement,
              L"SELECT setval(pg_get_serial_sequence('\"public\".\"orders\"', 'id'), "
              L"COALESCE((SELECT MAX(\"id\") FROM \"public\".\"orders\"), 1), "
              L"(SELECT MAX(\"id\") FROM \"public\".\"orders\") IS NOT NULL)");

    // MySQL: needs a literal; max+1 because AUTO_INCREMENT is the NEXT value.
    ExpectTrue("MySQL seq fix builds with a known max",
               BuildSequenceFixes(t, Dialect::MySQL, L"`orders`", 100, fixes, why));
    ExpectStr("MySQL AUTO_INCREMENT statement", fixes[0].statement,
              L"ALTER TABLE `orders` AUTO_INCREMENT = 101");
    ExpectTrue("MySQL fix is not self-computing", !fixes[0].selfComputing);

    // MySQL without a max must REFUSE, not guess — a too-low AUTO_INCREMENT is
    // exactly the latent duplicate-key bug this postamble exists to prevent.
    ExpectTrue("MySQL refuses when the max is unknown",
               !BuildSequenceFixes(t, Dialect::MySQL, L"`orders`", -1, fixes, why));
    ExpectTrue("...and says why", !why.IsEmpty() && fixes.empty());

    // SQL Server reseeds to the max itself (next identity = seed + 1).
    ExpectTrue("SQL Server seq fix builds",
               BuildSequenceFixes(t, Dialect::SqlServer, L"dbo.orders", 100, fixes, why));
    ExpectStr("SQL Server RESEED statement", fixes[0].statement,
              L"DBCC CHECKIDENT (N'dbo.orders', RESEED, 100)");

    // No identity column: nothing to do, and NOT an error (empty why).
    TableSchema plain;
    plain.name = L"log";
    plain.columns.push_back(MyCol(L"msg", ColKind::Text, L"text"));
    ExpectTrue("table without an identity column yields no fix",
               !BuildSequenceFixes(plain, Dialect::Postgres, L"\"log\"", -1, fixes, why));
    ExpectTrue("no identity column is not an error", why.IsEmpty() && fixes.empty());

    // SQLite maintains its own rowid/sqlite_sequence through the INSERTs.
    ExpectTrue("SQLite needs no repair",
               !BuildSequenceFixes(t, Dialect::Sqlite, L"\"orders\"", 100, fixes, why));
    ExpectTrue("SQLite skip is not an error", why.IsEmpty());

    // Oracle refuses honestly rather than emitting a wrong ALTER.
    ExpectTrue("Oracle refuses", !BuildSequenceFixes(t, Dialect::Oracle, L"\"ORDERS\"", 100, fixes, why));
    ExpectTrue("Oracle says why", !why.IsEmpty());
}

int main()
{
    std::printf("== SyncValueMapTests (ADR-015 T1 + T3 + T5) ==\n");
    TestMayEmit();
    TestSameEngineIsIdentity();
    TestZeroDate();
    TestBoolCoercion();
    TestUnsignedRangePerValue();
    TestBinaryReframe();
    TestNullVsEmpty();
    TestInvalidUtf8();
    TestBuildBridgesRejections();
    TestUnrepresentableCannotBecomeRowChange();
    TestRowChangeSetHappyPath();
    TestSampleCap();
    TestKeylessUpdateRefused();
    TestUpdateAndDeleteRendering();
    TestFindingBlocksSet();
    TestSequenceFixes();
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
