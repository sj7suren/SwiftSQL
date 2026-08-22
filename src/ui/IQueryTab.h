// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// IQueryTab.h — the minimal contract MainFrame::RunSql drives, so one async run
// path can feed both an SQL editor tab (EditorPage) and a dedicated open-table
// data tab (TableDataPage). Both own a ResultGridPanel underneath; this interface
// is just the seam RunSql talks to (query text in, result/error/running out).
#pragma once

#include <wx/defs.h>       // wxID_HIGHEST
#include <wx/string.h>
#include <vector>
#include "db/DbDriver.h"   // db::QueryResult / db::Dialect

namespace ui {

// Command IDs bubbled up to MainFrame (which binds wxEVT_BUTTON / wxEVT_MENU for
// them). The run/stop/txn buttons on the result grid's bottom toolbar and the
// SQL editor's top toolbar both post these, so MainFrame handles them uniformly.
enum { ID_RUN_QUERY = wxID_HIGHEST + 50, ID_FORMAT_SQL, ID_EXPLAIN_SQL, ID_STOP_QUERY,
       ID_TX_BEGIN, ID_TX_COMMIT, ID_TX_ROLLBACK,
       ID_AI_PERF };   // AI SQL 性能分析 (EXPLAIN + model reading the plan)

// Describes how the current result maps back to a table so it can be edited.
// Shared by ResultGridPanel (which diffs edits → DML) and MainFrame (which
// derives it from the SQL). Lives here so both the interface and the grid can
// reference it without either owning the other.
struct EditableSpec {
    bool                  editable = false;
    wxString              table;
    std::vector<wxString> pkColumns;   // primary-key column names (WHERE key)
    // Column type strings ("date"/"datetime"/"int"…) aligned with the result
    // columns (SELECT * order). Best-effort: empty when types weren't resolved.
    // ResultGridPanel uses this to attach a calendar editor to pure-`date` cols.
    std::vector<wxString> colTypes;
    db::Dialect           dialect = db::Dialect::MySQL;

    // Set (and `editable` left false) when the base table could not be
    // established SAFELY — an unparsable statement, an identifier quoting form
    // we don't model, or a db/schema-qualified name the write path cannot honour
    // (see ui::qsrc::ClassifyBind). Displayed under the result, because a grid
    // that is silently read-only reads to the user as a bug rather than as the
    // deliberate refusal it is. Empty for the ordinary "it's a join" case, which
    // needs no explanation.
    wxString              notEditableReason;
};

class IQueryTab {
public:
    virtual ~IQueryTab() = default;

    // The SQL to run when this tab is (re)executed: the editor's selection/buffer
    // for a query tab, or the current paged/filter SELECT for a data tab.
    virtual wxString QueryText() const = 0;
    virtual void SetRunning(bool on) = 0;
    virtual void ShowResult(const db::QueryResult& r, const wxString& target,
                            const EditableSpec& spec = {}) = 0;
    virtual void ShowError(const wxString& err) = 0;

    // Called before the tab is closed; return false to veto (e.g. the unsaved-edit
    // guard chose 取消). Default: always allow.
    virtual bool ConfirmClose() { return true; }
};

} // namespace ui
