// AiSqlCompleter.cpp - lightweight AI continuation for every SQL editor surface.
#include "ui/AiSqlCompleter.h"

#include <wx/event.h>
#include <wx/stc/stc.h>

#include <algorithm>

#include "ai/AiClient.h"
#include "ai/AiConfig.h"
#include "ai/AiConfigStore.h"
#include "ai/PromptBuilder.h"
#include "ui/Theme.h"

namespace ui {
namespace {

wxString DialectName(db::Dialect d)
{
    switch (d) {
    case db::Dialect::MySQL:     return L"MySQL";
    case db::Dialect::Postgres:  return L"PostgreSQL";
    case db::Dialect::Sqlite:    return L"SQLite";
    case db::Dialect::SqlServer: return L"SQL Server";
    case db::Dialect::Oracle:    return L"Oracle";
    }
    return L"SQL";
}

wxString OneLine(wxString s)
{
    s.Replace(L"\r", L" ");
    s.Replace(L"\n", L" ");
    while (s.Replace(L"  ", L" ")) {}
    s.Trim(true).Trim(false);
    if (s.length() > 96) s = s.Left(96) + L"...";
    return s;
}

} // namespace

AiSqlCompleter::AiSqlCompleter(wxStyledTextCtrl* stc)
    : stc_(stc)
    , timer_(this)
{
    Bind(wxEVT_TIMER, &AiSqlCompleter::OnTimer, this);
}

AiSqlCompleter::~AiSqlCompleter()
{
    Cancel();
}

void AiSqlCompleter::SetContextProvider(ContextProvider provider)
{
    contextProvider_ = std::move(provider);
}

void AiSqlCompleter::Schedule()
{
    showing_ = false;
    // THE OLD ANSWER IS NOW WRONG. It was computed for a caret position the
    // user has just moved past, and UpdateGhost prefers an AI suggestion over
    // the local skeleton whenever one exists — so leaving it set would pin
    // stale grey text on screen and suppress the instant local tier behind it.
    suggestion_.clear();
    timer_.StartOnce(750);
}

void AiSqlCompleter::Cancel()
{
    timer_.Stop();
    if (client_) client_->Cancel();
    showing_ = false;
}

bool AiSqlCompleter::HandleKey(wxKeyEvent& ev)
{
    if (!stc_) return false;
    // TAB IS DELIBERATELY NOT HANDLED HERE any more. EditorPage::AcceptGhost
    // owns it, because only the host knows whether the grey ghost or the
    // completion popup is currently on screen — and Tab must mean exactly one
    // thing at any moment.
    if (ev.GetKeyCode() == WXK_ESCAPE) {
        showing_ = false;
        suggestion_.clear();
        if (presenter_) presenter_();   // let the host clear the grey text too
    }
    return false;
}

void AiSqlCompleter::OnTimer(wxTimerEvent&)
{
    RequestSuggestion();
}

wxString AiSqlCompleter::PrefixBeforeCaret() const
{
    if (!stc_) return wxString();
    const int pos = stc_->GetCurrentPos();
    const int start = std::max(0, pos - 4000);
    return stc_->GetTextRange(start, pos);
}

void AiSqlCompleter::RequestSuggestion()
{
    if (!stc_ || stc_->AutoCompActive()) return;

    wxString prefix = PrefixBeforeCaret();
    wxString trimmed = prefix;
    trimmed.Trim(true).Trim(false);
    if (trimmed.length() < 8) return;
    if (prefix == pendingPrefix_) return;
    pendingPrefix_ = prefix;

    const ai::AiSettings settings = ai::AiConfigStore::Load();
    const ai::AiProviderConfig* prov = settings.DefaultProvider();
    if (!prov || prov->model.IsEmpty()) return;
    if (prov->apiKey.IsEmpty() && prov->kind != ai::ProviderKind::OllamaNative) return;

    ai::AiContext ctx = contextProvider_ ? contextProvider_() : ai::AiContext{};
    if (ctx.dialectName.IsEmpty()) ctx.dialectName = DialectName(ctx.dialect);

    ai::AiRequest req;
    req.model = prov->model;
    req.temperature = 0.15;
    req.maxTokens = 96;
    req.stream = false;
    req.messages = {
        { ai::Role::System,
          L"You are an SQL autocomplete engine. Return only the next SQL text "
          L"to insert at the cursor. Do not explain. Do not use markdown. "
          L"Keep it short and syntactically valid for the dialect." },
        { ai::Role::User,
          L"Dialect: " + ctx.dialectName + L"\n"
          L"Current SQL before cursor:\n" + prefix + L"\n\n"
          L"Return the continuation after the cursor only." }
    };

    // ONE CLIENT, REUSED — this is the caller half of the crash fixed in
    // AiHttp::Impl::Orphan(). Building a fresh AiClient per keystroke burst
    // destroyed a transport handler that wxWebRequest still held a raw pointer
    // to, and a fast typist superseded requests faster than they finished. The
    // transport now survives that on its own, but the churn was never needed:
    // an AiClient is designed to serve a stream of turns, and Cancel()+Chat()
    // is exactly how it is meant to be driven.
    //
    // It is rebuilt ONLY when the provider actually changed (the user picked a
    // different model or pasted a new key in Preferences), because cfg_ is
    // fixed at construction.
    const wxString key = ai::ProviderKey(*prov);
    if (!client_ || providerKey_ != key) {
        client_      = std::make_unique<ai::AiClient>(*prov);
        providerKey_ = key;
    }
    client_->Cancel();   // supersede whatever was still in flight
    reply_.clear();
    const long id = ++requestId_;

    ai::AiCallbacks cb;
    cb.onDelta = [this, id](const wxString& delta) {
        if (id == requestId_) reply_ += delta;
    };
    cb.onDone = [this, id](int, int) {
        if (id != requestId_) return;
        suggestion_ = CleanSuggestion(reply_);
        ShowSuggestion();
    };
    cb.onError = [this, id](const ai::AiError&) {
        if (id != requestId_) return;
        showing_ = false;
        suggestion_.clear();
        // Tell the host too, or a suggestion that failed mid-flight would keep
        // whatever was last drawn on screen.
        if (presenter_) presenter_();
    };
    client_->Chat(req, cb);
}

wxString AiSqlCompleter::CleanSuggestion(const wxString& text) const
{
    wxString s = text;
    const size_t fence = s.find(L"```");
    if (fence != wxString::npos) {
        size_t p = fence + 3;
        const size_t nl = s.find(L'\n', p);
        if (nl != wxString::npos) p = nl + 1;
        const size_t end = s.find(L"```", p);
        s = (end != wxString::npos) ? s.Mid(p, end - p) : s.Mid(p);
    }
    s.Trim(true).Trim(false);
    if (s.StartsWith(L"`") && s.EndsWith(L"`") && s.length() > 1)
        s = s.Mid(1, s.length() - 2);
    if (s.length() > 500) s = s.Left(500);
    if (s.IsEmpty()) return s;

    const int pos = stc_ ? stc_->GetCurrentPos() : 0;
    const wxChar prev = (stc_ && pos > 0) ? static_cast<wxChar>(stc_->GetCharAt(pos - 1)) : 0;
    const wxChar first = s[0];
    const bool needsSpace = prev && !wxIsspace(prev) && prev != '(' && prev != '.' &&
                            first != ',' && first != ')' && first != ';' &&
                            first != '\n' && !wxIsspace(first);
    if (needsSpace) s = L" " + s;
    return s;
}

void AiSqlCompleter::ShowSuggestion()
{
    if (!stc_ || suggestion_.IsEmpty()) return;
    // Collapsed to ONE LINE because the ghost is drawn on the caret's own line;
    // a multi-line model answer would otherwise paint over the code below it.
    // The collapsed form is also what Tab inserts, so what is shown and what is
    // accepted cannot diverge.
    suggestion_ = OneLine(suggestion_);
    if (suggestion_.IsEmpty()) return;
    showing_ = true;
    if (presenter_) presenter_();   // the host repaints the grey overlay
}

} // namespace ui
