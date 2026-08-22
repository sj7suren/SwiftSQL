// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncSqlPreviewPane.h — the 「SQL 预览」 tab of the single-screen compare page
// (T8 of ADR-015). Read-only SQL text bound to the current selection.
//
// ---------------------------------------------------------------------------
// THIS PANE'S TEXT IS NEVER EXECUTED — and cannot be
// ---------------------------------------------------------------------------
// Execution streams from the STRUCTURED diff: SyncSelection::Build() produces
// an ExecutionSpec, ui::FilterStructureBySpec reduces the db::sync::SyncPlan's
// DDL by it, ui::BuildDataExecPlan turns the per-(table, category) choices into
// a db::sync::DataExecPlan, and SyncEngine::Execute re-derives and STREAMS the
// data diff under that authorization. This pane renders the same source
// independently, for a human to read — and, for data, renders only the capped
// sample, which is another reason it could never be the execution path.
//
// Three things enforce it rather than merely asking for it:
//
//   1. THERE IS NO GETTER. No GetValue(), no GetText(), no GetSql(), and the
//      wxTextCtrl is private with no accessor. A caller cannot obtain this
//      pane's string at all, so "grab the preview text and run it" is not an
//      expression anyone can write. Copy/export are performed INSIDE the pane
//      (RenderFull -> clipboard/file) and hand their string to nobody.
//   2. WHAT IS DISPLAYED IS DELIBERATELY INCOMPLETE. Rendering stops at
//      kMaxStatements (200) and appends a 「-- 还有 N 条语句未显示」 footer, so
//      the visible text is not a runnable script for any non-trivial sync and
//      is visibly marked as such.
//   3. IT IS A wxPanel, NOT A CONTROL WRAPPER. It exposes refresh scheduling
//      and counters — nothing that resembles a statement source.
//
// ---------------------------------------------------------------------------
// DEBOUNCE
// ---------------------------------------------------------------------------
// Re-rendering on every checkbox click would re-walk the whole plan per
// keystroke-speed click, and a select-all over 200 tables fires 200 changes in
// a burst. ScheduleRefresh() coalesces them onto a single one-shot timer
// (kDebounceMs = 150), so the visible cost of a burst is one render.
#pragma once

#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/string.h>

class wxTextCtrl;
class wxStaticText;

namespace db::sync { struct SyncPlan; }

namespace ui {

class SyncSelection;

class SyncSqlPreviewPane : public wxPanel {
public:
    // Statements rendered into the visible text before truncation.
    static constexpr int kMaxStatements = 200;
    // Coalescing window for selection-change bursts.
    static constexpr int kDebounceMs = 150;

    explicit SyncSqlPreviewPane(wxWindow* parent);

    // Borrowed, not owned; both must outlive this pane. Passing the selection
    // rather than a pre-rendered string is what keeps the preview honest: it
    // re-derives from the same model the executor uses.
    void SetSource(const db::sync::SyncPlan* plan, const SyncSelection* selection);

    // Ask for a re-render. Coalesced — see the header comment.
    void ScheduleRefresh();

    // Render immediately, cancelling any pending debounce. For the moment the
    // page becomes visible, where waiting 150ms would show a stale pane.
    void RefreshNow();

    // Counters for the page footer. Statements, not tables.
    int RenderedStatements() const { return rendered_; }
    int TotalStatements() const { return total_; }

private:
    void OnTimer(wxTimerEvent&);
    void OnCopy(wxCommandEvent&);
    void OnExport(wxCommandEvent&);

    // The one place statements are turned into text. `cap` < 0 means no cap.
    // Private, and returns by value into a local — the string never escapes
    // the pane except into the clipboard or a file the user named.
    wxString Render(int cap, int& renderedOut, int& totalOut) const;

    const db::sync::SyncPlan* plan_ = nullptr;   // borrowed
    const SyncSelection*      sel_  = nullptr;   // borrowed

    wxTextCtrl*   text_ = nullptr;
    wxStaticText* info_ = nullptr;
    wxTimer       timer_;

    int rendered_ = 0;
    int total_    = 0;
};

} // namespace ui
