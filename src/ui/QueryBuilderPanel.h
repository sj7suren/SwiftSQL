// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// QueryBuilderPanel.h — a Navicat-style visual query builder tab. Left: the
// current database's table list. Centre: a self-drawn scrolled canvas of draggable
// table boxes whose columns can be checked (→ SELECT) and wired column-to-column
// into JOINs. Bottom: a live, read-only SQL preview + an "apply to editor" action.
//
// The panel owns one QueryBuilderModel (pure data + SQL generation, see
// QueryBuilderModel.h); the canvas is an internal class defined in the .cpp that
// mutates that model and calls back here to refresh the preview. The panel BORROWS
// its db::IConnection as a weak_ptr (like TableDesignView) — MainFrame registers the
// tab so it is closed before its connection is torn down, and the weak_ptr makes that
// registration a convenience rather than a safety requirement: a borrow that outlives
// the connection resolves to null and the panel reports 未连接 instead of crashing.
#pragma once

#include <wx/panel.h>
#include <functional>
#include <memory>
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
    void Load(std::shared_ptr<db::IConnection> conn, const wxString& database,
              db::Dialect dialect);

    // Called by the "应用到编辑器" button with the generated SQL. MainFrame wires
    // this to open a fresh SQL editor tab prefilled with the statement.
    using ApplyHook = std::function<void(const wxString& sql)>;
    void SetApplyHook(ApplyHook h) { apply_ = std::move(h); }

private:
    void AddTableToCanvas(const wxString& table);   // fetch columns + place a box
    void RegenSql();                                // model → preview text
    void RefreshTableList(const wxString& filter);  // (re)fill the left list

    // Not owned — the ConnEntry owns it. Reach it only through Conn(), which returns
    // null once that entry has released the driver (disconnect / drop / reconnect).
    std::weak_ptr<db::IConnection> conn_;
    std::shared_ptr<db::IConnection> Conn() const { return conn_.lock(); }
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
