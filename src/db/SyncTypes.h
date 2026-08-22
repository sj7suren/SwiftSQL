// SyncTypes.h — structured row streaming + literal rendering for data sync.
//
// The stringified QueryResult path can't drive a row-level data diff (it loses
// NULL vs "NULL", numeric vs quoted, binary framing). StreamRows yields typed
// Cells instead; RenderLiteral turns one Cell back into dialect SQL. Pure data
// + one free function — no IConnection dependency (DbDriver.h includes this).
#pragma once

#include <wx/string.h>
#include <functional>
#include <vector>

namespace db {

enum class Dialect;   // defined in DbDriver.h; only used by-value in RenderLiteral

// One typed cell of a streamed row. For Binary, `text` holds uppercase hex
// digits WITHOUT a 0x/\x prefix — RenderLiteral adds the dialect framing.
enum class CellKind { Null, Numeric, Text, Binary };
struct Cell {
    CellKind kind = CellKind::Null;
    wxString text;
};

// Options for a keyed, chunked row stream. orderBy is normally the primary key
// (required for the sorted-merge data diff). keyLowExcl drives chunk paging
// ("" = from the start); limit caps rows per chunk (-1 = stream the whole table).
//
// binaryOrder is the ordering contract the sorted-merge data diff depends on
// (ADR-015 T2). The merge walks two servers' streams in lock-step and compares
// keys itself, with CmpCell's code-point order; that is only sound if BOTH
// servers sorted by the same total order — and by that same one. Left to their
// own collations they do not: MySQL's stock utf8mb4_general_ci is
// case-insensitive and accent-insensitive, PostgreSQL's default collation is
// neither, so a VARCHAR key comes back in two different orders and the merge
// silently desyncs (spurious INSERTs and DELETEs, no error). Setting
// binaryOrder makes the driver emit an ORDER BY that forces *byte* ordering of
// the UTF-8 encoding — MySQL `CONVERT(col USING utf8mb4) COLLATE utf8mb4_bin`,
// PostgreSQL `col COLLATE "C"` — which is identical across engines and equals
// code-point order, i.e. exactly what the merge assumes.
//
// COLLATE is only legal on string expressions (`ORDER BY int_col COLLATE "C"`
// is an error on both engines), so the caller names the subset of orderBy
// columns the clause applies to in binaryOrderCols; every other order column is
// ordered natively, which is already collation-independent for
// integer/binary/uuid/date keys. binaryOrder without binaryOrderCols therefore
// means "byte order requested, no column needs an explicit collation" — a
// perfectly normal (and by far the most common) integer-PK stream.
struct StreamOptions {
    std::vector<wxString> columns;    // projection order ("" list = all columns, schema order)
    std::vector<wxString> orderBy;    // usually the PK
    wxString              keyLowExcl; // cursor for chunked paging ("" = start)
    long long             limit = -1; // rows per chunk (-1 = unbounded)
    bool                  binaryOrder = false;      // force byte (== code-point) ordering
    std::vector<wxString> binaryOrderCols;          // subset of orderBy needing the COLLATE
};

// Row sink: return false to stop the stream early (cancellation).
using RowSink = std::function<bool(const std::vector<Cell>&)>;

// Render one cell as a dialect SQL literal: NULL / bare numeric / quoted+escaped
// text / hex-framed binary. Extracted from MySqlDriver::DumpTableData so the
// escaping rules live in one testable place.
wxString RenderLiteral(const Cell& c, Dialect d);

} // namespace db
