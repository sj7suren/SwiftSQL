// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableDataPage.h — the dedicated "打开表" data browser tab. A thin IQueryTab
// wrapper around the shared rich ResultGridPanel (DataBrowser preset: toolbar +
// grid + right info panel + cell viewer + filter/sort/import/export + pager,
// minus the 表单 form view), plus a bottom read-only line echoing the current
// paged SELECT. No SQL editor — the pager/filter/sort own the query.
#pragma once

#include <wx/panel.h>
#include "ui/IQueryTab.h"   // IQueryTab / EditableSpec / db::QueryResult / db::Dialect

class wxStaticText;

namespace ui {

class ResultGridPanel;

class TableDataPage : public wxPanel, public IQueryTab {
public:
    TableDataPage(wxWindow* parent, const wxString& db, const wxString& table,
                  db::Dialect dialect);

    // The embedded results surface (so MainFrame can wire its grid handlers via
    // ConfigureGrid, exactly like an SQL editor tab).
    ResultGridPanel* Grid() const { return grid_; }

    void Load();   // count + load the first page; call once after the handlers are wired.

    // ---- IQueryTab — MainFrame::RunSql feeds results back here ----
    wxString QueryText() const override { return sql_; }   // the staged paged SELECT
    void SetRunning(bool on) override;
    void ShowResult(const db::QueryResult& r, const wxString& target,
                    const EditableSpec& spec = {}) override;
    void ShowError(const wxString& err) override;
    bool ConfirmClose() override;                          // unsaved-edit guard

private:
    ResultGridPanel* grid_    = nullptr;
    wxString         sql_;                 // last staged paged SELECT (QueryText)
};

} // namespace ui
