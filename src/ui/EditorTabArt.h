// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// EditorTabArt.h — custom wxAui tab art for the central editor notebook
// (MainFrame::editors_). Fully self-drawn to match the prototype
// (docs/UI/SwiftSql.dc.html:357-370): a flat 40px grey strip (#EEF0F4) with a 1px
// bottom rule, rectangular tabs (no slant/round), a 2px blue top accent + a blue
// dot on the selected tab, grey icon/text when unselected, and a close X that
// appears ONLY on the hovered tab.
//
// Hit-testing is the delicate part (this notebook has a history of a "ghost close
// button" mis-closing the wrong tab): the close-button hit rect is written NON-empty
// for AT MOST the single hovered, closable, non-special tab; every other tab —
// including the selected tab showing its dot and the pinned "总览" tab — gets an
// empty rect so it can never be hit. See EditorTabArt.cpp for the full rationale.
#pragma once

#include <wx/aui/auibook.h>
#include <wx/aui/tabart.h>
#include <wx/colour.h>

#include <map>

namespace ui {

class EditorTabArt : public wxAuiSimpleTabArt {
public:
    // colors_/special_ point at addresses that outlive this art provider (the
    // MainFrame member map + the overviewTab_ pointer), so the default copy
    // Clone() below keeps split-view clones reading the same live data.
    void SetColorMap(const std::map<wxWindow*, wxColour>* m) { colors_ = m; }
    void SetSpecialTabPtr(wxWindow* const* p) { special_ = p; }

    void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override;

    void DrawTab(wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page,
                 const wxRect& inRect, int closeButtonState,
                 wxRect* outTabRect, wxRect* outButtonRect, int* xExtent) override;

    wxSize GetTabSize(wxDC& dc, wxWindow* wnd, const wxString& caption,
                      const wxBitmapBundle& bitmap, bool active,
                      int closeButtonState, int* xExtent) override;

    int GetBestTabCtrlSize(wxWindow* wnd, const wxAuiNotebookPageArray& pages,
                           const wxSize& requiredBmpSize) override;

    int GetIndentSize() override { return 0; }

    wxAuiTabArt* Clone() override { return new EditorTabArt(*this); }

private:
    bool IsSpecial(wxWindow* w) const { return special_ && *special_ && w == *special_; }

    // Total tab width in logical px for a caption, including padding + icon + the
    // reserved trailing slot (dot / close X). Shared by GetTabSize + DrawTab so the
    // measured width and the drawn width never disagree (which would overlap tabs).
    int MeasureWidth(wxWindow* wnd, const wxString& caption) const;

    const std::map<wxWindow*, wxColour>* colors_ = nullptr;   // → MainFrame::tabColors_
    wxWindow* const*                     special_ = nullptr;  // → &MainFrame::overviewTab_
};

} // namespace ui
