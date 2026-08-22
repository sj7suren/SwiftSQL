// EditorTabArt.cpp — see EditorTabArt.h for the design summary.
//
// The whole tab is self-drawn (base wxAuiSimpleTabArt::DrawTab is NOT called) so we
// control every pixel: flat grey strip, rectangular tabs, blue top accent + dot on
// the selected tab, grey icon/text otherwise, and a close X that appears only on the
// hovered tab. GetTabSize + DrawTab share MeasureWidth() so the measured and drawn
// widths always agree (mismatched widths would overlap or gap the tabs).
//
// Three tab states, and how they are told apart:
//   • selected      = page.active                  → white body + 2px blue top rule + dot
//   • hovered        = page.hover (wxAui keeps this per-tab, updated on mouse motion)
//                      → shows the close X (replaces the dot in the trailing slot)
//   • close-hot      = closeButtonState & HOVER/PRESSED (mouse over the X itself)
//                      → the X darkens
//
// GHOST-HIT SAFETY (this notebook once mis-closed the wrong tab): *outButtonRect is
// written NON-empty for AT MOST ONE tab — the currently hovered, closable, non-special
// tab. Every other tab (idle tabs, the selected tab showing its dot, and the pinned
// "总览" tab) gets an empty rect, which wxAuiTabContainer::ButtonHitTest can never
// match. Because wxAui sets exactly one page.hover at a time, at most one live close
// rect can exist, so a click can only ever close the tab the X is actually drawn on.
#include "ui/EditorTabArt.h"

#include "ui/Theme.h"
#include "ui/UiFonts.h"

#include <wx/dc.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/window.h>

#include <algorithm>
#include <memory>

namespace ui {

namespace {

// Recolour a single-colour stroke glyph to `c`, preserving its anti-aliased alpha
// (the icon's shape is carried entirely by the alpha channel, so overwriting RGB
// just re-tints the silhouette). Used to flip tab icons blue↔grey per state.
wxBitmap TintToColour(const wxBitmap& src, const wxColour& c)
{
    if (!src.IsOk()) return src;
    wxImage img = src.ConvertToImage();
    if (!img.IsOk()) return src;
    if (!img.HasAlpha()) return src;         // no alpha → can't safely re-tint; leave as-is
    const int n = img.GetWidth() * img.GetHeight();
    unsigned char* d = img.GetData();
    for (int i = 0; i < n; ++i) {
        d[3 * i + 0] = c.Red();
        d[3 * i + 1] = c.Green();
        d[3 * i + 2] = c.Blue();
    }
    return wxBitmap(img);
}

// Prototype colours without a theme token (docs/UI/SwiftSql.dc.html:357-370).
const wxColour kIconIdle(0x9A, 0xA1, 0xAD);   // #9AA1AD unselected icon + idle close X
const wxColour kTextIdle(0x6B, 0x72, 0x80);   // #6B7280 unselected caption + hot close X

} // namespace

int EditorTabArt::MeasureWidth(wxWindow* wnd, const wxString& caption) const
{
    const int padX    = wnd->FromDIP(18);
    const int iconSz  = wnd->FromDIP(14);
    const int gapIT   = wnd->FromDIP(9);
    const int endGap  = wnd->FromDIP(6);
    const int endSlot = wnd->FromDIP(16);

    // Measure with the SELECTED (bold) face — the widest a caption can render — so the
    // tab never re-flows or chops when a tab becomes active and its text goes bold.
    wxFont f = Ui(9.5, /*bold*/ true);
    int tw = 0, th = 0;
    wnd->GetTextExtent(caption, &tw, &th, nullptr, nullptr, &f);

    return padX + iconSz + gapIT + tw + endGap + endSlot + padX;
}

void EditorTabArt::DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect)
{
    // Flat grey strip (#EEF0F4) + a 1px bottom rule (#E1E5EB) across the full width.
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(theme::kTabStripBg));
    dc.DrawRectangle(rect);
    dc.SetPen(wxPen(theme::kBorder, wnd->FromDIP(1)));
    dc.DrawLine(rect.x, rect.y + rect.height - 1,
                rect.x + rect.width, rect.y + rect.height - 1);
}

wxSize EditorTabArt::GetTabSize(wxDC& /*dc*/, wxWindow* wnd, const wxString& caption,
                                const wxBitmapBundle& /*bitmap*/, bool /*active*/,
                                int /*closeButtonState*/, int* xExtent)
{
    const int w = MeasureWidth(wnd, caption);
    *xExtent = w;                       // rectangular tabs do not overlap → extent == width
    return wxSize(w, wnd->FromDIP(40));
}

int EditorTabArt::GetBestTabCtrlSize(wxWindow* wnd, const wxAuiNotebookPageArray& /*pages*/,
                                     const wxSize& /*requiredBmpSize*/)
{
    return wnd->FromDIP(40);            // 40px tall strip (prototype)
}

void EditorTabArt::DrawTab(wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page,
                           const wxRect& inRect, int closeButtonState,
                           wxRect* outTabRect, wxRect* outButtonRect, int* xExtent)
{
    const bool special  = IsSpecial(page.window);
    const bool selected = page.active;
    const bool closable = !special;                    // pinned "总览" is never closable
    const bool tabHover = page.hover;                  // wxAui maintains this per-tab
    const bool closeHot = (closeButtonState & wxAUI_BUTTON_STATE_HOVER) ||
                          (closeButtonState & wxAUI_BUTTON_STATE_PRESSED);
    const bool showX    = tabHover && closable;        // X only on the hovered closable tab
    const bool showDot  = selected && !showX;          // dot when selected & not showing X

    const int padX    = wnd->FromDIP(18);
    const int iconSz  = wnd->FromDIP(14);
    const int gapIT   = wnd->FromDIP(9);
    const int endSlot = wnd->FromDIP(16);
    const int tabW    = MeasureWidth(wnd, page.caption);
    const int x = inRect.x, y = inRect.y, h = inRect.height;

    // Per-tab custom colour (right-click → 设置颜色); brand blue is the default accent.
    wxColour custom;
    if (colors_ && page.window) {
        auto it = colors_->find(page.window);
        if (it != colors_->end() && it->second.IsOk()) custom = it->second;
    }
    const wxColour accent = custom.IsOk() ? custom : theme::kPrimary;

    // DrawTab hands us an abstract wxDC& (an auto-buffered paint DC underneath);
    // CreateFromUnknownDC is the factory that accepts the base type.
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (gc) {
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->Clip(x, y, std::min(tabW, inRect.width), h);   // keep drawing inside this tab

        // Selected tab: white body + 2px top accent rule. Unselected: leave the grey
        // strip painted by DrawBackground showing through.
        if (selected) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(theme::kWhite));
            gc->DrawRectangle(x, y, tabW, h);
            gc->SetBrush(wxBrush(accent));
            gc->DrawRectangle(x, y, tabW, wnd->FromDIP(2));
        }

        // Right 1px divider (#E1E5EB), inset top/bottom like the prototype.
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kBorder).Width(1.0)));
        gc->StrokeLine(x + tabW - 0.5, y + wnd->FromDIP(8),
                       x + tabW - 0.5, y + h - wnd->FromDIP(8));

        // Icon — re-tinted: accent when selected, grey (or custom) when not.
        wxBitmap bmp = page.bitmap.GetBitmapFor(wnd);
        if (bmp.IsOk()) {
            const wxColour ic = selected ? accent
                                         : (custom.IsOk() ? custom : kIconIdle);
            gc->DrawBitmap(TintToColour(bmp, ic), x + padX, y + (h - iconSz) / 2,
                           iconSz, iconSz);
        }

        // Caption — bold ink when selected, regular grey otherwise.
        gc->SetFont(Ui(9.5, selected), selected ? theme::kText : kTextIdle);
        double tw = 0, th = 0;
        gc->GetTextExtent(page.caption, &tw, &th);
        gc->DrawText(page.caption, x + padX + iconSz + gapIT, y + (h - th) / 2.0);

        // Trailing slot: close X (hovered) OR blue dot (selected) — never both.
        const double scx = x + tabW - padX - endSlot / 2.0;
        const double scy = y + h / 2.0;
        if (showX) {
            const wxColour xc = closeHot ? kTextIdle : kIconIdle;
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(xc).Width(1.4)));
            const double r = wnd->FromDIP(4);
            gc->StrokeLine(scx - r, scy - r, scx + r, scy + r);
            gc->StrokeLine(scx - r, scy + r, scx + r, scy - r);
        } else if (showDot) {
            const double d = wnd->FromDIP(8);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(accent));
            gc->DrawEllipse(scx - d / 2.0, scy - d / 2.0, d, d);
        }

        gc->ResetClip();
    }

    *outTabRect = wxRect(x, y, tabW, h);

    // GHOST-HIT GUARD (see file header): a live close rect exists ONLY for the hovered
    // closable tab; everything else gets an empty rect that can never be hit-tested.
    if (outButtonRect) {
        if (showX) {
            *outButtonRect = wxRect(x + tabW - padX - endSlot, y + (h - endSlot) / 2,
                                    endSlot, endSlot);
        } else {
            *outButtonRect = wxRect();
        }
    }
    *xExtent = tabW;
}

} // namespace ui
