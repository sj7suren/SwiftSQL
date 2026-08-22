// AiChatPanel.h — the AI 助手 chat PAGE (a tab in the editors notebook, replacing the
// old "AI 功能建设中" placeholder). Layout:
//   • top bar: 连接 dropdown + 数据库 dropdown (选库 triggers the knowledge-base build)
//   • a knowledge-base status strip ("正在理解数据库结构…" → "已就绪")
//   • a scrollable chat transcript (user + AI turns, AI streamed live)
//   • an input box + 发送 button
//
// On send, the panel grounds the prompt with the built AiKnowledge and streams the
// model's answer. Generated SQL is classified and ROUTED:
//   • SELECT / WITH (a read) → host.runQuery(sql): open a query tab and run it, so the
//     user jumps straight to results.
//   • INSERT / UPDATE / DELETE / DDL (a write) → host.insertToEditor(sql): drop it into
//     the editor but DO NOT run it — the user reviews and runs it themselves (safety).
//
// Provider configuration lives in Preferences (偏好设置 ▸ AI), not here; the panel just
// reads the default provider via ai::AiConfigStore and shows a hint if none is set.
#pragma once

#include <wx/panel.h>
#include <wx/gauge.h>
#include <wx/timer.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "ai/PromptBuilder.h"   // ai::UseCase (SendTurn's template selector)
#include "ui/AiKnowledgeBase.h"
#include "ui/ChatTranscript.h"
#include "ui/ConversationStore.h"
#include "ui/FlatControls.h"

namespace ai { class AiClient; }
namespace db { class IConnection; }

class wxBoxSizer;
class wxCommandEvent;
class wxKeyEvent;
class wxStaticText;
class wxTextCtrl;

namespace ui {

class IconCircleButton;
class PillButton;
class ConversationDrawer;
class ChatTranscript;

// The seam to MainFrame. Connection handles are opaque (void*); the panel hands them
// back to `databases` / `connection`, never dereferences them.
struct AiChatHost {
    std::function<std::vector<std::pair<wxString, void*>>()> connections;    // (label, handle)
    std::function<std::vector<wxString>(void*)>              databases;      // handle → db names
    std::function<db::IConnection*(void*)>                   connection;     // handle → live conn / null
    // handle → a brand-new DEDICATED connection the caller owns (nullptr + err on
    // failure). Used by the knowledge base so its worker-thread introspection never
    // shares the UI's (non-thread-safe) connection. Built on the GUI thread.
    std::function<std::unique_ptr<db::IConnection>(void* handle, wxString& err)> makeConnection;
    std::function<void(const wxString& sql)>                 runQuery;       // SELECT → open a query tab + run
    std::function<void(const wxString& sql)>                 insertToEditor; // write SQL → insert, don't run
};

class AiChatPanel : public wxPanel {
public:
    AiChatPanel(wxWindow* parent, AiChatHost host);
    ~AiChatPanel() override;

    // SQL 性能分析. Asks the selected connection for `sql`'s EXECUTION PLAN and
    // then has the model read the plan, not just the SQL text — that is the
    // difference between "第 2 行全表扫描，预估 84 万行" and a generic "consider
    // an index". The plan comes from EXPLAIN only, NEVER EXPLAIN ANALYZE
    // (db::BuildExplainSql), so analysing an UPDATE or DELETE cannot execute it.
    //
    // Degrades rather than refusing: no connection, an un-explainable statement
    // (DDL/SET) or an engine with no plan facility still runs the analysis, just
    // without the plan section. A note in the transcript says which happened, so
    // a thinner answer is never a mystery.
    void AnalyzeSql(const wxString& sql);

    // Re-read 偏好设置 ▸ AI and rebuild the wire client if the provider changed.
    // Idempotent and cheap: when nothing changed it compares one string and returns.
    //
    // client_ bakes in kind/baseUrl/apiKey at construction, so without this a
    // provider switch left every open panel talking to the OLD endpoint with the
    // OLD key until the window was closed and reopened. Called after Preferences
    // is accepted (so the model pill updates immediately) and again at the top of
    // SendTurn (so a turn can never go out on a stale client).
    void SyncProviderFromSettings();

private:
    void BuildUi();
    void PopulateConnChoice();
    void PopulateDbChoice();
    void OnConnChanged(wxCommandEvent&);
    void OnDbChanged(wxCommandEvent&);      // → kick off knowledge-base build
    void OnSend(wxCommandEvent&);
    void OnStop(wxCommandEvent&);
    void FlushStreamingReply();             // coalesced repaint of the active reply bubble

    void RebuildKnowledge(bool forceRebuild = false); // (re)build KB; force → ignore cache
    wxString KbCacheKey() const;            // stable per-(connection, database) cache key
    void KbBusyShow();                      // show the determinate KB progress bar at 0%
    void KbBusyHide();                      // stop the climb timer + hide it
    void KbSetProgress(int percent);        // drive the bar (−1 = keep); creep 70→95 in LLM phase
    void ShowModelMenu();                   // pill → wxMenu of selectable models (current checked)
    void FetchModels();                     // lazy, idle-only ListModels → models_ cache
    void OnInputKey(wxKeyEvent&);           // Enter → send, Ctrl+Enter → newline
    void ShowBusyButtons(bool busy);        // swap the send ⇄ stop circle
    // One turn. `useCase` selects the prompt template; `selectedSql`/`explainText`
    // fill the AiContext slots the SqlPerf template reads and Nl2Sql ignores.
    // Parameterised rather than copied so both flows share ONE streaming,
    // busy-state, persistence and SQL-routing path.
    void SendTurn(const wxString& userText,
                  ai::UseCase useCase = ai::UseCase::Nl2Sql,
                  const wxString& selectedSql = wxString(),
                  const wxString& explainText = wxString());
    // Run the plan statements for `sql` on the selected connection and return the
    // rendered plan text ("" when unavailable). Synchronous: a plan query is
    // small and the panel has no worker of its own.
    wxString FetchExplain(const wxString& sql, wxString& why);
    void RouteGeneratedSql(const wxString& sql);   // classify SELECT vs write → run / insert
    db::IConnection* SelectedConnection() const;
    wxString SelectedDatabase() const;

    // ---- conversation persistence + history drawer (AiChatPanel_Conversations.cpp) ----
    void ToggleDrawer();                         // show/hide the history column + relayout
    void NewConversation();                      // save current, then start a blank one
    void OpenConversation(const wxString& id);   // save current, load `id`, rebuild bubbles
    void DeleteConversation(const wxString& id); // delete file, refresh drawer, blank if active
    void BatchDeleteConversations(const std::vector<wxString>& ids); // multi-select delete
    void SaveCurrentConversation();              // persist current_ if it has any turns
    void EnsureDrawerShown();                    // reveal the drawer if it is currently hidden
    void ClearTranscript();                      // clear the transcript, reset streaming state
    void RefreshDrawer();                        // reload the drawer list, highlight current_
    void AbortInFlight();                        // cancel any streaming turn before a switch

    AiChatHost                     host_;
    std::unique_ptr<ai::AiClient>  client_;
    AiKnowledgeBase                kb_;

    BoxDropdown*  connChoice_ = nullptr;
    BoxDropdown*  dbChoice_   = nullptr;
    PillButton*   modelPill_  = nullptr;    // in-chat model selector ("model ⌄")
    wxPanel*      kbStrip_    = nullptr;    // collapsible container: kbStatus_ + kbBusy_ (hidden when KB ready)
    wxStaticText* kbStatus_   = nullptr;    // knowledge-base status strip
    IconCircleButton* kbRefreshBtn_ = nullptr; // "更新知识库" → forced rebuild (lives in the top bar)
    wxGauge*      kbBusy_     = nullptr;    // determinate progress bar during KB build
    wxTimer       kbClimb_;                 // slow-creeps the bar 70→95 during the LLM pass
    ChatTranscript* transcript_ = nullptr;  // virtualized, self-drawn chat history
    wxTextCtrl*   input_      = nullptr;
    IconCircleButton* sendBtn_ = nullptr;
    IconCircleButton* stopBtn_ = nullptr;

    ConversationDrawer* drawer_ = nullptr;   // left history column (inside the chat area)
    IconCircleButton*   drawerBtn_ = nullptr;// top-bar toggle for the drawer
    bool          drawerVisible_ = false;
    Conversation  current_;                  // the conversation being viewed / appended to

    wxString curReply_;                     // accumulates the streaming reply for SQL extraction
    wxTimer  streamTimer_;                  // coalesces streaming-reply repaints (~80ms)
    bool     streamDirty_ = false;          // curReply_ changed since the last repaint
    wxString currentModel_;                 // model used for the next turn (pill selection)
    std::vector<wxString> models_;          // lazily-fetched provider model list (may be empty)
    // Identity (ai::ProviderKey) of the provider client_ was built for. A mismatch
    // means Preferences changed under us and client_ must be rebuilt.
    wxString providerKey_;
    bool     hasProvider_ = false;
    bool     busy_        = false;
};

} // namespace ui
