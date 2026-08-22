// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableImport.h — pure parsing of the P0 text formats (CSV / TXT / JSON / XML)
// back into columns + string rows. This is the inverse of TableExport and the
// "read format" half of the import/export split: it turns file *content* into
// rows via a per-row callback; it does NOT touch files, connections, or SQL.
//
// .sql is intentionally NOT handled here — a SQL dump is a script, so it goes to
// the existing RunScriptDialog path, not this parser.
//
// Robustness (P0 red line): malformed input (missing/extra columns, a broken
// quote, a half record) must yield a locatable per-row error and keep going, not
// crash. Fatal, unparseable structure (e.g. a JSON document that isn't an array)
// stops the parse with ok=false rather than throwing.
//
// The JSON/XML parsers are deliberately *targeted* at the shapes TableExport
// emits (an array of flat objects; <rows><row><field name=…>…). They tolerate
// real-world variants but are not general-purpose JSON/XML engines.
#pragma once

#include <wx/string.h>
#include <functional>
#include <vector>

namespace db {

// Formats this layer can parse. Unknown → caller should reject / route elsewhere.
enum class ImportFormat { Csv, Txt, Json, Xml, Unknown };

// Pick a format from a file extension (.csv/.txt/.json/.xml). ".sql" → Unknown
// here on purpose (that path belongs to RunScriptDialog).
ImportFormat DetectFormatByExtension(const wxString& path);

struct ImportOptions {
    ImportFormat format    = ImportFormat::Csv;
    wxString     delimiter = L"\t";   // TXT field separator (CSV forces ',')
    bool         hasHeader = true;    // CSV/TXT: first record is the column-name row
    wxString     nullText  = L"NULL"; // JSON null / XML null="true" map back to this
};

// One per-row parse problem: locatable (1-based source line where the record
// started, best effort), the offending raw fragment, and a human reason.
struct ImportError {
    long long lineNo = 0;
    wxString  raw;
    wxString  message;
};

// Called once per parsed data row (fields in column order). Return false to stop.
using ImportRowSink   = std::function<bool(const std::vector<wxString>&)>;
// Called once per recoverable per-row error.
using ImportErrorSink = std::function<void(const ImportError&)>;

struct ImportResult {
    std::vector<wxString> columns;      // detected / declared column names
    long long             rowCount   = 0;
    long long             errorCount = 0;
    bool                  ok         = true;   // false → fatal, structure unparseable
    wxString              fatal;              // reason when ok == false
};

// Parse `content` per `opt`, delivering rows to `rowSink` and recoverable errors
// to `errorSink` (either sink may be empty). Pure — no I/O. Detected column names
// land in the returned ImportResult.columns.
ImportResult ImportTable(const wxString&        content,
                         const ImportOptions&   opt,
                         const ImportRowSink&   rowSink,
                         const ImportErrorSink& errorSink);

} // namespace db
