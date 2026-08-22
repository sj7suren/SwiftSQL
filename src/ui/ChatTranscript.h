// ChatTranscript.h — a self-drawn, VIRTUALIZED chat transcript control.
//
// The old transcript stacked native child windows — every message was 4 HWNDs
// (row panel + rounded panel + caption + body wxStaticText). Windows must move and
// repaint each HWND on every scroll, so a long chat scrolled in visible jerks. This
// control owns ZERO child windows: it stores a flat vector of messages, caches one
// laid-out block per message (wrapped body lines + bubble rect + y offset), and in
// OnPaint draws ONLY the messages intersecting the viewport with a plain wxDC. Scroll
// is therefore O(visible messages), independent of the total count — the standard
// chat-app solution.
//
// Layout is recomputed ONLY when content or width changes, never while scrolling.
// Streaming updates re-lay-out just the streaming message (and shift the ones after
// it), so a fast token stream stays cheap.
#pragma once

#include <wx/scrolwin.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/bitmap.h>

#include <vector>

class wxDC;

namespace ui {

class ChatTranscript : public wxScrolledWindow {
public:
    enum class Kind { User, Ai, System, Error };

    explicit ChatTranscript(wxWindow* parent, const wxSize& size = wxDefaultSize);

    // Append a finished message, relay it out, scroll to the bottom.
    void AddMessage(Kind kind, const wxString& who, const wxString& text);

    // Start a live AI reply: appends one (initially empty) message that becomes the
    // "current streaming item". Its text is then driven by SetStreamText.
    void BeginAiStream(const wxString& who);
    // Update the streaming item's text — relays out only that message and after.
    void SetStreamText(const wxString& text);
    // End the streaming item (nothing else changes; the message stays in place).
    void FinishStream();
    bool HasStream() const { return hasStream_; }

    void Clear();
    void ScrollToBottom();

    // Attach a rendered diagram (e.g. a Mermaid flowchart) to the LAST message so it
    // paints inside that bubble, under the text. No-op if the transcript is empty or
    // `bmp` is not Ok. Used for the primary diagram of an AI reply.
    void SetLastDiagram(const wxBitmap& bmp);
    // Append a NEW image-only message carrying `bmp` (no caption text). Used for the
    // 2nd+ diagram in a single reply. No-op if `bmp` is not Ok.
    void AddDiagram(const wxBitmap& bmp);

private:
    struct Msg {
        Kind     kind;
        wxString who;
        wxString text;
        wxBitmap diagram;   // optional rendered diagram (empty = none), drawn under text
    };

    // One wrapped display line, plus the slice of the ORIGINAL msg.text it renders.
    // srcStart/srcLen map every column back to a raw character offset, so hit-testing
    // and copy reconstruct the source verbatim (no injected wrap newlines / lost
    // inter-word spaces). Invariant: srcLen == text.length(); a trailing wrap space is
    // trimmed from `text` and owned by no line (it reappears via the raw-substr copy).
    struct WLine {
        wxString text;
        int      srcStart = 0;   // offset into msg.text where this display line begins
        int      srcLen   = 0;   // == text.length(); column c ↔ raw offset srcStart + c
    };

    // One message's laid-out geometry, valid for `layoutW_`. Recomputed only on
    // content / width change — never touched during a scroll repaint.
    struct Block {
        int                 y = 0;        // top of the block in content coords
        int                 height = 0;   // full block height (incl. vertical gap)
        wxRect              bubble;        // bubble rect in content coords
        std::vector<WLine>  lines;        // wrapped body lines + source offsets
        wxRect              diagram;      // diagram draw rect in content coords (empty = none)
        wxBitmap            diagramBmp;   // the ready-to-blit (pre-scaled) diagram bitmap
    };

    // A caret position inside the transcript: which message + a raw offset into its text.
    struct SelPt {
        size_t block = 0;
        int    off   = 0;
        bool operator==(const SelPt& o) const { return block == o.block && off == o.off; }
        bool operator!=(const SelPt& o) const { return !(*this == o); }
        bool operator<(const SelPt& o) const {
            return block != o.block ? block < o.block : off < o.off;
        }
    };

    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnRightClick(wxMouseEvent&);              // right-click → copy-this / copy-all menu
    int  HitTest(const wxPoint& clientPt) const;   // client pt → message index or -1

    // --- free-text selection ---
    void OnLeftDown(wxMouseEvent&);
    void OnMotion(wxMouseEvent&);
    void OnLeftUp(wxMouseEvent&);
    void OnCaptureLost(wxMouseCaptureLostEvent&);
    void OnCharHook(wxKeyEvent&);
    void OnAutoScrollTimer(wxTimerEvent&);
    SelPt    HitTestText(const wxPoint& clientPt) const;  // client pt → char-level caret
    bool     HasSelection() const { return anchor_ != focus_; }
    wxString SelectedText() const;                        // raw slice of the source text
    void     ClearSelection();
    void     CopySelectionToClipboard();
    void     SelRange(SelPt& start, SelPt& end) const;    // normalized + clamped endpoints

    void RelayoutAll();                    // full reflow for the current client width
    void RelayoutFrom(size_t first);       // reflow message `first`..end (y cascade)
    void LayoutOne(wxDC& dc, size_t i, int clientW);
    void UpdateVirtualSize();
    std::vector<WLine> WrapText(wxDC& dc, const wxString& text, int maxW) const;

    std::vector<Msg>   msgs_;
    std::vector<Block> blocks_;            // parallel to msgs_
    int                contentH_  = 0;     // total content height (px)
    int                layoutW_   = -1;    // client width the cache was built for
    bool               hasStream_ = false;
    size_t             streamIdx_ = 0;     // index of the current streaming message

    SelPt              anchor_;            // selection start (fixed while dragging)
    SelPt              focus_;             // selection end   (follows the mouse)
    bool               selecting_ = false; // a left-drag is in progress
    wxTimer            autoScroll_;        // edge auto-scroll while drag-selecting

    wxFont             bodyFont_;
    wxFont             captionFont_;
    int                bodyLineH_    = 16; // body line height (px, from bodyFont_)
    int                captionH_     = 14; // caption height (px, from captionFont_)
    int                spaceW_       = 4;  // width of a space in bodyFont_ (px)
};

} // namespace ui
