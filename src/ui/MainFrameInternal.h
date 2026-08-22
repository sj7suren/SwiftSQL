// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MainFrameInternal.h — internal declarations shared across the MainFrame
// translation units (MainFrame.cpp / MainFrame_Chrome.cpp / MainFrame_Overview.cpp
// / MainFrame_Query.cpp / MainFrame_Tabs.cpp / MainFrame_Events.cpp). This is NOT a
// public API — only those .cpp files include it.
//
// MainFrame.cpp was split into several TUs (each < 1000 lines) with the class
// definition still living in MainFrame.h; the method bodies were moved verbatim,
// nothing else changed. Two file-local things had to become visible to more than
// one TU: the frame-scoped command IDs (menu built in one TU, toolbar in another,
// enabled/handled in a third) and DeriveEditable (used by both the query-run path
// and the scriptable auto-connect path). They are collected here instead of being
// promoted to the public MainFrame.h.
#pragma once

#include "db/DbDriver.h"    // db::IConnection
#include "ui/IQueryTab.h"   // ui::EditableSpec (+ wxID_HIGHEST via wx/defs.h)
#include "ui/EditorPage.h"  // EditorPage::ObjectTarget (return type of MakeObjectTarget)

#include <wx/string.h>

namespace ui {

class ConnectionTree;

// Frame-scoped command IDs for the custom menu row + toolbar. Built in
// BuildMenuBar (MainFrame.cpp) and BuildToolRow (MainFrame_Chrome.cpp), handled /
// enabled in the event TUs. (ID_RUN_QUERY / ID_STOP_QUERY / ID_FORMAT_SQL /
// ID_EXPLAIN_SQL / ID_TX_* live in IQueryTab.h — shared with the editor + grid
// toolbars — so the data-grid toolbar's buttons bubble up here the same way.)
enum {
    ID_NEW_CONNECTION = wxID_HIGHEST + 1,
    ID_NEW_QUERY,
    ID_TOOL_TABLE, ID_TOOL_VIEW, ID_TOOL_FUNC, ID_TOOL_PROC, ID_TOOL_MODEL, ID_TOOL_BACKUP,
    ID_TOOL_IMPORT, ID_TOOL_EXPORT, ID_TOOL_AUTORUN, ID_TOOL_AI, ID_TOOL_MORE,
    ID_VIEW_QUERY, ID_VIEW_DESIGN, ID_VIEW_ER,
    ID_SCRIPT_LIB,          // open the saved-SQL script library tab
    ID_QUERY_BUILDER,       // open a visual query-builder tab
    ID_PREFERENCES,
    ID_TOOL_SYNC_STRUCT, ID_TOOL_SYNC_DATA, ID_TOOL_SYNC_BOTH,
    ID_TOOL_SERVER_MONITOR, // 工具 ▸ 服务器监控: live session list for the active conn
    ID_AUTOMATION_TIMER,    // wxTimer id for the in-app automation scheduler tick
    ID_NEXT_TAB,            // 窗口 ▸ 下一个标签页 (Ctrl+Tab): editors_->AdvanceSelection()
};

// Decide whether a query result can be edited in-place: only a plain single-table
// SELECT whose table has a primary key. Defined in MainFrame_Query.cpp; also used
// by the scriptable auto-connect path (MainFrame_Events.cpp). Behaviour is
// unchanged from the original file-local helper — only its linkage was widened so
// the two TUs can share one definition.
EditableSpec DeriveEditable(db::IConnection* conn, const wxString& db,
                            const wxString& sql);

// Build the 连接/数据库 selector data source bound to the connection tree (lists
// only connected connections; every handle is re-validated via ct->isLive on use).
// Defined in MainFrame_Chrome.cpp; used by ConfigurePage (MainFrame_Tabs.cpp) so
// EVERY editor tab — plain SQL editors and object editors alike — gets the top
// selector. Linkage was widened from file-local to ui:: exactly like DeriveEditable
// above so the two TUs share one definition.
EditorPage::ObjectTarget MakeObjectTarget(ConnectionTree* ct);

}  // namespace ui
