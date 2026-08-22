// ObjectTemplates.h — CREATE-(OR REPLACE) skeletons and helpers for the
// view / function / procedure / package object editors. A "新建视图/函数/存储过程/包"
// menu opens a normal SQL editor tab (EditorPage) prefilled with one of these
// runnable templates; a "修改" opens it prefilled with the object's existing DDL
// re-wrapped into a replace-in-place form. Kept dialect-aware and idempotent
// (re-running replaces, never errors "already exists").
//
// Execution reuses MainFrame's F5/RUN path, which splits the buffer with
// db::SplitSqlScript. That splitter understands MySQL DELIMITER and PostgreSQL
// dollar-quoting but NOT Oracle/DM PL-SQL or SQL Server T-SQL blocks (their inner
// ';' would be mis-split), so ObjectCreateSupported() gates routine/package
// creation to the dialects that actually run correctly today.
#pragma once

#include <wx/string.h>
#include "db/DbDriver.h"   // db::Dialect

namespace ui {

enum class ObjectKind { View, Function, Procedure, Package };

// Can this (kind, dialect) be created/executed correctly through the shared SQL
// run path (db::SplitSqlScript) as it exists today? Views are single statements
// (safe everywhere); routines are safe only where the splitter handles their
// body wrapping (MySQL DELIMITER, PostgreSQL $$). Oracle/DM/SQL Server routines
// and all packages are gated off pending a block-aware splitter.
bool ObjectCreateSupported(ObjectKind kind, db::Dialect d);

// A runnable CREATE-(OR REPLACE) skeleton for (kind, dialect). Total over every
// (kind, dialect) so the code is complete even where the menu currently hides it.
wxString ObjectTemplate(ObjectKind kind, db::Dialect d);

// Default tab title for a fresh object editor ("新建视图" / "新建函数" / …).
wxString ObjectNewTitle(ObjectKind kind);

// Re-wrap an object's fetched DDL into a re-runnable replace-in-place form for
// the 修改 flow (CREATE OR REPLACE / CREATE OR ALTER / DROP-IF-EXISTS + CREATE).
wxString ObjectModifyDdl(ObjectKind kind, db::Dialect d,
                         const wxString& name, const wxString& fetchedDdl);

// Parse the object name from a CREATE (OR REPLACE/ALTER) VIEW/FUNCTION/PROCEDURE/
// PACKAGE [BODY] statement, so the tab can be renamed to it after a successful
// run. Returns an empty string when no such statement is found. Schema/owner
// qualifiers are stripped (returns the bare object name).
wxString ParseObjectName(const wxString& sql);

// Rewrite a fetched CREATE … {VIEW|FUNCTION|PROCEDURE} DDL so it (re)creates the
// object under a new name — the "rebuild" half of a rename on engines with no
// native ALTER … RENAME (MySQL, SQLite views). Precisely:
//   1. strips a MySQL DEFINER=`u`@`h` clause (privilege-safe re-run);
//   2. drops a leading "OR REPLACE"/"OR ALTER" so an existing new name ERRORS
//      instead of being silently clobbered (the rename must never overwrite);
//   3. replaces ONLY the object-name identifier immediately after the
//      VIEW/FUNCTION/PROCEDURE keyword — a same-named token inside the body is
//      left untouched.
// `newNameSql` is the already-quoted (and, for MySQL, db-qualified) new name to
// splice in. Returns false (leaving `out` untouched) when the object-type keyword
// or its following identifier can't be located, so the caller can abort safely.
bool BuildRenamedCreateDdl(ObjectKind kind, db::Dialect d,
                           const wxString& fetchedDdl,
                           const wxString& newNameSql, wxString& out);

} // namespace ui
