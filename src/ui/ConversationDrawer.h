// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConversationDrawer.h — the left-hand history drawer for the AI chat page.
//
// A slim (~220px) white column that lives INSIDE the chat area, left of the
// transcript. Top: a full-width "＋ 新对话" entry. Below: a scrollable list of past
// conversations (newest first) showing each title + a relative timestamp, with a
// per-row delete ✕. The currently-open conversation is highlighted.
//
// The drawer owns no data model of its own — it is a pure view over
// ConversationStore::List(). AiChatPanel calls Reload() after every mutation (new
// turn saved / conversation deleted / switched) and the drawer repaints. Open / new /
// delete intents are surfaced back through the three callbacks.
#pragma once

#include <wx/panel.h>
#include <wx/string.h>

#include <functional>
#include <vector>

class wxBoxSizer;
class wxScrolledWindow;
class wxWindow;

namespace ui {

class ConversationDrawer : public wxPanel {
public:
    ConversationDrawer(wxWindow* parent,
                       std::function<void()>                   onNew,
                       std::function<void(const wxString& id)> onOpen,
                       std::function<void(const wxString& id)> onDelete,
                       // Batch (multi-select) delete: fired once with every checked id
                       // so the host can delete + refresh in a single pass.
                       std::function<void(const std::vector<wxString>& ids)> onDeleteMany,
                       // Collapse (hide) the drawer — fired by the "«" header icon.
                       std::function<void()> onCollapse);

    // Rebuild the list from ConversationStore::List(); `activeId` is highlighted.
    void Reload(const wxString& activeId);

private:
    void RebuildHeader();                 // repaint the header for the current mode
    void EnterSelectMode();               // normal → multi-select (checkboxes)
    void ExitSelectMode();                // back to normal (rows open on click again)
    void OnRowToggled(const wxString& id, bool selected);  // a row's checkbox flipped
    void ToggleSelectAll();               // 全选 ⇄ 全不选 over the visible rows
    void CommitBatchDelete();             // confirm → onDeleteMany_ → exit select mode
    bool IsSelected(const wxString& id) const;
    void UpdateDeleteLabel();             // refresh the "删除(N)" count in the header

    std::function<void()>                   onNew_;
    std::function<void(const wxString& id)> onOpen_;
    std::function<void(const wxString& id)> onDelete_;
    std::function<void(const std::vector<wxString>& ids)> onDeleteMany_;
    std::function<void()>                   onCollapse_;

    wxBoxSizer*       root_       = nullptr;
    wxBoxSizer*       headerSizer_ = nullptr;   // horizontal header button strip
    wxScrolledWindow* list_       = nullptr;
    wxBoxSizer*       listSizer_  = nullptr;
    wxString          activeId_;

    bool                     selectMode_ = false;
    std::vector<wxString>    selected_;          // checked conversation ids
    std::vector<wxString>    allIds_;            // ids currently listed (for 全选)
    std::vector<wxWindow*>   rows_;              // live ConvRow* (for select-all repaint)
    wxWindow*                delCountBtn_ = nullptr;  // the "删除(N)" header button (TinyButton*)
};

} // namespace ui
