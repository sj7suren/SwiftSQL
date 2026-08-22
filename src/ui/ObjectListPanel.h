// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ObjectListPanel.h — a reusable "object list" tab: a report-style wxListCtrl of a
// database's objects of ONE kind (表 / 视图 / 函数 / 存储过程), with a top toolbar of
// type-specific actions, a matching right-click menu, and a double-click default
// action. Opened from the top toolbar buttons (表/视图/函数/存储过程) against the
// active connection + current database.
//
// The panel is deliberately a "dumb" view: it knows how to render columns + rows,
// run a supplied loader, and dispatch actions carrying the current selection. All
// the object operations (new/modify/drop/execute) and the loader closures live in
// MainFrame (ConfigureObjectList), which owns the connection lifetime and reuses
// ConnectionTree's existing object flows. This keeps the panel free of any
// db::IConnection* / ConnEntry* — so it can never dangle when a connection dies
// (MainFrame closes the tab via CloseTabsForEntry before the connection is freed).
#pragma once

#include <wx/panel.h>
#include <wx/string.h>
#include <functional>
#include <vector>

#include "ui/IconFactory.h"   // icons::Glyph

class wxListCtrl;
class wxListEvent;
class wxSearchCtrl;
class wxBoxSizer;

namespace ui {

class FlatButton;

class ObjectListPanel : public wxPanel {
public:
    // One report column: header text, initial pixel width, right-alignment.
    struct Column {
        wxString title;
        int      width      = 160;
        bool     rightAlign = false;
    };
    // One object row. cells[0] MUST be the object name (used by the selection
    // accessors + double-click); further cells map 1:1 to the extra columns.
    struct Row {
        std::vector<wxString> cells;
    };
    // A toolbar / context action. `run` receives the first-selected object name and
    // the full multi-selection, so an action can operate on one or many rows without
    // reaching back into the list. Actions with needsSelection are greyed until a
    // row is selected.
    struct Action {
        wxString     label;
        icons::Glyph glyph;
        wxColour     color;
        bool         needsSelection = false;
        std::function<void(const wxString& first,
                           const std::vector<wxString>& all)> run;
    };

    explicit ObjectListPanel(wxWindow* parent);

    // One-time configuration: the report columns, the toolbar/context actions, and
    // which action index a double-click fires (-1 = none). Must be called once,
    // before Reload().
    void Setup(std::vector<Column> columns, std::vector<Action> actions,
               int doubleClickAction);
    // Supply the data source. `loader` fills `out` and returns true, or returns
    // false and fills `err` on failure (a message is shown, the list left as-is).
    // Runs on the GUI thread (one small metadata query).
    void SetLoader(std::function<bool(std::vector<Row>& out, wxString& err)> loader);
    // (Re)run the loader and repopulate the list, preserving the current filter.
    void Reload();

    // Names of the selected rows (empty when nothing is selected).
    wxString              SelectedName() const;
    std::vector<wxString> SelectedNames() const;

private:
    void RenderRows();                    // (re)fill the list from rows_, filtered
    void UpdateActionEnabled();           // grey needsSelection buttons w/o a selection
    void OnItemActivated(wxListEvent&);   // double-click → default action
    void OnContextMenu(wxListEvent&);     // right-click → the same action set
    void DispatchAction(const Action& a);

    wxSearchCtrl*             search_  = nullptr;
    wxListCtrl*               list_    = nullptr;
    wxBoxSizer*               toolbar_ = nullptr;
    std::vector<Column>       columns_;
    std::vector<Action>       actions_;
    std::vector<FlatButton*>  buttons_;   // parallel to actions_ (for enable/disable)
    std::vector<Row>          rows_;       // full unfiltered data
    int                       dblAction_ = -1;
    std::function<bool(std::vector<Row>&, wxString&)> loader_;
};

} // namespace ui
