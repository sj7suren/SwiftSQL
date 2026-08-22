// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sqlscript_test.cpp — unit tests for db::SplitSqlScript, the dialect-aware SQL
// statement splitter (src/db/SqlScript.cpp).
//
// Why this is the first test target: SplitSqlScript is a *pure* function (no I/O,
// no drivers, no GUI) whose correctness is subtle — the hard cases are all the
// places a ';' can appear WITHOUT ending a statement (strings, quoted identifiers,
// comments, MySQL DELIMITER, PostgreSQL $$…$$). That is the highest-ROI thing to
// pin down with tests before we consolidate the editor's naive splitter onto it
// (Sprint 1 · S1-03 / risk R2).
//
// Harness: intentionally dependency-free (no doctest/Catch2). The project ships as
// a single self-contained binary and has no test framework vendored; a ~30-line
// assert harness fully covers a pure function that only returns vector<wxString>.
// Upgrading to doctest/Catch2 later is a documented follow-up, not a blocker.

#include "db/SqlScript.h"
#include "db/DbDriver.h"   // db::Dialect
#include <cstdio>
#include <vector>
#include <wx/string.h>

using db::SplitSqlScript;
using db::Dialect;

static int g_checks = 0;
static int g_fails  = 0;

static wxString Join(const std::vector<wxString>& v)
{
    wxString s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L" | ";
        s += L"<" + v[i] + L">";
    }
    return s;
}

static void ExpectVec(const char* name, const std::vector<wxString>& got,
                      const std::vector<wxString>& want)
{
    ++g_checks;
    bool ok = (got.size() == want.size());
    for (size_t i = 0; ok && i < got.size(); ++i) ok = (got[i] == want[i]);
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: %s\n    got : %s\n", name,
                    (const char*)Join(want).utf8_str(),
                    (const char*)Join(got).utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

int main()
{
    std::printf("== SplitSqlScript unit tests ==\n");

    // --- 1. empty / whitespace / comment-only produce no statements ---------
    ExpectVec("empty",              SplitSqlScript(L"", Dialect::MySQL), {});
    ExpectVec("whitespace-only",    SplitSqlScript(L"  \n\t ", Dialect::MySQL), {});
    ExpectVec("line-comment-only",  SplitSqlScript(L"-- just a note", Dialect::Postgres), {});
    ExpectVec("block-comment-only", SplitSqlScript(L"/* note */", Dialect::Postgres), {});

    // --- 2. basic splitting -------------------------------------------------
    ExpectVec("single, no terminator",
              SplitSqlScript(L"SELECT 1", Dialect::MySQL), { L"SELECT 1" });
    ExpectVec("two statements",
              SplitSqlScript(L"SELECT 1; SELECT 2;", Dialect::MySQL),
              { L"SELECT 1", L"SELECT 2" });
    ExpectVec("trailing statement without terminator",
              SplitSqlScript(L"SELECT 1;\nSELECT 2", Dialect::MySQL),
              { L"SELECT 1", L"SELECT 2" });

    // --- 3. a ';' inside quoted text must NOT split -------------------------
    ExpectVec("semicolon inside string literal",
              SplitSqlScript(L"SELECT ';';", Dialect::MySQL), { L"SELECT ';'" });
    ExpectVec("doubled-quote escape ('')",
              SplitSqlScript(L"SELECT 'a''b';", Dialect::MySQL), { L"SELECT 'a''b'" });
    ExpectVec("semicolon inside double-quoted identifier",
              SplitSqlScript(L"SELECT \"a;b\";", Dialect::Postgres), { L"SELECT \"a;b\"" });
    ExpectVec("semicolon inside backtick identifier (mysql)",
              SplitSqlScript(L"SELECT `a;b`;", Dialect::MySQL), { L"SELECT `a;b`" });
    ExpectVec("mysql backslash-escaped quote",
              SplitSqlScript(L"SELECT 'a\\';b';", Dialect::MySQL), { L"SELECT 'a\\';b'" });

    // --- 4. comments are preserved as part of their statement ---------------
    ExpectVec("trailing line comment (--)",
              SplitSqlScript(L"SELECT 1 -- tail\n;", Dialect::MySQL), { L"SELECT 1 -- tail" });
    ExpectVec("trailing hash comment (# , mysql)",
              SplitSqlScript(L"SELECT 1 # tail\n;", Dialect::MySQL), { L"SELECT 1 # tail" });
    ExpectVec("executable comment /*! ... */ (mysql) survives",
              SplitSqlScript(L"/*!40101 SET NAMES utf8 */;", Dialect::MySQL),
              { L"/*!40101 SET NAMES utf8 */" });
    ExpectVec("block comment bundles into following statement",
              SplitSqlScript(L"SELECT 1;/* mid */SELECT 2;", Dialect::MySQL),
              { L"SELECT 1", L"/* mid */SELECT 2" });

    // --- 5. MySQL DELIMITER reassignment ------------------------------------
    {
        const wxString in =
            L"DELIMITER //\n"
            L"CREATE PROCEDURE p() BEGIN SELECT 1; END //\n"
            L"DELIMITER ;\n"
            L"SELECT 2;\n";
        ExpectVec("DELIMITER // keeps inner ';' , splits on //",
                  SplitSqlScript(in, Dialect::MySQL),
                  { L"CREATE PROCEDURE p() BEGIN SELECT 1; END", L"SELECT 2" });
    }

    // --- 6. PostgreSQL dollar-quoting ---------------------------------------
    // Exact comparison (not a substring Contains) so a regression that mangles
    // the body or drops a newline is caught — this statement is what S1-03 relies
    // on staying byte-stable when the editor path adopts this splitter.
    ExpectVec("dollar-quote body keeps inner ';' and layout intact",
              SplitSqlScript(
                  L"CREATE FUNCTION f() RETURNS int AS $$\nBEGIN\n  RETURN 1;\nEND\n$$ LANGUAGE plpgsql;\n"
                  L"SELECT 2;",
                  Dialect::Postgres),
              { L"CREATE FUNCTION f() RETURNS int AS $$\nBEGIN\n  RETURN 1;\nEND\n$$ LANGUAGE plpgsql",
                L"SELECT 2" });
    ExpectVec("named dollar tag $tag$ ... $tag$",
              SplitSqlScript(L"SELECT $tag$ a;b $tag$;", Dialect::Postgres),
              { L"SELECT $tag$ a;b $tag$" });
    ExpectVec("bare $1 param is not a dollar-quote",
              SplitSqlScript(L"SELECT $1;", Dialect::Postgres), { L"SELECT $1" });

    // --- 7. dialect divergence: backslash escaping is MySQL-only ------------
    // Same input, two dialects, DIFFERENT results — this is the whole reason the
    // splitter is dialect-aware, and exactly what S1-03 must preserve. In MySQL
    // the \' is an escaped quote (one statement); in Postgres '\' has no special
    // meaning, so the quote closes and the ';' splits (two statements).
    ExpectVec("mysql: \\' escapes, stays one statement",
              SplitSqlScript(L"SELECT 'a\\';b';", Dialect::MySQL), { L"SELECT 'a\\';b'" });
    ExpectVec("postgres: no backslash escape, splits into two",
              SplitSqlScript(L"SELECT 'a\\';b';", Dialect::Postgres),
              { L"SELECT 'a\\'", L"b';" });

    // --- 8. robustness: empty statements & unterminated constructs ----------
    ExpectVec("empty statements between ';' are dropped",
              SplitSqlScript(L"SELECT 1;;SELECT 2;", Dialect::MySQL),
              { L"SELECT 1", L"SELECT 2" });
    ExpectVec("only semicolons yield nothing",
              SplitSqlScript(L";;;", Dialect::MySQL), {});
    ExpectVec("unclosed string to EOF returns partial (no hang/crash)",
              SplitSqlScript(L"SELECT 'abc", Dialect::MySQL), { L"SELECT 'abc" });
    // Editable-gate pin (arch-lead §4-B): a trailing comment must NOT become a
    // second statement, or the editor's "single SELECT is editable" gate breaks.
    ExpectVec("trailing comment does not create a 2nd statement",
              SplitSqlScript(L"SELECT 1; -- note", Dialect::MySQL), { L"SELECT 1" });

    // --- 9. StatementTouchesTables: does a run need a table-list reload? ----
    //
    // WHAT THIS PROTECTS. A plain SQL editor that runs CREATE TABLE has to
    // reload the sidebar and the 表信息 list, or the table the user just made is
    // simply absent — succeeded, nothing shown, no error. This predicate is what
    // decides; a FALSE here is a table that never appears.
    auto ExpectTouch = [](const char* name, const wxString& sql, bool want) {
        ++g_checks;
        const bool got = db::StatementTouchesTables(sql);
        if (got != want) {
            ++g_fails;
            std::printf("  FAIL %s  want=%s got=%s\n", name,
                        want ? "true" : "false", got ? "true" : "false");
        } else {
            std::printf("  ok   %s\n", name);
        }
    };

    ExpectTouch("CREATE TABLE touches tables", L"CREATE TABLE t (id int)", true);
    ExpectTouch("lower-case verb is recognised", L"create table t (id int)", true);
    ExpectTouch("CREATE TABLE IF NOT EXISTS", L"CREATE TABLE IF NOT EXISTS t (id int)", true);
    ExpectTouch("CREATE TEMPORARY TABLE", L"CREATE TEMPORARY TABLE t (id int)", true);
    ExpectTouch("DROP TABLE", L"DROP TABLE t", true);
    ExpectTouch("DROP TABLE IF EXISTS", L"DROP TABLE IF EXISTS `t`", true);
    ExpectTouch("ALTER TABLE", L"ALTER TABLE t ADD COLUMN c int", true);
    ExpectTouch("RENAME TABLE (MySQL)", L"RENAME TABLE a TO b", true);
    ExpectTouch("TRUNCATE with the optional TABLE keyword", L"TRUNCATE TABLE t", true);
    ExpectTouch("TRUNCATE without it — still only ever a table", L"TRUNCATE t", true);

    // NOT tables. Each of these has its own folder and its own refresh path;
    // claiming them here would reload the table list for something that can
    // never show up in it.
    ExpectTouch("CREATE VIEW is NOT a table", L"CREATE VIEW v AS SELECT 1", false);
    ExpectTouch("CREATE OR REPLACE VIEW is NOT a table",
                L"CREATE OR REPLACE VIEW v AS SELECT 1", false);
    ExpectTouch("CREATE INDEX is NOT a table", L"CREATE INDEX i ON t (c)", false);
    ExpectTouch("CREATE DATABASE is NOT a table", L"CREATE DATABASE d", false);
    ExpectTouch("CREATE FUNCTION is NOT a table",
                L"CREATE FUNCTION f() RETURNS int RETURN 1", false);
    ExpectTouch("INSERT is not DDL", L"INSERT INTO t VALUES (1)", false);
    ExpectTouch("SELECT is not DDL", L"SELECT * FROM t", false);
    ExpectTouch("UPDATE is not DDL", L"UPDATE t SET c = 1", false);
    ExpectTouch("empty statement", L"", false);

    // THE DUMP CASE, and the reason the scanner skips comments before the verb:
    // this program's own exporter writes a `-- 表 X` banner ahead of every
    // statement, so a version that stopped at the first word would answer "no"
    // for every table in a script it generated itself.
    ExpectTouch("a leading -- comment banner does not hide the verb",
                L"-- 表 users\nCREATE TABLE users (id int)", true);
    ExpectTouch("a leading /* */ comment does not hide the verb",
                L"/* header */ CREATE TABLE users (id int)", true);
    ExpectTouch("a MySQL # comment does not hide the verb",
                L"# header\nDROP TABLE users", true);

    // The whole-script form: one CREATE TABLE among DML is enough to need a reload.
    {
        ++g_checks;
        const std::vector<wxString> mixed = {
            L"SET FOREIGN_KEY_CHECKS=0", L"INSERT INTO a VALUES (1)",
            L"CREATE TABLE b (id int)", L"UPDATE a SET x=1"
        };
        if (!db::ScriptTouchesTables(mixed)) {
            ++g_fails;
            std::printf("  FAIL a script with one CREATE TABLE among DML needs a reload\n");
        } else {
            std::printf("  ok   a script with one CREATE TABLE among DML needs a reload\n");
        }
    }
    {
        ++g_checks;
        const std::vector<wxString> dmlOnly = {
            L"INSERT INTO a VALUES (1)", L"UPDATE a SET x=1", L"DELETE FROM a"
        };
        if (db::ScriptTouchesTables(dmlOnly)) {
            ++g_fails;
            std::printf("  FAIL a DML-only script must NOT trigger a catalog reload\n");
        } else {
            std::printf("  ok   a DML-only script must NOT trigger a catalog reload\n");
        }
    }

    // --- 10. IsExplainable / BuildExplainSql: the AI performance analysis ---
    //
    // THE SAFETY-CRITICAL PROPERTY IS THE ABSENCE OF ANALYZE. `EXPLAIN ANALYZE`
    // actually RUNS the statement; on the UPDATE/DELETE the user is asking about
    // that is not a slow analysis, it is data loss. Every builder case below is
    // therefore also asserted NOT to contain it.
    auto ExpectExplainable = [](const char* name, const wxString& sql, bool want) {
        ++g_checks;
        const bool got = db::IsExplainable(sql);
        if (got != want) {
            ++g_fails;
            std::printf("  FAIL %s  want=%s got=%s\n", name,
                        want ? "true" : "false", got ? "true" : "false");
        } else { std::printf("  ok   %s\n", name); }
    };
    auto ExpectExplain = [](const char* name, const wxString& sql, Dialect d,
                            const std::vector<wxString>& want) {
        ExpectVec(name, db::BuildExplainSql(sql, d), want);
        // The invariant, re-checked on every single case rather than once:
        ++g_checks;
        bool analyze = false;
        for (const wxString& s : db::BuildExplainSql(sql, d))
            if (s.Upper().Contains(L"ANALYZE")) analyze = true;
        if (analyze) {
            ++g_fails;
            std::printf("  FAIL %s -- EMITS ANALYZE, which would EXECUTE the statement\n", name);
        } else {
            std::printf("  ok   %s (no ANALYZE)\n", name);
        }
    };

    ExpectExplainable("SELECT is explainable", L"SELECT 1", true);
    ExpectExplainable("WITH is explainable", L"WITH t AS (SELECT 1) SELECT * FROM t", true);
    ExpectExplainable("UPDATE is explainable", L"UPDATE t SET c=1", true);
    ExpectExplainable("DELETE is explainable", L"DELETE FROM t", true);
    ExpectExplainable("a comment banner does not hide the verb",
                      L"-- 查询\nSELECT 1", true);
    ExpectExplainable("CREATE TABLE is NOT explainable", L"CREATE TABLE t (id int)", false);
    ExpectExplainable("SET is NOT explainable", L"SET FOREIGN_KEY_CHECKS=0", false);
    ExpectExplainable("SHOW is NOT explainable", L"SHOW TABLES", false);
    ExpectExplainable("empty is NOT explainable", L"", false);
    ExpectExplainable("comment-only is NOT explainable", L"-- nothing here", false);

    ExpectExplain("MySQL: plain EXPLAIN", L"SELECT * FROM t", Dialect::MySQL,
                  { L"EXPLAIN SELECT * FROM t" });
    ExpectExplain("Postgres: VERBOSE+COSTS, planner only", L"SELECT * FROM t",
                  Dialect::Postgres, { L"EXPLAIN (VERBOSE, COSTS) SELECT * FROM t" });
    ExpectExplain("SQLite: QUERY PLAN (not the opcode dump)", L"SELECT * FROM t",
                  Dialect::Sqlite, { L"EXPLAIN QUERY PLAN SELECT * FROM t" });
    ExpectExplain("Oracle: two steps, plan read back by the second",
                  L"SELECT * FROM t", Dialect::Oracle,
                  { L"EXPLAIN PLAN FOR SELECT * FROM t",
                    L"SELECT PLAN_TABLE_OUTPUT FROM TABLE(DBMS_XPLAN.DISPLAY())" });
    ExpectExplain("SQL Server: no usable non-executing plan -> nothing",
                  L"SELECT * FROM t", Dialect::SqlServer, {});
    // THE ONE THAT MATTERS MOST: a destructive statement still gets a plan-only
    // form. If this ever emits ANALYZE, running the analysis deletes the rows.
    ExpectExplain("DELETE on PG is planner-only", L"DELETE FROM t WHERE id=1",
                  Dialect::Postgres, { L"EXPLAIN (VERBOSE, COSTS) DELETE FROM t WHERE id=1" });
    ExpectExplain("UPDATE on MySQL is planner-only", L"UPDATE t SET c=1",
                  Dialect::MySQL, { L"EXPLAIN UPDATE t SET c=1" });
    ExpectExplain("a trailing ';' is stripped", L"SELECT 1;", Dialect::MySQL,
                  { L"EXPLAIN SELECT 1" });
    ExpectExplain("surrounding whitespace is trimmed", L"\n  SELECT 1  \n", Dialect::MySQL,
                  { L"EXPLAIN SELECT 1" });
    ExpectExplain("un-explainable statements produce nothing, on every dialect",
                  L"CREATE TABLE t (id int)", Dialect::MySQL, {});

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
