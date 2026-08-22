// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableDataPage.cpp — see header. A thin IQueryTab wrapper hosting the shared
// ResultGridPanel in its DataBrowser preset, with a bottom read-only line that
// echoes the paged SELECT the pager/filter/sort stage. All the data-browser
// machinery (pager, editing → DML, filter, server-side sort, cell viewer, info
// panel) lives in ResultGridPanel; this class only binds it to one table and
// surfaces the staged SQL as QueryText() for MainFrame::RunSql.
#include "ui/TableDataPage.h"

#include <wx/wx.h>

#include "ui/ResultGridPanel.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

TableDataPage::TableDataPage(wxWindow* parent, const wxString& db,
                             const wxString& table, db::Dialect dialect)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kWhite);
    auto* v = new wxBoxSizer(wxVERTICAL);

    // ---- the shared rich data-browser surface ----
    grid_ = new ResultGridPanel(this, ResultGridPanel::Features::DataBrowser());
    // The pager / filter / sort build a fresh (db-qualified) paged SELECT and
    // stage it here; QueryText() returns it so the same run path (ID_RUN_QUERY →
    // OnRunQuery → RunSql → QueryText) picks it up. The SQL is surfaced on the app's
    // global status bar (MainFrame), so this tab keeps no in-page SQL echo.
    grid_->SetQuerySink([this](const wxString& s) { sql_ = s; });
    // Bind the grid to this concrete table so its paging/sort/filter SQL is
    // db-qualified (`db`.`table` on MySQL — required when no default schema is
    // selected). Marks the panel as browse mode (page/where/sort state sticky).
    grid_->SetSourceTable(db, table, dialect);
    v->Add(grid_, 1, wxEXPAND);   // grid fills the whole tab — no in-page SQL bar
    SetSizer(v);
}

void TableDataPage::Load()
{
    grid_->LoadFirstPage();   // count + load page 1 through the shared async path
}

void TableDataPage::SetRunning(bool on) { grid_->SetRunning(on); }

void TableDataPage::ShowResult(const db::QueryResult& r, const wxString& target,
                               const EditableSpec& spec)
{
    grid_->ShowResult(r, target, spec);
}

void TableDataPage::ShowError(const wxString& err) { grid_->ShowError(err); }

bool TableDataPage::ConfirmClose() { return grid_->ConfirmDiscardPendingEdits(); }

} // namespace ui
