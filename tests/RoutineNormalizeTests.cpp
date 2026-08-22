// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// RoutineNormalizeTests.cpp — the adversarial corpus for db::sync::
// NormalizeRoutineBody (src/db/RoutineNormalize.{h,cpp}, ADR-013 Q3).
//
// ADR-013 called the normalizer the highest-risk piece of the routine diff and
// mandated that this file exist BEFORE the catalog readers do. The reason is
// that every other component fails LOUDLY — a reader that cannot see pg_proc
// reports PermissionDenied, a cross-engine body pair reports NotComparable — but
// a subtly wrong normalizer fails SILENTLY and in the worst direction: it tells
// the user two identical routines differ. The release bar for this feature is
// zero false positives, so the normalizer has to be proven on hostile input
// rather than smoke-tested on `BEGIN ... END`.
//
// The corpus below is organized by the specific way a naive "strip comments and
// squeeze whitespace" implementation gets each case wrong:
//
//   * `--` and `/* */` INSIDE `$$ ... $$`   -> naive strips them; they are text.
//   * nested `$outer$ ... $inner$ ... $outer$` -> naive closes on the wrong `$`.
//   * `$1` parameter refs                   -> naive opens a dollar quote.
//   * MySQL `x--1`                          -> naive eats the rest of the line.
//   * apostrophes / doubled quotes in strings -> naive de-syncs its quote state.
//   * mixed CRLF/LF                         -> naive reports a phantom diff.
//   * unterminated quotes                   -> naive returns a WRONG answer;
//                                              the contract is Unresolved.
//
// Pure logic: links swiftsql::db for wxString + the enum, no connection, no GUI.
#include "db/RoutineNormalize.h"
#include "db/DbDriver.h"

#include <cstdio>
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

static NormalizeResult Pg(const wxString& s)  { return NormalizeRoutineBody(s, PgNormalizeRules()); }
static NormalizeResult My(const wxString& s)  { return NormalizeRoutineBody(s, MySqlNormalizeRules()); }

// Do two bodies normalize to the same text (and both resolve)? This is exactly
// the predicate the compare layer uses to decide Identical vs BodyDiffers, so
// asserting on it is asserting on user-visible behaviour rather than on an
// internal string shape.
static bool Same(const NormalizeResult& a, const NormalizeResult& b)
{
    return a.Ok() && b.Ok() && a.text == b.text;
}

// ---------------------------------------------------------------------------
// 1. The baseline the whole feature rests on
// ---------------------------------------------------------------------------
static void TestWhitespaceAndCommentsInCode()
{
    std::printf("\n-- code-state stripping --\n");

    ExpectText("collapse runs of whitespace",
               Pg(L"SELECT   a,\n\t b").text, L"SELECT a, b");

    ExpectText("leading/trailing whitespace dropped",
               Pg(L"\n\n  BEGIN END  \n").text, L"BEGIN END");

    // Rule 4: a comment becomes a SPACE, never nothing. `a/*c*/b` collapsing to
    // `ab` would fuse two identifiers into a third that appears in neither body.
    ExpectText("block comment -> single space, tokens not fused",
               Pg(L"a/*c*/b").text, L"a b");

    ExpectText("line comment stripped to EOL",
               Pg(L"SELECT 1; -- explain\nSELECT 2;").text, L"SELECT 1; SELECT 2;");

    // EOF while in a line comment is well-formed: the last line needs no newline.
    ExpectTrue("EOF inside line comment is OK, not Unresolved",
               Pg(L"SELECT 1; -- trailing").Ok());
    ExpectText("EOF inside line comment yields the code before it",
               Pg(L"SELECT 1; -- trailing").text, L"SELECT 1;");

    // The headline equivalence: same routine, reformatted and commented.
    ExpectTrue("reformat + comment == identical",
               Same(Pg(L"BEGIN\n  -- do the thing\n  RETURN 1;\nEND"),
                    Pg(L"BEGIN RETURN 1; END")));
}

// ---------------------------------------------------------------------------
// 2. NO CASE FOLDING — deliberate, and it must stay that way
// ---------------------------------------------------------------------------
static void TestNoCaseFolding()
{
    std::printf("\n-- case is preserved --\n");

    // If anyone ever "improves" the normalizer by lower-casing, THIS is the test
    // that catches it — and the reason is the second assertion, not the first.
    ExpectText("keyword case survives", Pg(L"select 1").text, L"select 1");
    ExpectTrue("keyword case difference is reported as a difference",
               !Same(Pg(L"SELECT 1"), Pg(L"select 1")));

    // The real damage case-folding would do: two DIFFERENT routines becoming
    // indistinguishable because their string literals were folded together.
    ExpectTrue("string literals differing only in case are NOT equal",
               !Same(Pg(L"RETURN 'Alice';"), Pg(L"RETURN 'alice';")));
}

// ---------------------------------------------------------------------------
// 3. PostgreSQL dollar quoting — the case a dialect-blind stripper destroys
// ---------------------------------------------------------------------------
static void TestDollarQuoting()
{
    std::printf("\n-- PG dollar quoting --\n");

    // `--` inside $$ is TEXT. A naive stripper deletes " not a comment $$" and
    // then, having eaten the closing $$, produces garbage.
    ExpectText("-- inside $$ is literal text",
               Pg(L"$$ a -- not a comment\n b $$").text,
               L"$$ a -- not a comment\n b $$");

    ExpectText("/* */ inside $$ is literal text",
               Pg(L"$$ x /* still text */ y $$").text,
               L"$$ x /* still text */ y $$");

    // Whitespace inside a dollar-quoted run is content, so it is NOT collapsed.
    ExpectTrue("whitespace inside $$ is not collapsed",
               !Same(Pg(L"$$ a    b $$"), Pg(L"$$ a b $$")));

    // ...while whitespace OUTSIDE the same run still is.
    ExpectTrue("whitespace outside $$ still collapses",
               Same(Pg(L"AS    $$ a b $$   LANGUAGE sql"),
                    Pg(L"AS $$ a b $$ LANGUAGE sql")));

    // Nested tags: the outer run must close on $outer$, not on the first $inner$.
    const wxString nested = L"$outer$ begin $inner$ raw $inner$ end $outer$";
    ExpectText("nested $tag$ closes on the matching tag only",
               Pg(nested).text, nested);

    // Same body with the tail reformatted: proves the scanner really did return
    // to Code after $outer$ (if it had not, this would come back Unresolved).
    ExpectTrue("scanner returns to Code after the closing tag",
               Same(Pg(nested + L"   ;"), Pg(nested + L" ;")));

    // `$1` is a SQL-function parameter reference, not a dollar-quote opener.
    // Mis-framing it would swallow the rest of the body into a quote state.
    ExpectText("$1 parameter reference is code, not an opener",
               Pg(L"SELECT $1 +   $2").text, L"SELECT $1 + $2");
    ExpectTrue("$1 body still resolves", Pg(L"SELECT $1 + $2").Ok());

    // A comment BEFORE the body opener behaves normally.
    ExpectTrue("comment outside $$ is still stripped",
               Same(Pg(L"-- header\n$$ body $$"), Pg(L"$$ body $$")));

    // MySQL rules have dollarQuoting off: `$$` there is just two characters, and
    // must not put the MySQL scanner into a quote state.
    ExpectTrue("MySQL rules do not treat $$ as a quote", My(L"SET x = 1; $$").Ok());
}

// ---------------------------------------------------------------------------
// 4. MySQL's `--` needs a space; `#` comments; backticks
// ---------------------------------------------------------------------------
static void TestMySqlDialectQuirks()
{
    std::printf("\n-- MySQL quirks --\n");

    // `x--1` is `x - (-1)`. Deleting to EOL here silently rewrites arithmetic.
    ExpectText("MySQL: -- without a space is arithmetic",
               My(L"SET x = y--1;").text, L"SET x = y--1;");

    ExpectText("MySQL: -- with a space IS a comment",
               My(L"SET x = 1; -- note\nSET y = 2;").text, L"SET x = 1; SET y = 2;");

    // PG has no such requirement — the same text means different things per
    // dialect, which is precisely why the rules are parameterized.
    ExpectText("PG: bare -- IS a comment",
               Pg(L"SET x = y--1;").text, L"SET x = y");

    ExpectText("MySQL # line comment",
               My(L"SET x = 1; # note\nSET y = 2;").text, L"SET x = 1; SET y = 2;");
    ExpectText("PG has no # comment",
               Pg(L"SET x = 1; # note").text, L"SET x = 1; # note");

    // Backtick identifiers: contents are untouchable, including a `--` inside.
    ExpectText("backtick identifier content is verbatim",
               My(L"SELECT `weird -- name`   FROM t").text,
               L"SELECT `weird -- name` FROM t");
    ExpectText("doubled backtick stays inside the identifier",
               My(L"SELECT `a``b`  FROM t").text, L"SELECT `a``b` FROM t");

    // /*! ... */ is EXECUTED by MySQL. Stripping it would call two routines with
    // different behaviour identical — a false negative.
    ExpectText("MySQL executable comment preserved verbatim",
               My(L"SELECT /*!40001 SQL_NO_CACHE */ a").text,
               L"SELECT /*!40001 SQL_NO_CACHE */ a");
    ExpectTrue("executable comment is a real difference",
               !Same(My(L"SELECT /*!40001 SQL_NO_CACHE */ a"), My(L"SELECT a")));
    // ...but an ordinary MySQL block comment is still commentary.
    ExpectText("MySQL ordinary block comment still stripped",
               My(L"SELECT /* plain */ a").text, L"SELECT a");
}

// ---------------------------------------------------------------------------
// 5. String literals: apostrophes, doubling, backslashes
// ---------------------------------------------------------------------------
static void TestStringLiterals()
{
    std::printf("\n-- string literals --\n");

    ExpectText("doubled apostrophe stays inside the literal",
               Pg(L"RETURN 'it''s   fine';").text, L"RETURN 'it''s   fine';");

    // The classic de-sync: after a doubled quote a broken scanner thinks it is
    // in Code and starts stripping the rest of the literal.
    ExpectText("comment markers after a doubled quote are still literal",
               Pg(L"RETURN 'a''b -- c';").text, L"RETURN 'a''b -- c';");

    ExpectText("whitespace inside a literal is not collapsed",
               Pg(L"RETURN 'a    b';").text, L"RETURN 'a    b';");

    // MySQL honours \' inside a literal; PG (standard_conforming_strings=on)
    // does not. Same bytes, different framing — both must resolve, and the
    // MySQL one must keep the whole literal.
    ExpectText("MySQL backslash escape does not close the literal",
               My(L"RETURN 'it\\'s   ok';").text, L"RETURN 'it\\'s   ok';");
    ExpectTrue("MySQL backslash-escaped literal resolves", My(L"RETURN 'it\\'s ok';").Ok());

    // PG: `"..."` is a quoted identifier; MySQL default: it is a string. Either
    // way the contents are copied verbatim, so one state serves both.
    ExpectText("double-quoted run is verbatim",
               Pg(L"SELECT \"Odd -- Col\"   FROM t").text,
               L"SELECT \"Odd -- Col\" FROM t");
    ExpectText("doubled double-quote stays inside",
               Pg(L"SELECT \"a\"\"b\"  FROM t").text, L"SELECT \"a\"\"b\" FROM t");
}

// ---------------------------------------------------------------------------
// 6. Line endings
// ---------------------------------------------------------------------------
static void TestLineEndings()
{
    std::printf("\n-- line endings --\n");

    ExpectTrue("CRLF body == LF body (code)",
               Same(Pg(L"BEGIN\r\n  RETURN 1;\r\nEND"), Pg(L"BEGIN\n  RETURN 1;\nEND")));

    ExpectTrue("mixed CRLF/LF body == LF body",
               Same(Pg(L"BEGIN\r\n  a;\n  b;\r\nEND"), Pg(L"BEGIN\n a;\n b;\nEND")));

    // The documented trade: line endings are folded INSIDE quoted runs too, so
    // this pair is reported identical. Pinned as a decision, not an accident.
    ExpectTrue("CRLF inside a dollar-quoted body also folds (documented trade)",
               Same(Pg(L"$$ line1\r\nline2 $$"), Pg(L"$$ line1\nline2 $$")));

    // A lone CR (classic Mac / mangled transfer) folds too.
    ExpectTrue("lone CR folds to LF",
               Same(Pg(L"BEGIN\r RETURN 1;\r END"), Pg(L"BEGIN\n RETURN 1;\n END")));

    ExpectText("CRLF inside a line comment does not leak the CR",
               Pg(L"a; -- note\r\nb;").text, L"a; b;");
}

// ---------------------------------------------------------------------------
// 7. THE CONTRACT: unresolvable input degrades to "can't tell", never "differs"
// ---------------------------------------------------------------------------
static void TestUnresolvedDegradation()
{
    std::printf("\n-- unresolvable input --\n");

    struct Case { const wchar_t* body; const char* name; bool pg; };
    const Case cases[] = {
        { L"RETURN 'unterminated",              "unterminated single quote",  true  },
        { L"SELECT \"unterminated",             "unterminated double quote",  true  },
        { L"$$ body never closed",              "unterminated dollar quote",  true  },
        { L"$tag$ body never closed",           "unterminated $tag$",         true  },
        { L"$outer$ a $inner$ b $inner$ c",     "unterminated outer tag",     true  },
        { L"SELECT 1 /* never closed",          "unterminated block comment", true  },
        { L"SELECT `never closed",              "unterminated backtick",      false },
        { L"SELECT /*!40001 never closed",      "unterminated /*! comment",   false },
    };

    for (const auto& c : cases) {
        const NormalizeResult r = c.pg ? Pg(c.body) : My(c.body);
        ++g_checks;
        const bool ok = (r.status == NormalizeStatus::Unresolved) &&
                        (r.text == wxString(c.body)) &&      // ORIGINAL handed back
                        !r.reason.IsEmpty();                 // and a reason to show
        if (!ok) {
            ++g_fails;
            std::printf("  FAIL %s (status=%d, textEqualsOriginal=%d, reason=[%s])\n",
                        c.name, (int)r.status, (int)(r.text == wxString(c.body)),
                        (const char*)r.reason.utf8_str());
        } else {
            std::printf("  ok   %s -> Unresolved, original returned, reason given\n", c.name);
        }
    }

    // The point of the contract, stated as behaviour: an unresolvable body must
    // never be silently "normalized" into something that then compares UNEQUAL
    // to its own well-formed twin. The compare layer reads .Ok() and produces
    // NotComparable; it never reaches a text comparison at all.
    const NormalizeResult bad  = Pg(L"$$ oops");
    ExpectTrue("unresolvable body is not Ok, so no text verdict is possible",
               !bad.Ok());
    ExpectTrue("unresolvable body was not partially rewritten",
               bad.text == wxString(L"$$ oops"));
}

// ---------------------------------------------------------------------------
// 8. Realistic bodies, end to end
// ---------------------------------------------------------------------------
static void TestRealisticBodies()
{
    std::printf("\n-- realistic bodies --\n");

    // Same plpgsql routine, one copy reformatted and commented by a human.
    const wxString a =
        L"DECLARE\n"
        L"  total numeric := 0;\n"
        L"BEGIN\n"
        L"  -- sum the invoice lines\n"
        L"  SELECT sum(amount) INTO total FROM lines WHERE note <> 'n/a -- skip';\n"
        L"  RETURN total;\n"
        L"END";
    const wxString b =
        L"DECLARE total numeric := 0; BEGIN "
        L"SELECT sum(amount) INTO total FROM lines WHERE note <> 'n/a -- skip'; "
        L"RETURN total; END";
    ExpectTrue("plpgsql: reformatted twin is identical", Same(Pg(a), Pg(b)));

    // The `-- skip` inside the literal is content: change it and the bodies must
    // stop matching.
    const wxString c = b;
    wxString d = b;
    d.Replace(L"n/a -- skip", L"n/a -- keep");
    ExpectTrue("a change INSIDE the literal is a real difference", !Same(Pg(c), Pg(d)));

    // MySQL procedure body as information_schema.ROUTINES returns it.
    const wxString m1 =
        L"BEGIN\r\n"
        L"  # legacy note\r\n"
        L"  DECLARE done INT DEFAULT 0;\r\n"
        L"  UPDATE `order` SET total = total--1 WHERE id = p_id;\r\n"
        L"END";
    const wxString m2 =
        L"BEGIN DECLARE done INT DEFAULT 0; "
        L"UPDATE `order` SET total = total--1 WHERE id = p_id; END";
    ExpectTrue("mysql: CRLF + # comment + `--` arithmetic twin is identical",
               Same(My(m1), My(m2)));

    // ...and the arithmetic really did survive: dropping it is a difference.
    wxString m3 = m2;
    m3.Replace(L"total--1", L"total");
    ExpectTrue("mysql: the -- arithmetic was not eaten", !Same(My(m2), My(m3)));
}

// ---------------------------------------------------------------------------
// 9. Rules plumbing
// ---------------------------------------------------------------------------
static void TestRulesForDialect()
{
    std::printf("\n-- rules per dialect --\n");

    ExpectTrue("MySQL is modelled",    NormalizeRulesKnown(Dialect::MySQL));
    ExpectTrue("Postgres is modelled", NormalizeRulesKnown(Dialect::Postgres));
    ExpectTrue("SQLite is NOT modelled",    !NormalizeRulesKnown(Dialect::Sqlite));
    ExpectTrue("SQL Server is NOT modelled", !NormalizeRulesKnown(Dialect::SqlServer));
    ExpectTrue("Oracle is NOT modelled",     !NormalizeRulesKnown(Dialect::Oracle));

    ExpectTrue("RulesFor(MySQL) enables backticks",  RulesFor(Dialect::MySQL).backtickIdent);
    ExpectTrue("RulesFor(MySQL) requires -- space",  RulesFor(Dialect::MySQL).dashDashNeedsSpace);
    ExpectTrue("RulesFor(Postgres) enables $$",      RulesFor(Dialect::Postgres).dollarQuoting);
    ExpectTrue("RulesFor(Postgres) has no backslash escapes",
               !RulesFor(Dialect::Postgres).backslashEscapes);

    // An unmodelled dialect gets the conservative defaults — nothing dialect
    // specific is silently switched on.
    const NormalizeRules def = RulesFor(Dialect::Oracle);
    ExpectTrue("unmodelled dialect: no dollar quoting", !def.dollarQuoting);
    ExpectTrue("unmodelled dialect: no backticks",      !def.backtickIdent);
    ExpectTrue("unmodelled dialect: no # comments",     !def.hashLineComment);
}

int main()
{
    std::printf("=== RoutineNormalizeTests ===\n");
    TestWhitespaceAndCommentsInCode();
    TestNoCaseFolding();
    TestDollarQuoting();
    TestMySqlDialectQuirks();
    TestStringLiterals();
    TestLineEndings();
    TestUnresolvedDegradation();
    TestRealisticBodies();
    TestRulesForDialect();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
