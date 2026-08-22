// QuerySource.h — deriving the BASE TABLE of an editor SELECT, honestly.
//
// ===========================================================================
// WHAT WENT WRONG
// ===========================================================================
// MainFrame_Query's DeriveEditable used to find the base table like this:
//
//     const int f = low.Find(L" from ");
//     ... copy chars until space/tab/;/( ...
//     if (table.Contains(L".")) table = table.AfterLast('.');   // drop schema/db
//     table.Replace(L"`", L""); table.Replace(L"\"", L"");
//
// Three separate ways that hands the write path the WRONG TABLE:
//
//   1. DELETING quote characters is not unquoting. A table genuinely named
//      ``we`ird`` is written ``` `we``ird` ``` in MySQL; the Replace turns that
//      into `weird`. If a table named `weird` also exists, the grid becomes
//      editable and every UPDATE/DELETE the user saves hits the WRONG TABLE,
//      silently. The write path's quoting was unified onto db::QuoteIdent, but
//      quoting a name that is already lossy cannot recover it.
//   2. `AfterLast('.')` THROWS AWAY the qualifier and the lookup then runs
//      against the session's current database. `SELECT * FROM otherdb.orders`
//      therefore binds to the CURRENT db's `orders` — again a different table.
//      On PostgreSQL this is not hypothetical: QualifiedName.h records that a
//      measured `archive.orders` / `public.orders` pair resolves to public's
//      columns, so the grid would be bound to public's table while showing
//      archive's rows.
//   3. `low.Find(" from ")` is a substring search over RAW text: a ` from ` in a
//      comment or inside a string literal is found just the same.
//
// ===========================================================================
// HOW THIS MODULE ANSWERS IT
// ===========================================================================
// Comments are removed by db::sync::NormalizeRoutineBody — the SAME
// dialect-parameterized quote-state machine the routine-body differ uses (MySQL
// `#` and `--<space>`, PostgreSQL `$$…$$`, backslash escapes, `/*! … */` as
// code). It copies quoted runs BYTE FOR BYTE, so an identifier survives intact
// with its doubling, and it reports Unresolved rather than guessing when the
// text ends mid-quote. That is exactly this problem, already solved and already
// tested; it is reused, not reimplemented.
//
// On the comment-free text a small tokenizer walks identifiers with the quoting
// rules that apply, UN-DOUBLING an embedded quote char instead of deleting it.
// ``` `we``ird` ``` comes back as ``we`ird``, which db::QuoteIdent then re-quotes
// correctly on the write path.
//
// ===========================================================================
// THE FAILURE MODE IS REFUSAL, NEVER A DIFFERENT TABLE
// ===========================================================================
// A tokenizer is not a SQL parser and this one does not pretend to be. Anything
// it cannot frame — an unterminated quote, a quoting form it does not model
// (SQL Server `[brackets]`), a three-part name, a subquery in the FROM — makes
// the result NON-EDITABLE with a stated reason, the same shape of gate the write
// path grew for an incomplete primary key (gridsql::PkGate::IncompleteKey).
// Read-only is always a safe answer. Editing the wrong table is not, and "we
// couldn't tell" must never round to "probably that one".
#pragma once

#include <wx/string.h>

#include "db/DbDriver.h"   // db::Dialect

namespace ui::qsrc {

enum class SourceStatus {
    Ok,             // exactly one base table was identified; `table` is RAW
    NotSimple,      // a join / union / aggregate / subquery / multi-table FROM,
                    // or not a SELECT at all — the ordinary non-editable case
    Unparsable,     // the scanner ended mid-quote; the text is not framable
    UnknownQuoting  // an identifier quoting form this scanner does not model
};

// The FROM target of a plain single-table SELECT.
//
// `qualifier` and `table` are RAW: unquoted, with any doubled quote char
// collapsed back to one. They are therefore in exactly the shape db::QuoteIdent
// expects and must NOT be pre-quoted by callers.
struct SelectSource {
    SourceStatus status = SourceStatus::NotSimple;
    wxString     qualifier;   // db/schema as WRITTEN; empty when none was
    wxString     table;
};

// Parse the base table out of one SELECT statement. Pure: no I/O, no globals.
SelectSource ParseSelectSource(const wxString& sql, db::Dialect d);

// Why an otherwise-parsed source still must not be bound for editing.
enum class BindRefusal {
    None,             // bind it
    NotSimpleSelect,  // ordinary; the user expects no message for this
    Unparsable,
    UnknownQuoting,
    ForeignQualifier  // an explicit db/schema this path cannot honour — see below
};

// THE QUALIFIER RULE. EditableSpec carries a bare table name, and the DML the
// grid emits is qualified only for MySQL, only from the BROWSE-mode qualifier
// (gridsql::QualifyForDml) — which the editor path leaves empty. So a statement
// naming some other database or schema would be READ from there and WRITTEN
// somewhere else. This refuses instead:
//
//   * no qualifier written        → bind (the overwhelmingly common case).
//   * MySQL, qualifier == the session's current database → bind; the two names
//     denote the same table, so dropping it changes nothing. Compared exactly:
//     MySQL database names are case-sensitive on Linux filesystems.
//   * PostgreSQL → refuse. A qualifier there is a SCHEMA, not a database, and
//     whether it is the one search_path resolves to is not knowable from here.
//     `打开表` reaches those tables with a catalog-derived identity.
//   * anything else → refuse.
//
// `currentDb` is the session's current database (may be empty).
BindRefusal ClassifyBind(const SelectSource& src, db::Dialect d,
                         const wxString& currentDb);

} // namespace ui::qsrc
