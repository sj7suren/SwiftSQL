// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableExportBin.h — streaming writers for the two BINARY export formats,
// Excel (.xlsx) and dBASE (.dbf). These are the binary counterpart to the text
// TableWriter (TableExport.h): where that arm streams wxString through an
// ExportSink, binary bytes must never round-trip through a wxString (a sink
// would mangle them), so these writers take a wxOutputStream& and emit raw
// bytes directly.
//
// Streaming contract (same P0 red line as the text arm): the whole table is
// NEVER buffered. XlsxWriter opens the ZIP's sheet1.xml entry once and appends
// one <row> per WriteRow, closing the package on Finish(). DbfWriter writes the
// header + field descriptors up front, then one fixed-width record per WriteRow,
// patching the record count into the header on Finish() via a single SeekO on
// the (seekable) output file.
//
// Layer: db only. Depends on wxWidgets base (wxString / wxZipOutputStream /
// wxOutputStream). No ui/, no IConnection.
#pragma once

#include <wx/string.h>
#include <wx/zipstrm.h>
#include <memory>
#include <string>
#include <vector>

class wxOutputStream;

namespace db {

// Common interface so the export worker can drive either binary writer through
// one pointer. Same shape as TableWriter's row-facing methods.
class IBinaryTableWriter {
public:
    virtual ~IBinaryTableWriter() = default;

    virtual void WriteRow(const std::vector<wxString>& row)                 = 0;
    virtual void WriteRows(const std::vector<std::vector<wxString>>& rows)  = 0;
    // Close the document. Must be called exactly once; after Finish() the
    // underlying stream holds a complete, valid file.
    virtual void Finish()                                                   = 0;
    // False once any underlying stream write failed (disk full, etc.). The
    // caller reports this as a write error.
    virtual bool Ok() const                                                 = 0;
};

// ---- Excel (.xlsx) ---------------------------------------------------------
// A minimal, valid OOXML SpreadsheetML package written through wxZipOutputStream:
//   [Content_Types].xml, _rels/.rels, xl/workbook.xml,
//   xl/_rels/workbook.xml.rels, xl/worksheets/sheet1.xml
// Cells are emitted as inline strings (t="inlineStr") — no sharedStrings table —
// so rows stream without a global string pool. A cell equal to `nullText`
// (default "NULL") is written as an empty cell.
class XlsxWriter : public IBinaryTableWriter {
public:
    // `columns` are the header labels (written as sheet row 1 when `header`).
    // `os` must outlive the writer; it is the file stream the ZIP wraps.
    XlsxWriter(const std::vector<wxString>& columns, wxOutputStream& os,
               bool header = true, wxString nullText = L"NULL");
    ~XlsxWriter() override;

    void WriteRow(const std::vector<wxString>& row) override;
    void WriteRows(const std::vector<std::vector<wxString>>& rows) override;
    void Finish() override;
    bool Ok() const override { return ok_; }

private:
    void WritePart(const char* name, const wxString& utf8Body);
    void EmitDataRow(const std::vector<wxString>& row);

    wxZipOutputStream     zip_;
    std::vector<wxString> columns_;
    wxString              nullText_;
    long long             rowIndex_ = 0;   // 1-based sheet row cursor
    bool                  header_;
    bool                  finished_ = false;
    bool                  ok_       = true;
};

// ---- dBASE (.dbf) ----------------------------------------------------------
// dBASE III fixed-width table. Every field is Character(C) so no type inference
// (and thus no two-pass scan) is needed; each column gets a uniform, capped
// width. Chinese text is encoded as GBK/CP936 and the header's language-driver
// byte is set to 0x7A (=CP936) so readers decode it correctly. A cell equal to
// `nullText` is written as all blanks.
class DbfWriter : public IBinaryTableWriter {
public:
    // `os` must be seekable (a wxFileOutputStream): Finish() seeks back to patch
    // the record count. `os` must outlive the writer.
    DbfWriter(const std::vector<wxString>& columns, wxOutputStream& os,
              wxString nullText = L"NULL");
    ~DbfWriter() override;

    void WriteRow(const std::vector<wxString>& row) override;
    void WriteRows(const std::vector<std::vector<wxString>>& rows) override;
    void Finish() override;
    bool Ok() const override { return ok_; }

private:
    void WriteBytes(const void* data, size_t n);

    wxOutputStream&       os_;
    std::vector<wxString> columns_;
    wxString              nullText_;
    std::vector<int>      widths_;      // per-column byte width (all C fields)
    long long             recordCount_ = 0;
    bool                  headerDone_  = false;
    bool                  finished_    = false;
    bool                  ok_          = true;
};

} // namespace db
