// SqlScript.h — split a multi-statement SQL script into individual statements,
// dialect-aware. Pure function, no I/O — so it is unit-testable in isolation.
//
// The hard part is not the semicolon; it's everything that can *contain* a
// semicolon without ending a statement: string literals, quoted identifiers,
// line/block comments, MySQL's `DELIMITER` reassignment, and PostgreSQL's
// `$$ … $$` dollar-quoted bodies. Each SQL dialect draws these boundaries
// differently, so the caller passes the connection's Dialect.
#pragma once

#include <wx/string.h>
#include <vector>
#include "db/DbDriver.h"   // db::Dialect

namespace db {

// Return each top-level statement, trimmed, with its terminator removed.
// Comment text is preserved inside a statement (so MySQL `/*! … */` executable
// comments and optimiser hints survive), but comment-only / whitespace-only
// chunks are dropped. A trailing statement without a terminator is included.
std::vector<wxString> SplitSqlScript(const wxString& script, Dialect dialect);

// Does this statement CHANGE THE SET OF TABLES (or one table's shape)?
//
// WHAT IT IS FOR. After a script runs in a plain SQL editor, the sidebar tree
// and the 表信息 list have to be reloaded or a table the user just created is
// simply not there — succeeded, nothing shown, no error, which is exactly how
// that gap is reported. Reloading after EVERY successful statement would re-query
// the catalog after each INSERT/UPDATE, so the decision needs this predicate.
//
// TRUE for: CREATE TABLE, DROP TABLE, ALTER TABLE, RENAME TABLE, TRUNCATE.
// FALSE for DML (INSERT/UPDATE/DELETE/SELECT) and for object DDL that is NOT a
// table — CREATE VIEW / FUNCTION / PROCEDURE / TRIGGER / INDEX / DATABASE — each
// of which has its own folder and its own refresh path; claiming them here would
// reload the table list for something that never appears in it.
//
// Deliberately keyword-shaped rather than a parser: it reads the leading words,
// tolerating the qualifiers each dialect allows there (OR REPLACE, TEMPORARY,
// IF NOT EXISTS, …). A false positive costs one extra catalog read; a false
// negative costs the user a manual refresh, so it errs toward saying yes.
bool StatementTouchesTables(const wxString& statement);

// True when ANY statement in the list touches tables. The form the caller wants
// after running a whole script.
bool ScriptTouchesTables(const std::vector<wxString>& statements);

// ---------------------------------------------------------------------------
// Execution plans, for the AI performance analysis
// ---------------------------------------------------------------------------
// These live here, next to StatementTouchesTables, because they need the same
// thing it does: read the leading keywords of a statement without being fooled
// by comments or quoting. One scanner, not two that drift.

// Can this statement have a plan explained at all? SELECT / WITH / INSERT /
// UPDATE / DELETE / REPLACE only. DDL, SET, SHOW and friends cannot — asking an
// engine to EXPLAIN them is an ERROR, not an empty plan, so the caller must not
// try. Also false for an empty or comment-only statement.
bool IsExplainable(const wxString& statement);

// The statement(s) that produce an execution plan for `statement`, or an EMPTY
// vector when this engine has no plan facility we can use safely.
//
// NOTHING HERE EXECUTES THE STATEMENT. That is the whole contract, and it is
// why none of these forms use ANALYZE:
//
//   PostgreSQL  EXPLAIN (VERBOSE, COSTS)   — planner only. `EXPLAIN ANALYZE`
//                                            would RUN it, which on an UPDATE
//                                            or DELETE is a catastrophe, so it
//                                            is deliberately not offered.
//   MySQL / OceanBase / MariaDB  EXPLAIN   — planner only for every DML form.
//   SQLite      EXPLAIN QUERY PLAN         — the readable one; bare EXPLAIN
//                                            dumps VDBE opcodes nobody wants.
//   Oracle / DM EXPLAIN PLAN FOR … then a  — genuinely two statements: the
//               DBMS_XPLAN.DISPLAY select    first stores the plan, the second
//                                            reads it back.
//   SQL Server  (empty)                    — its plan facility is a session
//                                            SET that changes what the next
//                                            batch RETURNS; that does not fit
//                                            "run these and read the rows", and
//                                            guessing would be worse than the
//                                            analysis simply having no plan.
//
// A returned vector's LAST statement is the one whose rows are the plan; any
// earlier ones are setup and return nothing.
std::vector<wxString> BuildExplainSql(const wxString& statement, Dialect dialect);

} // namespace db
