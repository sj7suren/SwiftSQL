// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiChatPanel_Conversations.cpp — the conversation-persistence + history-drawer half
// of AiChatPanel, split into its own translation unit to keep AiChatPanel.cpp under
// the 1000-line charter limit (same class, cross-TU member definitions — mirrors the
// MainFrame_*.cpp split).
//
// Responsibilities:
//   • ToggleDrawer            — show/hide the left history column and relayout
//   • NewConversation         — save the current chat, then start a blank one
//   • OpenConversation        — save current, load a stored chat, rebuild its bubbles
//   • DeleteConversation      — remove a stored chat (blank the view if it was open)
//   • SaveCurrentConversation — persist current_ (encrypted) once it has real turns
//   • ClearTranscript         — clear the transcript, reset streaming state
//   • RefreshDrawer           — reload the drawer list, highlight current_
//   • AbortInFlight           — cancel a streaming turn before any conversation switch
//
// Switching conversations while a reply is streaming is made safe by AbortInFlight():
// client_->Cancel() guarantees no further onDelta/onDone re-enters after we clear the
// transcript. The transcript owns NO child windows, so clearing it can never leave a
// dangling bubble pointer (the old UAF surface is gone by construction).
#include "ui/AiChatPanel.h"

#include <wx/datetime.h>
#include <wx/event.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "ai/AiClient.h"
#include "ui/ChatTranscript.h"
#include "ui/ConversationDrawer.h"
#include "ui/ConversationStore.h"
#include "ui/I18n.h"
#include "ui/MermaidChat.h"     // re-render stored ```mermaid blocks on reopen

namespace ui {

namespace {

// Title for a conversation: its first user message, collapsed to one line and
// truncated to ~24 display characters (with an ellipsis when clipped).
wxString DeriveTitle(const Conversation& c)
{
    wxString first;
    for (const ConvMessage& m : c.messages) {
        if (m.role == L"user") { first = m.text; break; }
    }
    if (first.IsEmpty() && !c.messages.empty()) first = c.messages.front().text;

    first.Replace(L"\r", L" ");
    first.Replace(L"\n", L" ");
    first.Trim(true).Trim(false);
    if (first.length() > 24) first = first.Left(24) + L"…";
    return first;
}

} // namespace

// ------------------------------------------------------------------ drawer toggle

void AiChatPanel::ToggleDrawer()
{
    drawerVisible_ = !drawerVisible_;
    if (drawer_) drawer_->Show(drawerVisible_);
    if (drawerVisible_) RefreshDrawer();   // pull fresh list when it becomes visible
    Layout();
    // The transcript is double-buffered; when the drawer collapses it grows into the
    // freed space and that newly-exposed back-buffer region can flash black until the
    // next natural paint. Force an immediate repaint of the chat area.
    if (transcript_) { transcript_->Refresh(); transcript_->Update(); }
    Refresh();
}

void AiChatPanel::RefreshDrawer()
{
    if (drawer_ && drawerVisible_) drawer_->Reload(current_.id);
}

void AiChatPanel::EnsureDrawerShown()
{
    if (drawerVisible_) { RefreshDrawer(); return; }   // already open → just refresh
    drawerVisible_ = true;
    if (drawer_) drawer_->Show(true);
    Layout();
    RefreshDrawer();                                    // now visible → pull fresh list
    // Same anti-flash repaint the toggle uses when the transcript's width changes.
    if (transcript_) { transcript_->Refresh(); transcript_->Update(); }
    Refresh();
}

// ------------------------------------------------------------------ new / open / delete

void AiChatPanel::NewConversation()
{
    AbortInFlight();
    SaveCurrentConversation();     // persist whatever we were viewing (no-op if empty)
    ClearTranscript();

    current_    = Conversation{};
    current_.id = ConversationStore::NextId();
    RefreshDrawer();
    if (input_) input_->SetFocus();
}

void AiChatPanel::OpenConversation(const wxString& id)
{
    AbortInFlight();
    SaveCurrentConversation();     // don't lose the chat we're leaving

    Conversation loaded;
    if (!ConversationStore::Load(id, loaded)) {
        RefreshDrawer();           // stale row (file vanished) — just refresh
        return;
    }

    ClearTranscript();
    current_ = std::move(loaded);

    // Replay stored turns (user right / AI left), matching the live styling. Stored AI
    // text keeps any ```mermaid source: re-scan it here so the diagram re-renders, and
    // show the prose WITHOUT the fenced source (mirrors the live-turn path).
    for (const ConvMessage& m : current_.messages) {
        if (m.role == L"user") {
            transcript_->AddMessage(ChatTranscript::Kind::User, tr(L"你"), m.text);
            continue;
        }
        const MermaidScan scan = ScanMermaid(m.text);
        const wxString shown = scan.blocks.empty() ? m.text : scan.displayText;
        transcript_->AddMessage(ChatTranscript::Kind::Ai, L"AI", shown);
        if (!scan.blocks.empty()) {
            const int mw = std::max(120,
                static_cast<int>(transcript_->GetClientSize().x * 0.8));
            const std::vector<wxBitmap> diagrams = RenderMermaidBlocks(scan.blocks, mw);
            for (size_t k = 0; k < diagrams.size(); ++k) {
                if (k == 0) transcript_->SetLastDiagram(diagrams[k]);
                else        transcript_->AddDiagram(diagrams[k]);
            }
        }
    }

    RefreshDrawer();
}

void AiChatPanel::DeleteConversation(const wxString& id)
{
    ConversationStore::Delete(id);

    if (id == current_.id) {
        // The open conversation was deleted → drop to a fresh blank slate.
        AbortInFlight();
        ClearTranscript();
        current_    = Conversation{};
        current_.id = ConversationStore::NextId();
    }
    RefreshDrawer();
}

void AiChatPanel::BatchDeleteConversations(const std::vector<wxString>& ids)
{
    if (ids.empty()) return;

    bool deletedCurrent = false;
    for (const wxString& id : ids) {
        ConversationStore::Delete(id);
        if (id == current_.id) deletedCurrent = true;
    }

    if (deletedCurrent) {
        // The open conversation was among the deleted → drop to a fresh blank slate.
        AbortInFlight();
        ClearTranscript();
        current_    = Conversation{};
        current_.id = ConversationStore::NextId();
    }
    RefreshDrawer();

    // If nothing is left, collapse the drawer back to the clean first-run state (it
    // reappears the moment a new chat is saved). Keeps the "history shows only when it
    // exists" rule symmetric with the constructor's auto-reveal.
    if (ConversationStore::List().empty() && drawerVisible_) {
        drawerVisible_ = false;
        if (drawer_) drawer_->Show(false);
        Layout();
        if (transcript_) { transcript_->Refresh(); transcript_->Update(); }
        Refresh();
    }
}

// ------------------------------------------------------------------ persistence

void AiChatPanel::SaveCurrentConversation()
{
    if (current_.messages.empty()) return;      // nothing durable yet
    if (current_.id.IsEmpty()) current_.id = ConversationStore::NextId();

    const long long now = static_cast<long long>(wxDateTime::Now().GetTicks());
    if (current_.createdAt == 0) current_.createdAt = now;
    current_.updatedAt = now;
    if (current_.title.IsEmpty()) current_.title = DeriveTitle(current_);

    ConversationStore::Save(current_);
}

// ------------------------------------------------------------------ teardown helpers

void AiChatPanel::ClearTranscript()
{
    if (streamTimer_.IsRunning()) streamTimer_.Stop();
    streamDirty_ = false;
    curReply_.clear();
    if (transcript_) transcript_->Clear();   // drops all messages + resets scroll/stream
}

void AiChatPanel::AbortInFlight()
{
    if (!busy_) return;
    if (streamTimer_.IsRunning()) streamTimer_.Stop();
    // OnStop() cancels the client and restores the idle button/input state. It lives in
    // AiChatPanel.cpp where IconCircleButton is a complete type (it is only forward-
    // declared here), so we delegate rather than poke the buttons across the TU seam.
    wxCommandEvent e;
    OnStop(e);
}

} // namespace ui
