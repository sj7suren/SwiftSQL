// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// QueryBuilderPanel.h — a Navicat-style visual query builder tab. Left: the
// current database's table list. Centre: a self-drawn scrolled canvas of draggable
// table boxes whose columns can be checked (→ SELECT) and wired column-to-column
// into JOINs. Bottom: a live, read-only SQL preview + an "apply to editor" action.
//
// The panel owns one QueryBuilderModel (pure data + SQL generation, see
// QueryBuilderModel.h); the canvas is an internal class defined in the .cpp that
// mutates that model and calls back here to refresh the preview. The panel holds a
// raw db::IConnection* (like TableDesignView / ErDiagramView) — MainFrame registers
// the tab so it is closed before its connection is torn down, so the pointer never
// dangles while the panel is alive.
#pragma once

#include <wx/panel.h>
#include <functional>
#include "db/DbDriver.h"
#include "ui/QueryBuilderModel.h"

class wxListBox;
class wxSearchCtrl;
class wxStaticText;
class wxTextCtrl;

namespace ui {

class QbCanvas;   // the scrolled drawing surface (defined in the .cpp)

class QueryBuilderPanel : public wxPanel {
public:
    explicit QueryBuilderPanel(wxWindow* parent);

    // Bind to a live connection + database and populate the table list. Safe to
    // call once right after construction (MainFrame::OpenQueryBuilder does this).
    void Load(db::IConnection* conn, const wxString& database, db::Dialect dialect);

    // Called by the "应用到编辑器" button with the generated SQL. MainFrame wires
    // this to open a fresh SQL editor tab prefilled with the statement.
    using ApplyHook = std::function<void(const wxString& sql)>;
    void SetApplyHook(ApplyHook h) { apply_ = std::move(h); }

private:
    void AddTableToCanvas(const wxString& table);   // fetch columns + place a box
    void RegenSql();                                // model → preview text
    void RefreshTableList(const wxString& filter);  // (re)fill the left list

    db::IConnection*  conn_ = nullptr;   // not owned; valid while the tab lives
    wxString          db_;
    QueryBuilderModel model_;

    wxSearchCtrl*     search_    = nullptr;
    wxListBox*        tableList_ = nullptr;
    wxStaticText*     hint_      = nullptr;   // left-panel status line
    QbCanvas*         canvas_    = nullptr;
    wxTextCtrl*       preview_   = nullptr;

    std::vector<wxString> allTables_;         // unfiltered names (for the search)
    ApplyHook             apply_;
};

} // namespace ui
