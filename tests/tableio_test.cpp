// tableio_test.cpp — unit tests for db::TableExport (the five P0 format writers)
// and db::TableImport (the four P0 parsers), plus export→import round-trips.
//
// Same dependency-free harness as sqlscript_test.cpp: a tiny assert loop, no
// doctest/Catch2. These are pure functions over columns + string rows, so exact
// byte/text comparison is the right granularity. The target links swiftsql::db
// because the SQL writer reuses db::RenderLiteral (SqlLiteral.cpp).
#include "db/TableExport.h"
#include "db/TableImport.h"
#include "db/DbDriver.h"   // db::Dialect

#include <cstdio>
#include <vector>
#include <wx/string.h>

using namespace db;

static int g_checks = 0;
static int g_fails  = 0;

static wxString Vis(const wxString& s)
{
    wxString o = s;
    o.Replace(L"\r", L"\\r");
    o.Replace(L"\n", L"\\n");
    o.Replace(L"\t", L"\\t");
    return o;
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)Vis(want).utf8_str(),
                    (const char*)Vis(got).utf8_str());
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

using Rows = std::vector<std::vector<wxString>>;

// ---- export helpers --------------------------------------------------------
static wxString Exp(ExportFormat fmt, const std::vector<wxString>& cols,
                    const Rows& rows, ExportOptions opt = {})
{
    opt.format = fmt;
    return ExportTableToString(cols, rows, opt);
}

// ---- import helper: collect rows + columns ---------------------------------
static ImportResult Imp(ImportFormat fmt, const wxString& content, Rows& outRows,
                        ImportOptions opt = {})
{
    opt.format = fmt;
    outRows.clear();
    return ImportTable(content, opt,
        [&](const std::vector<wxString>& r) { outRows.push_back(r); return true; },
        nullptr);
}

static bool RowsEqual(const Rows& a, const Rows& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) return false;
        for (size_t j = 0; j < a[i].size(); ++j) if (a[i][j] != b[i][j]) return false;
    }
    return true;
}

int main()
{
    std::printf("== TableExport / TableImport unit tests ==\n");

    const std::vector<wxString> cols = { L"a", L"b" };

    // ---------------------------------------------------------------- CSV export
    {
        Rows rows = {
            { L"1", L"hello" },
            { L"NULL", L"x,y" },                 // null marker + embedded comma
            { L"q\"q", L"line\nbreak" },         // embedded quote + newline
            { L"2", wxString::FromUTF8("caf\xC3\xA9") },   // café (unicode)
        };
        wxString got = Exp(ExportFormat::Csv, cols, rows);
        wxString want =
            L"a,b\r\n"
            L"1,hello\r\n"
            L"NULL,\"x,y\"\r\n"
            L"\"q\"\"q\",\"line\nbreak\"\r\n"
            + wxString(L"2,") + wxString::FromUTF8("caf\xC3\xA9") + L"\r\n";
        ExpectStr("csv: escaping + crlf + unicode", got, want);
    }
    {
        ExpectStr("csv: empty rows, header only",
                  Exp(ExportFormat::Csv, cols, {}), L"a,b\r\n");
        ExportOptions o; o.header = false;
        ExpectStr("csv: empty rows, no header → empty",
                  Exp(ExportFormat::Csv, cols, {}, o), L"");
    }
    {
        ExportOptions o; o.nullAsEmpty = true;
        Rows rows = { { L"NULL", L"z" } };   // NULL → empty field (leading comma)
        ExpectStr("csv: nullAsEmpty option",
                  Exp(ExportFormat::Csv, cols, rows, o), L"a,b\r\n,z\r\n");
    }

    // ---------------------------------------------------------------- TXT export
    {
        ExportOptions o; o.delimiter = L"\t";
        Rows rows = { { L"1", L"x\ty" } };   // tab inside value → quoted
        ExpectStr("txt: tab delimiter, quote on embedded tab",
                  Exp(ExportFormat::Txt, cols, rows, o),
                  L"a\tb\r\n1\t\"x\ty\"\r\n");
    }

    // ---------------------------------------------------------------- JSON export
    {
        Rows rows = { { L"1", L"hi" }, { L"NULL", L"x" } };
        ExpectStr("json: object array, null token",
                  Exp(ExportFormat::Json, cols, rows),
                  L"[{\"a\":\"1\",\"b\":\"hi\"},{\"a\":null,\"b\":\"x\"}]");
    }
    {
        Rows rows = { { L"1", L"he\"llo\nx\t!" } };
        ExpectStr("json: string escaping",
                  Exp(ExportFormat::Json, cols, rows),
                  L"[{\"a\":\"1\",\"b\":\"he\\\"llo\\nx\\t!\"}]");
    }
    ExpectStr("json: empty → []", Exp(ExportFormat::Json, cols, {}), L"[]");

    // ---------------------------------------------------------------- XML export
    {
        Rows rows = { { L"1", L"<x>&" }, { L"NULL", L"y" } };
        wxString want =
            L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n"
            L"  <row><field name=\"a\">1</field><field name=\"b\">&lt;x&gt;&amp;</field></row>\n"
            L"  <row><field name=\"a\" null=\"true\"></field><field name=\"b\">y</field></row>\n"
            L"</rows>\n";
        ExpectStr("xml: escaping + null attr", Exp(ExportFormat::Xml, cols, rows), want);
    }
    ExpectStr("xml: empty → well-formed root",
              Exp(ExportFormat::Xml, cols, {}),
              L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n</rows>\n");

    // ---------------------------------------------------------------- SQL export
    {
        ExportOptions o; o.dialect = Dialect::MySQL; o.tableName = L"t"; o.sqlBatchRows = 2;
        Rows rows = { { L"1", L"x'y" }, { L"NULL", L"z" }, { L"3", L"w" } };
        wxString want =
            L"INSERT INTO `t` (`a`, `b`) VALUES ('1', 'x''y'),(NULL, 'z');\n"
            L"INSERT INTO `t` (`a`, `b`) VALUES ('3', 'w');\n";
        ExpectStr("sql: batched INSERT, ident quote, literal escape",
                  Exp(ExportFormat::Sql, cols, rows, o), want);
    }
    {
        ExportOptions o; o.dialect = Dialect::Postgres; o.tableName = L"pub.t";
        Rows rows = { { L"1", L"a\\b" } };  // backslash: PG does NOT escape it
        ExpectStr("sql: postgres ident + no backslash escaping",
                  Exp(ExportFormat::Sql, cols, rows, o),
                  L"INSERT INTO \"pub.t\" (\"a\", \"b\") VALUES ('1', 'a\\b');\n");
    }

    // ---------------------------------------------------------------- Import: CSV parse
    {
        Rows out;
        ImportResult r = Imp(ImportFormat::Csv, L"a,b\r\n1,\"x,y\"\r\n2,\"q\"\"q\"\r\n", out);
        ExpectTrue("csv import: ok", r.ok);
        ExpectTrue("csv import: columns", r.columns.size() == 2 && r.columns[0] == L"a");
        ExpectTrue("csv import: 2 rows", out.size() == 2);
        ExpectTrue("csv import: embedded comma", out.size() == 2 && out[0][1] == L"x,y");
        ExpectTrue("csv import: doubled quote", out.size() == 2 && out[1][1] == L"q\"q");
    }
    {
        // malformed: extra column produces a locatable error, row still delivered
        Rows out; std::vector<ImportError> errs;
        ImportOptions o; o.format = ImportFormat::Csv;
        ImportResult r = ImportTable(L"a,b\n1,2,3\n", o,
            [&](const std::vector<wxString>& row) { out.push_back(row); return true; },
            [&](const ImportError& e) { errs.push_back(e); });
        ExpectTrue("csv import: mismatch error emitted", errs.size() == 1 && errs[0].lineNo == 2);
        ExpectTrue("csv import: mismatched row still delivered", out.size() == 1 && out[0].size() == 3);
    }
    {
        // unterminated quote to EOF → error, no crash
        Rows out; std::vector<ImportError> errs;
        ImportOptions o; o.format = ImportFormat::Csv;
        ImportTable(L"a,b\n1,\"oops", o,
            [&](const std::vector<wxString>& row) { out.push_back(row); return true; },
            [&](const ImportError& e) { errs.push_back(e); });
        ExpectTrue("csv import: unterminated quote error", errs.size() == 1);
    }

    // ---------------------------------------------------------------- Import: JSON parse
    {
        Rows out;
        ImportResult r = Imp(ImportFormat::Json,
            L"[{\"a\":\"1\",\"b\":null},{\"a\":\"2\",\"b\":\"hi\"}]", out);
        ExpectTrue("json import: ok", r.ok);
        ExpectTrue("json import: columns a,b", r.columns.size() == 2 && r.columns[1] == L"b");
        ExpectTrue("json import: null→NULL", out.size() == 2 && out[0][1] == L"NULL");
        ExpectTrue("json import: value", out.size() == 2 && out[1][1] == L"hi");
    }
    {
        Rows out;
        ImportResult r = Imp(ImportFormat::Json, L"not-an-array", out);
        ExpectTrue("json import: fatal on non-array", !r.ok);
    }
    {
        Rows out;
        ImportResult r = Imp(ImportFormat::Json, L"[]", out);
        ExpectTrue("json import: empty array", r.ok && r.rowCount == 0 && out.empty());
    }

    // ---------------------------------------------------------------- Import: XML parse
    {
        Rows out;
        wxString xml =
            L"<?xml version=\"1.0\"?>\n<rows>\n"
            L"  <row><field name=\"a\">1</field><field name=\"b\" null=\"true\"></field></row>\n"
            L"  <row><field name=\"a\">&lt;2&gt;</field><field name=\"b\">y</field></row>\n"
            L"</rows>\n";
        ImportResult r = Imp(ImportFormat::Xml, xml, out);
        ExpectTrue("xml import: ok", r.ok);
        ExpectTrue("xml import: columns", r.columns.size() == 2 && r.columns[0] == L"a");
        ExpectTrue("xml import: null attr → NULL", out.size() == 2 && out[0][1] == L"NULL");
        ExpectTrue("xml import: entity unescape", out.size() == 2 && out[1][0] == L"<2>");
    }

    // ---------------------------------------------------------------- Detect
    ExpectTrue("detect .csv",  DetectFormatByExtension(L"/x/y.csv")  == ImportFormat::Csv);
    ExpectTrue("detect .TXT",  DetectFormatByExtension(L"a.TXT")     == ImportFormat::Txt);
    ExpectTrue("detect .json", DetectFormatByExtension(L"a.b.json")  == ImportFormat::Json);
    ExpectTrue("detect .xml",  DetectFormatByExtension(L"a.xml")     == ImportFormat::Xml);
    ExpectTrue("detect .sql → Unknown (RunScriptDialog)",
               DetectFormatByExtension(L"dump.sql") == ImportFormat::Unknown);

    // ---------------------------------------------------------------- Round-trips
    {
        const std::vector<wxString> rc = { L"id", L"name", L"note" };
        Rows data = {
            { L"1", L"Alice", L"hello, world" },
            { L"2", L"Bob \"the\" builder", L"NULL" },   // literal NULL marker
            { L"NULL", wxString::FromUTF8("Zo\xC3\xA9"), L"line1\nline2" },
        };

        // CSV
        {
            wxString s = Exp(ExportFormat::Csv, rc, data);
            Rows out; ImportOptions o; o.format = ImportFormat::Csv; o.hasHeader = true;
            ImportResult r = Imp(ImportFormat::Csv, s, out, o);
            ExpectTrue("round-trip csv: columns", r.columns == rc);
            ExpectTrue("round-trip csv: rows", RowsEqual(out, data));
        }
        // TXT
        {
            ExportOptions eo; eo.format = ExportFormat::Txt; eo.delimiter = L"\t";
            wxString s = ExportTableToString(rc, data, eo);
            Rows out; ImportOptions o; o.format = ImportFormat::Txt; o.delimiter = L"\t"; o.hasHeader = true;
            ImportResult r = Imp(ImportFormat::Txt, s, out, o);
            ExpectTrue("round-trip txt: columns", r.columns == rc);
            ExpectTrue("round-trip txt: rows", RowsEqual(out, data));
        }
        // JSON
        {
            wxString s = Exp(ExportFormat::Json, rc, data);
            Rows out; ImportResult r = Imp(ImportFormat::Json, s, out);
            ExpectTrue("round-trip json: columns", r.columns == rc);
            ExpectTrue("round-trip json: rows", RowsEqual(out, data));
        }
        // XML
        {
            wxString s = Exp(ExportFormat::Xml, rc, data);
            Rows out; ImportResult r = Imp(ImportFormat::Xml, s, out);
            ExpectTrue("round-trip xml: columns", r.columns == rc);
            ExpectTrue("round-trip xml: rows", RowsEqual(out, data));
        }
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
