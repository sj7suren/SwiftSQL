// AiChatWidgets.h — small self-drawn, borderless controls shared by the AiChatPanel
// translation units (AiChatPanel.cpp, AiChatPanel_Turn.cpp). Split out of
// AiChatPanel.cpp so the same painted controls can be constructed from more than one
// TU (BuildUi builds the input bar; the turn code paints chat bubbles).
//
//   RoundedPanel     : a plain rounded-rectangle surface (fill + 1px border) — used as
//                      the input capsule and as each chat bubble's background.
//   IconCircleButton : a 34×34 round icon button (send / stop / drawer / "+"), light
//                      hover disc, disabled state greys the glyph.
//   PillButton       : a rounded "label ⌄" pill (the in-chat model picker) that sizes
//                      itself to its content and opens a menu on click.
//
// Header-only: every method is defined in-class (implicitly inline), so it can be
// included from several translation units. Colours come from Theme tokens only.
#pragma once

#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

class RoundedPanel : public wxPanel {
public:
    RoundedPanel(wxWindow* parent, wxColour fill, wxColour border, int radius)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , fill_(fill)
        , border_(border)
        , radius_(radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &RoundedPanel::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : theme::kWhite));
        dc.Clear();

        // Plain wxDC — NOT wxGraphicsContext. Creating a GC per bubble on every scroll
        // repaint made a long transcript stutter; there can be hundreds of these. GDI
        // rounded-rect is far cheaper and visually equivalent at these bubble sizes.
        dc.SetBrush(wxBrush(fill_));
        dc.SetPen(wxPen(border_, 1));
        dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, radius_);
    }

    wxColour fill_;
    wxColour border_;
    int radius_;
};

class IconCircleButton : public wxPanel {
public:
    IconCircleButton(wxWindow* parent, icons::Glyph glyph, wxColour fg, wxColour fill,
                     std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(34, 34), wxBORDER_NONE)
        , glyph_(glyph)
        , fg_(fg)
        , fill_(fill)
        , onClick_(std::move(onClick))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(34, 34));
        Bind(wxEVT_PAINT, &IconCircleButton::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (IsEnabled() && onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true; Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }

    bool Enable(bool enable = true)
    {
        const bool ok = wxPanel::Enable(enable);
        Refresh();
        return ok;
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : theme::kWhite));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            const wxColour bg = !IsEnabled() ? theme::kBorderSoft
                              : hover_       ? fill_.ChangeLightness(92)
                                             : fill_;
            gc->SetBrush(wxBrush(bg));
            gc->SetPen(wxPen(bg, 1));
            gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1, sz.y - 1, 17);
        }

        const wxColour icon = IsEnabled() ? fg_ : theme::kTextGhost;
        dc.DrawBitmap(icons::Stroke(glyph_, 17, icon, 1.9),
                      (sz.x - 17) / 2, (sz.y - 17) / 2, true);
    }

    icons::Glyph glyph_;
    wxColour fg_;
    wxColour fill_;
    std::function<void()> onClick_;
    bool hover_ = false;
};

// A small rounded "pill" selector — light fill, a text label and a vector ⌄ chevron
// on the right (echoes the reference model picker). Sizes to its content; clicking
// runs onClick (the panel opens a model menu). Colours come from Theme tokens only.
class PillButton : public wxPanel {
public:
    PillButton(wxWindow* parent, std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, kH), wxBORDER_NONE)
        , onClick_(std::move(onClick))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(ui::Ui(9));
        SetMinSize(wxSize(64, kH));
        Bind(wxEVT_PAINT, &PillButton::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (IsEnabled() && onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }

    void SetText(const wxString& s) { text_ = s; FitToContent(); Refresh(); }

    bool Enable(bool enable = true) override
    {
        const bool ok = wxPanel::Enable(enable);
        Refresh();
        return ok;
    }

private:
    static constexpr int kH = 26, kRadius = 13, kLeftPad = 12, kGap = 6,
                         kTriW = 8, kTriH = 5, kRightPad = 10;

    void FitToContent()
    {
        const wxSize e = GetTextExtent(text_.IsEmpty() ? wxString(L"M") : text_);
        SetMinSize(wxSize(kLeftPad + e.x + kGap + kTriW + kRightPad, kH));
        InvalidateBestSize();
        if (GetParent()) GetParent()->Layout();
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : theme::kWhite));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            const wxColour fill = !IsEnabled() ? theme::kChromeBg
                                : hover_       ? theme::kChromeBtnHover : theme::kChromeBg;
            gc->SetBrush(wxBrush(fill));
            gc->SetPen(wxPen(theme::kBorderInput, 1));
            gc->DrawRoundedRectangle(0.5, 0.5, std::max(1, sz.x - 1), std::max(1, sz.y - 1), kRadius);
        }

        const wxColour fg = IsEnabled() ? theme::kTextSecondary : theme::kTextGhost;
        dc.SetFont(GetFont());
        dc.SetTextForeground(fg);
        const int textRight = sz.x - kRightPad - kTriW - kGap;
        wxCoord tw = 0, th = 0; dc.GetTextExtent(text_, &tw, &th);
        dc.SetClippingRegion(kLeftPad, 0, std::max(0, textRight - kLeftPad), sz.y);
        dc.DrawText(text_, kLeftPad, (sz.y - th) / 2 - 1);
        dc.DestroyClippingRegion();

        const int tx = sz.x - kRightPad - kTriW;
        const int ty = (sz.y - kTriH) / 2;
        const wxPoint tri[3] = { wxPoint(tx, ty), wxPoint(tx + kTriW, ty),
                                 wxPoint(tx + kTriW / 2, ty + kTriH) };
        dc.SetBrush(wxBrush(fg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawPolygon(3, tri);
    }

    wxString text_;
    std::function<void()> onClick_;
    bool hover_ = false;
};

} // namespace ui
