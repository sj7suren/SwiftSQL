// AiChatPanel.cpp — the AI 助手 chat PAGE (replaces the old "AI 功能建设中" placeholder).
//
// Layout (top → bottom):
//   • selector row  : 连接 dropdown + 数据库 dropdown
//   • status strip  : kbStatus_ — knowledge-base build progress / readiness
//   • transcript    : read-only, streamed chat history (user + AI turns)
//   • input row     : input_ + [发送] + [停止]
//
// Selecting a database kicks off AiKnowledgeBase::Build (introspection + adversarial
// self-check). Each chat turn is grounded with the built knowledge, and the model's
// answer is streamed live into the transcript. Generated SQL is classified by its first
// effective keyword and ROUTED: a read (SELECT/WITH/SHOW/EXPLAIN/DESC[RIBE]) runs in a
// query tab via host.runQuery; a write (INSERT/UPDATE/DELETE/DDL/…) is dropped into the
// editor unexecuted via host.insertToEditor so the user reviews it first.
#include "ui/AiChatPanel.h"

#include <wx/gauge.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <memory>

#include "ai/AiClient.h"
#include "ai/AiConfig.h"
#include "ai/AiConfigStore.h"
#include "ui/AiChatWidgets.h"
#include "ui/ConversationDrawer.h"
#include "ui/IconFactory.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// Timer id for the slow "creep" that animates the KB progress bar 70→95 during the
// single, indivisible LLM verify pass (which has no reportable sub-progress).
constexpr int kKbClimbTimerId = wxID_HIGHEST + 4200;
constexpr int kStreamTimerId  = wxID_HIGHEST + 4201;   // coalesces streaming-reply repaints

} // namespace

AiChatPanel::AiChatPanel(wxWindow* parent, AiChatHost host)
    : wxPanel(parent, wxID_ANY)
    , host_(std::move(host))
{
    SetBackgroundColour(theme::kWhite);
    BuildUi();

    // Provider comes from 偏好设置 ▸ AI. No usable provider → chat is disabled and the
    // status strip points the user at Preferences. Construction goes through the same
    // routine as a later provider switch, so the two can never drift apart.
    SyncProviderFromSettings();

    PopulateConnChoice();
    // Auto-build the KB for the initially-selected conn+db so the first turn is grounded.
    if (hasProvider_) RebuildKnowledge();

    // Start on a blank NEW conversation (simplest, deterministic startup — no auto-load
    // of a prior chat). Past conversations are listed in the history drawer.
    current_ = Conversation{};
    current_.id = ConversationStore::NextId();

    // Auto-reveal the history drawer when there IS prior history, so returning users
    // land on their chats. A first, clean install (no history) keeps it hidden so the
    // page stays uncluttered — the drawer appears the moment the first chat is saved.
    if (!ConversationStore::List().empty()) {
        drawerVisible_ = true;
        if (drawer_) drawer_->Show(true);
        Layout();
    }
    RefreshDrawer();
}

AiChatPanel::~AiChatPanel()
{
    // Terminate any in-flight work before members are torn down. Callbacks fire on the
    // GUI thread, and this dtor runs on the GUI thread, so a Cancel() here guarantees no
    // callback re-enters a half-destroyed panel (mirrors AiDialog's teardown intent).
    if (client_) client_->Cancel();
    kb_.Cancel();
}

// --------------------------------------------------------------------- provider sync

void AiChatPanel::SyncProviderFromSettings()
{
    // Never swap the transport out from under a streaming turn. Deferring is safe:
    // the send button is disabled while busy_, so no NEW turn can start on the stale
    // client, and SendTurn syncs again before the next one goes out.
    if (busy_) return;

    const ai::AiSettings settings = ai::AiConfigStore::Load();
    const ai::AiProviderConfig* prov = settings.DefaultProvider();
    const wxString key = prov ? ai::ProviderKey(*prov) : wxString();
    // Unchanged → nothing to do. An EMPTY key deliberately falls through: that is the
    // "nothing configured yet" state, and re-evaluating it is exactly how a first-time
    // configuration is picked up.
    if (!key.IsEmpty() && key == providerKey_) return;
    providerKey_ = key;

    // kb_.Build() BORROWS a raw AiClient*, so the knowledge base has to let go before
    // client_ is destroyed. Cancel() nulls that pointer and no-ops any late callbacks
    // — the same teardown the dtor and a database switch already use.
    kb_.Cancel();
    KbBusyHide();
    if (client_) client_->Cancel();
    client_.reset();
    models_.clear();          // the previous provider's model list means nothing now
    currentModel_.Clear();    // ...and neither does a pill selection made from it

    // A provider carrying no credentials is not usable. DefaultProvider() hands back
    // the user's selection as-is rather than silently substituting a configured one,
    // so an unconfigured selection lands here and gets the "please configure" hint.
    hasProvider_ = prov && ai::CanFetchModels(*prov);
    if (hasProvider_) {
        client_ = std::make_unique<ai::AiClient>(*prov);
        // Default the pill to the provider's configured model; the full model list is
        // fetched lazily (idle-only) the first time the user opens the picker.
        currentModel_ = prov->model;
        modelPill_->SetText(currentModel_.IsEmpty() ? tr(L"模型") : currentModel_);
        modelPill_->Enable(true);
        // Don't stomp a "知识库已就绪 ✓" strip: the KB is schema knowledge about the
        // database, not about the model, so a provider switch leaves it valid.
        if (!kb_.Ready()) {
            kbStatus_->SetLabel(tr(L"请选择连接与数据库以构建知识库"));
            kbStatus_->SetForegroundColour(theme::kTextSecondary);
        }
    } else {
        modelPill_->SetText(tr(L"模型"));
        modelPill_->Enable(false);
        kbStatus_->SetLabel(tr(L"请到 偏好设置 ▸ AI 配置服务"));
        kbStatus_->SetForegroundColour(theme::kDotAmber);
    }
    sendBtn_->Enable(hasProvider_);
}

// ---------------------------------------------------------------------------- layout

void AiChatPanel::BuildUi()
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(theme::kWhite);

    // ---- 连接 + 数据库 selectors ------------------------------------------
    auto* topRow = new wxBoxSizer(wxHORIZONTAL);
    // Drawer toggle (leftmost): stacked-cards glyph ⇒ "conversation history".
    drawerBtn_ = new IconCircleButton(this, icons::Glyph::ViewLayers,
                                      theme::kTextSecondary, theme::kWhite,
                                      [this] { ToggleDrawer(); });
    drawerBtn_->SetToolTip(tr(L"对话历史"));
    topRow->Add(drawerBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    // Clickable text next to the icon (users asked for a label, not just a glyph).
    auto* drawerLbl = new wxStaticText(this, wxID_ANY, tr(L"历史"));
    drawerLbl->SetForegroundColour(theme::kTextSecondary);
    drawerLbl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { ToggleDrawer(); });
    topRow->Add(drawerLbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    auto* cl = new wxStaticText(this, wxID_ANY, tr(L"连接:"));
    cl->SetForegroundColour(theme::kTextSecondary);
    topRow->Add(cl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    connChoice_ = new ui::BoxDropdown(this, wxSize(200, -1));
    connChoice_->SetOnChange([this] { wxCommandEvent e; OnConnChanged(e); });
    topRow->Add(connChoice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 14);

    auto* dl = new wxStaticText(this, wxID_ANY, tr(L"数据库:"));
    dl->SetForegroundColour(theme::kTextSecondary);
    topRow->Add(dl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    dbChoice_ = new ui::BoxDropdown(this, wxSize(180, -1));
    dbChoice_->SetOnChange([this] { wxCommandEvent e; OnDbChanged(e); });
    topRow->Add(dbChoice_, 0, wxALIGN_CENTER_VERTICAL);

    // Model selector pill + KB refresh, pushed to the right of the selector row.
    topRow->AddStretchSpacer(1);
    modelPill_ = new PillButton(this, [this] { ShowModelMenu(); });
    modelPill_->SetToolTip(tr(L"选择模型"));
    topRow->Add(modelPill_, 0, wxALIGN_CENTER_VERTICAL);
    // 更新知识库 lives in the ALWAYS-VISIBLE top bar (not the status strip), so a rebuild
    // stays one click away even after the strip collapses on readiness. Refresh glyph on
    // white reads as a plain icon (a white disc on white shows only stroke + faint hover).
    kbRefreshBtn_ = new IconCircleButton(this, icons::Glyph::Refresh,
                                         theme::kTextSecondary, theme::kWhite,
                                         [this] { RebuildKnowledge(true); });
    kbRefreshBtn_->SetToolTip(tr(L"更新知识库"));
    topRow->Add(kbRefreshBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    root->Add(topRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    // ---- knowledge-base status strip (COLLAPSIBLE) ------------------------
    // kbStatus_ + the progress gauge live in a member container we Hide() the moment the
    // KB is ready — that hands the freed vertical space to the transcript. It comes back
    // (Show + Layout) while building, on error, or when no connection/database is chosen.
    kbStrip_ = new wxPanel(this, wxID_ANY);
    kbStrip_->SetBackgroundColour(theme::kWhite);
    auto* stripSizer = new wxBoxSizer(wxVERTICAL);

    auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
    kbStatus_ = new wxStaticText(kbStrip_, wxID_ANY, wxEmptyString);
    kbStatus_->SetForegroundColour(theme::kTextSecondary);
    statusRow->Add(kbStatus_, 1, wxALIGN_CENTER_VERTICAL);
    stripSizer->Add(statusRow, 0, wxEXPAND);

    // A thin DETERMINATE progress bar shown only while the KB build runs. Introspection
    // reports real per-table progress (0→70%); the LLM verify pass then creeps 70→95%
    // on the climb timer below, snapping to 100% when it completes.
    kbBusy_ = new wxGauge(kbStrip_, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 6),
                          wxGA_HORIZONTAL | wxGA_SMOOTH);
    kbBusy_->Hide();
    stripSizer->Add(kbBusy_, 0, wxEXPAND | wxTOP, 8);

    kbStrip_->SetSizer(stripSizer);
    root->Add(kbStrip_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    kbClimb_.SetOwner(this, kKbClimbTimerId);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!kbBusy_) return;
        const int v = kbBusy_->GetValue();
        if (v < 95) kbBusy_->SetValue(v + 1);   // gentle time-based creep, never to 100
        else        kbClimb_.Stop();
    }, kKbClimbTimerId);

    // Coalesce streaming-reply repaints: onDelta only accumulates + marks dirty;
    // this timer repaints (SetLabel/Wrap/Layout) at most ~12×/s so a fast token
    // stream can't stutter the UI with a per-token O(n) re-wrap.
    streamTimer_.SetOwner(this, kStreamTimerId);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { FlushStreamingReply(); }, kStreamTimerId);

    // ---- history drawer | transcript (side by side inside the chat area) --
    auto* chatRow = new wxBoxSizer(wxHORIZONTAL);

    // Left history drawer — hidden until toggled. Its callbacks route to the
    // conversation-management methods (implemented in AiChatPanel_Conversations.cpp).
    drawer_ = new ConversationDrawer(
        this,
        [this] { NewConversation(); },
        [this](const wxString& id) { OpenConversation(id); },
        [this](const wxString& id) { DeleteConversation(id); },
        [this](const std::vector<wxString>& ids) { BatchDeleteConversations(ids); },
        [this] { ToggleDrawer(); });   // "«" header icon → collapse the drawer
    drawer_->Hide();
    chatRow->Add(drawer_, 0, wxEXPAND | wxRIGHT, 12);

    // ---- transcript (streamed, read-only) ---------------------------------
    // A VIRTUALIZED, self-drawn control: zero child windows, it paints all bubbles
    // itself and only the ones in the viewport. This is what makes a long chat scroll
    // smoothly — the old design stacked ~4 HWNDs per message, so Windows had to move +
    // repaint hundreds of native controls on every scroll tick.
    transcript_ = new ChatTranscript(this, wxSize(560, 320));
    chatRow->Add(transcript_, 1, wxEXPAND);
    root->Add(chatRow, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    // ---- input capsule: [+]  input_  [send/stop] --------------------------
    // A pill-shaped bar (white fill, 1px soft border) echoing the reference: a "+"
    // affordance on the left, the message field in the middle, and a solid dark
    // circular send button on the right (swapped for a stop circle while streaming).
    auto* inputPanel = new RoundedPanel(this, theme::kWhite, theme::kBorderInput, 24);
    inputPanel->SetBackgroundColour(theme::kWhite);
    auto* inRow = new wxBoxSizer(wxHORIZONTAL);

    // Left "+" — bare glyph on the white capsule (a white "circle" is invisible on
    // white, so it reads as a plain stroked plus with a faint hover disc). Starts a
    // brand-new conversation (saves the current one first).
    auto* plusBtn = new IconCircleButton(inputPanel, icons::Glyph::Plus,
                                         theme::kTextSecondary, theme::kWhite,
                                         [this] { NewConversation(); });
    plusBtn->SetToolTip(tr(L"新对话"));
    inRow->Add(plusBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);

    input_ = new wxTextCtrl(inputPanel, wxID_ANY, wxEmptyString,
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxBORDER_NONE | wxTE_PROCESS_ENTER | wxTE_NO_VSCROLL);
    input_->SetBackgroundColour(theme::kWhite);
    input_->SetHint(tr(L"输入消息，Enter 发送，Ctrl+Enter 换行"));
    input_->Bind(wxEVT_KEY_DOWN, &AiChatPanel::OnInputKey, this);
    inRow->Add(input_, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 8);

    // Solid dark send circle (white glyph) — the reference's black round button.
    sendBtn_ = new IconCircleButton(inputPanel, icons::Glyph::Play, theme::kWhite,
                                    theme::kTextStrong, [this] {
                                        wxCommandEvent ev;
                                        OnSend(ev);
                                    });
    sendBtn_->SetToolTip(tr(L"发送"));
    inRow->Add(sendBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);

    // Same size/shape, shown only while streaming (occupies the send slot).
    stopBtn_ = new IconCircleButton(inputPanel, icons::Glyph::Stop, theme::kWhite,
                                    theme::kDotRed, [this] {
                                        wxCommandEvent ev;
                                        OnStop(ev);
                                    });
    stopBtn_->SetToolTip(tr(L"停止"));
    stopBtn_->Enable(false);
    stopBtn_->Hide();
    inRow->Add(stopBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);

    inputPanel->SetSizer(inRow);
    root->Add(inputPanel, 0, wxEXPAND | wxALL, 16);

    SetSizer(root);
}

void AiChatPanel::PopulateConnChoice()
{
    if (!host_.connections) return;
    connChoice_->Clear();
    for (const auto& c : host_.connections())
        connChoice_->Append(c.first, c.second);   // (label, opaque handle)
    if (connChoice_->GetCount() > 0) connChoice_->SetSelection(0);
    PopulateDbChoice();
}

void AiChatPanel::PopulateDbChoice()
{
    dbChoice_->Clear();
    if (!host_.databases) return;
    const int ci = connChoice_->GetSelection();
    void* h = (ci != wxNOT_FOUND) ? connChoice_->GetClientData(ci) : nullptr;
    if (h)
        for (const wxString& d : host_.databases(h)) dbChoice_->Append(d);
    if (dbChoice_->GetCount() > 0) dbChoice_->SetSelection(0);
}

// ---------------------------------------------------------------------------- events

void AiChatPanel::OnConnChanged(wxCommandEvent&)
{
    PopulateDbChoice();
    if (hasProvider_) RebuildKnowledge();
}

void AiChatPanel::OnDbChanged(wxCommandEvent&)
{
    if (hasProvider_) RebuildKnowledge();
}

void AiChatPanel::OnSend(wxCommandEvent&)
{
    if (busy_ || !hasProvider_) return;

    wxString text = input_->GetValue();
    text.Trim(true).Trim(false);
    if (text.IsEmpty()) return;

    if (!SelectedConnection()) {
        kbStatus_->SetLabel(tr(L"所选连接不可用，请先连接数据库"));
        kbStatus_->SetForegroundColour(theme::kDotRed);
        if (kbStrip_ && !kbStrip_->IsShown()) { kbStrip_->Show(); Layout(); }  // surface the warning
        return;
    }

    input_->Clear();
    SendTurn(text);
}

void AiChatPanel::OnStop(wxCommandEvent&)
{
    if (client_) client_->Cancel();
    busy_ = false;
    ShowBusyButtons(false);
    sendBtn_->Enable(hasProvider_);
    stopBtn_->Enable(false);
    input_->Enable(true);
    connChoice_->Enable(true);
    dbChoice_->Enable(true);
}

// Enter (no Ctrl) sends; Ctrl+Enter inserts a newline. Both are consumed so the
// multiline control never also inserts a stray line break. Everything else passes
// through untouched.
void AiChatPanel::OnInputKey(wxKeyEvent& event)
{
    const int k = event.GetKeyCode();
    if (k == WXK_RETURN || k == WXK_NUMPAD_ENTER) {
        if (event.ControlDown()) {
            if (input_) input_->WriteText("\n");   // Ctrl+Enter → newline
            return;                                // eat (no default handling)
        }
        if (busy_ || !hasProvider_) return;        // ignore while streaming / no provider
        wxCommandEvent ev;
        OnSend(ev);                                // Enter → send
        return;                                    // eat (no newline)
    }
    event.Skip();
}

// Swap the send ⇄ stop circle in the shared input-bar slot.
void AiChatPanel::ShowBusyButtons(bool busy)
{
    if (sendBtn_) sendBtn_->Show(!busy);
    if (stopBtn_) stopBtn_->Show(busy);
    if (sendBtn_ && sendBtn_->GetParent()) sendBtn_->GetParent()->Layout();
}

// -------------------------------------------------------------------------- helpers

db::IConnection* AiChatPanel::SelectedConnection() const
{
    const int i = connChoice_->GetSelection();
    if (i == wxNOT_FOUND || !host_.connection) return nullptr;
    return host_.connection(connChoice_->GetClientData(i));
}

wxString AiChatPanel::SelectedDatabase() const
{
    const int i = dbChoice_->GetSelection();
    return (i != wxNOT_FOUND) ? dbChoice_->GetString(i) : wxString();
}

} // namespace ui
