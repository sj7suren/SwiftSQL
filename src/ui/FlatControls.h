// FlatControls.h — small self-drawn, borderless controls shared by the filter and
// sort panels so both read as a flat "no frame, just text / icon" surface.
//
//   InlineDropdown : a "label ▾" segment with a thin bottom underline; click opens
//                    a picker / menu. Fits its content width. Hover tints text+line.
//   UnderlineText  : a frameless wxTextCtrl over a thin bottom underline (primary on
//                    focus).
//   FlatButton     : a borderless text/icon button (no button face); keeps its
//                    semantic colour, lifts to a light hover tint so it still reads
//                    as clickable.
//   BoxDropdown    : a fixed-width "white box ▾" selector — white fill, a thin 1px
//                    light-grey rounded border, a vector chevron on the right. Reads
//                    like a clean input field floating on the grey chrome bar (used
//                    for the SQL/object editor's 连接 / 数据库 selectors, replacing the
//                    native wxChoice whose closed field the MSW theme paints itself
//                    and refuses to whiten). Clicking opens a wxMenu below it.
//
// Header-only: every method is defined in-class (implicitly inline), so it can be
// included from several translation units. Colours come from Theme tokens only.
#pragma once

#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>
#include <wx/menu.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

// ---------------------------------------------------------------------------
class InlineDropdown : public wxPanel {
public:
    InlineDropdown(wxWindow* parent, std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY), onClick_(std::move(onClick))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(theme::kMenuBg);
        SetFont(ui::Ui(9));
        Bind(wxEVT_PAINT, &InlineDropdown::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (enabled_ && onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }
    void SetText(const wxString& s)
    {
        text_ = s;
        // Fit to content: text width + the ▾ triangle area (gap + triangle + pad).
        // The chevron is drawn as a vector below, not a glyph, so no font dependency.
        const wxSize e = GetTextExtent(s);
        SetMinSize(wxSize(e.x + kLeftPad + kGap + kTriW + kRightPad, e.y + 8));
        InvalidateBestSize();
        if (GetParent()) GetParent()->Layout();
        Refresh();
    }
    void SetEnabledLook(bool on) { enabled_ = on; Refresh(); }
private:
    static constexpr int kLeftPad = 2, kGap = 5, kTriW = 8, kTriH = 5, kRightPad = 5;

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBrush(wxBrush(theme::kMenuBg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        dc.SetFont(GetFont());
        const wxColour fg = !enabled_ ? theme::kTextGhost
                          : hover_     ? theme::kPrimary : theme::kText;
        dc.SetTextForeground(fg);
        wxCoord tw = 0, th = 0; dc.GetTextExtent(text_, &tw, &th);
        dc.DrawText(text_, kLeftPad, (sz.y - th) / 2 - 1);
        // ▾ as a filled vector down-triangle (font-independent), colour = text colour.
        const int tx = kLeftPad + tw + kGap;
        const int ty = (sz.y - kTriH) / 2;
        const wxPoint tri[3] = { wxPoint(tx, ty), wxPoint(tx + kTriW, ty),
                                 wxPoint(tx + kTriW / 2, ty + kTriH) };
        dc.SetBrush(wxBrush(fg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawPolygon(3, tri);
        dc.SetPen(wxPen(hover_ && enabled_ ? theme::kPrimary : theme::kBorderInput, 1));
        dc.DrawLine(0, sz.y - 1, sz.x, sz.y - 1);   // the "underline"
    }
    std::function<void()> onClick_;
    wxString text_;
    bool hover_ = false, enabled_ = true;
};

// ---------------------------------------------------------------------------
class UnderlineText : public wxPanel {
public:
    explicit UnderlineText(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(theme::kWhite);
        auto* s = new wxBoxSizer(wxVERTICAL);
        text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxDefaultSize, wxBORDER_NONE | wxTE_PROCESS_ENTER);
        text_->SetBackgroundColour(theme::kWhite);
        s->Add(text_, 1, wxEXPAND);
        s->AddSpacer(2);                          // leave the bottom row for the line
        SetSizer(s);
        SetMinSize(wxSize(110, 24));
        text_->Bind(wxEVT_SET_FOCUS,  [this](wxFocusEvent& e) { focus_ = true;  Refresh(); e.Skip(); });
        text_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { focus_ = false; Refresh(); e.Skip(); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(this);
            const wxSize sz = GetClientSize();
            dc.SetPen(wxPen(focus_ ? theme::kPrimary : theme::kBorderInput, 1));
            dc.DrawLine(0, sz.y - 1, sz.x, sz.y - 1);
        });
    }
    wxTextCtrl* Text() const { return text_; }
private:
    wxTextCtrl* text_ = nullptr;
    bool focus_ = false;
};

// ---------------------------------------------------------------------------
class FlatButton : public wxPanel {
public:
    FlatButton(wxWindow* parent, const wxString& text, const wxColour& fg,
               std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY), text_(text), fg_(fg), onClick_(std::move(onClick))
    { Init(); }
    FlatButton(wxWindow* parent, icons::Glyph g, const wxColour& fg,
               std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY), glyph_(g), useGlyph_(true), fg_(fg),
          onClick_(std::move(onClick))
    { Init(); }
    // Glyph + text: icon on the left, label to its right (a labelled toolbar button).
    FlatButton(wxWindow* parent, icons::Glyph g, const wxString& text, const wxColour& fg,
               std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY), text_(text), glyph_(g), useGlyph_(true), fg_(fg),
          onClick_(std::move(onClick))
    { Init(); }

    void SetText(const wxString& s) { text_ = s; FitToContent(); Refresh(); }
    void SetToolTipText(const wxString& s) { SetToolTip(s); }
    void SetEnabledLook(bool on) { enabled_ = on; Refresh(); }

private:
    void Init()
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(theme::kMenuBg);
        SetFont(ui::Ui(9));
        FitToContent();
        Bind(wxEVT_PAINT, &FlatButton::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (enabled_ && onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }
    static constexpr int kGlyphPx = 16, kIconTextGap = 5;

    void FitToContent()
    {
        int w, h;
        if (useGlyph_ && !text_.IsEmpty()) {          // glyph + label
            const wxSize e = GetTextExtent(text_);
            w = 8 + kGlyphPx + kIconTextGap + e.x + 8;
            h = std::max(26, e.y + 8);
        } else if (useGlyph_) {                        // glyph only
            w = 28; h = 26;
        } else {                                       // text only
            const wxSize e = GetTextExtent(text_.IsEmpty() ? wxString(L"M") : text_);
            w = e.x + 14; h = e.y + 8;
        }
        SetMinSize(wxSize(w, h));
        InvalidateBestSize();
        if (GetParent()) GetParent()->Layout();
    }
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBrush(wxBrush(hover_ && enabled_ ? theme::kChromeBtnHover : theme::kMenuBg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        const wxColour c = enabled_ ? fg_ : theme::kTextGhost;
        if (useGlyph_ && !text_.IsEmpty()) {
            const int gy = (sz.y - kGlyphPx) / 2;
            dc.DrawBitmap(icons::Stroke(glyph_, kGlyphPx, c, 1.8), 8, gy, true);
            dc.SetFont(GetFont());
            dc.SetTextForeground(c);
            wxCoord tw = 0, th = 0; dc.GetTextExtent(text_, &tw, &th);
            dc.DrawText(text_, 8 + kGlyphPx + kIconTextGap, (sz.y - th) / 2 - 1);
        } else if (useGlyph_) {
            dc.DrawBitmap(icons::Stroke(glyph_, kGlyphPx, c, 1.8),
                          (sz.x - kGlyphPx) / 2, (sz.y - kGlyphPx) / 2, true);
        } else {
            dc.SetFont(GetFont());
            dc.SetTextForeground(c);
            wxCoord tw = 0, th = 0; dc.GetTextExtent(text_, &tw, &th);
            dc.DrawText(text_, (sz.x - tw) / 2, (sz.y - th) / 2 - 1);
        }
    }
    wxString     text_;
    icons::Glyph glyph_ = icons::Glyph::Plus;
    bool         useGlyph_ = false;
    wxColour     fg_;
    std::function<void()> onClick_;
    bool hover_ = false, enabled_ = true;
};

// ---------------------------------------------------------------------------
// BoxDropdown — a self-drawn, fixed-width selector that reads as a clean white
// input box (white fill + thin light-grey rounded border + a vector chevron)
// floating on the grey chrome bar. Drop-in for the couple of wxChoice methods the
// editor selectors use. Opening the list goes through wxMenu + the synchronous
// GetPopupMenuSelectionFromUser — never wxComboBox::Popup() (a known crash path).
class BoxDropdown : public wxPanel {
public:
    BoxDropdown(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(theme::kChromeBg);   // behind the rounded corners
        SetFont(ui::Ui(9));
        const int w = size.x > 0 ? size.x : 160;
        SetMinSize(wxSize(w, kBoxH));
        Bind(wxEVT_PAINT, &BoxDropdown::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &BoxDropdown::OnClick, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }

    // ---- wxChoice-compatible surface (only the members the editor uses) ----
    int Append(const wxString& label, void* clientData = nullptr)
    {
        items_.emplace_back(label, clientData);
        Refresh();
        return static_cast<int>(items_.size()) - 1;
    }
    void Clear() { items_.clear(); sel_ = wxNOT_FOUND; Refresh(); }
    unsigned GetCount() const { return static_cast<unsigned>(items_.size()); }
    int GetSelection() const { return sel_; }
    void SetSelection(int i)
    {
        sel_ = (i >= 0 && i < static_cast<int>(items_.size())) ? i : wxNOT_FOUND;
        Refresh();
    }
    void* GetClientData(int i) const
    {
        return (i >= 0 && i < static_cast<int>(items_.size())) ? items_[i].second : nullptr;
    }
    wxString GetString(int i) const
    {
        return (i >= 0 && i < static_cast<int>(items_.size())) ? items_[i].first : wxString();
    }
    void SetOnChange(std::function<void()> cb) { onChange_ = std::move(cb); }
    void SetEnabledLook(bool on) { enabled_ = on; Refresh(); }

    // Honour wxWindow enable state too: callers (e.g. the AI panel) disable the
    // selectors while busy via Enable(false). A disabled wxPanel does not repaint
    // itself, so force a redraw here; painting/click both fall back to IsEnabled().
    bool Enable(bool enable = true) override
    {
        const bool ok = wxPanel::Enable(enable);
        Refresh();
        return ok;
    }

private:
    static constexpr int kBoxH = 26, kRadius = 4, kLeftPad = 9, kGap = 6,
                         kTriW = 8, kTriH = 5, kRightPad = 9;

    // Enabled look requires BOTH the explicit look flag and the real window state.
    bool IsLive() const { return enabled_ && IsEnabled(); }

    void OnClick(wxMouseEvent&)
    {
        if (!IsLive() || items_.empty()) return;
        // A radio-checked wxMenu shows the current pick; the synchronous helper
        // returns the chosen id (or wxID_NONE) without any lingering event binding.
        wxMenu menu;
        const int id0 = wxID_HIGHEST + 1;
        for (size_t i = 0; i < items_.size(); ++i) {
            wxMenuItem* it = menu.AppendRadioItem(id0 + static_cast<int>(i), items_[i].first);
            if (static_cast<int>(i) == sel_) it->Check(true);
        }
        const int r = GetPopupMenuSelectionFromUser(menu, wxPoint(0, GetSize().y));
        if (r == wxID_NONE) return;
        const int idx = r - id0;
        if (idx >= 0 && idx < static_cast<int>(items_.size()) && idx != sel_) {
            sel_ = idx;
            Refresh();
            if (onChange_) onChange_();
        }
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        const bool en = IsLive();
        // Fill with the chrome bg so the rounded corners blend into the bar.
        dc.SetBrush(wxBrush(theme::kChromeBg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        // White field + 1px light-grey rounded border (deepens a touch on hover).
        const wxColour border = !en    ? theme::kBorder
                              : hover_  ? theme::kTextGhost : theme::kBorderInput;
        dc.SetBrush(wxBrush(theme::kWhite));
        dc.SetPen(wxPen(border, 1));
        dc.DrawRoundedRectangle(0, 0, sz.x - 1, sz.y - 1, kRadius);

        const wxColour fg = en ? theme::kText : theme::kTextGhost;
        // Label, clipped so a long name never runs under the chevron.
        const int textRight = sz.x - kRightPad - kTriW - kGap;
        dc.SetFont(GetFont());
        dc.SetTextForeground(fg);
        const wxString label = sel_ != wxNOT_FOUND ? items_[sel_].first : wxString();
        if (!label.IsEmpty()) {
            dc.SetClippingRegion(kLeftPad, 0, std::max(0, textRight - kLeftPad), sz.y);
            wxCoord tw = 0, th = 0; dc.GetTextExtent(label, &tw, &th);
            dc.DrawText(label, kLeftPad, (sz.y - th) / 2 - 1);
            dc.DestroyClippingRegion();
        }
        // ▾ vector chevron on the right (font-independent; lighter when disabled).
        const int tx = sz.x - kRightPad - kTriW;
        const int ty = (sz.y - kTriH) / 2;
        const wxPoint tri[3] = { wxPoint(tx, ty), wxPoint(tx + kTriW, ty),
                                 wxPoint(tx + kTriW / 2, ty + kTriH) };
        dc.SetBrush(wxBrush(en ? theme::kTextSecondary : theme::kTextGhost));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawPolygon(3, tri);
    }

    std::vector<std::pair<wxString, void*>> items_;
    int  sel_ = wxNOT_FOUND;
    std::function<void()> onChange_;
    bool hover_ = false, enabled_ = true;
};

} // namespace ui
