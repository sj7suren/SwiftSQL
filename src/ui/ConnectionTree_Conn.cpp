// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionTree_Conn.cpp — connection lifecycle (disconnect / edit / delete /
// duplicate / colour) and the new-connection entry points, split out of
// ConnectionTree.cpp per the ≤1000-line charter (docs/CHARTER.md). The method
// bodies below are the exact originals; behavior is unchanged.
#include "ui/ConnectionTree.h"
#include "ui/ConnectionTreeInternal.h"

#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>
#include <algorithm>

#include "core/ConnectionStore.h"
#include "core/GroupStore.h"   // 分组 membership follows a rename / delete
#include "net/SshTunnel.h"
#include "ui/ConnectionDialog.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

namespace ui {

// ---------------------------------------------------------------------------
void ConnectionTree::DisconnectEntry(ConnEntry* e)
{
    if (!e || !e->IsConnected()) return;
    AbortCatWorker();   // stop the loader before the connection it may be querying is freed
    // 无条件通知:表列表标签可能正显示这个 entry(即便它不是 active_),必须在
    // conn 释放前让 MainFrame 停掉相关 worker 并清引用,否则悬空/UAF。
    if (hooks_.entryClosing) hooks_.entryClosing(e);
    if (active_ == e) { if (hooks_.beforeClose) hooks_.beforeClose(e); active_ = nullptr; }
    e->conn.reset();
    e->tunnel.reset();          // tear down the SSH tunnel after the DB socket
    e->currentDb.clear();
    tree_->DeleteChildren(e->node);
    tree_->Collapse(e->node);
    tree_->SetItemHasChildren(e->node, true);   // keep the expand chevron (re-connect on expand)
    SetConnIcon(e);   // → grey
    hooks_.status(tr(L"已断开") + L" — " + e->profile.name);
}

void ConnectionTree::EditConnection(ConnEntry* e)
{
    if (!e) return;
    const wxString oldName = e->profile.name;
    ConnectionDialog dlg(dialogParent_, e->profile.type, &e->profile);
    if (dlg.ShowModal() != wxID_OK) return;

    core::ConnectionProfile p = dlg.GetProfile();
    p.color = e->profile.color;   // dialog doesn't carry colour — keep the existing one
    if (p.name != oldName) {
        core::ConnectionStore::Remove(oldName);
        // 分组 membership is keyed by connection NAME (a pointer could not
        // survive a restart), so a rename must carry it or the connection
        // silently falls out of its folder on the next launch — while STILL
        // rendering inside it until then, which is the worst kind of wrong.
        core::GroupStore::RenameMember(core::GroupStore::ConnScope(), oldName, p.name);
    }
    core::ConnectionStore::Save(p);
    e->profile = p;
    tree_->SetItemText(e->node, p.name);
    SetConnIcon(e);
    ApplyConnColor(e);
}

void ConnectionTree::DeleteConnection(ConnEntry* e)
{
    if (!e) return;
    if (wxMessageBox(tr(L"确定删除此连接?") + L"\n\n" + e->profile.name,
                     tr(L"删除连接"), wxYES_NO | wxICON_QUESTION, dialogParent_) != wxYES)
        return;
    AbortCatWorker();   // stop the loader before this entry / its node is destroyed
    // 无条件通知(见 DisconnectEntry):在 conns_.erase 销毁 ConnEntry 之前清引用。
    if (hooks_.entryClosing) hooks_.entryClosing(e);
    if (active_ == e) { if (hooks_.beforeClose) hooks_.beforeClose(e); active_ = nullptr; }
    core::ConnectionStore::Remove(e->profile.name);
    // Drop its 分组 membership too — otherwise a later connection created with
    // the same name would inherit a folder it was never put in.
    core::GroupStore::RemoveMember(core::GroupStore::ConnScope(), e->profile.name);
    tree_->Delete(e->node);
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                     [e](const std::unique_ptr<ConnEntry>& up) { return up.get() == e; }),
                 conns_.end());
}

void ConnectionTree::ApplyConnColor(ConnEntry* e)
{
    if (!e || !e->node.IsOk()) return;
    // Empty → fall back to the tree's default body colour; a hex string colours
    // just this first-level node's label.
    const wxColour c = e->profile.color.IsEmpty()
                           ? theme::kTextBody
                           : wxColour(e->profile.color);
    tree_->SetItemTextColour(e->node, c.IsOk() ? c : theme::kTextBody);
}

void ConnectionTree::SetConnColor(ConnEntry* e, const wxString& hex)
{
    if (!e) return;
    e->profile.color = hex;
    core::ConnectionStore::Save(e->profile);   // persist alongside the profile
    ApplyConnColor(e);
}

void ConnectionTree::DuplicateConnection(ConnEntry* e)
{
    if (!e) return;
    core::ConnectionProfile p = e->profile;
    // give the copy a unique name: "X 副本", "X 副本 2", …
    auto taken = [this](const wxString& nm) {
        for (auto& up : conns_) if (up->profile.name == nm) return true;
        return false;
    };
    const wxString base = p.name + L" " + tr(L"副本");
    wxString name = base;
    for (int n = 2; taken(name); ++n) name = base + wxString::Format(L" %d", n);
    p.name = name;
    core::ConnectionStore::Save(p);
    AddConnection(p);   // grey node, not connected
}

// ---------------------------------------------------------------------------
void ConnectionTree::ShowNewConnectionMenu()
{
    wxMenu menu;
    for (const db::DbTypeInfo& info : db::AllDbTypes()) {
        auto* item = new wxMenuItem(&menu, wxID_ANY, info.name);
        item->SetBitmap(icons::DbBrand(info.type, 16));
        menu.Append(item);
        const db::DbType type = info.type;
        menu.Bind(wxEVT_MENU, [this, type](wxCommandEvent&) {
            OpenConnectionDialog(type);
        }, item->GetId());
    }
    tree_->PopupMenu(&menu);
}

void ConnectionTree::PopupNodeMenu(ConnEntry* e)
{
    if (e && e->node.IsOk()) ShowNodeContextMenu(e->node);
}

void ConnectionTree::NewConnection(db::DbType preset)
{
    OpenConnectionDialog(preset);
}

void ConnectionTree::OpenConnectionDialog(db::DbType preset)
{
    ConnectionDialog dlg(dialogParent_, preset);
    if (dlg.ShowModal() != wxID_OK) return;

    core::ConnectionProfile p = dlg.GetProfile();
    core::ConnectionStore::Save(p);
    ConnEntry* e = AddConnection(p);
    ConnectEntry(e);
}

} // namespace ui
