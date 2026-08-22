// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// GhostTextOverlay.cpp — see header.
#include "ui/GhostTextOverlay.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>

#include "ui/Theme.h"

namespace ui {

GhostTextOverlay::GhostTextOverlay(wxWindow* parent)
    : wxPopupWindow(parent, wxBORDER_NONE)
{
    // The illusion depends entirely on this matching the editor: same
    // background, same font, no border. Anything else and it reads as a tooltip
    // sitting on top of the code rather than as part of the line.
    SetBackgroundColour(theme::kEditorBg);
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // required by wxAutoBufferedPaintDC
    Bind(wxEVT_PAINT, &GhostTextOverlay::OnPaint, this);
}

void GhostTextOverlay::ShowGhost(const wxPoint& screenPos, const wxFont& font,
                                 const wxString& text, int lineHeight, int maxWidth)
{
    if (text.IsEmpty() || maxWidth <= 0) { HideGhost(); return; }

    text_ = text;
    font_ = font;

    // Measure with the SAME font we will paint with, on a DC for this window —
    // measuring against a different DC is how the box ends up a few pixels off
    // and the grey text looks misaligned with the line it belongs to.
    wxClientDC dc(this);
    dc.SetFont(font_);
    wxSize ext = dc.GetTextExtent(text_);
    if (ext.x <= 0) { HideGhost(); return; }

    const int w = wxMin(ext.x + 2, maxWidth);   // +2 so the last glyph is not clipped
    const int h = lineHeight > 0 ? lineHeight : ext.y;

    SetSize(screenPos.x, screenPos.y, w, h);
    active_ = true;
    if (!IsShown()) Show();
    Refresh();
    Update();   // paint now: a suggestion that appears a frame late reads as lag
}

void GhostTextOverlay::HideGhost()
{
    text_.clear();
    active_ = false;
    if (IsShown()) Hide();
}

void GhostTextOverlay::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(theme::kEditorBg));
    dc.Clear();
    if (text_.IsEmpty()) return;
    dc.SetFont(font_);
    // Grey, not the syntax palette: the whole point is that this is NOT code
    // yet. kTextFaint is the same tone the tree uses for disconnected items.
    dc.SetTextForeground(theme::kTextFaint);
    dc.DrawText(text_, 0, 0);
}

} // namespace ui
