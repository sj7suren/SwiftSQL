// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncSqlPreviewPane.cpp — see header.
#include "ui/SyncSqlPreviewPane.h"

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "db/SyncEngine.h"
#include "ui/I18n.h"
#include "ui/SyncCompareRow.h"
#include "ui/SyncSelection.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

bool IsDangerStmt(const wxString& s)
{
    const wxString u = s.Upper();
    return u.Contains(L"DROP ") || u.Contains(L"DELETE ") || u.Contains(L"TRUNCATE");
}

wxString Terminate(const wxString& s)
{
    wxString t = s;
    t.Trim(true).Trim(false);
    if (!t.EndsWith(L";")) t += L";";
    return t;
}

} // namespace

SyncSqlPreviewPane::SyncSqlPreviewPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
    , timer_(this)
{
    auto* s = new wxBoxSizer(wxVERTICAL);

    text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxBORDER_NONE);
    text_->SetFont(Mono(9));
    text_->SetBackgroundColour(theme::kEditorBg);
    s->Add(text_, 1, wxEXPAND);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    info_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    info_->SetFont(Ui(9));
    info_->SetForegroundColour(theme::kTextSecondary);
    bar->Add(info_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

    auto* copy = new wxButton(this, wxID_ANY, tr(L"复制"));
    copy->Bind(wxEVT_BUTTON, &SyncSqlPreviewPane::OnCopy, this);
    auto* exp = new wxButton(this, wxID_ANY, tr(L"导出 .sql…"));
    exp->Bind(wxEVT_BUTTON, &SyncSqlPreviewPane::OnExport, this);
    bar->Add(copy, 0, wxRIGHT, 6);
    bar->Add(exp, 0, wxRIGHT, 8);
    s->Add(bar, 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    SetSizer(s);
    Bind(wxEVT_TIMER, &SyncSqlPreviewPane::OnTimer, this);
}

void SyncSqlPreviewPane::SetSource(const db::sync::SyncPlan* plan, const SyncSelection* selection)
{
    plan_ = plan;
    sel_  = selection;
    RefreshNow();
}

void SyncSqlPreviewPane::ScheduleRefresh()
{
    timer_.Start(kDebounceMs, wxTIMER_ONE_SHOT);   // restarting coalesces the burst
}

void SyncSqlPreviewPane::RefreshNow()
{
    timer_.Stop();
    const wxString sql = Render(kMaxStatements, rendered_, total_);
    text_->SetValue(sql);
    text_->ShowPosition(0);

    if (total_ > rendered_) {
        info_->SetLabel(wxString::Format(
            tr(L"已显示 %d / %d 条语句（预览截断，执行以结构化差异为准）"), rendered_, total_));
        info_->SetForegroundColour(theme::kDotAmber);
    } else {
        info_->SetLabel(wxString::Format(tr(L"共 %d 条语句"), total_));
        info_->SetForegroundColour(theme::kTextSecondary);
    }
    Layout();
}

void SyncSqlPreviewPane::OnTimer(wxTimerEvent&)
{
    RefreshNow();
}

wxString SyncSqlPreviewPane::Render(int cap, int& renderedOut, int& totalOut) const
{
    renderedOut = 0;
    totalOut    = 0;
    if (!plan_ || !sel_) return wxString();

    // Re-derive from the SAME structured path the executor takes, so the text a
    // user reads and the statements that would run cannot drift apart: the
    // selection produces one ExecutionSpec, FilterStructureBySpec reduces the
    // DDL by it, and each sample row is admitted by its own RowChange::Op —
    // the category the producer tagged, never a verb sniffed out of text.
    const ExecutionSpec      spec     = sel_->Build();
    const db::sync::SyncPlan filtered = FilterStructureBySpec(*plan_, spec);

    wxString out;
    int danger = 0;
    int truncatedTables = 0;
    int excludedRows = 0;

    auto emit = [&](const wxString& stmt) {
        ++totalOut;
        if (cap >= 0 && renderedOut >= cap) return;
        if (IsDangerStmt(stmt)) { out << L"-- ⚠ " << tr(L"危险语句") << L"\n"; ++danger; }
        out << Terminate(stmt) << L"\n";
        ++renderedOut;
    };

    for (const wxString& s : filtered.preamble) emit(s);
    if (!filtered.preamble.empty()) out << L"\n";

    for (const db::sync::SyncPlan::TableUnit& u : filtered.units) {
        // Header lines are comments, not statements — they are not counted, so
        // the cap really does mean "200 statements".
        if (cap < 0 || renderedOut < cap)
            out << L"-- ---- " << tr(L"表") << L" " << u.table.Display() << L" ----\n";
        for (const wxString& s : u.ddl) emit(s);

        // DATA. Rendered HERE, from the capped structured sample, rather than
        // read out of the plan's `dml` vector: `dml` is preview text the engine
        // rendered with no per-category tagging, so showing only the checked
        // categories from it would mean classifying statements by verb again.
        // RowChange::Operation() is the tag itself.
        const TableExecSpec* ts = spec.Find(TableKey(u.table.Key()));
        db::sync::RowRenderSpec rs;
        if (ts && MakeRowRenderSpec(u, rs)) {
            for (const db::sync::RowChange& r : u.rows.Sample()) {
                if (!RowAllowedBy(*ts, r.Operation())) continue;
                // A row the user individually unchecked must not appear here.
                // The preview's whole job is to show what will run, and
                // TableDataSpec::ExcludeRow will keep this row from being
                // written — so rendering its statement would be the preview
                // disagreeing with the execution over the same selection.
                //
                // Matched on RowChange::RowKey() VERBATIM, which is the same
                // string ui::BuildDataExecPlan hands the data layer. Nothing
                // re-encodes Key() to produce a comparison value: for a coerced
                // cross-engine key that would silently fail to match, and this
                // pane would then show a statement the executor drops.
                if (ts->ExcludedRows().count(RowKey(r.RowKey())) != 0) { ++excludedRows; continue; }
                const wxString s = db::sync::RenderRowDml(r, rs);
                if (!s.IsEmpty()) emit(s);
            }
            if (u.rows.Truncated()) {
                ++truncatedTables;
                if (cap < 0 || renderedOut < cap)
                    out << L"-- " << wxString::Format(
                        tr(L"（%s 的差异行数超过明细上限，以上仅为样本；执行时会完整同步）"),
                        u.table.Display()) << L"\n";
            }
        }
        if (cap < 0 || renderedOut < cap) out << L"\n";
    }

    for (const wxString& s : filtered.postamble) emit(s);

    if (totalOut == 0)
        out << L"-- " << tr(L"当前未勾选任何变更。") << L"\n";

    if (cap >= 0 && totalOut > renderedOut) {
        out << L"\n-- " << wxString::Format(tr(L"还有 %d 条语句未显示"), totalOut - renderedOut)
            << L"\n";
    }

    if (truncatedTables > 0) {
        out << L"-- " << wxString::Format(
            tr(L"有 %d 张表的差异行数超过明细上限，预览中的 DML 仅为样本；"
               L"执行时按行流式重跑差异，不会只写入样本。"), truncatedTables)
            << L"\n";
    }
    if (excludedRows > 0) {
        // Stated rather than left as a silently shorter script: a preview that
        // just omits the rows reads as "these rows had nothing to do", which is
        // a different claim from "you excluded them".
        out << L"-- " << wxString::Format(
            tr(L"已按逐行勾选排除 %d 行，其语句未列出，执行时也不会写入。"), excludedRows)
            << L"\n";
    }
    if (danger > 0) {
        out << L"-- " << wxString::Format(
            tr(L"含 %d 条破坏性语句（DROP / DELETE / TRUNCATE）。"), danger) << L"\n";
    }

    return out;
}

// Copy/export render the FULL script (cap = -1) rather than the truncated text
// on screen: handing the user a silently-cut-off .sql file would be worse than
// either showing all of it or showing none. The string is built here, consumed
// here, and returned to no caller — see the header's point 1.
void SyncSqlPreviewPane::OnCopy(wxCommandEvent&)
{
    int r = 0, t = 0;
    const wxString full = Render(/*cap*/ -1, r, t);
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(full));
        wxTheClipboard->Close();
    }
}

void SyncSqlPreviewPane::OnExport(wxCommandEvent&)
{
    wxFileDialog fd(this, tr(L"导出 SQL"), wxEmptyString, L"sync.sql",
                    tr(L"SQL 文件 (*.sql)|*.sql|所有文件 (*.*)|*.*"),
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK) return;

    int r = 0, t = 0;
    const wxString full = Render(/*cap*/ -1, r, t);
    wxFile f(fd.GetPath(), wxFile::write);
    if (!f.IsOpened() || !f.Write(full, wxConvUTF8)) {
        wxMessageBox(tr(L"无法写入文件:") + L"\n\n" + fd.GetPath(),
                     tr(L"导出 SQL"), wxOK | wxICON_ERROR, this);
    }
}

} // namespace ui
