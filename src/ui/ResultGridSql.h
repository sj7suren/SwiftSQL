// ResultGridSql.h — the PURE SQL-text generator behind the result grid.
//
// Every statement the data browser can emit is built here, as free functions
// over plain values: the edit→DML diff that 保存 applies, the 复制为 SQL
// clipboard statements, the pager's paged SELECT, and the cell-value filter
// predicate. No wxWindow, no wxGrid — ResultGridPanel reads its cells out of the
// widget and passes them in, so all of this is exercisable headlessly. See
// tests/ResultGridSqlTests.cpp.
//
// ONE QUOTING REGIME: db::QuoteIdent, everywhere.
// This module used to carry two. A local QuoteVerbatim() wrapped identifiers in
// ` ` / " " WITHOUT doubling an embedded quote char (used by the DML diff, the
// 复制为 clipboard statements and the cell filter), while the pager path used
// db::QuoteIdent(), which doubles correctly. The same identifier therefore came
// out malformed — and in principle injectable — on one set of paths and correct
// on the other. QuoteVerbatim is gone; db::QuoteIdent is the only quoter here.
//
// Every identifier reaching this module is a RAW, unquoted, unqualified name:
//   ctx.table      ← EditableSpec::table, produced by ui::qsrc (QuerySource.h),
//                    which UN-DOUBLES an embedded quote char rather than
//                    deleting it, and REFUSES (editable=false + a stated
//                    notEditableReason) when the statement is unparsable, names
//                    a different database, or is not a simple single-table
//                    SELECT. So a raw name reaching here is faithful, not the
//                    lossily-stripped one an earlier DeriveEditable produced.
//   ctx.db         ← the browse qualifier from the object tree (catalog name).
//   ctx.columns    ← result-set metadata (driver column names).
//   ctx.pkColumns  ← conn->GetColumns() (catalog column names).
// Nothing arrives pre-quoted, so quoting once here is correct and does not
// double-quote an already-quoted name. `db`.`table` is composed by quoting the
// two parts SEPARATELY and joining with '.', never by quoting "db.table".
#pragma once

#include <wx/string.h>
#include <vector>

#include "db/DbDriver.h"        // db::Dialect, db::QuoteIdent, db::LimitOffsetClause
#include "ui/ResultGridModel.h" // ui::SortKey

namespace ui::gridsql {

using CellRow  = gridmodel::CellRow;
using CellGrid = gridmodel::CellGrid;

// Identity of the table an editable result is bound to, plus its column layout.
// `db` is the browse-mode qualifier: MySQL needs `db`.`table` so DML works when
// the connection has no default schema selected ("No database selected").
struct DmlContext {
    wxString              table;
    wxString              db;
    db::Dialect           dialect = db::Dialect::MySQL;
    std::vector<wxString> columns;
    std::vector<wxString> pkColumns;
};

// ---------------------------------------------------------------------------
// Primitives. Identifier quoting is db::QuoteIdent (see the note above).
// ---------------------------------------------------------------------------

// Value literal: the driver's "NULL" marker passes through unquoted; anything
// else is single-quoted with `'` doubled.
wxString RenderValue(const wxString& v);

// `db`.`table` (MySQL with a browse qualifier) or `table` — each part quoted
// separately through db::QuoteIdent.
wxString QualifyForDml(const DmlContext& ctx);

// Result-column indices of the primary-key columns, in pkColumns order. A PK
// column absent from the result yields a SHORTER vector than pkColumns — callers
// must check, since a partial key would produce an under-constrained WHERE.
std::vector<int> MapPkIndices(const DmlContext& ctx);

// The declared PK columns that are NOT present in the result, in declaration
// order. Empty ⇔ the whole key was located. This is what a refusal message
// names, so the user knows which columns to add to their query.
std::vector<wxString> MissingPkColumns(const DmlContext& ctx);

// `pk1='a' AND pk2='b'` built from `row` at the given PK column indices.
// Returns "" if pkIdx does not cover EVERY pk column: a partial key must fail
// loudly (a WHERE-less statement is a syntax error the server rejects) rather
// than quietly widen to match rows the user never touched.
wxString BuildPkWhere(const DmlContext& ctx, const std::vector<int>& pkIdx,
                      const CellRow& row);

// ---------------------------------------------------------------------------
// The edit → DML diff (保存).
// ---------------------------------------------------------------------------

// Why the edit path can refuse to build anything. The WHERE of every UPDATE and
// DELETE it emits is the primary key, so an incomplete key is not a cosmetic
// problem: it silently widens the statement to every row sharing the partial
// key. All-or-nothing is the only safe rule.
enum class PkGate {
    Ok,             // every declared PK column was located in the result
    NoPrimaryKey,   // the bound table declares no PK at all
    IncompleteKey   // some declared PK columns are missing from the result
};

// Outcome of the edit diff. `sql` is EMPTY both when nothing changed and when
// the gate refused, so it cannot be used to answer "are there pending edits?" —
// that is what `dirty` is for. Answering it with `sql` is precisely how a
// refusal turns into silent data loss: the 保存 button greys out, the
// unsaved-changes guard stays quiet, and a page turn throws the edits away.
struct EditDml {
    wxString              sql;                  // "" when clean OR when refused
    bool                  dirty = false;        // the grid differs from the result
    PkGate                gate  = PkGate::Ok;
    std::vector<wxString> missingPk;            // set when gate == IncompleteKey
};

// True when the grid differs from the original result at all. Deliberately
// independent of the PK — it must stay answerable when the gate refuses.
bool HasPendingEdits(const DmlContext& ctx,
                     const CellGrid& origRows,
                     const std::vector<int>& rowOrig,
                     const CellGrid& deleted,
                     const CellGrid& current);

// Diff the CURRENT grid contents against the original result rows.
//   `current`  — the grid as it stands now, one entry per visible row.
//   `rowOrig`  — per grid row, its index into `origRows` (-1 = a new row).
//   `deleted`  — original rows the user removed.
// Emits DELETEs first, then per grid row an INSERT (non-empty cells only, so
// defaults / auto-increment still apply) or an UPDATE of just the changed
// columns.
//
// THE GATE: statements are emitted only when EVERY declared PK column was found
// in the result — the same all-or-nothing rule the 复制为 UPDATE/DELETE path
// below has always applied. It used to be the weaker `pkIdx is non-empty` here,
// which let a COMPOSITE key with only some columns present emit an UPDATE keyed
// on the found column(s) alone, rewriting rows the user never edited. When the
// gate refuses, `gate`/`missingPk` say why so the caller can TELL THE USER; a
// refusal must not read to them as "saved" or as "nothing to save".
EditDml BuildEditDml(const DmlContext& ctx,
                     const CellGrid& origRows,
                     const std::vector<int>& rowOrig,
                     const CellGrid& deleted,
                     const CellGrid& current);

// ---------------------------------------------------------------------------
// Scripted edit (the SWIFTSQL_AUTOEDIT verification hook).
// ---------------------------------------------------------------------------

// Why a scripted edit produced no statements. The hook is machine-read (a live
// verification run diffs its output file), so "no statements" MUST NOT collapse
// into one error-shaped answer: "the value was already what you asked for",
// "the result is not bound to a table so the write was REFUSED", and "the PK
// gate refused" are three different findings, and a run that cannot tell them
// apart is not evidence of anything. One status per distinguishable outcome.
enum class ScriptedEditStatus {
    Ok,             // a cell was written and the diff produced statements
    NotEditable,    // EditableSpec::editable is false — nothing was written
    NoSuchColumn,   // the named column is not in the result
    NoRows,         // the result has no row 0 to edit
    NoChange,       // the cell already held that value
    PkRefused       // BuildEditDml's PkGate refused (no PK / incomplete key)
};

// What ResultGridPanel::ScriptedEdit hands back. `sql` is non-empty ONLY for
// Ok — every other status means nothing was generated, and `detail` says which
// thing was missing (the refusal reason, the column name, the missing PK
// columns). Never infer the outcome from `sql`.
struct ScriptedEditResult {
    ScriptedEditStatus status = ScriptedEditStatus::Ok;
    wxString           sql;
    wxString           detail;
};

// The hook's one-line, terse, machine-readable verdict:
//   OK | UNCHANGED | NO_ROWS | NO_COLUMN:<col>
//   REFUSED:NOT_EDITABLE[:<reason>] | REFUSED:NO_PRIMARY_KEY
//   REFUSED:INCOMPLETE_KEY:<missing,cols>
// The caller may replace OK with its own execution verdict (EXEC_ERR:<err>);
// everything else is decided here, without a server in the loop.
wxString ScriptedEditToken(const ScriptedEditResult& r);

// ---------------------------------------------------------------------------
// 复制为 SQL — clipboard statements for a set of picked rows.
// ---------------------------------------------------------------------------

// `INSERT INTO t (every column) VALUES (...);` per row. With no bound table the
// placeholder `table_name` is used, so the snippet is still paste-able. Returns
// "" when the result has no columns.
wxString BuildInsertStatements(const DmlContext& ctx, const CellGrid& rows);

// `UPDATE t SET <every non-PK column> WHERE <pk>;` (asUpdate) or
// `DELETE FROM t WHERE <pk>;` per row. Returns "" unless EVERY pk column is
// present in the result — the stricter gate the copy path has always applied.
wxString BuildRowDmlStatements(const DmlContext& ctx, const CellGrid& rows,
                               bool asUpdate);

// ---------------------------------------------------------------------------
// Pager / filter (db::QuoteIdent regime).
// ---------------------------------------------------------------------------

// `db`.`table` for MySQL browse mode, else `table` — properly escaped.
wxString QualifyForPaging(db::Dialect d, const wxString& db, const wxString& table);

// ORDER BY body (no keyword) in key-priority order; out-of-range keys skipped.
wxString BuildOrderByBody(const std::vector<wxString>& columns,
                          const std::vector<SortKey>& keys, db::Dialect d);

// The pager's full statement: SELECT * FROM t [WHERE ..] [ORDER BY ..] LIMIT/OFFSET;
wxString BuildPageSelect(db::Dialect d, const wxString& db, const wxString& table,
                         const wxString& where, const std::vector<wxString>& columns,
                         const std::vector<SortKey>& keys, int pageSize, int page);

// 按此格值筛选 / 排除此格值 → a one-condition WHERE body. The NULL marker maps to
// IS [NOT] NULL rather than an = comparison that would never match.
wxString BuildCellFilterClause(db::Dialect d, const wxString& column,
                               const wxString& value, bool exclude);

} // namespace ui::gridsql
