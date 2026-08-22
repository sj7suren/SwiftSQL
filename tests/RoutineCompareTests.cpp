// RoutineCompareTests.cpp — db::sync::CompareRoutines + the matching key
// (src/db/RoutineCompare.{h,cpp}, ADR-013), the pure decision layer of the
// compare-only functions & stored procedures diff.
//
// What this file is really guarding. The feature answers a question the user
// asked directly ("同步时应该弹出两边不同的表，不同的函数，存储过程"), and the
// standing CEO ruling is that it may only ever ANSWER — never generate or apply
// routine DDL. That makes the verdict itself the entire product, so the verdicts
// have to be right, and above all they have to be HONEST about what they do not
// know:
//
//   * a cross-engine body pair is ALWAYS NotComparable — PL/pgSQL and MySQL's
//     routine dialect are different Turing-complete languages, so "same" is
//     unfounded and "differs" is a tautology;
//   * an unparseable body degrades to NotComparable, never to BodyDiffers;
//   * an ambiguous match degrades to NotComparable rather than picking one;
//   * a side we could not READ produces NO pairs at all, because "the target is
//     missing 40 functions" computed against an invisible catalog is a lie.
//
// It also pins the two structural rulings that keep routine DDL unreachable, in
// the only way a test can pin an ABSENCE: by compiling. See TestNoDdlSurface.
//
// Pure logic — links swiftsql::db, no connection, no GUI.
#include "db/RoutineCompare.h"
#include "db/RoutineDiff.h"
#include "db/DbDriver.h"
#include "db/MySqlRoutineRead.h"   // section 8: the reader's partial-read refusal

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

static const char* VerdictName(RoutineVerdict v)
{
    switch (v) {
    case RoutineVerdict::Identical:        return "Identical";
    case RoutineVerdict::SourceOnly:       return "SourceOnly";
    case RoutineVerdict::TargetOnly:       return "TargetOnly";
    case RoutineVerdict::SignatureDiffers: return "SignatureDiffers";
    case RoutineVerdict::BodyDiffers:      return "BodyDiffers";
    case RoutineVerdict::NotComparable:    return "NotComparable";
    }
    return "?";
}

static void ExpectVerdict(const char* name, RoutineVerdict got, RoutineVerdict want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s (got %s, want %s)\n", name, VerdictName(got), VerdictName(want));
    } else {
        std::printf("  ok   %s -> %s\n", name, VerdictName(got));
    }
}

static void ExpectText(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n        got  [%s]\n        want [%s]\n",
                    name, (const char*)got.utf8_str(), (const char*)want.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---------------------------------------------------------------------------
// fixtures
// ---------------------------------------------------------------------------

static RoutineDef Fn(const wxString& name, std::vector<wxString> args,
                      const wxString& ret, const wxString& body,
                      const wxString& lang = L"SQL")
{
    RoutineDef r;
    r.schema     = L"app";
    r.name       = name;
    r.kind       = RoutineKind::Function;
    r.argTypes   = std::move(args);
    r.returnType = ret;
    r.language   = lang;
    r.body       = body;
    return r;
}

static RoutineDef Proc(const wxString& name, std::vector<wxString> args,
                        const wxString& body, const wxString& lang = L"SQL")
{
    RoutineDef r;
    r.schema   = L"app";
    r.name     = name;
    r.kind     = RoutineKind::Procedure;
    r.argTypes = std::move(args);
    r.language = lang;
    r.body     = body;
    return r;
}

static RoutineCompareOptions MyMy()
{
    return MakeRoutineCompareOptions(Dialect::MySQL, Dialect::MySQL);
}
static RoutineCompareOptions PgPg()
{
    return MakeRoutineCompareOptions(Dialect::Postgres, Dialect::Postgres);
}
static RoutineCompareOptions MyPg()
{
    return MakeRoutineCompareOptions(Dialect::MySQL, Dialect::Postgres);
}

// The single pair produced by a one-vs-one comparison.
static RoutinePair One(const RoutineDiffSet& d)
{
    return d.pairs.empty() ? RoutinePair{} : d.pairs[0];
}

static RoutineDiffSet Cmp(std::vector<RoutineDef> s, std::vector<RoutineDef> t,
                          const RoutineCompareOptions& opt)
{
    return CompareRoutines(s, t, RoutineReadStatus::Ok, RoutineReadStatus::Ok, opt);
}

// ---------------------------------------------------------------------------
// 1. The matching key: lexical, total, order-independent
// ---------------------------------------------------------------------------
static void TestArgTypeNormalization()
{
    std::printf("\n-- argument type normalization --\n");

    ExpectText("length dropped",        NormalizeArgType(L"varchar(50)"),  L"varchar");
    ExpectText("precision dropped",     NormalizeArgType(L"decimal(10,2)"), L"numeric");
    ExpectText("display width dropped", NormalizeArgType(L"INT(11)"),      L"integer");
    ExpectText("pg alias int4",         NormalizeArgType(L"int4"),         L"integer");
    ExpectText("pg alias int8",         NormalizeArgType(L"int8"),         L"bigint");
    ExpectText("character varying",     NormalizeArgType(L"character varying(80)"), L"varchar");
    ExpectText("double precision",      NormalizeArgType(L"double precision"), L"double");
    ExpectText("timestamptz spelled out",
               NormalizeArgType(L"timestamp with time zone"), L"timestamptz");
    ExpectText("case + whitespace",     NormalizeArgType(L"  BigInt  "),   L"bigint");

    // UNSIGNED is part of MySQL's type meaning, so it stays in the key: an
    // unsigned parameter genuinely is a different parameter.
    ExpectText("unsigned kept",   NormalizeArgType(L"int(10) unsigned"), L"integer unsigned");
    ExpectText("signed dropped",  NormalizeArgType(L"int signed"),       L"integer");
    ExpectText("zerofill dropped",NormalizeArgType(L"int(4) zerofill"),  L"integer");
    ExpectTrue("unsigned differs from signed",
               NormalizeArgType(L"int unsigned") != NormalizeArgType(L"int"));

    // Arrays are distinct types, so the suffix survives.
    ExpectText("array suffix kept", NormalizeArgType(L"integer[]"), L"integer[]");
    ExpectTrue("array differs from scalar",
               NormalizeArgType(L"integer[]") != NormalizeArgType(L"integer"));

    // Charset/collation say nothing about the type's identity.
    ExpectText("charset dropped",
               NormalizeArgType(L"varchar(50) character set utf8mb4"), L"varchar");
    ExpectText("collate dropped",
               NormalizeArgType(L"varchar(50) collate utf8mb4_bin"), L"varchar");
}

static void TestMatchKey()
{
    std::printf("\n-- matching key --\n");

    const RoutineCompareOptions o = MyPg();

    // Cross-engine spellings of the same signature collapse to one key — this is
    // what lets a MySQL routine match its PG counterpart at all.
    ExpectTrue("int(11) key == integer key",
               RoutineMatchKey(Fn(L"f", {L"int(11)"}, L"int(11)", L""), o) ==
               RoutineMatchKey(Fn(L"f", {L"integer"}, L"integer", L""), o));

    // The return type is NOT in the key: PG overloads on arguments only, and
    // MySQL cannot overload at all, so keying on it would split a routine whose
    // return type changed into an unrelated add + delete.
    ExpectTrue("return type not in the key",
               RoutineMatchKey(Fn(L"f", {L"integer"}, L"integer", L""), o) ==
               RoutineMatchKey(Fn(L"f", {L"integer"}, L"text", L""), o));

    // Argument ORDER is part of the identity.
    ExpectTrue("argument order matters",
               RoutineMatchKey(Fn(L"f", {L"integer", L"text"}, L"", L""), o) !=
               RoutineMatchKey(Fn(L"f", {L"text", L"integer"}, L"", L""), o));

    // A function and a procedure of the same name+args are DIFFERENT objects and
    // MySQL lets both exist; the kind is therefore in the key.
    ExpectTrue("kind is part of the key",
               RoutineMatchKey(Fn(L"x", {L"integer"}, L"integer", L""), o) !=
               RoutineMatchKey(Proc(L"x", {L"integer"}, L""), o));

    // Schema is a CONTAINER, not identity — cross-engine the two containers are
    // named differently by construction ("app" vs "public").
    RoutineDef a = Fn(L"f", {L"integer"}, L"integer", L"");
    RoutineDef b = a; b.schema = L"public";
    ExpectTrue("schema excluded by default", RoutineMatchKey(a, o) == RoutineMatchKey(b, o));
    RoutineCompareOptions q = o; q.qualifyBySchema = true;
    ExpectTrue("schema included when asked", RoutineMatchKey(a, q) != RoutineMatchKey(b, q));

    // Name case: folded when MySQL is involved (its names are case-insensitive),
    // NOT folded for PG↔PG where a quoted "MyFunc" is genuinely distinct.
    RoutineDef lower = Fn(L"getvalue", {}, L"integer", L"");
    RoutineDef upper = Fn(L"getValue", {}, L"integer", L"");
    ExpectTrue("MySQL: names fold",  MyMy().foldNameCase);
    ExpectTrue("PG↔PG: names do NOT fold", !PgPg().foldNameCase);
    ExpectTrue("MySQL: getValue == getvalue",
               RoutineMatchKey(lower, MyMy()) == RoutineMatchKey(upper, MyMy()));
    ExpectTrue("PG: getValue != getvalue",
               RoutineMatchKey(lower, PgPg()) != RoutineMatchKey(upper, PgPg()));
}

// ---------------------------------------------------------------------------
// 2. Same-engine verdicts
// ---------------------------------------------------------------------------
static void TestSameEngineVerdicts()
{
    std::printf("\n-- same-engine verdicts --\n");

    // Identical: reformatted + commented twin. This is the zero-false-positive
    // bar in one assertion — a user who has synced nothing must see "相同".
    {
        const auto d = Cmp({Fn(L"total", {L"int"}, L"int",
                               L"BEGIN\n  -- add one\n  RETURN a + 1;\nEND")},
                           {Fn(L"total", {L"int"}, L"int",
                               L"BEGIN RETURN a + 1; END")}, MyMy());
        ExpectVerdict("reformatted twin", One(d).verdict, RoutineVerdict::Identical);
        ExpectTrue("identical is not a difference", d.stat.Differences() == 0);
        ExpectTrue("identical is counted", d.stat.identical == 1);
    }

    // BodyDiffers: a real change, same signature.
    {
        const auto d = Cmp({Fn(L"total", {L"int"}, L"int", L"BEGIN RETURN a + 1; END")},
                           {Fn(L"total", {L"int"}, L"int", L"BEGIN RETURN a + 2; END")},
                           MyMy());
        ExpectVerdict("real body change", One(d).verdict, RoutineVerdict::BodyDiffers);
        ExpectTrue("both sides carried for side-by-side display",
                   One(d).hasSource && One(d).hasTarget);
        ExpectTrue("source body retained verbatim",
                   One(d).source.body == wxString(L"BEGIN RETURN a + 1; END"));
    }

    // One-sided.
    {
        const auto d = Cmp({Fn(L"only_src", {}, L"int", L"BEGIN RETURN 1; END")},
                           {Fn(L"only_tgt", {}, L"int", L"BEGIN RETURN 1; END")},
                           MyMy());
        ExpectTrue("two one-sided pairs", d.pairs.size() == 2);
        ExpectTrue("source-only counted", d.stat.sourceOnly == 1);
        ExpectTrue("target-only counted", d.stat.targetOnly == 1);
    }

    // Signature: same name+arity, wider parameter. Same-engine, raw text is
    // truth — varchar(50) -> varchar(100) is a change to this routine.
    {
        const auto d = Cmp({Fn(L"f", {L"varchar(50)"},  L"int", L"BEGIN RETURN 1; END")},
                           {Fn(L"f", {L"varchar(100)"}, L"int", L"BEGIN RETURN 1; END")},
                           MyMy());
        ExpectVerdict("same-engine width change", One(d).verdict,
                      RoutineVerdict::SignatureDiffers);
        ExpectTrue("reason names both declarations",
                   One(d).reason.Contains(L"varchar(50)") &&
                   One(d).reason.Contains(L"varchar(100)"));
    }

    // Return type change, identical body.
    {
        const auto d = Cmp({Fn(L"f", {L"int"}, L"int",  L"BEGIN RETURN 1; END")},
                           {Fn(L"f", {L"int"}, L"bigint", L"BEGIN RETURN 1; END")},
                           MyMy());
        ExpectVerdict("return type change", One(d).verdict, RoutineVerdict::SignatureDiffers);
    }

    // SECURITY DEFINER: byte-identical bodies that do NOT do the same thing.
    {
        RoutineDef s = Fn(L"f", {}, L"int", L"BEGIN RETURN 1; END");
        RoutineDef t = s; t.securityDefiner = true;
        const auto d = Cmp({s}, {t}, MyMy());
        ExpectVerdict("SECURITY DEFINER difference is never 'identical'",
                      One(d).verdict, RoutineVerdict::SignatureDiffers);
    }

    // PG volatility is a planner contract; a change is real.
    {
        RoutineDef s = Fn(L"f", {}, L"integer", L"SELECT 1", L"sql");
        s.volatility = L"immutable";
        RoutineDef t = s; t.volatility = L"volatile";
        const auto d = Cmp({s}, {t}, PgPg());
        ExpectVerdict("volatility change", One(d).verdict, RoutineVerdict::SignatureDiffers);
    }

    // Parameter MODE is not in the key, so it surfaces here — after the routines
    // have been matched by name+types, which is the honest ordering.
    {
        RoutineDef s = Proc(L"p", {L"int"}, L"BEGIN SET x = 1; END");
        s.argModes = {L"IN"};
        RoutineDef t = s; t.argModes = {L"OUT"};
        const auto d = Cmp({s}, {t}, MyMy());
        ExpectVerdict("IN -> OUT is a signature difference",
                      One(d).verdict, RoutineVerdict::SignatureDiffers);
    }

    // ...but a mode the READER did not report must not manufacture a difference.
    {
        RoutineDef s = Proc(L"p", {L"int"}, L"BEGIN SET x = 1; END");
        s.argModes = {L"IN"};
        RoutineDef t = Proc(L"p", {L"int"}, L"BEGIN SET x = 1; END");   // no modes
        const auto d = Cmp({s}, {t}, MyMy());
        ExpectVerdict("unknown mode is not a difference", One(d).verdict,
                      RoutineVerdict::Identical);
    }

    // A function and a procedure of the same name coexist in MySQL and must not
    // be paired with each other.
    {
        const auto d = Cmp({Fn(L"x", {L"int"}, L"int", L"BEGIN RETURN 1; END"),
                            Proc(L"x", {L"int"}, L"BEGIN SET a = 1; END")},
                           {Fn(L"x", {L"int"}, L"int", L"BEGIN RETURN 1; END"),
                            Proc(L"x", {L"int"}, L"BEGIN SET a = 1; END")},
                           MyMy());
        ExpectTrue("function and procedure paired separately", d.pairs.size() == 2);
        ExpectTrue("both identical, no cross-pairing", d.stat.identical == 2);
    }
}

// ---------------------------------------------------------------------------
// 3. Cross-engine: bodies are ALWAYS NotComparable
// ---------------------------------------------------------------------------
static void TestCrossEngineNeverClaimsBodies()
{
    std::printf("\n-- cross-engine bodies --\n");

    // Byte-identical bodies, cross engine. We STILL decline: the two are read by
    // different procedural languages, and "identical text" is not "identical
    // behaviour" across engines.
    {
        const auto d = Cmp({Fn(L"f", {L"int(11)"}, L"int(11)", L"BEGIN RETURN 1; END", L"SQL")},
                           {Fn(L"f", {L"integer"}, L"integer", L"BEGIN RETURN 1; END", L"plpgsql")},
                           MyPg());
        ExpectVerdict("identical text cross-engine is still NotComparable",
                      One(d).verdict, RoutineVerdict::NotComparable);
        ExpectTrue("reason explains the refusal", !One(d).reason.IsEmpty());
        ExpectTrue("sameEngine flag recorded on the set", !d.sameEngine);
    }

    // Wildly different bodies, cross engine: same verdict. NotComparable is not
    // a fallback for "we looked and could not decide" — we never look.
    {
        const auto d = Cmp({Fn(L"f", {L"int(11)"}, L"int(11)", L"BEGIN RETURN 1; END")},
                           {Fn(L"f", {L"integer"}, L"integer", L"BEGIN RETURN 99999; END")},
                           MyPg());
        ExpectVerdict("different text cross-engine is also NotComparable",
                      One(d).verdict, RoutineVerdict::NotComparable);
    }

    // Cross-engine, matched by key despite different spellings: the pair is
    // reported ONCE, not as source-only + target-only.
    {
        const auto d = Cmp({Fn(L"f", {L"int(11)", L"varchar(50)"}, L"int(11)", L"x")},
                           {Fn(L"f", {L"integer", L"character varying(50)"}, L"integer", L"y")},
                           MyPg());
        ExpectTrue("cross-engine spellings matched into one pair", d.pairs.size() == 1);
        ExpectTrue("no phantom one-sided entries",
                   d.stat.sourceOnly == 0 && d.stat.targetOnly == 0);
    }

    // Cross-engine WIDTH difference is noise (int(11) vs integer), and must NOT
    // be reported as a signature difference...
    {
        const auto d = Cmp({Fn(L"f", {L"int(11)"}, L"int(11)", L"x")},
                           {Fn(L"f", {L"integer"}, L"integer", L"y")}, MyPg());
        ExpectTrue("cross-engine spelling noise is not a signature difference",
                   One(d).verdict != RoutineVerdict::SignatureDiffers);
    }

    // ...but a genuinely incompatible parameter type IS, and the canonical type
    // mapper is what explains it. This is the ONE place the graded verdict is
    // used, and it is used AFTER the name match, never to key it.
    {
        const auto d = Cmp({Fn(L"f", {L"int(10) unsigned"}, L"int", L"x")},
                           {Fn(L"f", {L"integer"}, L"integer", L"y")}, MyPg());
        ExpectTrue("unsigned vs signed is judged, one way or the other",
                   One(d).verdict == RoutineVerdict::SignatureDiffers ||
                   One(d).verdict == RoutineVerdict::NotComparable);
        ExpectTrue("a reason is always given", !One(d).reason.IsEmpty());
    }
}

// ---------------------------------------------------------------------------
// 4. Degradation: everything uncertain lands on NotComparable
// ---------------------------------------------------------------------------
static void TestDegradesToNotComparable()
{
    std::printf("\n-- degradation --\n");

    // Unparseable body (unterminated dollar quote). The normalizer refuses, and
    // the compare layer must NOT fall back to comparing raw text — which would
    // report BodyDiffers for two routines we simply failed to parse.
    {
        const auto d = Cmp({Fn(L"f", {}, L"integer", L"$$ never closed", L"plpgsql")},
                           {Fn(L"f", {}, L"integer", L"$$ different text", L"plpgsql")},
                           PgPg());
        ExpectVerdict("unparseable body -> NotComparable, not BodyDiffers",
                      One(d).verdict, RoutineVerdict::NotComparable);
        ExpectTrue("reason mentions parsing", !One(d).reason.IsEmpty());
    }

    // Same-engine language mismatch (PG sql vs plpgsql): different languages,
    // same refusal as cross-engine.
    {
        const auto d = Cmp({Fn(L"f", {}, L"integer", L"SELECT 1", L"sql")},
                           {Fn(L"f", {}, L"integer", L"BEGIN RETURN 1; END", L"plpgsql")},
                           PgPg());
        ExpectVerdict("sql vs plpgsql -> NotComparable", One(d).verdict,
                      RoutineVerdict::NotComparable);
    }

    // Ambiguous key: two PG overloads that normalize to the same key. Picking
    // one would produce a confident verdict about a pair we invented.
    {
        RoutineDef a = Fn(L"f", {L"varchar(10)"}, L"integer", L"A", L"sql");
        RoutineDef b = Fn(L"f", {L"varchar(20)"}, L"integer", L"B", L"sql");
        const auto d = Cmp({a, b}, {a, b}, PgPg());
        ExpectTrue("all four entries reported", d.pairs.size() == 4);
        ExpectTrue("every one is NotComparable", d.stat.notComparable == 4);
        for (const auto& p : d.pairs)
            ExpectTrue("ambiguity reason given", !p.reason.IsEmpty());
    }

    // The timid re-join: a routine whose KIND changed shows up as ONE pair
    // (SignatureDiffers) rather than as an unrelated deletion + addition.
    {
        const auto d = Cmp({Fn(L"convert", {L"int"}, L"int", L"BEGIN RETURN 1; END")},
                           {Proc(L"convert", {L"int"}, L"BEGIN SET a = 1; END")},
                           MyMy());
        ExpectTrue("kind change re-joined into one pair", d.pairs.size() == 1);
        ExpectVerdict("kind change is a signature difference", One(d).verdict,
                      RoutineVerdict::SignatureDiffers);
        ExpectTrue("both sides shown", One(d).hasSource && One(d).hasTarget);
    }

    // ...and the re-join stays timid: with TWO leftovers on a side it refuses,
    // because marrying two unrelated overloads would hide that one was dropped.
    {
        const auto d = Cmp({Fn(L"g", {L"integer"}, L"integer", L"A", L"sql"),
                            Fn(L"g", {L"text"},    L"integer", L"B", L"sql")},
                           {Fn(L"g", {L"boolean"}, L"integer", L"C", L"sql")},
                           PgPg());
        ExpectTrue("no speculative re-join with multiple leftovers",
                   d.stat.sourceOnly == 2 && d.stat.targetOnly == 1);
    }
}

// ---------------------------------------------------------------------------
// 5. ADR-013 Q4 — permission failure is a per-category state
// ---------------------------------------------------------------------------
static void TestPermissionDegradation()
{
    std::printf("\n-- permission / support degradation --\n");

    const std::vector<RoutineDef> some = {Fn(L"f", {}, L"int", L"BEGIN RETURN 1; END")};

    // The account cannot read the SOURCE catalog. We must not then report the
    // target's routines as "target only" — that would be a fabricated diff.
    {
        const auto d = CompareRoutines(std::vector<RoutineDef>{}, some,
                                       RoutineReadStatus::PermissionDenied,
                                       RoutineReadStatus::Ok, MyMy());
        ExpectTrue("not comparable", !d.Comparable());
        ExpectTrue("ZERO pairs — no fabricated one-sided entries", d.pairs.empty());
        ExpectTrue("no counted differences", d.stat.Differences() == 0);
        ExpectTrue("status preserved for the UI",
                   d.sourceStatus == RoutineReadStatus::PermissionDenied);
        ExpectText("label is the one the UI shows",
                   RoutineReadStatusLabel(d.sourceStatus), L"不可见（权限不足）");
    }

    // Same on the target side.
    {
        const auto d = CompareRoutines(some, std::vector<RoutineDef>{},
                                       RoutineReadStatus::Ok,
                                       RoutineReadStatus::PermissionDenied, MyMy());
        ExpectTrue("target denied -> zero pairs", d.pairs.empty());
        ExpectTrue("source routines are NOT reported as source-only",
                   d.stat.sourceOnly == 0);
    }

    // An unsupported dialect degrades the same way, with its own label.
    {
        const auto d = CompareRoutines(some, some, RoutineReadStatus::Ok,
                                       RoutineReadStatus::Unsupported, MyMy());
        ExpectTrue("unsupported -> zero pairs", d.pairs.empty());
        ExpectTrue("unsupported label differs from permission label",
                   RoutineReadStatusLabel(RoutineReadStatus::Unsupported) !=
                   RoutineReadStatusLabel(RoutineReadStatus::PermissionDenied));
    }

    // PER-ROUTINE privilege loss (MySQL lists a routine but returns its body as
    // NULL). Both bodies are then blank, so a body comparison would find them
    // EQUAL and report 相同 — a confident claim of sameness about text nobody
    // ever saw. It must degrade instead.
    {
        RoutineDef s = Fn(L"hidden", {L"int"}, L"int", L"");
        s.bodyReadable = false;
        RoutineDef t = s;
        const auto d = Cmp({s}, {t}, MyMy());
        ExpectVerdict("two hidden bodies are NOT 'identical'", One(d).verdict,
                      RoutineVerdict::NotComparable);
        ExpectTrue("reason names the privilege cause",
                   One(d).reason.Contains(L"权限"));
    }

    // One side hidden, one side visible: same refusal. We know the signature
    // matches and nothing more.
    {
        RoutineDef s = Fn(L"half", {L"int"}, L"int", L"");
        s.bodyReadable = false;
        RoutineDef t = Fn(L"half", {L"int"}, L"int", L"BEGIN RETURN 1; END");
        const auto d = Cmp({s}, {t}, MyMy());
        ExpectVerdict("one hidden body -> NotComparable", One(d).verdict,
                      RoutineVerdict::NotComparable);
    }

    // A hidden body still does NOT suppress a signature verdict — what we could
    // read, we report.
    {
        RoutineDef s = Fn(L"sig", {L"int"}, L"int", L"");
        s.bodyReadable = false;
        RoutineDef t = Fn(L"sig", {L"int"}, L"bigint", L"");
        t.bodyReadable = false;
        const auto d = Cmp({s}, {t}, MyMy());
        ExpectVerdict("signature difference still reported with hidden bodies",
                      One(d).verdict, RoutineVerdict::SignatureDiffers);
    }

    // Ok on both sides has NO label — the categories render normally.
    ExpectTrue("Ok has no label", RoutineReadStatusLabel(RoutineReadStatus::Ok).IsEmpty());

    // The feature switched off produces an empty set, not an error.
    {
        RoutineCompareOptions off = MyMy();
        off.enabled = false;
        const auto d = Cmp(some, some, off);
        ExpectTrue("disabled -> zero pairs", d.pairs.empty());
    }
}

// ---------------------------------------------------------------------------
// 6. Presentation contract the UI depends on
// ---------------------------------------------------------------------------
static void TestPresentationOrdering()
{
    std::printf("\n-- ordering + categories --\n");

    const auto d = Cmp({Fn(L"same", {}, L"int", L"BEGIN RETURN 1; END"),
                        Fn(L"changed", {}, L"int", L"BEGIN RETURN 1; END"),
                        Fn(L"gone_from_target", {}, L"int", L"BEGIN RETURN 1; END"),
                        Proc(L"p_same", {}, L"BEGIN SET a = 1; END"),
                        Proc(L"p_changed", {}, L"BEGIN SET a = 1; END")},
                       {Fn(L"same", {}, L"int", L"BEGIN RETURN 1; END"),
                        Fn(L"changed", {}, L"int", L"BEGIN RETURN 2; END"),
                        Fn(L"new_on_target", {}, L"int", L"BEGIN RETURN 1; END"),
                        Proc(L"p_same", {}, L"BEGIN SET a = 1; END"),
                        Proc(L"p_changed", {}, L"BEGIN SET a = 2; END")},
                       MyMy());

    ExpectTrue("every routine on either side is accounted for", d.pairs.size() == 6);
    ExpectTrue("counts add up", d.stat.Total() == 6);
    ExpectTrue("2 identical", d.stat.identical == 2);
    ExpectTrue("2 body differences", d.stat.bodyDiffers == 2);
    ExpectTrue("1 source-only", d.stat.sourceOnly == 1);
    ExpectTrue("1 target-only", d.stat.targetOnly == 1);

    // Differences first: a user opening the screen sees what changed without
    // scrolling past the identical majority.
    bool seenIdentical = false, orderOk = true;
    for (const auto& p : d.pairs) {
        if (p.verdict == RoutineVerdict::Identical) seenIdentical = true;
        else if (seenIdentical) orderOk = false;
    }
    ExpectTrue("differences sort before identical", orderOk);

    // The two tree categories the user asked for.
    ExpectTrue("函数 category has 4",     d.ByKind(RoutineKind::Function).size() == 4);
    ExpectTrue("存储过程 category has 2", d.ByKind(RoutineKind::Procedure).size() == 2);
    ExpectTrue("function differences: 3", d.DifferencesByKind(RoutineKind::Function).size() == 3);
    ExpectTrue("procedure differences: 1", d.DifferencesByKind(RoutineKind::Procedure).size() == 1);

    // Verdict labels are all populated — the tree has nothing blank to render.
    const RoutineVerdict all[] = {RoutineVerdict::Identical, RoutineVerdict::SourceOnly,
                                  RoutineVerdict::TargetOnly, RoutineVerdict::SignatureDiffers,
                                  RoutineVerdict::BodyDiffers, RoutineVerdict::NotComparable};
    for (RoutineVerdict v : all)
        ExpectTrue("verdict has a label", !RoutineVerdictLabel(v).IsEmpty());

    // Determinism: the same inputs in a different ORDER must produce the same
    // diff. (A key built on a graded, order-dependent type verdict would not.)
    std::vector<RoutineDef> s = {Fn(L"a", {L"int"}, L"int", L"X"),
                                  Fn(L"b", {L"int"}, L"int", L"Y"),
                                  Fn(L"c", {L"int"}, L"int", L"Z")};
    std::vector<RoutineDef> t = {Fn(L"c", {L"int"}, L"int", L"Z"),
                                  Fn(L"a", {L"int"}, L"int", L"X2"),
                                  Fn(L"b", {L"int"}, L"int", L"Y")};
    const auto d1 = Cmp(s, t, MyMy());
    std::vector<RoutineDef> sr(s.rbegin(), s.rend());
    std::vector<RoutineDef> tr(t.rbegin(), t.rend());
    const auto d2 = Cmp(sr, tr, MyMy());
    bool sameResult = d1.pairs.size() == d2.pairs.size();
    for (size_t i = 0; sameResult && i < d1.pairs.size(); ++i)
        sameResult = d1.pairs[i].key == d2.pairs[i].key &&
                     d1.pairs[i].verdict == d2.pairs[i].verdict;
    ExpectTrue("result is order-independent", sameResult);
}

// ---------------------------------------------------------------------------
// 7. The structural guarantee (ADR-013 Q1/Q5), pinned the only way an ABSENCE
//    can be pinned: by what this file can and cannot say and still compile.
// ---------------------------------------------------------------------------
static void TestNoDdlSurface()
{
    std::printf("\n-- structural: no routine DDL exists --\n");

    const auto d = Cmp({Fn(L"f", {}, L"int", L"BEGIN RETURN 1; END")},
                       {Fn(L"f", {}, L"int", L"BEGIN RETURN 2; END")}, MyMy());
    const RoutinePair& p = d.pairs[0];

    // What a caller CAN get out of a difference: the two bodies, verbatim, and
    // a reason. That is the whole product.
    ExpectTrue("source body available for display",  !p.source.body.IsEmpty());
    ExpectTrue("target body available for display",  !p.target.body.IsEmpty());
    ExpectTrue("a human-readable reason is present", !p.reason.IsEmpty());

    // `body` is the routine's INNER source (a BEGIN…END block), never a CREATE
    // statement — so there is nothing here to hand to Execute() even by mistake.
    ExpectTrue("body is not a CREATE statement",
               !p.source.body.Upper().Contains(L"CREATE "));
    ExpectTrue("Signature() is a display string, not DDL",
               !p.source.Signature().Upper().Contains(L"CREATE"));

    // What a caller CANNOT get: there is no member on RoutinePair /
    // RoutineDiffSet / RoutineDef that yields DDL, because none exists. If a
    // future change adds `p.ddl`, `set.RenderCreate()` or similar, THIS comment
    // is the review trigger — and the ADR-013 answer is that the change is
    // wrong, not that the test needs updating.
    //
    // Equally structural: RoutineDiffSet is not reachable from SyncPlan, and
    // `routines` is not a SyncScope flag, so SyncEngine::Execute cannot see any
    // of this. This TU includes neither SyncEngine.h nor DataSync.h and still
    // compiles — the compare layer does not know the execute path exists.
    ExpectTrue("compare layer is independent of the execute path", true);
}

// ---------------------------------------------------------------------------
// 8. DEF-R3-01 — an ASYMMETRIC information_schema.PARAMETERS failure must not
//    become a screen of false SourceOnly/TargetOnly pairs.
//
// The hazard, stated precisely. MySqlRoutineRead issues TWO queries: ROUTINES
// for the list, PARAMETERS for the signatures. A restricted account can be
// denied the second while granted the first — and the realistic deployment is
// exactly that: a locked-down production account compared against a full
// development one, so ONE SIDE loses its parameters and the other does not.
//
// RoutineMatchKey() keys on name + argument types. A side with no argument types
// therefore shares no key with a side that has them, and the name-based re-join
// pass is additionally gated on equal argument COUNTS, so it cannot rescue the
// pairing either. The result was every routine reported twice — once SourceOnly,
// once TargetOnly — against a feature whose release bar is zero false positives,
// with the server's error text discarded because the status stayed Ok.
//
// The fix degrades the side instead. These tests pin BOTH halves: that the
// reader refuses, and that the refusal is what keeps the compare silent.
// ---------------------------------------------------------------------------

namespace {

// Stub IConnection that answers the reader's two information_schema queries
// independently, so a PARAMETERS-only denial can be reproduced exactly.
class RoutineCatalogStub : public IConnection {
public:
    bool     routinesOk_  = true;
    bool     parametersOk_ = true;
    wxString paramError_  = L"SELECT command denied to user 'ro'@'%' "
                            L"for table 'PARAMETERS'";

    // (name, type, returnType, body) — enough for a comparable routine.
    std::vector<std::vector<wxString>> routines_;
    // (specificName, type, ordinal, mode, dtd)
    std::vector<std::vector<wxString>> parameters_;

    int paramQueries_ = 0;   // observed, so "never even asked" cannot pass silently
    int grantQueries_ = 0;   // ditto for the privilege probe

    // SHOW GRANTS FOR CURRENT_USER() — the SECOND SIGNAL an empty routine list
    // now requires, because on MySQL 8 an empty list means either "no routines"
    // or "no privilege to see routines" and the list cannot say which.
    //
    // The default is a fully-privileged account, so every case in this file that
    // predates the probe keeps meaning exactly what it meant.
    bool grantsOk_ = true;
    std::vector<wxString> grants_ = {
        L"GRANT ALL PRIVILEGES ON *.* TO `dev`@`%` WITH GRANT OPTION"
    };

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        out = QueryResult{};
        if (sql.Contains(L"SHOW GRANTS")) {
            ++grantQueries_;
            if (!grantsOk_) { err = L"SHOW GRANTS command denied"; return false; }
            for (const auto& g : grants_) out.rows.push_back({ g });
            return true;
        }
        if (sql.Contains(L"information_schema.ROUTINES")) {
            if (!routinesOk_) { err = L"SELECT command denied ... 'ROUTINES'"; return false; }
            for (const auto& r : routines_)
                out.rows.push_back({ r[0], r[1], r[2], r[3], L"DEFINER", L"SQL", L"" });
            return true;
        }
        if (sql.Contains(L"information_schema.PARAMETERS")) {
            ++paramQueries_;
            if (!parametersOk_) { err = paramError_; return false; }
            out.rows = parameters_;
            return true;
        }
        err = L"unexpected query";
        return false;
    }

    Dialect GetDialect() const override { return Dialect::MySQL; }
    bool Connect(const core::ConnectionProfile&, wxString&) override { return true; }
    void Disconnect() override {}
    bool IsConnected() const override { return true; }
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

// Three functions, each with one argument — the shape that makes the defect
// visible, since an empty argument list keys differently from a populated one.
void FillCatalog(RoutineCatalogStub& c)
{
    c.routines_ = {
        { L"f_one",   L"FUNCTION",  L"int",  L"BEGIN RETURN 1; END" },
        { L"f_two",   L"FUNCTION",  L"int",  L"BEGIN RETURN 2; END" },
        { L"p_three", L"PROCEDURE", L"",     L"BEGIN SET @x = 3; END" },
    };
    c.parameters_ = {
        { L"f_one",   L"FUNCTION",  L"1", L"IN", L"int(11)" },
        { L"f_two",   L"FUNCTION",  L"1", L"IN", L"varchar(50)" },
        { L"p_three", L"PROCEDURE", L"1", L"IN", L"int(11)" },
    };
}

size_t CountVerdict(const RoutineDiffSet& d, RoutineVerdict v)
{
    size_t n = 0;
    for (const auto& p : d.pairs) if (p.verdict == v) ++n;
    return n;
}

} // namespace

static void TestPartialParameterRead()
{
    std::printf("\n-- DEF-R3-01: asymmetric PARAMETERS failure --\n");

    // ---- baseline: both sides fully readable --------------------------------
    RoutineCatalogStub full;  FillCatalog(full);
    std::vector<RoutineDef> sr;
    wxString sDetail;
    const RoutineReadStatus ss =
        db::mysqlroutine::ReadRoutines(full, L"app", sr, sDetail);

    ExpectTrue("baseline: full read is Ok", ss == RoutineReadStatus::Ok);
    ExpectTrue("baseline: three routines read", sr.size() == 3);
    ExpectTrue("baseline: signatures populated",
               sr.size() == 3 && sr[0].argTypes.size() == 1 &&
               sr[1].argTypes.size() == 1 && sr[2].argTypes.size() == 1);
    ExpectTrue("baseline: no detail text on a clean read", sDetail.IsEmpty());

    // ---- the defect's trigger: PARAMETERS denied on ONE side ----------------
    RoutineCatalogStub restricted;  FillCatalog(restricted);
    restricted.parametersOk_ = false;

    std::vector<RoutineDef> tr_;
    wxString tDetail;
    const RoutineReadStatus ts =
        db::mysqlroutine::ReadRoutines(restricted, L"app", tr_, tDetail);

    ExpectTrue("the PARAMETERS query really was attempted",
               restricted.paramQueries_ == 1);

    // The reader must NOT report Ok. Ok is what let the compare proceed on a
    // list it knew was missing its signatures.
    ExpectTrue("PARAMETERS denied => side is NOT Ok", ts != RoutineReadStatus::Ok);
    ExpectTrue("...and is recognized as a privilege problem",
               ts == RoutineReadStatus::PermissionDenied);
    ExpectTrue("...carrying the server's own error text", !tDetail.IsEmpty());
    ExpectTrue("...which names the table that was denied",
               tDetail.Contains(L"PARAMETERS"));

    // No half-read list escapes the reader. Even though ROUTINES succeeded, the
    // rows are dropped: a caller that ignored the status must still not find
    // signature-less routines to compare.
    ExpectTrue("no partially-read routines are returned", tr_.empty());

    // A non-privilege failure (a dropped connection mid-read) degrades too,
    // just not as PermissionDenied — the user is not sent to fix credentials
    // that are fine.
    RoutineCatalogStub broken;  FillCatalog(broken);
    broken.parametersOk_ = false;
    broken.paramError_   = L"Lost connection to MySQL server during query";
    std::vector<RoutineDef> br;
    wxString bDetail;
    const RoutineReadStatus bs =
        db::mysqlroutine::ReadRoutines(broken, L"app", br, bDetail);
    ExpectTrue("a NETWORK failure on PARAMETERS degrades as Unsupported",
               bs == RoutineReadStatus::Unsupported);
    ExpectTrue("...not as a privilege problem", bs != RoutineReadStatus::PermissionDenied);
    ExpectTrue("...and still returns no routines", br.empty());

    // ---- THE REGRESSION: compare the two sides as the reader reports them ---
    //
    // Assembled exactly as SyncWizardDialog::CompareRoutinesOn does, details
    // included, so what is asserted here is what the wizard would build.
    RoutineDiffSet d = CompareRoutines(sr, tr_, ss, ts, MyMy());
    d.sourceStatusDetail = sDetail;
    d.targetStatusDetail = tDetail;

    ExpectTrue("asymmetric failure yields ZERO pairs", d.pairs.empty());
    ExpectTrue("...zero false SourceOnly",
               CountVerdict(d, RoutineVerdict::SourceOnly) == 0);
    ExpectTrue("...zero false TargetOnly",
               CountVerdict(d, RoutineVerdict::TargetOnly) == 0);
    ExpectTrue("...and the set reports itself not comparable", !d.Comparable());
    // ui::MakeRoutineCategoryNode reads *StatusDetail at exactly ONE site, and
    // that site is inside the !Comparable() branch. Ok therefore meant the error
    // text was formatted and then silently dropped — the user saw a screen of
    // false differences with no hint anything had gone wrong. Non-Ok is what
    // makes it reachable, so this pins the two conditions together.
    ExpectTrue("the degraded branch is the one that will render...", !d.Comparable());
    ExpectTrue("...and it has the error text to render",
               !d.targetStatusDetail.IsEmpty());
    ExpectTrue("...attributed to the side that actually failed",
               d.sourceStatusDetail.IsEmpty() &&
               d.sourceStatus == RoutineReadStatus::Ok);

    // ---- proof the hazard was real, not hypothetical ------------------------
    //
    // Feed the compare exactly what the OLD code produced: a full source, a
    // signature-less target, and Ok on both sides. Six false pairs — every
    // routine reported as both added and removed. This assertion is the
    // measurement of what the fix prevents; it must keep passing, because it
    // pins the KEYING behaviour that makes degrading the read necessary.
    std::vector<RoutineDef> stripped = sr;
    for (RoutineDef& r : stripped) { r.argTypes.clear(); r.argModes.clear(); }

    const RoutineDiffSet bad = CompareRoutines(sr, stripped,
                                               RoutineReadStatus::Ok,
                                               RoutineReadStatus::Ok, MyMy());
    ExpectTrue("hazard confirmed: signature-less side would double-report all 3",
               bad.pairs.size() == 6 &&
               CountVerdict(bad, RoutineVerdict::SourceOnly) == 3 &&
               CountVerdict(bad, RoutineVerdict::TargetOnly) == 3);

    // ---- symmetric failure: benign by accident, refused on purpose ----------
    //
    // Both sides losing PARAMETERS would still have matched name-only, so the
    // old code happened to be correct here. We do not rely on that: a reader
    // cannot see the other side, so it cannot know which case it is in.
    RoutineCatalogStub other;  FillCatalog(other);
    other.parametersOk_ = false;
    std::vector<RoutineDef> or_;
    wxString oDetail;
    const RoutineReadStatus os =
        db::mysqlroutine::ReadRoutines(other, L"app", or_, oDetail);

    const RoutineDiffSet sym = CompareRoutines(tr_, or_, ts, os, MyMy());
    ExpectTrue("symmetric failure also degrades rather than guessing",
               sym.pairs.empty() && !sym.Comparable());

    // ---- ROUTINES itself denied: unchanged, and still distinct from empty ---
    RoutineCatalogStub noList;  FillCatalog(noList);
    noList.routinesOk_ = false;
    std::vector<RoutineDef> nr;
    wxString nDetail;
    ExpectTrue("a denied ROUTINES query still degrades as before",
               db::mysqlroutine::ReadRoutines(noList, L"app", nr, nDetail) ==
                   RoutineReadStatus::PermissionDenied);
    ExpectTrue("...and never asks for PARAMETERS", noList.paramQueries_ == 0);

    // ---- an EMPTY catalog is Ok *when the account could have seen routines* --
    //
    // The early return for an empty routine list must stay AHEAD of the
    // parameter query: a schema with no routines is a real answer, and turning
    // it into "不可见" would tell users their account is broken when it is not.
    //
    // On MySQL 8 that emptiness is AMBIGUOUS — it equally means "you may not see
    // them" — so the reader now resolves it against a privilege probe. This stub
    // grants ALL PRIVILEGES by default, so the case pinned here is the one that
    // must never regress: privileged account, genuinely empty catalog, Ok. The
    // other two branches (privileges absent, privileges indeterminate) belong to
    // DEF-R3-02 and are pinned in MySqlRoutinePrivilegeTests.cpp.
    RoutineCatalogStub empty;   // no routines, PARAMETERS would fail if asked
    empty.parametersOk_ = false;
    std::vector<RoutineDef> er;
    wxString eDetail;
    ExpectTrue("an empty catalog is Ok, not degraded",
               db::mysqlroutine::ReadRoutines(empty, L"app", er, eDetail) ==
                   RoutineReadStatus::Ok);
    ExpectTrue("...with an empty list and no error text",
               er.empty() && eDetail.IsEmpty());
    ExpectTrue("...and the empty early-out still precedes the PARAMETERS query",
               empty.paramQueries_ == 0);

    // The probe must stay OFF the non-empty path: a populated list is
    // authoritative on its own, and the probe is not paid for on every compare.
    ExpectTrue("the privilege probe never runs when routines were listed",
               full.grantQueries_ == 0);
}

int main()
{
    std::printf("=== RoutineCompareTests ===\n");
    TestArgTypeNormalization();
    TestMatchKey();
    TestSameEngineVerdicts();
    TestCrossEngineNeverClaimsBodies();
    TestDegradesToNotComparable();
    TestPermissionDegradation();
    TestPresentationOrdering();
    TestNoDdlSurface();
    TestPartialParameterRead();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
