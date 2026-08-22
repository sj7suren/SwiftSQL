// GhostTextOverlay.h — the grey inline suggestion, painted OVER the editor
// rather than inserted into it.
//
// ===========================================================================
// WHY A WINDOW AND NOT TEXT IN THE DOCUMENT
// ===========================================================================
// The obvious way to get grey text sitting after the caret is to insert it into
// the buffer and colour it. Do not. The buffer is read by everything that
// matters — F5 executes GetText(), Ctrl+S saves it, the AI context builder
// feeds it to a model, the dirty flag watches it — so ghost text living in the
// document is one missed cleanup path away from being EXECUTED or SAVED as if
// the user had typed it. There is no way to make that safe by remembering to
// scrub it everywhere; the only safe version is one where it was never in the
// document to begin with.
//
// So the suggestion is painted by a window positioned at the caret. It cannot
// be executed, cannot be saved, cannot be selected, cannot enter the undo
// stack, and cannot survive a crash — because as far as the document is
// concerned it does not exist.
//
// ===========================================================================
// WHY wxPopupWindow AND NOT A CHILD WINDOW
// ===========================================================================
// A plain child of the wxStyledTextCtrl gets painted over by Scintilla's own
// drawing. wxSTC's autocomplete list has exactly this problem and solves it the
// same way — see wxSTCPopupWindow in wx's PlatWX.cpp, which is a wxPopupWindow
// for this reason. Following the proven shape rather than inventing one.
//
// It never takes focus (AcceptsFocus() is false, as wxSTC's own popup does), so
// the editor keeps the caret and keystrokes keep flowing to it while the ghost
// is visible.
#pragma once

#include <wx/popupwin.h>
#include <wx/font.h>
#include <wx/string.h>

namespace ui {

class GhostTextOverlay : public wxPopupWindow {
public:
    explicit GhostTextOverlay(wxWindow* parent);

    // Paint `text` at `screenPos` (top-left of the caret) in the editor's font.
    // `maxWidth` clips it to what fits before the editor's right edge — a long
    // AI suggestion must not spill a floating box across the whole desktop.
    // Empty text hides the overlay.
    void ShowGhost(const wxPoint& screenPos, const wxFont& font,
                   const wxString& text, int lineHeight, int maxWidth);
    void HideGhost();

    // True when a suggestion is currently on screen. This is the gate the
    // editor's Tab handler asks about, so it must mean exactly "there is
    // something to accept".
    bool Active() const { return active_; }
    // What Tab would insert. Empty when not active.
    const wxString& Suggestion() const { return text_; }

    bool AcceptsFocus() const override { return false; }

private:
    void OnPaint(wxPaintEvent&);

    wxString text_;
    wxFont   font_;
    bool     active_ = false;
};

} // namespace ui
