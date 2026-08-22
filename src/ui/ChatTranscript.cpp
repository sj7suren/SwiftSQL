// ChatTranscript.cpp — implementation of the virtualized, self-drawn chat transcript.
//
// See ChatTranscript.h for the "why". Key invariants:
//   • blocks_ is parallel to msgs_ and holds each message's laid-out geometry for the
//     current client width (layoutW_). It is rebuilt ONLY on content / width change.
//   • OnPaint applies the scroll transform (DoPrepareDC), computes the visible content
//     y-range, and draws only the blocks that intersect it — O(visible), not O(total).
//   • WrapText word-wraps latin on spaces and CJK per-character (no spaces), honouring
//     explicit '\n'; a too-long unbreakable run is hard-broken so nothing overflows.
#include "ui/ChatTranscript.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/image.h>
#include <wx/menu.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// Geometry tokens (px). Bubble padding / radius echo the previous native bubbles.
constexpr int kSideMargin = 10;   // gap from the transcript edge to a bubble
constexpr int kMsgGap     = 6;    // vertical gap above+below each bubble
constexpr int kPadX       = 12;   // bubble inner horizontal padding
constexpr int kPadY       = 10;   // bubble inner vertical padding
constexpr int kCaptionGap = 4;    // gap between caption and body
constexpr int kRadius     = 14;
constexpr double kMaxWidthFrac = 0.82;  // bubble ≤ 82% of the client width
constexpr int kScrollUnit = 16;   // vertical scroll granularity (px per unit)

bool IsCjk(wxUniChar c)
{
    const unsigned v = c.GetValue();
    return (v >= 0x1100 && v <= 0x11FF) ||   // Hangul Jamo
           (v >= 0x2E80 && v <= 0x9FFF) ||   // CJK radicals … unified ideographs
           (v >= 0xA000 && v <= 0xA4CF) ||   // Yi
           (v >= 0xAC00 && v <= 0xD7A3) ||   // Hangul syllables
           (v >= 0xF900 && v <= 0xFAFF) ||   // CJK compat ideographs
           (v >= 0xFF00 && v <= 0xFFEF) ||   // full-width forms
           (v >= 0x20000 && v <= 0x2FA1F);   // CJK ext B..F
}

// Per-kind bubble style: fill, border, text colour, and right-alignment (user only).
struct Style { wxColour fill, border, caption, body; bool rightAlign; };

Style StyleFor(ChatTranscript::Kind k)
{
    switch (k) {
    case ChatTranscript::Kind::User:
        return { wxColour(0xEA, 0xF3, 0xFF), theme::kBorderSoft,
                 theme::kTextMuted, theme::kText, true };
    case ChatTranscript::Kind::System:
        return { wxColour(0xF0, 0xF4, 0xFB), theme::kBorderSoft,
                 theme::kTextMuted, theme::kTextSecondary, false };
    case ChatTranscript::Kind::Error:
        return { theme::kDiffDelBg, wxColour(0xEE, 0xC9, 0xCC),
                 theme::kDiffDelFg, theme::kDiffDelFg, false };
    case ChatTranscript::Kind::Ai:
    default:
        return { wxColour(0xF7, 0xF9, 0xFC), theme::kBorderSoft,
                 theme::kTextMuted, theme::kText, false };
    }
}

int ClientW(const wxScrolledWindow* w) { return std::max(1, w->GetClientSize().x); }

} // namespace

ChatTranscript::ChatTranscript(wxWindow* parent, const wxSize& size)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, size,
                       wxVSCROLL | wxBORDER_NONE | wxWANTS_CHARS)
    , bodyFont_(ui::Ui(10.5))
    , captionFont_(ui::Ui(9))
{
    SetBackgroundColour(theme::kWhite);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetDoubleBuffered(true);
    // Vertical-only scrolling; pixel-metric rows come from the layout cache.
    EnableScrolling(false, true);
    SetScrollRate(0, kScrollUnit);
    ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);   // hidden bar; wheel/drag still scroll
    SetCursor(wxCursor(wxCURSOR_IBEAM));   // text-selectable surface

    // Measure the two fonts once (line heights are constant per font).
    {
        wxClientDC dc(this);
        dc.SetFont(bodyFont_);
        bodyLineH_ = dc.GetTextExtent(L"Ág中").y + 2;
        spaceW_    = std::max(1, dc.GetTextExtent(L" ").x);
        dc.SetFont(captionFont_);
        captionH_ = dc.GetTextExtent(L"Ág中").y;
    }

    autoScroll_.SetOwner(this);

    Bind(wxEVT_PAINT, &ChatTranscript::OnPaint, this);
    Bind(wxEVT_SIZE, &ChatTranscript::OnSize, this);
    Bind(wxEVT_RIGHT_DOWN, &ChatTranscript::OnRightClick, this);
    Bind(wxEVT_LEFT_DOWN, &ChatTranscript::OnLeftDown, this);
    Bind(wxEVT_MOTION, &ChatTranscript::OnMotion, this);
    Bind(wxEVT_LEFT_UP, &ChatTranscript::OnLeftUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &ChatTranscript::OnCaptureLost, this);
    Bind(wxEVT_CHAR_HOOK, &ChatTranscript::OnCharHook, this);
    Bind(wxEVT_TIMER, &ChatTranscript::OnAutoScrollTimer, this);
}

// client point → index of the message whose block spans that y (or -1).
int ChatTranscript::HitTest(const wxPoint& clientPt) const
{
    int ux = 0, uy = 0;
    CalcUnscrolledPosition(clientPt.x, clientPt.y, &ux, &uy);
    for (size_t i = 0; i < blocks_.size(); ++i)
        if (uy >= blocks_[i].y && uy < blocks_[i].y + blocks_[i].height)
            return static_cast<int>(i);
    return -1;
}

// Right-click → copy the clicked message, or the whole conversation, to the clipboard.
void ChatTranscript::OnRightClick(wxMouseEvent& e)
{
    if (msgs_.empty()) return;
    const int idx = HitTest(e.GetPosition());

    enum { kCopyOne = wxID_HIGHEST + 1, kCopyAll };
    wxMenu menu;
    if (idx >= 0) menu.Append(kCopyOne, tr(L"复制"));
    menu.Append(kCopyAll, tr(L"复制全部对话"));

    const int r = GetPopupMenuSelectionFromUser(menu, e.GetPosition());
    wxString text;
    if (r == kCopyOne && idx >= 0) {
        // With an active free-text selection, "复制" copies the selection; otherwise
        // it falls back to the whole clicked message.
        text = HasSelection() ? SelectedText() : msgs_[static_cast<size_t>(idx)].text;
    } else if (r == kCopyAll) {
        for (const Msg& m : msgs_) {
            if (m.text.IsEmpty()) continue;
            text += m.who + L"：" + m.text + L"\n\n";
        }
        text.Trim();
    }
    if (text.IsEmpty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

// ------------------------------------------------------------- free-text selection

void ChatTranscript::ClearSelection()
{
    anchor_ = focus_ = SelPt{};
}

// Normalize anchor/focus into (start ≤ end) order and clamp both endpoints so callers
// never index past msgs_ / a message's text (content may have shrunk since the drag).
void ChatTranscript::SelRange(SelPt& start, SelPt& end) const
{
    start = anchor_; end = focus_;
    if (end < start) std::swap(start, end);
    const size_t last = msgs_.empty() ? 0 : msgs_.size() - 1;
    auto clamp = [&](SelPt& p) {
        if (p.block > last) { p.block = last; p.off = 0; }
        if (!msgs_.empty()) {
            const int n = static_cast<int>(msgs_[p.block].text.length());
            p.off = std::max(0, std::min(p.off, n));
        } else {
            p.off = 0;
        }
    };
    clamp(start); clamp(end);
}

// client point → nearest character caret. Never returns an out-of-range offset: a click
// in a gap / bubble margin snaps to the closest line's nearest character boundary.
ChatTranscript::SelPt ChatTranscript::HitTestText(const wxPoint& clientPt) const
{
    if (msgs_.empty() || blocks_.empty()) return SelPt{};

    int ux = 0, uy = 0;
    CalcUnscrolledPosition(clientPt.x, clientPt.y, &ux, &uy);

    // Locate the block: first whose bottom is below uy; else the last block.
    size_t b = blocks_.size() - 1;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (uy < blocks_[i].y + blocks_[i].height) { b = i; break; }
    }
    const Block& bl = blocks_[b];
    if (bl.lines.empty())
        return SelPt{ b, static_cast<int>(msgs_[b].text.length()) };

    // Locate the wrapped line within the block by y.
    const int firstLineY = bl.bubble.y + kPadY + captionH_ + kCaptionGap;
    int li = (uy - firstLineY) / bodyLineH_;
    li = std::max(0, std::min(li, static_cast<int>(bl.lines.size()) - 1));
    const WLine& wl = bl.lines[static_cast<size_t>(li)];

    // Locate the nearest character boundary within the line by x.
    const int textX0 = bl.bubble.x + kPadX;
    const int localX = ux - textX0;
    int col = static_cast<int>(wl.text.length());
    if (localX <= 0) {
        col = 0;
    } else if (!wl.text.empty()) {
        wxClientDC dc(const_cast<ChatTranscript*>(this));
        dc.SetFont(bodyFont_);
        wxArrayInt ext;
        if (dc.GetPartialTextExtents(wl.text, ext) && !ext.IsEmpty()) {
            int prev = 0;
            col = static_cast<int>(ext.GetCount());
            for (size_t k = 0; k < ext.GetCount(); ++k) {
                const int cur = ext[k];
                if (localX < (prev + cur) / 2) { col = static_cast<int>(k); break; }
                prev = cur;
            }
        }
    }
    return SelPt{ b, wl.srcStart + col };
}

// Rebuild the selected text from the RAW source slices — never from wrapped display
// lines — so no wrap newline is injected and inter-word spaces survive verbatim.
wxString ChatTranscript::SelectedText() const
{
    if (!HasSelection() || msgs_.empty()) return {};
    SelPt s, e; SelRange(s, e);
    if (s == e) return {};

    if (s.block == e.block)
        return msgs_[s.block].text.Mid(s.off, e.off - s.off);

    wxString out = msgs_[s.block].text.Mid(s.off);
    for (size_t b = s.block + 1; b < e.block; ++b)
        out += L"\n\n" + msgs_[b].text;
    out += L"\n\n" + msgs_[e.block].text.Left(e.off);
    return out;
}

void ChatTranscript::CopySelectionToClipboard()
{
    const wxString text = SelectedText();
    if (text.IsEmpty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

void ChatTranscript::OnLeftDown(wxMouseEvent& e)
{
    SetFocus();   // so wxEVT_CHAR_HOOK (Ctrl+C) reaches this window
    anchor_ = focus_ = HitTestText(e.GetPosition());
    selecting_ = true;
    if (!HasCapture()) CaptureMouse();
    Refresh();
    e.Skip();
}

void ChatTranscript::OnMotion(wxMouseEvent& e)
{
    if (!selecting_ || !e.LeftIsDown()) { e.Skip(); return; }

    focus_ = HitTestText(e.GetPosition());

    // Auto-scroll when the pointer is dragged past the top / bottom edge, so a selection
    // can extend beyond the visible viewport. A cheap repeating timer keeps it going
    // while the button stays held near the edge.
    const int y = e.GetPosition().y;
    const int h = GetClientSize().y;
    if (y < 0 || y > h) {
        if (!autoScroll_.IsRunning()) autoScroll_.Start(40);
    } else {
        autoScroll_.Stop();
    }
    Refresh();
    e.Skip();
}

void ChatTranscript::OnAutoScrollTimer(wxTimerEvent&)
{
    if (!selecting_) { autoScroll_.Stop(); return; }
    const wxPoint p = ScreenToClient(wxGetMousePosition());
    const int h = GetClientSize().y;
    int vx = 0, vy = 0;
    GetViewStart(&vx, &vy);
    if (p.y < 0)      Scroll(vx, std::max(0, vy - 2));
    else if (p.y > h) Scroll(vx, vy + 2);
    else { autoScroll_.Stop(); return; }
    focus_ = HitTestText(p);
    Refresh();
}

void ChatTranscript::OnLeftUp(wxMouseEvent& e)
{
    selecting_ = false;
    autoScroll_.Stop();
    if (HasCapture()) ReleaseMouse();
    Refresh();   // a click with no drag leaves anchor_==focus_ → selection cleared
    e.Skip();
}

void ChatTranscript::OnCaptureLost(wxMouseCaptureLostEvent&)
{
    selecting_ = false;
    autoScroll_.Stop();
}

void ChatTranscript::OnCharHook(wxKeyEvent& e)
{
    if ((e.ControlDown() || e.CmdDown()) &&
        (e.GetKeyCode() == 'C' || e.GetKeyCode() == 'c') && HasSelection()) {
        CopySelectionToClipboard();
        return;   // consume
    }
    e.Skip();
}

// ------------------------------------------------------------------ public API

void ChatTranscript::AddMessage(Kind kind, const wxString& who, const wxString& text)
{
    ClearSelection();   // appended content shifts nothing before it, but keep it simple
    msgs_.push_back(Msg{ kind, who, text });
    blocks_.emplace_back();
    RelayoutFrom(msgs_.size() - 1);
    UpdateVirtualSize();
    Refresh();
    ScrollToBottom();
}

void ChatTranscript::BeginAiStream(const wxString& who)
{
    ClearSelection();
    msgs_.push_back(Msg{ Kind::Ai, who, wxString() });
    blocks_.emplace_back();
    hasStream_ = true;
    streamIdx_ = msgs_.size() - 1;
    RelayoutFrom(streamIdx_);
    UpdateVirtualSize();
    Refresh();
    ScrollToBottom();
}

void ChatTranscript::SetStreamText(const wxString& text)
{
    if (!hasStream_ || streamIdx_ >= msgs_.size()) return;
    if (msgs_[streamIdx_].text == text) return;
    ClearSelection();   // the streaming block's wrap/offsets change under the selection
    msgs_[streamIdx_].text = text;
    RelayoutFrom(streamIdx_);   // only this block + the y-cascade after it
    UpdateVirtualSize();
    Refresh();
    ScrollToBottom();
}

void ChatTranscript::FinishStream()
{
    hasStream_ = false;
}

// Attach a diagram to the last message (typically the just-finished AI bubble). The
// bubble grows to fit it; relayout is limited to that block + the y-cascade after it.
void ChatTranscript::SetLastDiagram(const wxBitmap& bmp)
{
    if (msgs_.empty() || !bmp.IsOk()) return;
    const size_t idx = msgs_.size() - 1;
    msgs_[idx].diagram = bmp;
    RelayoutFrom(idx);
    UpdateVirtualSize();
    Refresh();
    ScrollToBottom();
}

// Append an image-only message (no caption / body text) carrying `bmp`. Skipped by the
// "copy" menus (they ignore empty-text messages), so it never pollutes copied text.
void ChatTranscript::AddDiagram(const wxBitmap& bmp)
{
    if (!bmp.IsOk()) return;
    ClearSelection();
    Msg m{ Kind::Ai, wxString(), wxString(), bmp };
    msgs_.push_back(std::move(m));
    blocks_.emplace_back();
    RelayoutFrom(msgs_.size() - 1);
    UpdateVirtualSize();
    Refresh();
    ScrollToBottom();
}

void ChatTranscript::Clear()
{
    ClearSelection();
    msgs_.clear();
    blocks_.clear();
    contentH_  = 0;
    hasStream_ = false;
    streamIdx_ = 0;
    UpdateVirtualSize();
    Scroll(0, 0);
    Refresh();
}

void ChatTranscript::ScrollToBottom()
{
    // Defer so the just-updated virtual size / new client width is in effect.
    CallAfter([this] {
        const int units = (contentH_ + kScrollUnit - 1) / kScrollUnit;
        Scroll(0, units);   // wxScrolledWindow clamps to the real maximum
    });
}

// ------------------------------------------------------------------ layout

void ChatTranscript::RelayoutAll()
{
    const int clientW = ClientW(this);
    layoutW_ = clientW;
    wxClientDC dc(this);
    int y = kMsgGap;
    for (size_t i = 0; i < msgs_.size(); ++i) {
        blocks_[i].y = y;
        LayoutOne(dc, i, clientW);
        y += blocks_[i].height;
    }
    contentH_ = std::max(0, y);
}

void ChatTranscript::RelayoutFrom(size_t first)
{
    const int clientW = ClientW(this);
    // A width change (or first ever layout) invalidates every cached block.
    if (clientW != layoutW_ || first == 0) { RelayoutAll(); return; }

    wxClientDC dc(this);
    // Resume the y-cursor at the END of the previous (already-valid) block — the new /
    // edited block's own cached y may be stale (0 for a freshly appended message).
    int y = blocks_[first - 1].y + blocks_[first - 1].height;
    for (size_t i = first; i < msgs_.size(); ++i) {
        blocks_[i].y = y;
        LayoutOne(dc, i, clientW);
        y += blocks_[i].height;
    }
    contentH_ = std::max(0, y);
}

// Compute one message's wrapped lines + bubble rect at content-y blocks_[i].y.
void ChatTranscript::LayoutOne(wxDC& dc, size_t i, int clientW)
{
    const Msg&   m  = msgs_[i];
    Block&       bl = blocks_[i];
    const Style  st = StyleFor(m.kind);

    const int maxBubbleW = std::max(80, static_cast<int>(clientW * kMaxWidthFrac));
    const int maxTextW   = std::max(20, maxBubbleW - 2 * kPadX);

    dc.SetFont(bodyFont_);
    bl.lines = WrapText(dc, m.text, maxTextW);

    // Actual content width = widest of (caption, body lines), capped at maxTextW.
    dc.SetFont(captionFont_);
    int contentW = dc.GetTextExtent(m.who).x;
    dc.SetFont(bodyFont_);
    for (const WLine& ln : bl.lines)
        contentW = std::max(contentW, dc.GetTextExtent(ln.text).x);

    // Diagram (optional): scale to fit the text column, growing the bubble as needed.
    // Pre-scale here (layout is infrequent) so OnPaint stays a plain, cheap DrawBitmap.
    int diagW = 0, diagH = 0;
    bl.diagramBmp = wxBitmap();
    if (m.diagram.IsOk() && m.diagram.GetWidth() > 0 && m.diagram.GetHeight() > 0) {
        const int bw = m.diagram.GetWidth();
        const int bh = m.diagram.GetHeight();
        if (bw > maxTextW) {                                   // too wide → scale down
            const double s  = static_cast<double>(maxTextW) / bw;
            const int    sw = maxTextW;
            const int    sh = std::max(1, static_cast<int>(bh * s + 0.5));
            wxImage img = m.diagram.ConvertToImage();
            if (img.IsOk()) {
                bl.diagramBmp = wxBitmap(img.Scale(sw, sh, wxIMAGE_QUALITY_HIGH));
                if (bl.diagramBmp.IsOk()) { diagW = sw; diagH = sh; }
            }
        } else {                                               // fits → blit native size
            bl.diagramBmp = m.diagram;
            diagW = bw; diagH = bh;
        }
        if (diagW > 0) contentW = std::max(contentW, diagW);
    }
    contentW = std::min(contentW, maxTextW);

    const int bubbleW = contentW + 2 * kPadX;
    const int linesH  = static_cast<int>(bl.lines.size()) * bodyLineH_;
    int bubbleH = kPadY + captionH_ + kCaptionGap + linesH;
    if (diagH > 0) bubbleH += (linesH > 0 ? kCaptionGap : 0) + diagH;
    bubbleH += kPadY;

    const int bx = st.rightAlign ? (clientW - kSideMargin - bubbleW) : kSideMargin;
    bl.bubble = wxRect(std::max(kSideMargin, bx), bl.y, bubbleW, bubbleH);

    if (diagH > 0) {
        const int dx = bl.bubble.x + (bubbleW - diagW) / 2;    // centred in the bubble
        const int dy = bl.bubble.y + kPadY + captionH_ + kCaptionGap + linesH
                       + (linesH > 0 ? kCaptionGap : 0);
        bl.diagram = wxRect(dx, dy, diagW, diagH);
    } else {
        bl.diagram = wxRect();
    }

    bl.height = bubbleH + 2 * kMsgGap;   // gap above (already in y start) + below
}

void ChatTranscript::UpdateVirtualSize()
{
    SetVirtualSize(ClientW(this), contentH_);
}

// Greedy word-wrap: latin breaks on spaces, CJK breaks per-character (no spaces),
// explicit '\n' forces a break, and an unbreakable run wider than maxW is hard-split
// so nothing ever overflows the bubble. O(n) via a per-call single-char width cache.
//
// Every emitted line also records where it starts in the RAW source text. Invariant:
// while a line is being built, `cur == text.substr(lineSrc, i - lineSrc)` — each char
// appended advances i by one, so column c in `cur` maps to raw offset lineSrc + c. A
// trailing wrap space is trimmed from the visible text but the source cursor still steps
// over it (it belongs to no visible line and reappears only via the raw-substr copy).
std::vector<ChatTranscript::WLine>
ChatTranscript::WrapText(wxDC& dc, const wxString& text, int maxW) const
{
    std::vector<WLine> lines;
    if (maxW < 1) maxW = 1;

    std::unordered_map<unsigned, int> cw;   // char → pixel width (this DC/font)
    auto charW = [&](wxUniChar c) -> int {
        const unsigned key = c.GetValue();
        auto it = cw.find(key);
        if (it != cw.end()) return it->second;
        const int w = dc.GetTextExtent(wxString(c)).x;
        cw.emplace(key, w);
        return w;
    };
    auto emit = [&](const wxString& s, int srcStart) {
        WLine wl; wl.text = s; wl.srcStart = srcStart;
        wl.srcLen = static_cast<int>(s.length());
        lines.push_back(std::move(wl));
    };

    const size_t n = text.length();
    wxString cur;
    int curW = 0;
    int breakAt = -1;    // char index in `cur` where a wrap is allowed
    int lineSrc = 0;     // raw offset where `cur` (the current line) begins
    size_t i = 0;
    while (i < n) {
        const wxUniChar c = text[i];
        if (c == '\n') {
            emit(cur, lineSrc);
            cur.clear(); curW = 0; breakAt = -1;
            ++i; lineSrc = static_cast<int>(i);   // skip the newline itself
            continue;
        }
        const int w = charW(c);
        const bool cjk = IsCjk(c);
        if (curW + w <= maxW) {
            if (cjk && !cur.empty()) breakAt = static_cast<int>(cur.length());  // break before
            cur += c; curW += w;
            if (c == ' ' || cjk) breakAt = static_cast<int>(cur.length());       // break after
            ++i;
            continue;
        }
        // Overflow → emit a line and reprocess `c` on the next.
        if (cur.empty()) {                       // single glyph wider than maxW
            cur += c; emit(cur, lineSrc);
            cur.clear(); curW = 0; breakAt = -1;
            ++i; lineSrc = static_cast<int>(i);
            continue;
        }
        if (breakAt > 0 && breakAt < static_cast<int>(cur.length())) {
            wxString head = cur.Left(breakAt);
            wxString tail = cur.Mid(breakAt);
            head.Trim(true);                     // drop the trailing break space(s)
            emit(head, lineSrc);
            cur = tail;
            lineSrc += breakAt;                  // raw cursor steps over head + trimmed space
            curW = 0;
            for (size_t k = 0; k < cur.length(); ++k) curW += charW(cur[k]);
        } else {                                  // no break point → hard split
            emit(cur, lineSrc);
            lineSrc += static_cast<int>(cur.length());
            cur.clear(); curW = 0;
        }
        breakAt = -1;
        // note: i not advanced — `c` is reconsidered against the fresh line
    }
    emit(cur, lineSrc);
    return lines;
}

// ------------------------------------------------------------------ paint

void ChatTranscript::OnSize(wxSizeEvent& e)
{
    // Width change → reflow every block against the new width, then repaint. The wrap
    // (and thus per-line column mapping) changes, so drop the selection to stay safe.
    if (ClientW(this) != layoutW_) {
        ClearSelection();
        RelayoutAll();
        UpdateVirtualSize();
        Refresh();
    }
    e.Skip();
}

void ChatTranscript::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc);   // apply the scroll transform → draw in content coords

    dc.SetBackground(wxBrush(theme::kWhite));
    dc.Clear();

    if (msgs_.empty()) return;

    // Visible content-y range (accounts for the current scroll offset).
    int ox = 0, oy = 0;
    CalcUnscrolledPosition(0, 0, &ox, &oy);
    const int top    = oy;
    const int bottom = oy + GetClientSize().y;

    // Resolve the selection span once; only visible blocks are ever inspected below,
    // so highlight cost stays O(visible), matching the paint loop.
    const bool haveSel = HasSelection();
    SelPt selS, selE;
    if (haveSel) SelRange(selS, selE);

    dc.SetBackgroundMode(wxTRANSPARENT);   // text never paints over the selection wash

    for (size_t i = 0; i < msgs_.size(); ++i) {
        const Block& bl = blocks_[i];
        if (bl.y + bl.height <= top) continue;   // fully above the viewport
        if (bl.y >= bottom) break;               // below — blocks are y-ordered

        const Style st = StyleFor(msgs_[i].kind);
        const wxRect& b = bl.bubble;

        dc.SetBrush(wxBrush(st.fill));
        dc.SetPen(wxPen(st.border, 1));
        dc.DrawRoundedRectangle(b.x, b.y, b.width, b.height, kRadius);

        dc.SetFont(captionFont_);
        dc.SetTextForeground(st.caption);
        dc.DrawText(msgs_[i].who, b.x + kPadX, b.y + kPadY);

        dc.SetFont(bodyFont_);
        const bool blockSel = haveSel && i >= selS.block && i <= selE.block;
        const int msgLen = static_cast<int>(msgs_[i].text.length());
        const int selLo  = (i == selS.block) ? selS.off : 0;       // block-relative
        const int selHi  = (i == selE.block) ? selE.off : msgLen;

        const int textX0 = b.x + kPadX;
        int ly = b.y + kPadY + captionH_ + kCaptionGap;
        for (const WLine& ln : bl.lines) {
            // Selection wash (behind the glyphs) for the covered column range.
            if (blockSel) {
                const int ls = ln.srcStart;
                const int le = ln.srcStart + ln.srcLen;
                const int a  = std::max(selLo, ls);
                const int bnd= std::min(selHi, le);
                const bool continues = (i < selE.block) || (selHi > le);
                const bool covered   = (ln.srcLen == 0) && (selLo <= ls) && continues;
                if (a < bnd || covered) {
                    wxArrayInt ext;
                    if (!ln.text.empty())
                        dc.GetPartialTextExtents(ln.text, ext);
                    auto atCol = [&](int c) -> int {
                        if (c <= 0 || ext.IsEmpty()) return 0;
                        if (c > static_cast<int>(ext.GetCount())) c = static_cast<int>(ext.GetCount());
                        return ext[static_cast<size_t>(c) - 1];
                    };
                    int xs, xe;
                    if (covered) {                       // blank line inside the selection
                        xs = 0; xe = spaceW_;
                    } else {
                        xs = atCol(a - ls);
                        xe = continues ? atCol(ln.srcLen) + spaceW_   // trailing newline hint
                                       : atCol(bnd - ls);
                    }
                    if (xe > xs) {
                        dc.SetBrush(wxBrush(theme::kSelectionBg));
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.DrawRectangle(textX0 + xs, ly, xe - xs, bodyLineH_);
                    }
                }
            }
            dc.SetTextForeground(st.body);
            dc.DrawText(ln.text, textX0, ly);
            ly += bodyLineH_;
        }

        // Diagram sits under the text, inside the bubble. Only visible blocks reach
        // here, so virtualization is preserved. The image never joins the text selection.
        if (bl.diagramBmp.IsOk() && !bl.diagram.IsEmpty())
            dc.DrawBitmap(bl.diagramBmp, bl.diagram.x, bl.diagram.y, true);
    }
}

} // namespace ui
