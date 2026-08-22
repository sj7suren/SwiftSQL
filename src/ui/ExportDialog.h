// ExportDialog.h — export the data browser's rows to a file (CSV/TXT/JSON/XML/SQL)
// with a config step (scope / format / path / options) and a live progress step
// (gauge + row counter + log). The write runs on a worker thread so a large
// table can't freeze the window; the whole-table scope streams chunk-by-chunk so
// only one 5000-row block is ever in memory (P0 streaming red line).
#pragma once

#include <wx/string.h>
#include <atomic>
#include <thread>
#include <vector>

#include "db/DbDriver.h"
#include "db/TableExport.h"     // db::ExportFormat / db::ExportOptions
#include "ui/CenteredDialog.h"
#include "ui/DataTransfer.h"

class wxStopWatch;

class wxRadioBox;
class wxChoice;
class wxCheckBox;
class wxFilePickerCtrl;
class wxGauge;
class wxStaticText;
class wxTextCtrl;
class wxButton;
class wxSimplebook;

namespace ui {

class ExportDialog : public CenteredDialog {
public:
    // `conn` must outlive the dialog (the caller owns it). `req` carries the rows /
    // whole-table query parts packaged by the grid.
    ExportDialog(wxWindow* parent, db::IConnection* conn, const ExportRequest& req);
    ~ExportDialog() override;

private:
    enum class Scope { CurrentPage, WholeTable, Selected };

    wxWindow* BuildConfigPage();
    wxWindow* BuildRunPage();

    void OnFormatChanged();
    void OnStart(wxCommandEvent&);

    // Worker-thread body for the binary (Excel / DBF) formats: streams raw bytes
    // to a wxFileOutputStream through db::XlsxWriter / db::DbfWriter. Shares the
    // scope logic (current page / selection / whole-table chunking) with the text
    // path but bypasses the wxString sink.
    void RunBinaryExport(db::IConnection* conn, const wxString& path,
                         db::ExportFormat fmt, const db::ExportOptions& opt,
                         Scope scope, const std::vector<wxString>& columns,
                         const std::vector<std::vector<wxString>>& memRows,
                         const wxString& qual, const wxString& where,
                         const wxString& orderBy, wxStopWatch& sw);
    void OnStopOrClose(wxCommandEvent&);
    void OnCloseWindow(wxCloseEvent&);
    void JoinWorker();

    wxString DefaultExt() const;                 // ext for the selected format
    wxString QualifiedTable() const;             // `db`.`table` for whole-table SQL

    // worker → UI (always via CallAfter on the GUI thread)
    void SetRows(long long rows);
    void Log(const wxString& line);
    void Finish(bool cancelled, bool writeErr, long long rows, long elapsedMs);

    db::IConnection* conn_ = nullptr;
    ExportRequest    req_;

    // config page
    wxSimplebook*     book_       = nullptr;
    wxRadioBox*       scopeBox_   = nullptr;
    wxChoice*         formatChoice_ = nullptr;
    wxFilePickerCtrl* pathPicker_ = nullptr;
    wxCheckBox*       overwrite_  = nullptr;
    wxCheckBox*       header_     = nullptr;
    wxChoice*         delimChoice_ = nullptr;
    wxChoice*         encoding_   = nullptr;
    wxButton*         startBtn_   = nullptr;

    // run page
    wxStaticText* runTitle_    = nullptr;
    wxGauge*      gauge_       = nullptr;
    wxStaticText* rowTxt_      = nullptr;
    wxTextCtrl*   log_         = nullptr;
    wxButton*     primaryBtn_  = nullptr;   // 停止 while running → 关闭 when done

    std::thread            worker_;
    std::atomic<bool>      stopFlag_{false};
    std::atomic<bool>      running_{false};
    std::atomic<long long> total_{-1};   // gauge denominator (COUNT / row count)
};

} // namespace ui
