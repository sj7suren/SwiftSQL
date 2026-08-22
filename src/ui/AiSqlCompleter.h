// AiSqlCompleter.h - AI-backed SQL continuation for wxStyledTextCtrl.
//
// IT NO LONGER OWNS ANY UI. It used to open Scintilla's autocomplete list with
// a single "AI ..." row and consume Tab itself. Now it only produces text and
// tells the host, via SetPresenter, that a new suggestion is ready; the host
// (EditorPage) draws it as grey ghost text in the same overlay the offline
// skeleton tier uses. One place a suggestion can appear, one key that accepts
// it — two owners of the Tab key is precisely the bug that arrangement invites.
#pragma once

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <functional>
#include <memory>

#include "ai/PromptBuilder.h"

class wxKeyEvent;
class wxStyledTextCtrl;

namespace ai { class AiClient; }

namespace ui {

class AiSqlCompleter : public wxEvtHandler {
public:
    using ContextProvider = std::function<ai::AiContext()>;

    explicit AiSqlCompleter(wxStyledTextCtrl* stc);
    ~AiSqlCompleter() override;

    void SetContextProvider(ContextProvider provider);
    // Called on the GUI thread when a new suggestion has landed (or been
    // cleared). The host re-reads Suggestion() and repaints.
    using Presenter = std::function<void()>;
    void SetPresenter(Presenter p) { presenter_ = std::move(p); }

    void Schedule();
    void Cancel();

    // The current AI continuation, or "" when there is none. The host decides
    // whether to draw it — this class no longer draws anything.
    const wxString& Suggestion() const { return suggestion_; }
    // Forget it (the host accepted it, or the caret moved away from it).
    void ClearSuggestion() { suggestion_.clear(); }

    // Returns true when the key was consumed. Tab is NOT consumed here any more
    // — accepting the suggestion is the host's job, because the host is the one
    // that knows whether the ghost or the completion popup is on screen.
    bool HandleKey(wxKeyEvent& ev);

private:
    void OnTimer(wxTimerEvent&);
    void RequestSuggestion();
    void ShowSuggestion();
    Presenter presenter_;
    wxString PrefixBeforeCaret() const;
    wxString CleanSuggestion(const wxString& text) const;

    wxStyledTextCtrl* stc_ = nullptr;
    ContextProvider   contextProvider_;
    wxTimer           timer_;
    std::unique_ptr<ai::AiClient> client_;

    // Identity of the provider `client_` was built for. A mismatch (user changed
    // model / key in Preferences) is the ONLY reason to rebuild it — see the
    // note in RequestSuggestion.
    wxString providerKey_;
    wxString pendingPrefix_;
    wxString reply_;
    wxString suggestion_;
    long     requestId_ = 0;
    bool     showing_ = false;
};

} // namespace ui
