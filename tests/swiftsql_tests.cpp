// swiftsql_tests.cpp — unit tests for the BINARY export writers
// (db::XlsxWriter / db::DbfWriter, src/db/TableExportBin.cpp) plus a compact set
// of text export→import round-trips (CSV / JSON / XML).
//
// The binary writers are the coverage gap: sqlscript_test pins the SQL splitter
// and tableio_test pins the five text writers + four text parsers, but nothing
// exercises the OOXML/ZIP (xlsx) or dBASE-III (dbf) byte layout. Those formats
// have exact on-disk contracts (ZIP part set + XML escaping; dbf header fields +
// fixed-width GBK records), so this target writes real bytes into in-memory /
// temp-file streams and reads them back to assert the layout.
//
// Harness: the same dependency-free assert loop the other two test binaries use
// (no doctest/Catch2 vendored). These are pure serializers over columns + string
// rows; exact byte comparison is the right granularity. Links swiftsql::db, which
// supplies the writers and pulls wxbase (wxString / wxZip* / wxCSConv) — no wxApp
// or driver/connection is instantiated, so it runs headless in CI.
#include "db/TableExportBin.h"
#include "db/TableExport.h"
#include "db/TableImport.h"

#include <wx/string.h>
#include <wx/mstream.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <wx/filename.h>
#include <wx/filefn.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace db;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

// ---- byte helpers ----------------------------------------------------------
static bool Has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}
static unsigned U8(const std::string& s, size_t i)
{
    return static_cast<unsigned char>(s[i]);
}
static unsigned Le16(const std::string& s, size_t i)
{
    return U8(s, i) | (U8(s, i + 1) << 8);
}
static unsigned long Le32(const std::string& s, size_t i)
{
    return static_cast<unsigned long>(U8(s, i)) | (U8(s, i + 1) << 8)
         | (static_cast<unsigned long>(U8(s, i + 2)) << 16)
         | (static_cast<unsigned long>(U8(s, i + 3)) << 24);
}

// Read every byte of an input stream into a std::string (raw bytes, no wxString
// round-trip so binary/UTF-8 content is preserved exactly).
static std::string ReadAll(wxInputStream& in)
{
    std::string out;
    char buf[8192];
    for (;;) {
        in.Read(buf, sizeof buf);
        const size_t n = in.LastRead();
        if (n == 0) break;
        out.append(buf, n);
    }
    return out;
}

// Chinese "数据库" as explicit UTF-8 bytes → independent of source file encoding.
static wxString CJK() { return wxString::FromUTF8("\xE6\x95\xB0\xE6\x8D\xAE\xE5\xBA\x93"); }

using Rows = std::vector<std::vector<wxString>>;

// ===========================================================================
// XlsxWriter — OOXML/ZIP package layout, XML escaping, UTF-8, NULL → empty cell
// ===========================================================================
static void TestXlsx()
{
    std::printf("-- XlsxWriter --\n");

    const std::vector<wxString> cols = { L"id", CJK(), L"note" };
    Rows rows = {
        { L"1", CJK(),        L"NULL"    },   // C2 == nullText → empty (absent) cell
        { L"2", L"<&>'\"",    L"tail"    },   // special chars → XML-escaped
    };

    // Write the package into memory.
    wxMemoryOutputStream mos;
    {
        XlsxWriter w(cols, mos, /*header=*/true, /*nullText=*/L"NULL");
        w.WriteRows(rows);
        w.Finish();
        ExpectTrue("xlsx: writer Ok() after Finish", w.Ok());
    }

    // Snapshot the bytes, then re-open as a ZIP.
    const size_t sz = mos.GetSize();
    std::vector<char> data(sz);
    mos.CopyTo(data.data(), sz);
    ExpectTrue("xlsx: non-empty package", sz > 0);

    std::map<wxString, std::string> parts;
    {
        wxMemoryInputStream min(data.data(), sz);
        wxZipInputStream zin(min);
        for (;;) {
            std::unique_ptr<wxZipEntry> e(zin.GetNextEntry());
            if (!e) break;
            // GetName() defaults to wxPATH_NATIVE, which rewrites the ZIP's
            // internal '/' separators to '\' on Windows. Ask for wxPATH_UNIX so
            // we compare against the canonical OOXML part names ('/'-separated).
            parts[e->GetName(wxPATH_UNIX)] = ReadAll(zin);
        }
    }

    // 1) The four mandatory OOXML parts (plus workbook rels) are present.
    ExpectTrue("xlsx: [Content_Types].xml present", parts.count(L"[Content_Types].xml") == 1);
    ExpectTrue("xlsx: _rels/.rels present",         parts.count(L"_rels/.rels") == 1);
    ExpectTrue("xlsx: xl/workbook.xml present",      parts.count(L"xl/workbook.xml") == 1);
    ExpectTrue("xlsx: xl/worksheets/sheet1.xml present",
               parts.count(L"xl/worksheets/sheet1.xml") == 1);

    const std::string sheet = parts.count(L"xl/worksheets/sheet1.xml")
                            ? parts[L"xl/worksheets/sheet1.xml"] : std::string();

    // 2) Header labels + data values are in the sheet.
    const std::string cjkU8(CJK().utf8_str().data(), CJK().utf8_str().length());
    ExpectTrue("xlsx: header 'id' present",   Has(sheet, ">id<") || Has(sheet, ">id</t>"));
    ExpectTrue("xlsx: header 'note' present", Has(sheet, "note"));
    ExpectTrue("xlsx: data '1' present",      Has(sheet, ">1<"));
    ExpectTrue("xlsx: data 'tail' present",   Has(sheet, "tail"));

    // 3) Special chars are XML-escaped ( &  <  >  → entities; the writer does not
    //    escape ' or " inside a text node, which is legal).
    ExpectTrue("xlsx: '<&>' escaped to &lt;&amp;&gt;", Has(sheet, "&lt;&amp;&gt;"));

    // 4) Chinese is stored as UTF-8 bytes (and not mojibake / not GBK).
    ExpectTrue("xlsx: Chinese present as UTF-8", Has(sheet, cjkU8));

    // 5) The NULL cell (C2) is emitted as an absent cell, and the marker text
    //    "NULL" never appears as cell content anywhere.
    ExpectTrue("xlsx: NULL cell C2 absent", !Has(sheet, "r=\"C2\""));
    ExpectTrue("xlsx: nullText not emitted as data", !Has(sheet, ">NULL<"));

    // Sanity: header occupies row 1, so data rows are r=2 / r=3.
    ExpectTrue("xlsx: header row r=1 present", Has(sheet, "<row r=\"1\">"));
    ExpectTrue("xlsx: data row r=3 present",   Has(sheet, "<row r=\"3\">"));
}

// ===========================================================================
// DbfWriter — dBASE III header fields, ≤10-byte field names, fixed-width GBK
// records, 0x20 leading flag, 0x1A EOF, CP936 language driver.
// ===========================================================================
static void TestDbf()
{
    std::printf("-- DbfWriter --\n");

    const std::vector<wxString> cols = { L"id", CJK(), L"note" };
    Rows rows = {
        { L"1", CJK(),   L"NULL" },   // NULL → blanks
        { L"2", L"abc",  L"xyz"  },
    };
    const size_t nFields = cols.size();
    const size_t nRows   = rows.size();

    // dbf needs a seekable stream (Finish() patches the record count via SeekO),
    // so write to a temp file, then read the raw bytes back.
    const wxString path = wxFileName::CreateTempFileName(L"swiftsql_dbf");
    {
        wxFileOutputStream fos(path);
        {
            DbfWriter w(cols, fos, /*nullText=*/L"NULL");
            w.WriteRows(rows);
            w.Finish();
            ExpectTrue("dbf: writer Ok() after Finish", w.Ok());
        }
    }

    std::string d;
    {
        wxFileInputStream fin(path);
        ExpectTrue("dbf: temp file readable", fin.IsOk());
        d = ReadAll(fin);
    }
    wxRemoveFile(path);

    const unsigned headerLen = 32u + 32u * static_cast<unsigned>(nFields) + 1u;
    const unsigned recLen    = 1u + 254u * static_cast<unsigned>(nFields);   // uniform C(254)

    ExpectTrue("dbf: file large enough", d.size() >= headerLen + 1);

    // ---- header ----
    ExpectTrue("dbf: version byte 0x03",        U8(d, 0) == 0x03);
    ExpectTrue("dbf: language driver [29]=0x7A", U8(d, 29) == 0x7A);
    ExpectTrue("dbf: header length correct",     Le16(d, 8) == headerLen);
    ExpectTrue("dbf: record length correct",     Le16(d, 10) == recLen);
    ExpectTrue("dbf: record count correct",      Le32(d, 4) == nRows);

    // ---- field descriptors: name ≤10 bytes, NUL-terminated, type 'C' ----
    bool namesOk = true, typesOk = true;
    for (size_t i = 0; i < nFields; ++i) {
        const size_t base = 32 + i * 32;
        if (base + 11 >= d.size()) { namesOk = false; break; }
        // fd[10] is the 11th name byte slot; the writer only ever fills fd[0..9]
        // so this stays NUL → proves the name never exceeds 10 bytes.
        if (U8(d, base + 10) != 0x00) namesOk = false;
        if (U8(d, base + 11) != 'C')  typesOk = false;
    }
    ExpectTrue("dbf: field names <=10 bytes (NUL at [10])", namesOk);
    ExpectTrue("dbf: all fields are Character 'C'",         typesOk);

    // ---- records: fixed width, exact file size incl. 0x1A EOF ----
    const size_t expectSize = headerLen + nRows * recLen + 1;
    ExpectTrue("dbf: total size = header + N*recLen + EOF", d.size() == expectSize);
    ExpectTrue("dbf: first record deletion flag 0x20", U8(d, headerLen) == 0x20);
    ExpectTrue("dbf: trailing EOF byte 0x1A", !d.empty() && U8(d, d.size() - 1) == 0x1A);

    // ---- Chinese encoded as GBK/CP936, NOT UTF-8 ----
    wxCSConv cp936(wxFONTENCODING_CP936);
    const wxScopedCharBuffer gbk = CJK().mb_str(cp936);
    const std::string gbkNeedle(gbk.data(), gbk.length());
    const std::string u8Needle(CJK().utf8_str().data(), CJK().utf8_str().length());

    // The GBK encoding of "数据库" is 6 bytes (3 double-byte chars); UTF-8 is 9.
    ExpectTrue("dbf: GBK encoding is 6 bytes", gbkNeedle.size() == 6);
    // Record 2 (row {"2","abc","xyz"}) has no CJK; the CJK is in record 1's 2nd
    // field. Assert the GBK bytes are present and the UTF-8 bytes are NOT.
    const std::string recRegion = d.substr(headerLen, nRows * recLen);
    ExpectTrue("dbf: Chinese present as GBK bytes", Has(recRegion, gbkNeedle));
    ExpectTrue("dbf: Chinese NOT stored as UTF-8",  !Has(recRegion, u8Needle));

    // Known GBK bytes for "数据库": CA FD  BE DD  BF E2 — pin them explicitly so a
    // regression to a different encoding is caught even if wxCSConv changed.
    const unsigned char known[6] = { 0xCA, 0xFD, 0xBE, 0xDD, 0xBF, 0xE2 };
    ExpectTrue("dbf: GBK bytes == CA FD BE DD BF E2",
               gbkNeedle == std::string(reinterpret_cast<const char*>(known), 6));

    // NULL field (row1 col3) is all blanks: the record for row1, field 3 starts at
    // offset 1 + 254*2 within the record. Its 254 bytes must be spaces.
    {
        const size_t rec1 = headerLen;                 // start of record 1
        const size_t f3   = rec1 + 1 + 254 * 2;        // field 3 payload
        bool allBlank = (f3 + 254 <= d.size());
        for (size_t k = 0; allBlank && k < 254; ++k)
            if (U8(d, f3 + k) != 0x20) allBlank = false;
        ExpectTrue("dbf: NULL field is all blanks", allBlank);
    }
}

// ===========================================================================
// Text round-trips — CSV / JSON / XML export→import identity (NULL, embedded
// delimiter/quote/newline, Chinese). Mirrors tableio_test's round-trip block; a
// self-contained restatement so this target validates the full text path too.
// ===========================================================================
static bool RowsEqual(const Rows& a, const Rows& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) return false;
        for (size_t j = 0; j < a[i].size(); ++j) if (a[i][j] != b[i][j]) return false;
    }
    return true;
}

static Rows ImportAll(ImportFormat fmt, const wxString& content, ImportResult& r,
                      ImportOptions opt = {})
{
    opt.format = fmt;
    Rows out;
    r = ImportTable(content, opt,
        [&](const std::vector<wxString>& row) { out.push_back(row); return true; },
        nullptr);
    return out;
}

static void TestTextRoundTrips()
{
    std::printf("-- text round-trips --\n");

    const std::vector<wxString> cols = { L"id", L"name", L"note" };
    Rows data = {
        { L"1", L"Alice",                 L"hello, world"   },   // comma
        { L"2", L"Bob \"the\" builder",   L"line1\nline2"   },   // quote + newline
        { L"NULL", CJK(),                 L"a\tb"           },   // NULL + CJK + tab
    };

    // CSV
    {
        ExportOptions eo; eo.format = ExportFormat::Csv;
        wxString s = ExportTableToString(cols, data, eo);
        ImportResult r; ImportOptions io; io.hasHeader = true;
        Rows out = ImportAll(ImportFormat::Csv, s, r, io);
        ExpectTrue("round-trip csv: ok",      r.ok);
        ExpectTrue("round-trip csv: columns", r.columns == cols);
        ExpectTrue("round-trip csv: rows",    RowsEqual(out, data));
    }
    // JSON
    {
        ExportOptions eo; eo.format = ExportFormat::Json;
        wxString s = ExportTableToString(cols, data, eo);
        ImportResult r;
        Rows out = ImportAll(ImportFormat::Json, s, r);
        ExpectTrue("round-trip json: ok",      r.ok);
        ExpectTrue("round-trip json: columns", r.columns == cols);
        ExpectTrue("round-trip json: rows",    RowsEqual(out, data));
    }
    // XML
    {
        ExportOptions eo; eo.format = ExportFormat::Xml;
        wxString s = ExportTableToString(cols, data, eo);
        ImportResult r;
        Rows out = ImportAll(ImportFormat::Xml, s, r);
        ExpectTrue("round-trip xml: ok",      r.ok);
        ExpectTrue("round-trip xml: columns", r.columns == cols);
        ExpectTrue("round-trip xml: rows",    RowsEqual(out, data));
    }
}

int main()
{
    std::printf("== SwiftSQL binary-export + text round-trip tests ==\n");
    TestXlsx();
    TestDbf();
    TestTextRoundTrips();
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
