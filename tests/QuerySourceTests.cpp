// QuerySourceTests.cpp — headless unit tests for ui::qsrc (src/ui/QuerySource
// .{h,cpp}), which derives the BASE TABLE of an editor SELECT so the result grid
// can be bound to it for editing.
//
// WHY THIS FILE EXISTS. This logic used to be a substring search inline in
// MainFrame_Query.cpp — a wx translation unit, reachable only by clicking
// through a running app against a live server. It decides WHICH TABLE the 保存
// button writes to. Getting it wrong is not a cosmetic bug: the user edits the
// rows they can see and a different table changes.
//
// It WAS wrong. `table.Replace(L"`", L"")` deleted quote characters instead of
// unquoting, so a table named ``we`ird`` (written ``` `we``ird` ```) arrived as
// `weird` — a DIFFERENT, possibly existing, table. `AfterLast('.')` threw away
// the qualifier, so `otherdb.orders` bound to the current database's `orders`.
// And the raw `low.Find(" from ")` found a ` from ` inside a comment or a string
// literal just as happily as a real one.
//
// Every one of those is asserted below as a REGRESSION test, together with the
// refusals that replaced the guessing. The rule the whole module serves: when
// the base table cannot be established, the answer is READ-ONLY, never a
// different table.
//
// Same dependency-free harness as the rest of tests/: an ExpectEq loop, non-zero
// exit on failure, no doctest/Catch2. Compiles QuerySource.cpp directly and
// links swiftsql::db for db::Dialect and for db::sync::NormalizeRoutineBody —
// the shared dialect quote-state machine this module reuses for comment removal.
// No wxWindow, no wxGrid, no connection.
#include "ui/QuerySource.h"

#include <cstdio>

using db::Dialect;
using ui::qsrc::BindRefusal;
using ui::qsrc::SelectSource;
using ui::qsrc::SourceStatus;
namespace qs = ui::qsrc;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectEq(const char* what, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n        want: [%s]\n        got : [%s]\n",
                    what, (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", what);
    }
}

static void ExpectTrue(const char* what, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", what); }
    else       { std::printf("  ok   %s\n", what); }
}

// Assert the whole parse outcome at once: status, qualifier, table.
static void ExpectSource(const char* what, const wxString& sql, Dialect d,
                         SourceStatus status, const wxString& qualifier,
                         const wxString& table)
{
    const SelectSource s = qs::ParseSelectSource(sql, d);
    ++g_checks;
    if (s.status != status || s.qualifier != qualifier || s.table != table) {
        ++g_fails;
        std::printf("  FAIL %s\n        sql : [%s]\n"
                    "        want: status=%d qual=[%s] table=[%s]\n"
                    "        got : status=%d qual=[%s] table=[%s]\n",
                    what, (const char*)sql.utf8_str(),
                    (int)status, (const char*)qualifier.utf8_str(),
                    (const char*)table.utf8_str(),
                    (int)s.status, (const char*)s.qualifier.utf8_str(),
                    (const char*)s.table.utf8_str());
    } else {
        std::printf("  ok   %s\n", what);
    }
}

static void ExpectNotSimple(const char* what, const wxString& sql,
                            Dialect d = Dialect::MySQL)
{
    ExpectSource(what, sql, d, SourceStatus::NotSimple, wxString(), wxString());
}

// A table this program can be asked to edit. Both names are legal on their
// dialect and both used to collapse onto an existing, DIFFERENT table.
static const wchar_t* kMyExotic = L"we`ird";    // MySQL: CREATE TABLE `we``ird`
static const wchar_t* kPgExotic = L"we\"ird";   // PG:    CREATE TABLE "we""ird"

// ---------------------------------------------------------------------------
// The defect this module exists for
// ---------------------------------------------------------------------------
static void TestExoticNamesSurviveIntact()
{
    // THE regression. Old behaviour: `weird`. That is a different table, and if
    // it exists with a primary key the grid silently became editable against it.
    ExpectSource("MySQL: a doubled backtick un-doubles to one literal backtick",
                 L"SELECT * FROM `we``ird`", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), kMyExotic);
    ExpectTrue("...and is NOT the quote-stripped `weird`",
               qs::ParseSelectSource(L"SELECT * FROM `we``ird`", Dialect::MySQL)
                   .table != L"weird");

    ExpectSource("PG: a doubled double-quote un-doubles to one literal quote",
                 L"SELECT * FROM \"we\"\"ird\"", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), kPgExotic);

    // A quote char that is not the dialect's delimiter is ORDINARY DATA in an
    // unquoted name, and deleting it was equally lossy.
    ExpectSource("PG: a backtick inside a quoted name is data, not a delimiter",
                 L"SELECT * FROM \"we`ird\"", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), L"we`ird");
    ExpectSource("MySQL: a double quote inside a backtick name is data",
                 L"SELECT * FROM `we\"ird`", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"we\"ird");

    // A dot INSIDE a quoted identifier is part of the name. The old
    // `AfterLast('.')` split it, turning `my.table` into `table` — a third way
    // to reach a different table, and legal on both engines.
    ExpectSource("MySQL: a dot inside a quoted name is not a qualifier",
                 L"SELECT * FROM `my.table`", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"my.table");
    ExpectSource("PG: a dot inside a quoted name is not a qualifier",
                 L"SELECT * FROM \"my.table\"", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), L"my.table");

    // Quoting is not required for the name to be exotic.
    ExpectSource("an unquoted CJK name is one identifier",
                 L"SELECT * FROM 订单表", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"订单表");
    ExpectSource("a quoted name with a space survives",
                 L"SELECT * FROM `order items`", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"order items");
}

// ---------------------------------------------------------------------------
// Ordinary statements must keep working
// ---------------------------------------------------------------------------
static void TestPlainSelects()
{
    ExpectSource("bare table", L"SELECT * FROM users", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("lower-case keywords", L"select * from users", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("trailing semicolon", L"SELECT * FROM users;", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("an alias does not change the base table",
                 L"SELECT u.id FROM users AS u", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("WHERE / ORDER BY / LIMIT are fine",
                 L"SELECT * FROM users WHERE age > 3 ORDER BY id LIMIT 10",
                 Dialect::MySQL, SourceStatus::Ok, wxString(), L"users");
    ExpectSource("newlines and tabs between clauses",
                 L"SELECT *\n\tFROM\n\tusers\nWHERE id = 1", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("case is PRESERVED — it may matter to the server",
                 L"SELECT * FROM Users", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"Users");
    // `select` / `from` as SUBSTRINGS of a name must not be mistaken for keywords.
    ExpectSource("a table named from_log is a name, not the FROM keyword",
                 L"SELECT * FROM from_log", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"from_log");
}

// ---------------------------------------------------------------------------
// Comments and string literals are not SQL text
// ---------------------------------------------------------------------------
// The old `low.Find(" from ")` searched RAW text, so any of these could hand the
// write path a name lifted out of a comment or a string.
static void TestCommentsAndLiteralsCannotSupplyATable()
{
    ExpectSource("a line comment naming another table is ignored",
                 L"SELECT * FROM users -- FROM audit_log", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("a block comment between tokens is ignored",
                 L"SELECT /* FROM audit_log */ * FROM users", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("MySQL # line comment",
                 L"SELECT * FROM users # FROM audit_log", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("a string literal containing ' from ' is not a FROM clause",
                 L"SELECT * FROM users WHERE note = 'copied from audit_log'",
                 Dialect::MySQL, SourceStatus::Ok, wxString(), L"users");
    // MySQL: `--` is a comment only when followed by whitespace. `a--1` is
    // arithmetic, and treating it as a comment would swallow the real FROM.
    ExpectSource("MySQL: `x--1` is arithmetic, not a comment",
                 L"SELECT a--1 FROM users", Dialect::MySQL,
                 SourceStatus::Ok, wxString(), L"users");
    // PostgreSQL: bare `--` IS a comment.
    ExpectSource("PG: bare -- starts a comment",
                 L"SELECT * FROM users --x", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), L"users");
    // A dollar-quoted literal is a STRING. Its contents must not tokenize as SQL,
    // or `from x` inside one supplies the table.
    ExpectSource("PG: $$ … $$ contents are a literal, not a FROM clause",
                 L"SELECT $$ from x $$ FROM users", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), L"users");
    ExpectSource("PG: a tagged $tag$ … $tag$ literal likewise",
                 L"SELECT $t$ from x $t$ FROM users", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), L"users");
    // `$1` is a parameter reference, not a literal opener — it must not swallow
    // the rest of the statement.
    ExpectSource("PG: $1 is a parameter, not a dollar-quote opener",
                 L"SELECT $1 FROM users", Dialect::Postgres,
                 SourceStatus::Ok, wxString(), L"users");
}

// ---------------------------------------------------------------------------
// The qualifier
// ---------------------------------------------------------------------------
static void TestQualifier()
{
    ExpectSource("MySQL: db.table splits into qualifier + table",
                 L"SELECT * FROM shop.orders", Dialect::MySQL,
                 SourceStatus::Ok, L"shop", L"orders");
    ExpectSource("MySQL: a QUOTED qualifier un-doubles too",
                 L"SELECT * FROM `sh``op`.`we``ird`", Dialect::MySQL,
                 SourceStatus::Ok, L"sh`op", kMyExotic);
    ExpectSource("PG: schema.table splits",
                 L"SELECT * FROM archive.orders", Dialect::Postgres,
                 SourceStatus::Ok, L"archive", L"orders");
    // db.schema.table cannot be carried by a bare-name EditableSpec.
    ExpectNotSimple("a three-part name is refused",
                    L"SELECT * FROM d.s.t");

    // ---- policy -----------------------------------------------------------
    const SelectSource sameDb  = qs::ParseSelectSource(L"SELECT * FROM shop.orders",
                                                       Dialect::MySQL);
    const SelectSource pgQual  = qs::ParseSelectSource(L"SELECT * FROM archive.orders",
                                                       Dialect::Postgres);
    const SelectSource plain   = qs::ParseSelectSource(L"SELECT * FROM orders",
                                                       Dialect::MySQL);

    ExpectTrue("no qualifier written → bind",
               qs::ClassifyBind(plain, Dialect::MySQL, L"shop") == BindRefusal::None);
    ExpectTrue("MySQL: qualifier == current db → same table, bind",
               qs::ClassifyBind(sameDb, Dialect::MySQL, L"shop") == BindRefusal::None);
    // The one that used to silently edit the current db's same-named table.
    ExpectTrue("MySQL: a DIFFERENT db is refused, not silently dropped",
               qs::ClassifyBind(sameDb, Dialect::MySQL, L"other") ==
                   BindRefusal::ForeignQualifier);
    ExpectTrue("MySQL: db names are case-sensitive — no case-insensitive match",
               qs::ClassifyBind(sameDb, Dialect::MySQL, L"SHOP") ==
                   BindRefusal::ForeignQualifier);
    ExpectTrue("MySQL: no current db selected → a qualifier cannot be verified",
               qs::ClassifyBind(sameDb, Dialect::MySQL, wxString()) ==
                   BindRefusal::ForeignQualifier);
    // PostgreSQL: the qualifier is a SCHEMA. The DML the grid emits carries no
    // schema, so whether it lands on archive.orders or public.orders is decided
    // by search_path — the measured conflation recorded in QualifiedName.h.
    ExpectTrue("PG: a schema qualifier is always refused",
               qs::ClassifyBind(pgQual, Dialect::Postgres, L"shop") ==
                   BindRefusal::ForeignQualifier);
}

// ---------------------------------------------------------------------------
// Refusals — the failure mode must be read-only, never a different table
// ---------------------------------------------------------------------------
static void TestRefusals()
{
    ExpectNotSimple("a join",     L"SELECT * FROM a JOIN b ON a.id = b.id");
    ExpectNotSimple("a union",    L"SELECT * FROM a UNION SELECT * FROM b");
    ExpectNotSimple("group by",   L"SELECT x FROM a GROUP BY x");
    ExpectNotSimple("distinct",   L"SELECT DISTINCT x FROM a");
    ExpectNotSimple("an aggregate", L"SELECT COUNT(*) FROM a");
    ExpectNotSimple("a comma join", L"SELECT * FROM a, b");
    ExpectNotSimple("a derived table", L"SELECT * FROM (SELECT 1) z");
    ExpectNotSimple("a scalar subquery", L"SELECT (SELECT 1) FROM a");
    ExpectNotSimple("no FROM at all", L"SELECT 1");
    ExpectNotSimple("not a SELECT",  L"UPDATE a SET x = 1");
    ExpectNotSimple("empty input",   L"");
    // A CTE is the sharp one: the old substring scan found the ` from ` INSIDE
    // the CTE body and bound the grid to that table, not to the one selected.
    ExpectNotSimple("a CTE is not a simple select",
                    L"WITH c AS (SELECT * FROM audit_log) SELECT * FROM c");

    // Unterminated quoting: the text cannot be framed, so nothing is guessed.
    ExpectSource("an unterminated quoted identifier is Unparsable",
                 L"SELECT * FROM `users", Dialect::MySQL,
                 SourceStatus::Unparsable, wxString(), wxString());
    ExpectSource("an unterminated string literal is Unparsable",
                 L"SELECT * FROM users WHERE a = 'oops", Dialect::MySQL,
                 SourceStatus::Unparsable, wxString(), wxString());
    ExpectSource("an unterminated block comment is Unparsable",
                 L"SELECT * FROM users /* oops", Dialect::MySQL,
                 SourceStatus::Unparsable, wxString(), wxString());

    // SQL Server bracket quoting is not modelled. Refusing is the honest answer;
    // the old code produced the NAME `[t]`, which simply failed to resolve.
    ExpectSource("SQL Server [brackets] are an unmodelled quoting form",
                 L"SELECT * FROM [dbo].[t]", Dialect::SqlServer,
                 SourceStatus::UnknownQuoting, wxString(), wxString());

    // …and every refusal maps to a non-binding classification.
    ExpectTrue("Unparsable does not bind",
               qs::ClassifyBind(qs::ParseSelectSource(L"SELECT * FROM `users",
                                                      Dialect::MySQL),
                                Dialect::MySQL, L"shop") == BindRefusal::Unparsable);
    ExpectTrue("UnknownQuoting does not bind",
               qs::ClassifyBind(qs::ParseSelectSource(L"SELECT * FROM [t]",
                                                      Dialect::SqlServer),
                                Dialect::SqlServer, L"shop") ==
                   BindRefusal::UnknownQuoting);
    ExpectTrue("NotSimple does not bind",
               qs::ClassifyBind(qs::ParseSelectSource(L"SELECT * FROM a, b",
                                                      Dialect::MySQL),
                                Dialect::MySQL, L"shop") ==
                   BindRefusal::NotSimpleSelect);
}

// ---------------------------------------------------------------------------
// The contract with the write path
// ---------------------------------------------------------------------------
// ui::gridsql quotes with db::QuoteIdent and documents that every identifier
// reaching it is RAW. This is the other half of that contract: what this module
// hands over must round-trip back to the SQL that was written.
static void TestRoundTripsThroughQuoteIdent()
{
    const SelectSource my = qs::ParseSelectSource(L"SELECT * FROM `we``ird`",
                                                  Dialect::MySQL);
    ExpectEq("MySQL: parsed name re-quotes to the ORIGINAL written form",
             db::QuoteIdent(my.table, Dialect::MySQL), L"`we``ird`");

    const SelectSource pg = qs::ParseSelectSource(L"SELECT * FROM \"we\"\"ird\"",
                                                  Dialect::Postgres);
    ExpectEq("PG: parsed name re-quotes to the ORIGINAL written form",
             db::QuoteIdent(pg.table, Dialect::Postgres), L"\"we\"\"ird\"");

    const SelectSource dotted = qs::ParseSelectSource(L"SELECT * FROM `my.table`",
                                                      Dialect::MySQL);
    ExpectEq("a dotted name re-quotes as ONE identifier, not a qualified pair",
             db::QuoteIdent(dotted.table, Dialect::MySQL), L"`my.table`");
}

int main()
{
    TestExoticNamesSurviveIntact();
    TestPlainSelects();
    TestCommentsAndLiteralsCannotSupplyATable();
    TestQualifier();
    TestRefusals();
    TestRoundTripsThroughQuoteIdent();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
