// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableExportBin.cpp — implementations of XlsxWriter (OOXML/ZIP) and DbfWriter
// (dBASE III). See TableExportBin.h for the streaming contract and format notes.
#include "db/TableExportBin.h"

#include <wx/stream.h>
#include <wx/datetime.h>
#include <wx/strconv.h>

#include <algorithm>
#include <set>

namespace db {

namespace {

// ---- shared helpers --------------------------------------------------------

// Write a wxString as UTF-8 bytes into any stream (used for all XLSX parts).
void PutUtf8(wxOutputStream& os, const wxString& s)
{
    const wxScopedCharBuffer b = s.utf8_str();
    if (b.length()) os.Write(b.data(), b.length());
}

// Little-endian integer serialization for the DBF header/records.
void Le16(unsigned char* p, unsigned v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void Le32(unsigned char* p, unsigned long v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

// ---- XLSX: XML text escaping ----------------------------------------------
// Escape the entities that matter inside an OOXML <t> text node and drop the
// C0 control chars that are illegal in XML 1.0 (except TAB/LF/CR) so the sheet
// always parses.
wxString XmlEsc(const wxString& v)
{
    wxString out;
    out.reserve(v.length() + 8);
    for (wxString::const_iterator it = v.begin(); it != v.end(); ++it) {
        const wxUniChar ch = *it;
        const wxUniChar::value_type c = ch.GetValue();
        switch (c) {
        case L'&': out += L"&amp;";  break;
        case L'<': out += L"&lt;";   break;
        case L'>': out += L"&gt;";   break;
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

// Spreadsheet column reference: 0 → "A", 25 → "Z", 26 → "AA", ...
wxString ColRef(size_t col0)
{
    wxString s;
    unsigned long n = static_cast<unsigned long>(col0) + 1;
    while (n) {
        const unsigned long r = (n - 1) % 26;
        s = wxString(static_cast<wxChar>(L'A' + r)) + s;
        n = (n - 1) / 26;
    }
    return s;
}

// ---- DBF: field-name and value encoding -----------------------------------
const int kDbfMaxFieldWidth  = 254;   // C-field byte cap
const int kDbfDefFieldWidth  = 254;   // uniform width before the record-size clamp

// Encode a wxString to CP936 (GBK) bytes.
std::string ToCp936(const wxString& s)
{
    wxCSConv conv(wxFONTENCODING_CP936);
    const wxScopedCharBuffer b = s.mb_str(conv);
    return std::string(b.data(), b.length());
}

wxString UpperAscii(const std::string& s)
{
    wxString u;
    for (char c : s) u += static_cast<wxChar>(::toupper(static_cast<unsigned char>(c)));
    return u;
}

// Derive ≤10-byte, de-duplicated ASCII/CP936 DBF field names from the columns.
std::vector<std::string> MakeFieldNames(const std::vector<wxString>& columns)
{
    std::vector<std::string> names;
    names.reserve(columns.size());
    std::set<wxString> used;   // case-insensitive collision guard
    for (size_t i = 0; i < columns.size(); ++i) {
        std::string base = ToCp936(columns[i]);
        // strip control chars / embedded NULs that would corrupt the descriptor
        std::string clean;
        for (char c : base) {
            const unsigned char u = static_cast<unsigned char>(c);
            clean += (u < 0x20) ? '_' : c;
        }
        if (clean.size() > 10) clean.resize(10);
        // trim trailing spaces
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (clean.empty()) clean = "FIELD";

        std::string name = clean;
        int k = 1;
        while (used.count(UpperAscii(name))) {
            std::string suf = std::to_string(k++);
            const size_t keep = (suf.size() < 10) ? (10 - suf.size()) : 0;
            std::string b2 = clean;
            if (b2.size() > keep) b2.resize(keep);
            name = b2 + suf;
            if (name.size() > 10) name.resize(10);
        }
        used.insert(UpperAscii(name));
        names.push_back(name);
    }
    return names;
}

// Copy CP936 bytes into a fixed-width, space-padded field, cutting on a clean
// GBK character boundary so a truncated double-byte char is never split.
void PutFixedField(std::string& rec, const std::string& bytes, int width)
{
    size_t i = 0, cut = 0;
    while (i < bytes.size()) {
        const unsigned char u = static_cast<unsigned char>(bytes[i]);
        const size_t clen = (u >= 0x81 && u <= 0xFE && i + 1 < bytes.size()) ? 2 : 1;
        if (i + clen > static_cast<size_t>(width)) break;
        i += clen;
        cut = i;
    }
    rec.append(bytes, 0, cut);
    rec.append(static_cast<size_t>(width) - cut, ' ');   // right-pad with blanks
}

} // namespace

// ===========================================================================
// XlsxWriter
// ===========================================================================
XlsxWriter::XlsxWriter(const std::vector<wxString>& columns, wxOutputStream& os,
                       bool header, wxString nullText)
    : zip_(os)
    , columns_(columns)
    , nullText_(std::move(nullText))
    , header_(header)
{
    // Static package parts (each PutNextEntry closes the previous entry).
    WritePart("[Content_Types].xml",
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        L"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        L"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        L"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        L"<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        L"<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        L"</Types>");
    WritePart("_rels/.rels",
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        L"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        L"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        L"</Relationships>");
    WritePart("xl/workbook.xml",
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        L"<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        L"xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        L"<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets>"
        L"</workbook>");
    WritePart("xl/_rels/workbook.xml.rels",
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        L"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        L"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        L"</Relationships>");

    // Open the streaming sheet entry and emit the prologue + optional header row.
    if (!zip_.PutNextEntry(wxString(L"xl/worksheets/sheet1.xml"))) ok_ = false;
    PutUtf8(zip_,
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        L"<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        L"<sheetData>");
    if (header_) EmitDataRow(columns_);
    if (!zip_.IsOk()) ok_ = false;
}

XlsxWriter::~XlsxWriter() { Finish(); }

void XlsxWriter::WritePart(const char* name, const wxString& utf8Body)
{
    if (!zip_.PutNextEntry(wxString::FromUTF8(name))) { ok_ = false; return; }
    PutUtf8(zip_, utf8Body);
    if (!zip_.IsOk()) ok_ = false;
}

void XlsxWriter::EmitDataRow(const std::vector<wxString>& row)
{
    ++rowIndex_;
    wxString out;
    out.reserve(row.size() * 24 + 24);
    out += wxString::Format(L"<row r=\"%lld\">", rowIndex_);
    for (size_t c = 0; c < row.size(); ++c) {
        if (row[c] == nullText_) continue;   // NULL → empty (absent) cell
        const wxString ref = ColRef(c) + wxString::Format(L"%lld", rowIndex_);
        out += L"<c r=\"" + ref + L"\" t=\"inlineStr\"><is><t xml:space=\"preserve\">";
        out += XmlEsc(row[c]);
        out += L"</t></is></c>";
    }
    out += L"</row>";
    PutUtf8(zip_, out);
    if (!zip_.IsOk()) ok_ = false;
}

void XlsxWriter::WriteRow(const std::vector<wxString>& row) { EmitDataRow(row); }

void XlsxWriter::WriteRows(const std::vector<std::vector<wxString>>& rows)
{
    for (const auto& r : rows) EmitDataRow(r);
}

void XlsxWriter::Finish()
{
    if (finished_) return;
    finished_ = true;
    PutUtf8(zip_, L"</sheetData></worksheet>");
    if (!zip_.Close()) ok_ = false;
}

// ===========================================================================
// DbfWriter
// ===========================================================================
DbfWriter::DbfWriter(const std::vector<wxString>& columns, wxOutputStream& os,
                     wxString nullText)
    : os_(os)
    , columns_(columns)
    , nullText_(std::move(nullText))
{
    // Uniform width per column, clamped so the record never overflows the
    // 16-bit record-length field. This is a data-independent decision → no
    // second pass over the rows (streaming stays intact).
    const size_t n = columns_.size();
    int w = kDbfDefFieldWidth;
    if (n > 0) {
        const long long recLen = 1LL + static_cast<long long>(w) * static_cast<long long>(n);
        if (recLen > 65535) {
            w = static_cast<int>((65535 - 1) / static_cast<long long>(n));
            if (w < 1) w = 1;
            if (w > kDbfMaxFieldWidth) w = kDbfMaxFieldWidth;
        }
    }
    widths_.assign(n, w);

    const std::vector<std::string> names = MakeFieldNames(columns_);

    // ---- 32-byte file header ----
    unsigned char hdr[32] = {0};
    hdr[0] = 0x03;                       // dBASE III, no memo
    wxDateTime now = wxDateTime::Now();
    hdr[1] = static_cast<unsigned char>(now.GetYear() - 1900);
    hdr[2] = static_cast<unsigned char>(now.GetMonth() + 1);   // wxDateTime month is 0-based
    hdr[3] = static_cast<unsigned char>(now.GetDay());
    // hdr[4..7] record count — patched in Finish().
    const unsigned headerSize = 32u + 32u * static_cast<unsigned>(n) + 1u;
    Le16(hdr + 8, headerSize);
    unsigned recSize = 1;                 // deletion flag byte
    for (int fw : widths_) recSize += static_cast<unsigned>(fw);
    Le16(hdr + 10, recSize);
    hdr[29] = 0x7A;                       // language driver = CP936 (GBK)
    WriteBytes(hdr, 32);

    // ---- one 32-byte field descriptor per column ----
    for (size_t i = 0; i < n; ++i) {
        unsigned char fd[32] = {0};
        const std::string& nm = names[i];
        for (size_t j = 0; j < nm.size() && j < 10; ++j)
            fd[j] = static_cast<unsigned char>(nm[j]);   // rest stays NUL-padded
        fd[11] = 'C';                                    // Character type
        fd[16] = static_cast<unsigned char>(widths_[i]); // field length (1 byte)
        fd[17] = 0;                                       // decimal count
        WriteBytes(fd, 32);
    }

    const unsigned char terminator = 0x0D;                // header end
    WriteBytes(&terminator, 1);
    headerDone_ = true;
    if (!os_.IsOk()) ok_ = false;
}

DbfWriter::~DbfWriter() { Finish(); }

void DbfWriter::WriteBytes(const void* data, size_t n)
{
    if (n == 0) return;
    os_.Write(data, n);
    if (!os_.IsOk()) ok_ = false;
}

void DbfWriter::WriteRow(const std::vector<wxString>& row)
{
    std::string rec;
    rec.reserve(64);
    rec.push_back(' ');                                   // 0x20 = not deleted
    for (size_t c = 0; c < widths_.size(); ++c) {
        const int w = widths_[c];
        if (c < row.size() && row[c] != nullText_) {
            PutFixedField(rec, ToCp936(row[c]), w);
        } else {
            rec.append(static_cast<size_t>(w), ' ');      // NULL / missing → blanks
        }
    }
    WriteBytes(rec.data(), rec.size());
    ++recordCount_;
}

void DbfWriter::WriteRows(const std::vector<std::vector<wxString>>& rows)
{
    for (const auto& r : rows) WriteRow(r);
}

void DbfWriter::Finish()
{
    if (finished_) return;
    finished_ = true;
    const unsigned char eof = 0x1A;
    WriteBytes(&eof, 1);
    // Patch the record count into the header (offset 4, uint32 LE).
    if (os_.SeekO(4) != wxInvalidOffset) {
        unsigned char c[4];
        Le32(c, static_cast<unsigned long>(recordCount_));
        os_.Write(c, 4);
        if (!os_.IsOk()) ok_ = false;
    } else {
        ok_ = false;   // non-seekable stream → count stays 0 (unreadable)
    }
}

} // namespace db
