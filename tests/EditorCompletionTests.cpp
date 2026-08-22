// EditorCompletionTests.cpp — headless unit tests for ui::editorcomp
// (src/ui/EditorCompletion.{h,cpp}), the pure decision layer behind the SQL
// editor's autocompletion popup.
//
// WHY THIS FILE EXISTS. All of this used to be inline in EditorPage.cpp, reading
// characters straight out of a wxStyledTextCtrl and pushing results into
// AutoCompShow — reachable only by typing into a running app attached to a live
// server. And the thing that was SUPPOSED to make it verifiable, the
// SWIFTSQL_AUTOEDIT ScriptedComplete hook, was running a SECOND, independently
// written copy of the qualifier parse. A verification hook that re-implements
// the logic it is verifying cannot detect a bug in either; it can only detect
// disagreement, and nothing was comparing them. Both call sites now share this
// module, and this file is what actually checks it.
//
// This is a CHARACTERIZATION suite: it pins current behaviour so the split that
// produced it (EditorPage.cpp, 991 lines → four TUs) is reviewable as
// behaviour-preserving. Equivalence with the pre-split implementation was
// additionally established by a temporary differential-fuzz harness running the
// transcribed git-HEAD original against this module over 140,000 randomized
// inputs, zero divergences.
//
// ONE PIN IS ON A KNOWN DEFECT, asserted as current behaviour rather than fixed
// here — see TestAliasResolution. Marked DEFECT.
//
// Compiles EditorCompletion.cpp directly and links only swiftsql::core. That is
// a FITNESS FUNCTION: this module must depend on nothing but wxString, so if
// anyone pulls a db::Dialect or a wx widget into it, this target stops linking.
#include "ui/EditorCompletion.h"

#include <wx/wxcrt.h>   // wxIsalnum — the pre-fix byte scan is transcribed here

#include <algorithm>
#include <cstdio>
#include <map>

using namespace ui::editorcomp;

static int g_checks = 0, g_fails = 0;

// Renders a wxString so a failure is READABLE on any console. The fixtures in
// this file are partly CJK and the Windows console is not UTF-8, so mb_str()
// turns the interesting half of every diff into question marks — which is how a
// test that fails for one reason gets debugged as though it failed for another.
// ASCII passes through; anything else is escaped to its codepoint.
static wxString Show(const wxString& s)
{
    wxString out;
    for (wxUniChar c : s) {
        if (c >= 0x20 && c < 0x7F) out += c;
        else out += wxString::Format(L"<U+%04X>", (int)c.GetValue());
    }
    return out;
}

// Prints a line on SUCCESS as well as failure. The success line is not noise:
// the assertion-census lint (tests/lint/assertion_census.py) reads the stream of
// per-assertion labels, because an assertion that stops executing is invisible
// to a count but shows up as a vanished label. While this helper printed only
// on failure the suite was census-blind — it reported "57 checks" that the lint
// could not see one of, degrading it to a total-only check on exactly the two
// suites whose author was most alert to this problem.
static void ExpectEq(const char* what, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got == want) { std::printf("  ok   %s\n", what); return; }
    ++g_fails;
    std::printf("FAIL %s\n  want |%s|\n  got  |%s|\n", what,
                (const char*)Show(want).mb_str(), (const char*)Show(got).mb_str());
}

static wxString Join(const std::vector<wxString>& v)
{
    wxString s;
    for (const wxString& x : v) { if (!s.IsEmpty()) s += L','; s += x; }
    return s;
}

// ---------------------------------------------------------------------------
// A stub schema. `shop` and `MAIN` are databases; `users`/`orders` are tables in
// the active database. Deliberately includes a database and a table that differ
// only in case, and a name (`orders`) that is BOTH a table and a keyword-looking
// word, so precedence is actually exercised.
// ---------------------------------------------------------------------------
struct Stub {
    std::map<wxString, std::vector<wxString>> cols {
        { L"users",  { L"id", L"name", L"email" } },
        { L"orders", { L"id", L"user_id", L"total" } },
    };
    std::map<wxString, std::vector<wxString>> dbTables {
        { L"shop", { L"users", L"orders" } },
        { L"MAIN", { L"cfg" } },
    };
    std::map<wxString, std::vector<wxString>> qcols {
        { L"shop.users", { L"id", L"name" } },
        { L"MAIN.cfg",   { L"k", L"v" } },
    };
    std::vector<wxString> dbs { L"shop", L"MAIN" };

    std::vector<wxString> Columns(const wxString& t) const {
        auto i = cols.find(t); return i == cols.end() ? std::vector<wxString>{} : i->second;
    }
    bool IsDb(const wxString& d) const {
        for (const wxString& x : dbs) if (x.IsSameAs(d, false)) return true;
        return false;
    }
};

// Templated on the stub so the CJK fixture further down can reuse it without
// perturbing the `Stub` the 38 original checks are pinned against.
template <class S>
static SchemaLookup MakeLookup(const S& s, const wxString& buffer)
{
    SchemaLookup look;
    look.isDatabase = [&s](const wxString& d) { return s.IsDb(d); };
    look.tablesOf   = [&s](const wxString& d) {
        auto i = s.dbTables.find(d);
        return i == s.dbTables.end() ? std::vector<wxString>{} : i->second;
    };
    look.columnsOf  = [&s](const wxString& d, const wxString& t) {
        auto i = s.qcols.find(d + L"." + t);
        return i == s.qcols.end() ? std::vector<wxString>{} : i->second;
    };
    look.columns      = [&s](const wxString& t) { return s.Columns(t); };
    look.resolveTable = [&s, &buffer](const wxString& id) {
        return ResolveTableForIdent(buffer, id,
                                    [&s](const wxString& t) { return !s.Columns(t).empty(); });
    };
    return look;
}

// ---------------------------------------------------------------------------
static void TestQualifierChainParse()
{
    ExpectEq("single identifier", Join(ParseQualifierChain(L"users").parts), L"users");

    // Order is SOURCE order even though the scan runs right-to-left. Getting this
    // backwards would swap db and table in the columnsOf lookup below.
    ExpectEq("two-part chain", Join(ParseQualifierChain(L"shop.users").parts), L"shop,users");
    ExpectEq("three-part chain",
             Join(ParseQualifierChain(L"a.b.c").parts), L"a,b,c");

    // Only the trailing chain is taken — the statement in front of it is not.
    ExpectEq("chain stops at a non-identifier",
             Join(ParseQualifierChain(L"SELECT * FROM shop.users").parts), L"shop,users");
    ExpectEq("chain stops at a paren",
             Join(ParseQualifierChain(L"count(users").parts), L"users");

    // A double dot yields no identifier at that position → truncated, and the
    // chain is whatever preceded it (here nothing).
    const QualifierChain dd = ParseQualifierChain(L"a.");
    ExpectEq("double dot truncates", Join(dd.parts), L"");
    ExpectEq("double dot sets the flag", dd.truncated ? L"yes" : L"no", L"yes");

    const QualifierChain clean = ParseQualifierChain(L"users");
    ExpectEq("a clean chain is not truncated", clean.truncated ? L"yes" : L"no", L"no");

    // Empty input: nothing to scan, and that IS the truncated condition.
    const QualifierChain e = ParseQualifierChain(L"");
    ExpectEq("empty input yields nothing", Join(e.parts), L"");
    ExpectEq("empty input is truncated", e.truncated ? L"yes" : L"no", L"yes");

    ExpectEq("underscores are identifier characters",
             Join(ParseQualifierChain(L"_my_tbl9").parts), L"_my_tbl9");
}

static void TestQualifierResolution()
{
    Stub s;
    const wxString buf = L"SELECT * FROM users";

    // "users." → columns of the table, via the active database.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"users"), MakeLookup(s, buf));
        ExpectEq("table. → its columns", Join(r.candidates), L"id,name,email");
        ExpectEq("table. → column category",
                 wxString::Format(L"%d", r.category), wxString::Format(L"%d", (int)kColumn));
    }

    // "shop." → the TABLES of that database, with the table icon. A database and
    // a table are told apart here and nowhere else; getting it wrong offers
    // column names where table names belong.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"shop"), MakeLookup(s, buf));
        ExpectEq("db. → its tables", Join(r.candidates), L"users,orders");
        ExpectEq("db. → table category",
                 wxString::Format(L"%d", r.category), wxString::Format(L"%d", (int)kTable));
    }

    // Database matching is CASE-INSENSITIVE: `main.` finds the db named `MAIN`.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"MAIN"), MakeLookup(s, buf));
        ExpectEq("db name matched case-insensitively", Join(r.candidates), L"cfg");
    }

    // "shop.users." → the db-qualified column source, NOT the active-database one.
    // These differ on purpose (shop.users has 2 columns, users has 3) so a test
    // that hit the wrong source would notice.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"shop.users"), MakeLookup(s, buf));
        ExpectEq("db.table. → db-qualified columns", Join(r.candidates), L"id,name");
        ExpectEq("db.table. → column category",
                 wxString::Format(L"%d", r.category), wxString::Format(L"%d", (int)kColumn));
    }

    // An unknown name is treated as a table, finds nothing, and offers nothing —
    // it must NOT fall back to some other table's columns.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"nosuch"), MakeLookup(s, buf));
        ExpectEq("unknown qualifier offers nothing", Join(r.candidates), L"");
    }

    // A truncated (empty) chain resolves to nothing rather than to everything.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"a."), MakeLookup(s, buf));
        ExpectEq("truncated chain offers nothing", Join(r.candidates), L"");
    }

    // With NO sources bound at all (a fresh editor, no connection) nothing is
    // offered and nothing crashes — every callable in SchemaLookup is optional.
    {
        const auto r = ResolveQualifier(ParseQualifierChain(L"users"), SchemaLookup{});
        ExpectEq("no schema bound → nothing offered", Join(r.candidates), L"");
    }
}

static void TestAliasResolution()
{
    Stub s;
    auto has = [&s](const wxString& t) { return !s.Columns(t).empty(); };

    // A real table resolves to itself without consulting the buffer.
    ExpectEq("a real table resolves to itself",
             ResolveTableForIdent(L"", L"users", has), L"users");

    // Implicit alias: "FROM users u" binds u → users.
    ExpectEq("implicit alias",
             ResolveTableForIdent(L"SELECT * FROM users u WHERE 1", L"u", has), L"users");

    // Explicit AS alias, either case.
    ExpectEq("explicit AS alias",
             ResolveTableForIdent(L"SELECT * FROM orders AS o", L"o", has), L"orders");
    ExpectEq("lower-case as alias",
             ResolveTableForIdent(L"SELECT * FROM orders as o", L"o", has), L"orders");

    // Two tables in one statement must not cross-bind.
    ExpectEq("multi-table join binds the right one",
             ResolveTableForIdent(L"SELECT * FROM users u JOIN orders o ON u.id=o.user_id",
                                  L"o", has),
             L"orders");

    // An unbound identifier degrades to itself — never to some other table. A
    // wrong answer here would silently offer another table's column names.
    ExpectEq("unknown alias degrades to itself",
             ResolveTableForIdent(L"SELECT * FROM users u", L"zz", has), L"zz");

    // Alias matching is case-insensitive (matching the SQL engines' own behaviour
    // for unquoted identifiers on the platforms this app targets).
    ExpectEq("alias matched case-insensitively",
             ResolveTableForIdent(L"SELECT * FROM users U", L"u", has), L"users");

    // Without a schema callable there is nothing to check a binding against, so
    // the identifier passes through untouched.
    ExpectEq("no schema callable → identity",
             ResolveTableForIdent(L"FROM users u", L"u", nullptr), L"u");

    // DEFECT (pinned, not fixed): the scan is a flat word walk with no notion of
    // SQL structure, so ANY adjacent "<known table> <word>" pair reads as a
    // binding — including one spanning a comma, and including the columns of an
    // INSERT list. Here `name` is bound to `users` purely because the two words
    // are adjacent, and "users." would then complete with users' columns even
    // though nothing declared that alias.
    ExpectEq("adjacent words are read as a binding regardless of syntax",
             ResolveTableForIdent(L"INSERT INTO users name", L"name", has), L"users");
    // Fixing it means actually parsing FROM/JOIN clauses, which is a behaviour
    // change (it would stop resolving some aliases that resolve today) and
    // belongs in its own commit. It is benign in practice — the fallback is
    // "offer this table's columns", never a write path.
}

static void TestWordCandidateMerge()
{
    // Precedence: keywords and functions are inserted WEAKLY (first wins);
    // databases and tables OVERWRITE. So a live schema object named like a
    // keyword shows as the schema object, which is what the user means.
    const std::vector<wxString> kws  { L"SELECT", L"COUNT" };
    const std::vector<wxString> fns  { L"COUNT", L"NOW" };   // COUNT already a keyword
    const std::vector<wxString> dbs  { L"shop" };
    const std::vector<wxString> tbls { L"users", L"COUNT" }; // COUNT also a table

    const auto merged = MergeWordCandidates(kws, fns, dbs, tbls);

    wxString s;
    for (const auto& p : merged) {
        if (!s.IsEmpty()) s += L' ';
        s += p.first + wxString::Format(L":%d", p.second);
    }
    // Sorted by word (Scintilla requires it); wxString ordering puts upper-case
    // before lower-case. COUNT is claimed by the TABLE list despite appearing in
    // both dialect lists first.
    ExpectEq("merge: order and precedence", s,
             wxString::Format(L"COUNT:%d NOW:%d SELECT:%d shop:%d users:%d",
                              (int)kTable, (int)kFunction, (int)kKeyword,
                              (int)kDatabase, (int)kTable));

    // Duplicates collapse; empty sources are legal.
    const auto only = MergeWordCandidates({ L"A", L"A", L"A" }, {}, {}, {});
    ExpectEq("duplicates collapse", wxString::Format(L"%d", (int)only.size()), L"1");

    const auto none = MergeWordCandidates({}, {}, {}, {});
    ExpectEq("all-empty merge is empty", wxString::Format(L"%d", (int)none.size()), L"0");
}

static void TestListRendering()
{
    // The Scintilla AutoCompShow payload: "word?type" joined by '\n'. The '?' is
    // the registered type separator and the number selects the icon — this is a
    // wire format, not a display string.
    ExpectEq("typed list rendering",
             BuildAutoCompList({ { L"id", kColumn }, { L"users", kTable } }),
             wxString::Format(L"id?%d\nusers?%d", (int)kColumn, (int)kTable));

    ExpectEq("uniform-category list rendering",
             BuildAutoCompList({ L"a", L"b" }, kColumn),
             wxString::Format(L"a?%d\nb?%d", (int)kColumn, (int)kColumn));

    ExpectEq("empty list renders empty",
             BuildAutoCompList(std::vector<wxString>{}, kColumn), L"");

    // The scripted hook's machine-read form is space-joined, NOT '?'-typed.
    ExpectEq("scripted hook join", JoinWords({ L"a", L"b", L"c" }), L"a b c");
    ExpectEq("scripted hook join, empty", JoinWords({}), L"");
}

// ===========================================================================
// The two implementations that USED to exist, transcribed, and the proof that
// unifying them was a fix rather than a coin toss.
//
// Until this suite's companion commit there were two qualifier-chain scans: the
// live popup's, over Scintilla BYTE positions, and the SWIFTSQL_AUTOEDIT hook's,
// over wxString CHARACTERS. The hook is the mechanism that VERIFIES completion,
// so the verifier and its subject did not share offset semantics — which means
// the verifier was checking nothing. Both are transcribed below and both are
// kept, permanently, because they are what makes the CJK pins EVIDENCE: an
// assertion that the fixed path handles `用户表.` proves nothing unless something
// in the same file demonstrates that the code it replaced did not.
// ===========================================================================

// PRE-FIX PRODUCTION, byte for byte. wxStyledTextCtrl::GetCharAt returns
// `(unsigned char)SendMsg(SCI_GETCHARAT, ...)` — a raw UTF-8 BYTE, 0..255 — and
// GetTextRange takes BYTE positions and UTF-8-decodes the range (stc2wx, which
// yields an EMPTY string for an invalid fragment). Both are modelled exactly.
static QualifierCompletion LegacyByteScan(const wxString& textBeforeDot,
                                          const SchemaLookup& look)
{
    // The document as Scintilla stores it: the text, the triggering '.', and the
    // caret sitting one byte past it.
    //
    // `docStr` MUST be a named object. wxString::utf8_str() may return a
    // NON-OWNED wxScopedCharBuffer pointing into the string's own storage, so
    // writing `(textBeforeDot + L".").utf8_str()` dangles the moment the
    // temporary dies — which it does at the end of that full-expression. The
    // first draft of this harness did exactly that and reported 1985 divergences
    // out of 140000 in a build where BOTH SIDES CALLED THE SAME FUNCTION. That
    // is the only reason it was caught: an impossible number is visible, whereas
    // had it dangled the other way it would have printed a clean 0 and been
    // believed.
    const wxString docStr = textBeforeDot + L".";
    const wxScopedCharBuffer u8 = docStr.utf8_str();
    const int len = static_cast<int>(u8.length());
    const unsigned char* doc = reinterpret_cast<const unsigned char*>(u8.data());

    auto GetCharAt = [&](int p) -> int { return (p >= 0 && p < len) ? doc[p] : 0; };
    auto GetTextRange = [&](int a, int b) -> wxString {
        if (b <= a) return wxString();
        return wxString::FromUTF8(reinterpret_cast<const char*>(doc) + a,
                                  static_cast<size_t>(b - a));
    };

    const int pos = len;        // caret just after the '.'
    int i = pos - 2;            // BYTE before the '.'
    QualifierChain chain;
    for (;;) {
        int end = i + 1;
        while (i >= 0) {
            const wxChar c = static_cast<wxChar>(GetCharAt(i));
            if (wxIsalnum(c) || c == '_') --i; else break;
        }
        wxString tok = GetTextRange(i + 1, end);
        tok.Replace(L"`", L""); tok.Replace(L"\"", L"");
        if (tok.IsEmpty()) return {};       // production bailed out ENTIRELY
        chain.parts.insert(chain.parts.begin(), tok);
        if (i >= 0 && static_cast<wxChar>(GetCharAt(i)) == '.') { --i; continue; }
        break;
    }
    QualifierCompletion out = ResolveQualifier(chain, look);
    std::sort(out.candidates.begin(), out.candidates.end());
    return out;
}

// PRE-FIX HOOK, byte for byte: the character-based parse with NO truncated-chain
// gate, which is the second divergence — the one that has nothing to do with
// offsets and bites on pure ASCII.
static QualifierCompletion LegacyHookScan(const wxString& textBeforeDot,
                                          const SchemaLookup& look)
{
    QualifierCompletion out = ResolveQualifier(ParseQualifierChain(textBeforeDot), look);
    std::sort(out.candidates.begin(), out.candidates.end());
    return out;
}

// ---------------------------------------------------------------------------
// A CJK schema. Deliberately includes `a` AND `表a`, because the byte scan's
// most dangerous failure is not offering nothing — it is finding the ASCII TAIL
// of a mixed identifier and confidently completing a DIFFERENT table.
// ---------------------------------------------------------------------------
struct CjkStub {
    std::map<wxString, std::vector<wxString>> cols {
        { L"用户表", { L"编号", L"姓名" } },
        { L"a",      { L"aid" } },
        { L"表a",    { L"x1" } },
    };
    std::map<wxString, std::vector<wxString>> dbTables {
        { L"商城", { L"订单" } },
    };
    std::map<wxString, std::vector<wxString>> qcols {
        { L"商城.订单", { L"金额" } },
    };
    std::vector<wxString> dbs { L"商城" };

    std::vector<wxString> Columns(const wxString& t) const {
        auto i = cols.find(t); return i == cols.end() ? std::vector<wxString>{} : i->second;
    }
    bool IsDb(const wxString& d) const {
        for (const wxString& x : dbs) if (x.IsSameAs(d, false)) return true;
        return false;
    }
};

static void TestCjkQualifierCompletion()
{
    CjkStub s;
    const wxString buf = L"SELECT * FROM 用户表";
    const SchemaLookup look = MakeLookup(s, buf);

    // ---- 1. A CJK table name completes at all. --------------------------------
    // U+8868 encodes as E8 A1 A8. The byte scan asked wxIsalnum of 0xA8 — the
    // LAST byte, which is where a right-to-left scan starts — got false, produced
    // an empty token, and bailed. The user got no popup whatsoever.
    ExpectEq("CJK table completes to its columns",
             Join(CompleteQualified(L"用户表", look).candidates), L"姓名,编号");
    ExpectEq("...and the pre-fix BYTE scan offered nothing",
             Join(LegacyByteScan(L"用户表", look).candidates), L"");

    // ---- 2. The silent-wrong-answer case. -------------------------------------
    // `表a.` — the byte scan walks left over ASCII 'a', hits 0xA8, stops, and
    // hands back the token "a". There IS a table called `a`, so it offered `a`'s
    // columns for a query about `表a`: not a missing popup, a confidently wrong
    // one. This is the reason byte offsets could not simply be kept.
    ExpectEq("mixed CJK/ASCII identifier completes as itself",
             Join(CompleteQualified(L"表a", look).candidates), L"x1");
    ExpectEq("...where the pre-fix BYTE scan completed a DIFFERENT table",
             Join(LegacyByteScan(L"表a", look).candidates), L"aid");

    // ---- 3. CJK on both sides of the dot. -------------------------------------
    ExpectEq("CJK db.table. → db-qualified columns",
             Join(CompleteQualified(L"商城.订单", look).candidates), L"金额");
    ExpectEq("...pre-fix BYTE scan: nothing",
             Join(LegacyByteScan(L"商城.订单", look).candidates), L"");

    // ---- 4. A CJK database name resolves as a database, not as a table. -------
    ExpectEq("CJK db. → its tables",
             Join(CompleteQualified(L"商城", look).candidates), L"订单");
    ExpectEq("CJK db. → table category",
             wxString::Format(L"%d", CompleteQualified(L"商城", look).category),
             wxString::Format(L"%d", (int)kTable));

    // ---- 5. The chain scan is not fooled by CJK in the surrounding text. ------
    ExpectEq("CJK chain stops at a non-identifier",
             Join(CompleteQualified(L"SELECT * FROM 用户表", look).candidates),
             L"姓名,编号");

    // A CJK ALIAS binds like an ASCII one — ResolveTableForIdent splits on the
    // same wxIsalnum predicate, so this works for free once the chain scan does.
    auto has = [&s](const wxString& t) { return !s.Columns(t).empty(); };
    ExpectEq("CJK alias binds to its CJK table",
             ResolveTableForIdent(L"SELECT * FROM 用户表 甲", L"甲", has), L"用户表");
}

static void TestTruncatedChainPolicy()
{
    // The SECOND divergence, independent of offsets and reachable on pure ASCII:
    // on a malformed chain the popup bailed entirely while the hook resolved
    // whatever it had collected before the break. `a..orders.` offered orders'
    // columns through the verification hook and NOTHING through the popup the
    // user actually sees — so the hook was reporting a completion that did not
    // exist. Production's answer is the one that survives.
    Stub s;
    const wxString buf = L"SELECT * FROM orders";
    const SchemaLookup look = MakeLookup(s, buf);

    ExpectEq("malformed chain completes to nothing",
             Join(CompleteQualified(L"a..orders", look).candidates), L"");
    ExpectEq("...as the live popup always did",
             Join(LegacyByteScan(L"a..orders", look).candidates), L"");
    ExpectEq("...while the pre-fix HOOK claimed a completion",
             Join(LegacyHookScan(L"a..orders", look).candidates), L"id,total,user_id");

    // A leading dot is the same shape and was the same disagreement.
    ExpectEq("leading dot completes to nothing",
             Join(CompleteQualified(L".orders", look).candidates), L"");
    ExpectEq("...while the pre-fix HOOK claimed a completion",
             Join(LegacyHookScan(L".orders", look).candidates), L"id,total,user_id");

    // Sorting is now part of the one decision, not something two call sites each
    // had to remember. Source order for `orders` is id,user_id,total.
    ExpectEq("CompleteQualified sorts", Join(CompleteQualified(L"orders", look).candidates),
             L"id,total,user_id");
    ExpectEq("ResolveQualifier still does not",
             Join(ResolveQualifier(ParseQualifierChain(L"orders"), look).candidates),
             L"id,user_id,total");
}

// ---------------------------------------------------------------------------
// ASCII EQUIVALENCE, by differential fuzz against the pre-fix production scan.
//
// The CJK pins above prove the change BITES. This proves it bites nowhere else:
// over pure-ASCII input the byte scan and the unified character scan must agree
// exactly, because on ASCII a byte IS a character. If they ever disagree, the
// unification silently changed what a user's editor offers.
//
// Mutation-checked, per the standing rule that a zero must be earned: deleting
// the `if (chain.truncated) return {}` gate from CompleteQualified takes this
// from 0 divergences to thousands, so the harness can in fact see a difference.
// ---------------------------------------------------------------------------
static void TestAsciiDifferentialFuzz()
{
    Stub s;
    const wxString buf = L"SELECT * FROM users u JOIN orders o ON u.id=o.user_id";
    const SchemaLookup look = MakeLookup(s, buf);

    // Weighted so that real schema names, dots and identifier characters all
    // appear often — a uniform alphabet would produce almost no resolvable chains
    // and the comparison would be between two empty results.
    static const wchar_t* kTok[] = {
        L"users", L"orders", L"shop", L"MAIN", L"u", L"o", L"id", L"_x9",
        L".", L".", L".", L"..", L" ", L"(", L")", L"*", L"=", L"`", L"\"",
        L"a", L"B", L"7", L"_", L"SELECT", L"FROM",
    };
    const int kNumTok = (int)(sizeof(kTok) / sizeof(kTok[0]));

    unsigned int rng = 0x5EED1234u;      // fixed seed: reproducible
    auto Next = [&rng]() { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7FFF; };

    const int kRuns = 140000;
    int diverged = 0, resolvedNonEmpty = 0;
    for (int n = 0; n < kRuns; ++n) {
        wxString head;
        const int parts = 1 + (int)(Next() % 6);
        for (int p = 0; p < parts; ++p) head += kTok[Next() % kNumTok];

        const auto legacy = LegacyByteScan(head, look);
        const auto fixed  = CompleteQualified(head, look);
        if (!fixed.candidates.empty()) ++resolvedNonEmpty;
        if (Join(legacy.candidates) != Join(fixed.candidates) ||
            legacy.category != fixed.category) {
            if (++diverged <= 3)
                std::printf("  DIVERGE on |%s|\n    legacy |%s|\n    fixed  |%s|\n",
                            (const char*)Show(head).mb_str(),
                            (const char*)Show(Join(legacy.candidates)).mb_str(),
                            (const char*)Show(Join(fixed.candidates)).mb_str());
        }
    }
    std::printf("   [fuzz] %d ASCII inputs, %d resolved to a non-empty completion, "
                "%d divergences\n", kRuns, resolvedNonEmpty, diverged);

    // The coverage assertion matters as much as the divergence one: 0 divergences
    // over inputs that never resolved to anything would be a vacuous green.
    ExpectEq("fuzz: byte scan and unified scan agree on ASCII",
             wxString::Format(L"%d", diverged), L"0");
    ExpectEq("fuzz: the corpus actually reached the schema",
             resolvedNonEmpty > 1000 ? L"yes" : L"no", L"yes");
}

int main()
{
    TestQualifierChainParse();
    TestQualifierResolution();
    TestAliasResolution();
    TestWordCandidateMerge();
    TestListRendering();
    TestCjkQualifierCompletion();
    TestTruncatedChainPolicy();
    TestAsciiDifferentialFuzz();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
