// TableExport.cpp — the five P0 format writers behind TableWriter.
//
// Each arm is pure text assembly over one row at a time; the class glues rows
// together (JSON commas, SQL VALUES batches) so no arm ever needs the whole
// table. Escaping rules are lifted to match the existing codebase byte-for-byte:
//   CSV/TXT — ResultGridPanelOps.cpp::ExportResultCsv (quote on delimiter/"/newline,
//             "" doubling).
//   SQL     — db::RenderLiteral (SqlLiteral.cpp), the same escaping DumpTableData
//             and the data-sync DML path use, so a SQL export round-trips.
#include "db/TableExport.h"

#include "db/SyncTypes.h"   // Cell / RenderLiteral

namespace db {

namespace {

const wxString kLf   = L"\n";
const wxString kCrlf = L"\r\n";

// ---- CSV / TXT -------------------------------------------------------------
// A field needs quoting when it contains the active delimiter, a double quote,
// or a CR/LF. Inside quotes, each '"' is doubled. (Superset of ExportResultCsv,
// which checked ',' / '"' / '\n'; we also guard the custom TXT delimiter and a
// bare '\r' so a field can never break the record framing.)
wxString CsvField(const wxString& v, const wxString& delim)
{
    bool needQuote = v.Contains(L"\"") || v.Contains(L"\n") || v.Contains(L"\r");
    if (!needQuote && !delim.IsEmpty() && v.Contains(delim)) needQuote = true;
    if (!needQuote) return v;
    wxString e = v;
    e.Replace(L"\"", L"\"\"");
    return L"\"" + e + L"\"";
}

// ---- JSON ------------------------------------------------------------------
// Escape per RFC 8259: the two mandatory escapes ('"', '\\'), the short forms
// for the common control chars, and \u00XX for any remaining C0 control.
wxString JsonString(const wxString& v)
{
    wxString out;
    out.reserve(v.length() + 2);
    out += L'"';
    for (wxString::const_iterator it = v.begin(); it != v.end(); ++it) {
        const wxUniChar ch = *it;
        const wxUniChar::value_type c = ch.GetValue();
        switch (c) {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\b': out += L"\\b";  break;
        case L'\f': out += L"\\f";  break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:
            if (c < 0x20) {
                out += wxString::Format(L"\\u%04x", static_cast<unsigned>(c));
            } else {
                out += ch;
            }
        }
    }
    out += L'"';
    return out;
}

// ---- XML -------------------------------------------------------------------
// Escape the five predefined entities and drop characters illegal in XML 1.0
// (the C0 controls except TAB/LF/CR), so the document always parses.
wxString XmlText(const wxString& v)
{
    wxString out;
    out.reserve(v.length());
    for (wxString::const_iterator it = v.begin(); it != v.end(); ++it) {
        const wxUniChar ch = *it;
        const wxUniChar::value_type c = ch.GetValue();
        switch (c) {
        case L'&':  out += L"&amp;";  break;
        case L'<':  out += L"&lt;";   break;
        case L'>':  out += L"&gt;";   break;
        case L'"':  out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default:
            if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) {
                // illegal in XML 1.0 — skip
            } else {
                out += ch;
            }
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
TableWriter::TableWriter(const ExportOptions& opt,
                         std::vector<wxString> columns,
                         ExportSink            sink)
    : opt_(opt), columns_(std::move(columns)), sink_(std::move(sink))
{
}

void TableWriter::EnsurePrologue()
{
    if (started_) return;
    started_ = true;
    const wxString& nl = opt_.crlf ? kCrlf : kLf;
    switch (opt_.format) {
    case ExportFormat::Csv:
    case ExportFormat::Txt: {
        if (opt_.header) {
            const wxString delim = (opt_.format == ExportFormat::Csv) ? wxString(L",")
                                                                      : opt_.delimiter;
            wxString line;
            for (size_t c = 0; c < columns_.size(); ++c) {
                if (c) line += delim;
                line += CsvField(columns_[c], delim);
            }
            line += nl;
            sink_(line);
        }
        break;
    }
    case ExportFormat::Json:
        sink_(L"[");
        break;
    case ExportFormat::Xml:
        sink_(L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n");
        break;
    case ExportFormat::Sql:
        break;   // nothing — each INSERT batch is self-contained
    case ExportFormat::Xlsx:
    case ExportFormat::Dbf:
        break;   // binary formats never reach TableWriter (see TableExportBin.h)
    }
}

void TableWriter::WriteRow(const std::vector<wxString>& row)
{
    EnsurePrologue();
    switch (opt_.format) {
    case ExportFormat::Csv:
    case ExportFormat::Txt: EmitRowCsv(row);  break;
    case ExportFormat::Json: EmitRowJson(row); break;
    case ExportFormat::Xml:  EmitRowXml(row);  break;
    case ExportFormat::Sql:  EmitRowSql(row);  break;
    case ExportFormat::Xlsx:
    case ExportFormat::Dbf:  break;   // unreachable — binary path bypasses TableWriter
    }
    ++rowCount_;
}

void TableWriter::WriteRows(const std::vector<std::vector<wxString>>& rows)
{
    for (const auto& r : rows) WriteRow(r);
}

void TableWriter::Finish()
{
    if (finished_) return;
    finished_ = true;
    EnsurePrologue();   // ensure an empty table still emits a valid document
    switch (opt_.format) {
    case ExportFormat::Csv:
    case ExportFormat::Txt:
        break;
    case ExportFormat::Json:
        sink_(L"]");
        break;
    case ExportFormat::Xml:
        sink_(L"</rows>\n");
        break;
    case ExportFormat::Sql:
        if (sqlBatchOpen_) { sink_(L";\n"); sqlBatchOpen_ = false; }
        break;
    case ExportFormat::Xlsx:
    case ExportFormat::Dbf:
        break;   // binary formats never reach TableWriter
    }
}

// ---- per-format row emit ---------------------------------------------------
void TableWriter::EmitRowCsv(const std::vector<wxString>& row)
{
    const wxString delim = (opt_.format == ExportFormat::Csv) ? wxString(L",")
                                                              : opt_.delimiter;
    const wxString& nl = opt_.crlf ? kCrlf : kLf;
    wxString line;
    for (size_t c = 0; c < row.size(); ++c) {
        if (c) line += delim;
        wxString v = row[c];
        if (v == opt_.nullText && opt_.nullAsEmpty) v.clear();
        line += CsvField(v, delim);
    }
    line += nl;
    sink_(line);
}

void TableWriter::EmitRowJson(const std::vector<wxString>& row)
{
    wxString obj;
    if (rowCount_) obj += L",";
    obj += L"{";
    const size_t n = row.size();
    for (size_t c = 0; c < n; ++c) {
        if (c) obj += L",";
        const wxString key = (c < columns_.size()) ? columns_[c]
                                                    : wxString::Format(L"col%u", static_cast<unsigned>(c));
        obj += JsonString(key);
        obj += L":";
        if (row[c] == opt_.nullText) obj += L"null";
        else                         obj += JsonString(row[c]);
    }
    obj += L"}";
    sink_(obj);
}

void TableWriter::EmitRowXml(const std::vector<wxString>& row)
{
    wxString out = L"  <row>";
    const size_t n = row.size();
    for (size_t c = 0; c < n; ++c) {
        const wxString name = (c < columns_.size()) ? columns_[c]
                                                     : wxString::Format(L"col%u", static_cast<unsigned>(c));
        out += L"<field name=\"" + XmlText(name) + L"\"";
        if (row[c] == opt_.nullText) {
            out += L" null=\"true\"></field>";   // distinguishable from empty ""
        } else {
            out += L">" + XmlText(row[c]) + L"</field>";
        }
    }
    out += L"</row>\n";
    sink_(out);
}

void TableWriter::EmitRowSql(const std::vector<wxString>& row)
{
    // Open a new INSERT ... VALUES batch when none is running.
    if (!sqlBatchOpen_) {
        wxString head = L"INSERT INTO " + QuoteIdent(opt_.tableName, opt_.dialect)
                      + L" (";
        for (size_t c = 0; c < columns_.size(); ++c) {
            if (c) head += L", ";
            head += QuoteIdent(columns_[c], opt_.dialect);
        }
        head += L") VALUES ";
        sink_(head);
        sqlBatchOpen_ = true;
        sqlBatchRows_ = 0;
    }
    if (sqlBatchRows_) sink_(L",");

    wxString tuple = L"(";
    for (size_t c = 0; c < row.size(); ++c) {
        if (c) tuple += L", ";
        Cell cell;
        if (row[c] == opt_.nullText) cell.kind = CellKind::Null;
        else { cell.kind = CellKind::Text; cell.text = row[c]; }
        tuple += RenderLiteral(cell, opt_.dialect);
    }
    tuple += L")";
    sink_(tuple);

    if (++sqlBatchRows_ >= (opt_.sqlBatchRows > 0 ? opt_.sqlBatchRows : 1)) {
        sink_(L";\n");
        sqlBatchOpen_ = false;
        sqlBatchRows_ = 0;
    }
}

// ---------------------------------------------------------------------------
wxString ExportTableToString(const std::vector<wxString>&              columns,
                             const std::vector<std::vector<wxString>>& rows,
                             const ExportOptions&                      opt)
{
    wxString buf;
    TableWriter w(opt, columns, [&buf](const wxString& s) { buf += s; });
    w.WriteRows(rows);
    w.Finish();
    return buf;
}

} // namespace db
