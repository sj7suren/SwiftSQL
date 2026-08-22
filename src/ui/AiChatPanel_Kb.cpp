// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiChatPanel_Kb.cpp — the knowledge-base + model-picker half of AiChatPanel, split
// into its own translation unit to keep AiChatPanel.cpp under the 1000-line charter
// limit (same class, cross-TU member definitions — mirrors the MainFrame_*.cpp /
// AiChatPanel_Conversations.cpp split).
//
// Responsibilities:
//   • KbBusyShow / KbBusyHide — show/hide the determinate KB progress bar
//   • KbSetProgress           — drive the bar (0→70 real, 70→95 creep during LLM pass)
//   • RebuildKnowledge        — (re)build the KB for the selected conn+db (cache-aware)
//   • KbCacheKey              — stable per-(connection, database) cache key
//   • FetchModels             — lazy, idle-only ListModels → models_ cache
//   • ShowModelMenu           — pill → wxMenu of selectable models (current checked)
#include "ui/AiChatPanel.h"

#include <wx/gauge.h>
#include <wx/menu.h>
#include <wx/stattext.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "ai/AiClient.h"
#include "ai/AiConfig.h"
#include "ai/AiConfigStore.h"
#include "db/DbDriver.h"
#include "ui/AiChatWidgets.h"
#include "ui/AiKnowledgeStore.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace ui {

// ------------------------------------------------------------------- knowledge base

// Show the determinate bar reset to 0%. Progress is then fed via KbSetProgress.
void AiChatPanel::KbBusyShow()
{
    if (!kbBusy_) return;
    if (kbClimb_.IsRunning()) kbClimb_.Stop();
    kbBusy_->SetValue(0);
    kbBusy_->Show();
    if (kbBusy_->GetParent()) kbBusy_->GetParent()->Layout();
}

void AiChatPanel::KbBusyHide()
{
    if (kbClimb_.IsRunning()) kbClimb_.Stop();
    if (!kbBusy_) return;
    kbBusy_->Hide();
    if (kbBusy_->GetParent()) kbBusy_->GetParent()->Layout();
}

// Drive the bar from a KB build progress report (GUI thread). −1 = keep current.
// 0→70 is real per-table introspection progress; at 70 (the LLM verify phase begins)
// we start a slow time-based creep toward 95 so the user still sees motion during the
// single, indivisible LLM call. 100 (or any completion) stops the creep.
void AiChatPanel::KbSetProgress(int percent)
{
    if (!kbBusy_ || percent < 0) return;
    kbBusy_->SetValue(std::min(100, percent));
    if (percent >= 70 && percent < 100) {
        if (!kbClimb_.IsRunning()) kbClimb_.Start(220);
    } else if (kbClimb_.IsRunning()) {
        kbClimb_.Stop();
    }
}

void AiChatPanel::RebuildKnowledge(bool forceRebuild)
{
    const int ci = connChoice_->GetSelection();
    void* handle = (ci != wxNOT_FOUND) ? connChoice_->GetClientData(ci) : nullptr;
    db::IConnection* conn = SelectedConnection();   // only to gate "is a live conn picked?"
    const wxString db = SelectedDatabase();
    if (!conn || !handle || db.IsEmpty()) {
        KbBusyHide();
        kbStatus_->SetLabel(tr(L"请选择连接与数据库以构建知识库"));
        kbStatus_->SetForegroundColour(theme::kTextSecondary);
        if (kbStrip_ && !kbStrip_->IsShown()) { kbStrip_->Show(); Layout(); }  // re-expand: show the prompt
        return;
    }

    const wxString key = KbCacheKey();

    kb_.Cancel();   // abandon any prior in-flight build

    // Cache pre-check: if a ready KB is already cached (and we're not forcing a
    // rebuild) we do NOT even open a dedicated connection — Build reloads the
    // encrypted cache instantly. Passing a null `dedicated` is correct here: Build's
    // own cache hit returns before it ever needs the connection.
    std::unique_ptr<db::IConnection> dedicated;
    bool cacheHit = false;
    if (!forceRebuild) {
        AiKnowledge probe;
        cacheHit = AiKnowledgeStore::Load(key, probe) && probe.ready;
    }

    if (!cacheHit) {
        kbStatus_->SetLabel(tr(L"正在理解数据库结构…"));
        kbStatus_->SetForegroundColour(theme::kTextSecondary);
        sendBtn_->Enable(false);
        if (kbStrip_ && !kbStrip_->IsShown()) { kbStrip_->Show(); Layout(); }  // re-expand while building
        KbBusyShow();

        // Build a DEDICATED connection for the KB here on the GUI thread (its connect /
        // SSH steps must not run on the worker), then hand ownership to kb_.Build, whose
        // introspection worker becomes its sole owner. It never shares the UI's live
        // connection, so the driver can't be raced (no heap corruption) and closing the
        // UI connection can't dangle it (no UAF).
        wxString cerr;
        dedicated = host_.makeConnection ? host_.makeConnection(handle, cerr) : nullptr;
        if (!dedicated) {
            KbBusyHide();
            kbStatus_->SetLabel(tr(L"无法为知识库建立连接：") + cerr);
            kbStatus_->SetForegroundColour(theme::kDotAmber);
            sendBtn_->Enable(hasProvider_ && !busy_);
            return;
        }
    }

    kb_.Build(
        std::move(dedicated), db, key, client_.get(), forceRebuild,
        [this](const wxString& status, int percent) {   // onProgress (GUI thread)
            kbStatus_->SetLabel(status);
            kbStatus_->SetForegroundColour(theme::kTextSecondary);
            KbSetProgress(percent);
        },
        [this](const AiKnowledge& k) {            // onDone (GUI thread)
            KbBusyHide();
            if (k.ready) {
                kbStatus_->SetLabel(tr(L"知识库已就绪 ✓"));
                kbStatus_->SetForegroundColour(theme::kGreen);
                // Ready → collapse the whole strip so the transcript grows. The user
                // already watched the progress; the 更新知识库 button in the top bar
                // remains available to trigger a rebuild (which re-expands this).
                if (kbStrip_ && kbStrip_->IsShown()) { kbStrip_->Hide(); Layout(); }
            } else {
                wxString msg = tr(L"表结构读取失败");
                if (!k.error.IsEmpty()) msg += L"：" + k.error;
                kbStatus_->SetLabel(msg);
                kbStatus_->SetForegroundColour(theme::kDotAmber);
                if (kbStrip_ && !kbStrip_->IsShown()) { kbStrip_->Show(); Layout(); }  // keep error visible
            }
            // Sending is enabled whenever a provider exists; readiness only affects
            // whether the turn carries schema grounding.
            sendBtn_->Enable(hasProvider_ && !busy_);
        });
}

// Stable per-(connection, database) cache key: the connection's display label + a
// unit separator + the database name. AiKnowledgeStore hashes it into a safe
// filename, so any characters in either part are fine.
wxString AiChatPanel::KbCacheKey() const
{
    const int ci = connChoice_->GetSelection();
    const wxString connLabel = (ci != wxNOT_FOUND) ? connChoice_->GetString(ci) : wxString();
    return connLabel + wxString(L"\x1f") + SelectedDatabase();
}

// ---------------------------------------------------------------------------- models

// Lazy, idle-only model discovery. The AiClient serves one request at a time and is
// shared with chat + the KB's verify pass, so we only fetch when it is NOT busy; on
// success we cache the ids in models_ (the pill keeps showing currentModel_). A
// failure is silent — the picker still offers the configured model.
void AiChatPanel::FetchModels()
{
    if (!client_ || client_->Busy()) return;
    client_->ListModels([this](std::vector<wxString> models, ai::AiError err) {
        if (!err.ok()) return;
        models_ = std::move(models);
    });
}

void AiChatPanel::ShowModelMenu()
{
    if (!hasProvider_) return;

    // Kick a one-shot fetch the first time (when idle); results show next open.
    if (models_.empty()) FetchModels();

    const ai::AiSettings settings = ai::AiConfigStore::Load();
    const ai::AiProviderConfig* prov = settings.DefaultProvider();
    const wxString configModel = prov ? prov->model : wxString();
    const wxString active = currentModel_.IsEmpty() ? configModel : currentModel_;

    // Ordered, de-duplicated list: configured model first, then anything fetched.
    std::vector<wxString> list;
    auto addUnique = [&list](const wxString& m) {
        if (m.IsEmpty()) return;
        for (const wxString& x : list) if (x == m) return;
        list.push_back(m);
    };
    addUnique(configModel);
    addUnique(active);
    for (const wxString& m : models_) addUnique(m);

    wxMenu menu;
    wxMenuItem* title = menu.Append(wxID_ANY, tr(L"模型"));
    title->Enable(false);                       // non-clickable header row
    menu.AppendSeparator();

    const int id0 = wxID_HIGHEST + 4300;
    for (size_t i = 0; i < list.size(); ++i) {
        wxMenuItem* it = menu.AppendCheckItem(id0 + static_cast<int>(i), list[i]);
        if (list[i] == active) it->Check(true);
    }

    const int r = modelPill_->GetPopupMenuSelectionFromUser(
        menu, wxPoint(0, modelPill_->GetSize().y));
    if (r == wxID_NONE) return;
    const int idx = r - id0;
    if (idx >= 0 && idx < static_cast<int>(list.size())) {
        currentModel_ = list[idx];
        modelPill_->SetText(currentModel_);
    }
}

} // namespace ui
