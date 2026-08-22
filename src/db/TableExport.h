// TableExport.h — pure, streaming serialization of tabular data into the P0 text
// formats (CSV / TXT / JSON / XML / SQL-INSERT). This is the "write format" half
// of the import/export split: a worker pulls rows from a driver (the "take data"
// half) and feeds them here in chunks; this layer knows nothing about
// IConnection, the UI, or where the bytes go.
//
// Streaming contract (P0 red line): the whole table is NEVER built into one giant
// wxString — a large table would OOM. Instead the caller drives a TableWriter,
// pushing rows chunk-by-chunk to a Sink (a byte/text callback). The writer holds
// just enough state to emit the format prologue once, glue rows together
// correctly (JSON commas, SQL VALUES batching), and close the document on
// Finish().
//
// Layer: db only. Depends on wxString + db::Dialect/QuoteIdent (DbDriver.h) +
// RenderLiteral (SyncTypes.h). No ui/, no IConnection. P1 (XLSX) / P2 (DBF) are
// out of scope this round but slot in as new ExportFormat values + a writer arm.
#pragma once

#include <wx/string.h>
#include <functional>
#include <vector>
#include "db/DbDriver.h"    // db::Dialect + QuoteIdent (SQL arm)

namespace db {

// The text formats plus the two binary formats. Xlsx / Dbf are NOT served by
// TableWriter (which streams text through an ExportSink): binary bytes must not
// pass through a wxString sink. They live here only so the UI can select them;
// the export worker branches to the binary writers in TableExportBin.h instead.
enum class ExportFormat { Csv, Txt, Json, Xml, Sql, Xlsx, Dbf };

// Where serialized text goes. Called incrementally (once per prologue / row /
// batch / epilogue) — the callee streams it to a file/socket without buffering.
using ExportSink = std::function<void(const wxString&)>;

// Knobs shared by the format arms. Cells arrive already stringified per the
// driver contract (NULL → the `nullText` marker, default "NULL"); the writer
// re-detects NULL by matching that marker so it can emit real null tokens
// (JSON null, SQL NULL, XML null="true").
struct ExportOptions {
    ExportFormat format  = ExportFormat::Csv;
    Dialect      dialect = Dialect::MySQL;   // SQL arm: literal escaping + ident quoting

    // CSV / TXT
    bool     header       = true;    // emit a leading column-name row
    wxString delimiter    = L"\t";   // TXT field separator (CSV always uses ',')
    bool     nullAsEmpty  = false;   // NULL → empty field (else the literal marker text)
    bool     crlf         = true;    // row terminator "\r\n" (false → "\n")

    // NULL detection (all formats)
    wxString nullText     = L"NULL"; // cell text equal to this is treated as NULL

    // SQL (INSERT)
    wxString tableName    = L"table_name"; // target table for INSERT statements
    int      sqlBatchRows = 100;     // rows per multi-row VALUES (...),(...) batch
};

// Incremental writer. Lifecycle: construct (captures columns + sink), push rows
// via WriteRow/WriteRows any number of times, then Finish() exactly once to close
// the document. The prologue (CSV header / JSON '[' / XML root) is emitted lazily
// on the first write or on Finish(), so an empty table still yields a well-formed
// document (e.g. "[]", "<rows></rows>", or just the CSV header line).
class TableWriter {
public:
    TableWriter(const ExportOptions& opt,
                std::vector<wxString> columns,
                ExportSink            sink);

    void WriteRow(const std::vector<wxString>& row);
    void WriteRows(const std::vector<std::vector<wxString>>& rows);
    void Finish();

private:
    void EnsurePrologue();          // emit header/'['/root once
    void EmitRowCsv(const std::vector<wxString>& row);
    void EmitRowJson(const std::vector<wxString>& row);
    void EmitRowXml(const std::vector<wxString>& row);
    void EmitRowSql(const std::vector<wxString>& row);

    ExportOptions         opt_;
    std::vector<wxString> columns_;
    ExportSink            sink_;

    bool      started_  = false;   // prologue emitted
    bool      finished_ = false;
    long long rowCount_ = 0;       // JSON comma placement

    // SQL batching state
    bool sqlBatchOpen_ = false;    // an INSERT ... VALUES is currently open
    int  sqlBatchRows_ = 0;        // rows in the open batch
};

// Convenience buffer-everything helper — for SMALL results and unit tests only.
// Real export paths use TableWriter with a streaming sink; this defeats the
// flat-memory guarantee and must not be used on unbounded tables.
wxString ExportTableToString(const std::vector<wxString>&              columns,
                             const std::vector<std::vector<wxString>>& rows,
                             const ExportOptions&                      opt);

} // namespace db
