// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionTree_Groups.cpp — 分组管理: user-defined folders in the sidebar, at
// two levels, sharing one implementation.
//
// ===========================================================================
// WHAT A GROUP IS HERE
// ===========================================================================
//   * CONNECTION groups sit under the tree root and contain connections.
//   * OBJECT groups sit under a database's 表/视图/函数/存储过程 folder and
//     contain that folder's leaves.
// Both are the same shape — an ordered list of names plus a member→group map —
// so both persist through core::GroupStore under different SCOPES, and the two
// levels differ here only in which parent node they hang from and which scope
// string they use. Everything else (create / rename / delete / move / drag) is
// written once.
//
// Groups are a VIEW concern only. Nothing in this file issues SQL, and deleting
// a group never deletes what is in it — a folder that could destroy connections
// or tables would be a data-loss bug wearing a folder icon.
//
// ===========================================================================
// WHY MOVING A NODE MEANS RE-CREATING IT
// ===========================================================================
// wxTreeCtrl has no reparent. Moving a node is delete + re-append, and the two
// levels pay very different prices for that, so they are handled differently:
//
//   OBJECT leaves are leaves. Re-creating one costs nothing, so RegroupCategory
//   rebuilds the whole level from the leaves ALREADY IN THE TREE — no server
//   round-trip, which is why moving one table does not re-list the schema.
//
//   CONNECTION nodes own a whole loaded subtree (databases, categories, tables).
//   Re-creating one drops that subtree, so MoveConnectionToGroup re-runs
//   LoadDatabases for a live connection — the same repopulate 刷新 does. It also
//   AbortCatWorker()s first: a category load in flight holds tree nodes that are
//   about to be freed, and its queued CallAfter must be invalidated before the
//   delete, not after. That is the established pattern for every structural
//   delete in this class, and the one place it was missed was a real crash.
#include "ui/ConnectionTree.h"
#include "ui/ConnectionTreeInternal.h"

#include <wx/wx.h>
#include <wx/textdlg.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>
#include <vector>

#include "core/GroupStore.h"
#include "ui/I18n.h"
#include "ui/ObjectTemplates.h"   // ObjectKind (the GroupTrack* overloads)

namespace ui {
namespace {

// One already-loaded object leaf, captured so the level can be rebuilt without
// asking the server again. `data` is a COPY of the leaf's NodeData fields — the
// original wxTreeItemData is destroyed with its node.
struct LeafSnapshot {
    wxString  label;
    int       image = 0;
    wxString  db;
    wxString  table;
    Category  cat = Category::Tables;
    ConnEntry* entry = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// Scopes — the ONLY place a GroupStore scope string is built.
// ---------------------------------------------------------------------------
wxString ConnectionTree::ConnScope() const
{
    return core::GroupStore::ConnScope();
}

wxString ConnectionTree::ObjScope(ConnEntry* e, const wxString& db, Category cat) const
{
    // Keyed on the connection NAME, not the ConnEntry pointer: the grouping must
    // survive a restart, and a pointer does not. Names are unique across saved
    // connections (ConnectionStore keys on them), so this is a stable identity.
    return core::GroupStore::ObjectScope(e ? e->profile.name : wxString(), db,
                                         CategoryTag(cat));
}

// ---------------------------------------------------------------------------
// Group-node lookup / placement
// ---------------------------------------------------------------------------
wxTreeItemId ConnectionTree::FindGroupNode(const wxTreeItemId& parent,
                                           const wxString& name) const
{
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = tree_->GetFirstChild(parent, cookie); c.IsOk();
         c = tree_->GetNextChild(parent, cookie)) {
        auto* d = dynamic_cast<NodeData*>(tree_->GetItemData(c));
        if (d && d->kind == NodeData::Group && d->group == name) return c;
    }
    return wxTreeItemId();
}

// Group folders are kept ABOVE ungrouped members at their level, so a new group
// is inserted after the last existing group rather than appended at the end.
size_t ConnectionTree::GroupNodeCount(const wxTreeItemId& parent) const
{
    size_t n = 0;
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = tree_->GetFirstChild(parent, cookie); c.IsOk();
         c = tree_->GetNextChild(parent, cookie)) {
        auto* d = dynamic_cast<NodeData*>(tree_->GetItemData(c));
        if (d && d->kind == NodeData::Group) ++n;
    }
    return n;
}

// Create any connection-group folder the store knows about but the tree does not
// yet show, in STORE ORDER. Idempotent — safe to call after every mutation.
void ConnectionTree::SyncConnGroupNodes()
{
    for (const wxString& g : core::GroupStore::Groups(ConnScope())) {
        if (FindGroupNode(root_, g).IsOk()) continue;
        auto* gd = new NodeData(NodeData::Group, nullptr);
        gd->group = g;
        const wxTreeItemId n = tree_->InsertItem(root_, GroupNodeCount(root_), g,
                                                 IMG_GROUP, IMG_GROUP, gd);
        // Keep the chevron even while empty: a group the user just created has
        // no members yet, and a chevron-less folder reads as "broken".
        tree_->SetItemHasChildren(n, true);
        tree_->Expand(n);
    }
}

void ConnectionTree::CreateConnectionNode(ConnEntry* e)
{
    if (!e) return;
    const wxString g = core::GroupStore::GroupOf(ConnScope(), e->profile.name);
    wxTreeItemId parent = root_;
    if (!g.IsEmpty()) {
        SyncConnGroupNodes();                    // the folder may not exist yet
        const wxTreeItemId gn = FindGroupNode(root_, g);
        if (gn.IsOk()) parent = gn;              // else: fall back to the root
    }
    const int img = ConnImg(e->profile.type, e->IsConnected());
    e->node = tree_->AppendItem(parent, e->profile.name, img, img,
                                new NodeData(NodeData::Connection, e));
    tree_->SetItemBold(e->node, true);
    tree_->SetItemHasChildren(e->node, true);
    ApplyConnColor(e);
    if (parent != root_) tree_->Expand(parent);
}

// Walk connection → database → category. Shared with RefreshCategory, which
// needs the same node for a different reason (reload vs repaint).
wxTreeItemId ConnectionTree::FindCategoryNode(ConnEntry* e, const wxString& db,
                                              Category cat) const
{
    if (!isLive(e) || !e->node.IsOk()) return wxTreeItemId();

    wxTreeItemIdValue dbCookie;
    for (wxTreeItemId dbNode = tree_->GetFirstChild(e->node, dbCookie); dbNode.IsOk();
         dbNode = tree_->GetNextChild(e->node, dbCookie)) {
        auto* dd = dynamic_cast<NodeData*>(tree_->GetItemData(dbNode));
        if (!dd || dd->kind != NodeData::Database || dd->db != db) continue;
        // Database not opened → its category folders have not been built yet.
        if (!dd->loaded) return wxTreeItemId();

        wxTreeItemIdValue catCookie;
        for (wxTreeItemId catNode = tree_->GetFirstChild(dbNode, catCookie); catNode.IsOk();
             catNode = tree_->GetNextChild(dbNode, catCookie)) {
            auto* cd = dynamic_cast<NodeData*>(tree_->GetItemData(catNode));
            if (cd && cd->kind == NodeData::CategoryNode && cd->cat == cat) return catNode;
        }
        return wxTreeItemId();   // database found but this engine has no such folder
    }
    return wxTreeItemId();
}

// ---------------------------------------------------------------------------
// Object level: rebuild one category folder's children into group folders.
// ---------------------------------------------------------------------------
void ConnectionTree::RegroupCategory(const wxTreeItemId& catNode)
{
    auto* cd = catNode.IsOk() ? dynamic_cast<NodeData*>(tree_->GetItemData(catNode))
                              : nullptr;
    if (!cd || cd->kind != NodeData::CategoryNode) return;

    // STILL LOADING → do nothing. The only child right now is the 加载中…
    // placeholder, which carries no NodeData; rebuilding here would delete it and
    // render an empty folder list while the query is still in flight. The load's
    // own completion calls back into here once the real members are in.
    if (!cd->loaded) return;

    const wxString scope = ObjScope(cd->entry, cd->db, cd->cat);
    const std::vector<wxString> groups = core::GroupStore::Groups(scope);
    const std::map<wxString, wxString> member = core::GroupStore::Memberships(scope);

    // NOTHING TO DO IS THE COMMON CASE. A user with no groups in this scope must
    // pay no rebuild at all — the level already renders exactly right, and a
    // needless delete/re-append would collapse selection and scroll position on
    // every single category load.
    //
    // BUT "the store has no groups" IS NOT THE SAME QUESTION as "the tree shows
    // no folders", and conflating them was a real bug: deleting the LAST group of
    // a category updated the store, came back here, saw an empty group list, and
    // returned — leaving the just-deleted folder still drawn in the tree with its
    // tables inside it. Nothing appeared to happen. The tree must therefore be
    // asked too, and it is the side that decides whether a rebuild is owed.
    if (groups.empty() && GroupNodeCount(catNode) == 0) return;

    // Snapshot every leaf currently under this category, wherever it sits (top
    // level or already inside a folder), then rebuild. Leaves WITHOUT NodeData
    // (trigger names) are skipped and would be lost, which is exactly why
    // AppendGroupMenu does not offer grouping for 触发器.
    std::vector<LeafSnapshot> leaves;
    std::vector<wxTreeItemId> stack{ catNode };
    while (!stack.empty()) {
        const wxTreeItemId parent = stack.back();
        stack.pop_back();
        wxTreeItemIdValue cookie;
        for (wxTreeItemId c = tree_->GetFirstChild(parent, cookie); c.IsOk();
             c = tree_->GetNextChild(parent, cookie)) {
            auto* d = dynamic_cast<NodeData*>(tree_->GetItemData(c));
            if (d && d->kind == NodeData::Group) { stack.push_back(c); continue; }
            if (!d || d->kind != NodeData::Table) continue;
            LeafSnapshot s;
            s.label = tree_->GetItemText(c);
            s.image = tree_->GetItemImage(c);
            s.db    = d->db;
            s.table = d->table;
            s.cat   = d->cat;
            s.entry = d->entry;
            leaves.push_back(std::move(s));
        }
    }
    // NO `if (leaves.empty()) return;` HERE. It used to stand in for "still
    // loading", which the cd->loaded check above now answers properly — and as a
    // leaf-count test it ALSO swallowed the legitimate case of a genuinely empty
    // category, where deleting its last group must still clear the stale folder.
    // An empty category simply rebuilds into an empty (or folders-only) level.
    tree_->DeleteChildren(catNode);

    auto appendLeaf = [&](const wxTreeItemId& parent, const LeafSnapshot& s) {
        auto* nd = new NodeData(NodeData::Table, s.entry, s.db, s.table);
        nd->cat = s.cat;
        tree_->AppendItem(parent, s.label, s.image, s.image, nd);
    };

    // Folders first (store order), then whatever is ungrouped — the same
    // top-to-bottom rule the connection level uses.
    std::map<wxString, wxTreeItemId> folder;
    for (const wxString& g : groups) {
        auto* gd = new NodeData(NodeData::Group, cd->entry, cd->db);
        gd->group = g;
        gd->cat   = cd->cat;
        const wxTreeItemId n = tree_->AppendItem(catNode, g, IMG_GROUP, IMG_GROUP, gd);
        tree_->SetItemHasChildren(n, true);
        folder[g] = n;
    }
    for (const LeafSnapshot& s : leaves) {
        auto it = member.find(s.table);
        wxTreeItemId parent = catNode;
        if (it != member.end()) {
            auto f = folder.find(it->second);
            if (f != folder.end()) parent = f->second;
        }
        appendLeaf(parent, s);
    }
    for (const auto& f : folder) tree_->Expand(f.second);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
// Prompt for a name and create the group in `scope`. Returns the created name,
// or "" if the user cancelled or the store refused it. Split out of NewGroupUi
// because the 移动到分组 submenu also needs "create one, then move into it" —
// and a second copy of the prompt+AddGroup+complain sequence is how the two
// entry points would end up validating names differently.
wxString ConnectionTree::PromptNewGroup(const wxString& scope)
{
    const wxString name = wxGetTextFromUser(tr(L"新分组名称:"), tr(L"新建分组"),
                                            wxEmptyString, dialogParent_);
    if (name.IsEmpty()) return wxString();   // cancelled, or empty — same no-op

    if (!core::GroupStore::AddGroup(scope, name)) {
        // The store refuses duplicates and blank names; say WHICH, because
        // "nothing happened" after typing a name reads as a broken button.
        wxMessageBox(tr(L"分组名已存在或无效:") + L" " + name, tr(L"新建分组"),
                     wxOK | wxICON_INFORMATION, dialogParent_);
        return wxString();
    }
    return name;
}

void ConnectionTree::NewGroupUi(ConnEntry* e, const wxString& db, Category cat,
                                bool connLevel)
{
    const wxString scope = connLevel ? ConnScope() : ObjScope(e, db, cat);
    const wxString name  = PromptNewGroup(scope);
    if (name.IsEmpty()) return;

    if (connLevel) {
        SyncConnGroupNodes();
    } else {
        // Find the category node this scope belongs to and repaint it. The
        // caller may have invoked this from the category node OR from a leaf
        // inside it, so re-derive it rather than trusting a passed-in item.
        RegroupCategory(FindCategoryNode(e, db, cat));
    }
    hooks_.status(tr(L"已创建分组:") + L" " + name);
}

void ConnectionTree::RenameGroupUi(const wxTreeItemId& groupNode)
{
    auto* d = groupNode.IsOk() ? dynamic_cast<NodeData*>(tree_->GetItemData(groupNode))
                               : nullptr;
    if (!d || d->kind != NodeData::Group) return;

    const bool connLevel = (d->entry == nullptr);
    const wxString scope = connLevel ? ConnScope() : ObjScope(d->entry, d->db, d->cat);
    const wxString oldName = d->group;
    const wxString name = wxGetTextFromUser(tr(L"分组名称:"), tr(L"重命名分组"),
                                            oldName, dialogParent_);
    if (name.IsEmpty() || name == oldName) return;

    if (!core::GroupStore::RenameGroup(scope, oldName, name)) {
        wxMessageBox(tr(L"分组名已存在或无效:") + L" " + name, tr(L"重命名分组"),
                     wxOK | wxICON_INFORMATION, dialogParent_);
        return;
    }
    // The store carried the members across; the tree only needs the label. NO
    // rebuild here on purpose — a rename must not collapse the user's expanded
    // databases just to change a caption.
    d->group = name;
    tree_->SetItemText(groupNode, name);
}

void ConnectionTree::DeleteGroupUi(const wxTreeItemId& groupNode)
{
    auto* d = groupNode.IsOk() ? dynamic_cast<NodeData*>(tree_->GetItemData(groupNode))
                               : nullptr;
    if (!d || d->kind != NodeData::Group) return;

    const bool connLevel = (d->entry == nullptr);
    const wxString scope = connLevel ? ConnScope() : ObjScope(d->entry, d->db, d->cat);
    const wxString name  = d->group;

    // Say plainly what survives. "删除分组" next to a list of connections reads
    // as "delete these connections" unless the dialog rules it out.
    if (wxMessageBox(tr(L"删除分组") + L" \"" + name + L"\" ?\n" +
                         tr(L"组内项目不会被删除,只会移回上一级。"),
                     tr(L"删除分组"), wxYES_NO | wxICON_QUESTION, dialogParent_) != wxYES)
        return;

    ConnEntry* const e   = d->entry;      // captured before the node dies
    const wxString   db  = d->db;
    const Category   cat = d->cat;
    core::GroupStore::RemoveGroup(scope, name);

    if (connLevel) {
        // Members OUT first, then the (now childless) folder. Deleting the folder
        // first would take the connection nodes down with it.
        EvacuateConnGroup(groupNode);
        tree_->Delete(groupNode);
    } else {
        RegroupCategory(FindCategoryNode(e, db, cat));
    }
    hooks_.status(tr(L"已删除分组:") + L" " + name);
}

// Re-home every connection currently inside `groupNode`, leaving the folder
// childless but intact. Shared by 删除分组 (which then deletes the folder) and
// 清空分组 (which keeps it) — the rescue is identical and the two must not drift.
//
// Each connection is re-created wherever GroupStore NOW says it belongs, which
// is the root once the caller has already cleared the membership. The
// AbortCatWorker is the usual precondition for a structural delete: a category
// load in flight holds nodes we are about to free.
void ConnectionTree::EvacuateConnGroup(const wxTreeItemId& groupNode)
{
    std::vector<ConnEntry*> orphans;
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = tree_->GetFirstChild(groupNode, cookie); c.IsOk();
         c = tree_->GetNextChild(groupNode, cookie)) {
        auto* d = dynamic_cast<NodeData*>(tree_->GetItemData(c));
        if (d && d->kind == NodeData::Connection && d->entry) orphans.push_back(d->entry);
    }
    if (orphans.empty()) return;

    AbortCatWorker();
    ClearSearchHighlight();
    for (ConnEntry* o : orphans) {
        if (o->node.IsOk()) tree_->Delete(o->node);
        o->node = wxTreeItemId();
        CreateConnectionNode(o);
        if (o->IsConnected()) LoadDatabases(o);   // repopulate, same as 刷新
    }
}

void ConnectionTree::ClearGroupUi(const wxTreeItemId& groupNode)
{
    auto* d = groupNode.IsOk() ? dynamic_cast<NodeData*>(tree_->GetItemData(groupNode))
                               : nullptr;
    if (!d || d->kind != NodeData::Group) return;

    const bool connLevel = (d->entry == nullptr);
    const wxString scope = connLevel ? ConnScope() : ObjScope(d->entry, d->db, d->cat);
    const wxString name  = d->group;
    const size_t   n     = core::GroupStore::MembersOf(scope, name).size();

    // Say so rather than popping a confirm for a no-op. A dialog that asks
    // permission to do nothing trains people to click through dialogs.
    if (n == 0) { hooks_.status(tr(L"分组已经是空的:") + L" " + name); return; }

    if (wxMessageBox(wxString::Format(tr(L"清空分组 \"%s\" 中的 %zu 项?"), name, n) +
                         L"\n" + tr(L"项目不会被删除,只会移回上一级;分组本身保留。"),
                     tr(L"清空分组"), wxYES_NO | wxICON_QUESTION, dialogParent_) != wxYES)
        return;

    ConnEntry* const e   = d->entry;
    const wxString   db  = d->db;
    const Category   cat = d->cat;
    core::GroupStore::ClearGroup(scope, name);

    if (connLevel) EvacuateConnGroup(groupNode);
    else           RegroupCategory(FindCategoryNode(e, db, cat));
    hooks_.status(wxString::Format(tr(L"已清空分组 \"%s\"(%zu 项移回上一级)"), name, n));
}

void ConnectionTree::MoveConnectionToGroup(ConnEntry* e, const wxString& group)
{
    if (!e) return;
    if (core::GroupStore::GroupOf(ConnScope(), e->profile.name) == group) return;
    core::GroupStore::SetMemberGroup(ConnScope(), e->profile.name, group);

    // See the file header: a connection node owns a subtree, so the move is a
    // delete + re-create, and the in-flight category loader must be invalidated
    // before the delete frees the nodes it holds.
    const bool live = e->IsConnected();
    AbortCatWorker();
    ClearSearchHighlight();
    if (e->node.IsOk()) tree_->Delete(e->node);
    e->node = wxTreeItemId();
    CreateConnectionNode(e);
    if (live) LoadDatabases(e);   // repopulate the database list, same as 刷新
    tree_->SelectItem(e->node);
}

void ConnectionTree::MoveObjectToGroup(const wxTreeItemId& leaf, const wxString& group)
{
    auto* d = leaf.IsOk() ? dynamic_cast<NodeData*>(tree_->GetItemData(leaf)) : nullptr;
    if (!d || d->kind != NodeData::Table || !d->entry) return;

    const wxString scope = ObjScope(d->entry, d->db, d->cat);
    if (core::GroupStore::GroupOf(scope, d->table) == group) return;
    core::GroupStore::SetMemberGroup(scope, d->table, group);
    RegroupCategory(FindCategoryNode(d->entry, d->db, d->cat));
}

// A 分组 folder was selected / opened → tell the host which list to show.
//
// WHICH GROUPS GET A LIST VIEW, AND WHY NOT ALL OF THEM. A connection group goes
// to its own view (slot 2, connections are not tables). A 表 group reuses the
// 表信息 grid, which is built from db::TableMeta — so 视图/函数/存储过程 groups
// have no list view at all: their objects have no metadata row to render, and a
// name-only grid with every other column showing "—" would look broken rather
// than minimal. Those folders stay expand-only, which is what they already did.
void ConnectionTree::NotifyGroupSelected(NodeData* d, bool focus)
{
    if (!d || d->kind != NodeData::Group) return;
    if (d->entry == nullptr) {                      // connection group
        if (hooks_.showConnGroup) hooks_.showConnGroup(d->group, focus);
        return;
    }
    if (d->cat != Category::Tables) return;         // see the note above
    active_ = d->entry;
    d->entry->currentDb = d->db;
    if (hooks_.showTableGroup) hooks_.showTableGroup(d->entry, d->db, d->group, focus);
}

// ---------------------------------------------------------------------------
// Bookkeeping for objects the DATABASE changed (rename / drop)
// ---------------------------------------------------------------------------
void ConnectionTree::GroupTrackRename(ConnEntry* e, const wxString& db, Category cat,
                                      const wxString& from, const wxString& to)
{
    if (!e) return;
    core::GroupStore::RenameMember(ObjScope(e, db, cat), from, to);
}

void ConnectionTree::GroupTrackDrop(ConnEntry* e, const wxString& db, Category cat,
                                    const wxString& name)
{
    if (!e) return;
    core::GroupStore::RemoveMember(ObjScope(e, db, cat), name);
}

// ObjectKind → Category, mirroring RefreshCategory's overload. Package has no
// tree folder and therefore no group scope.
namespace {
bool CatForKind(ObjectKind kind, Category& out)
{
    switch (kind) {
    case ObjectKind::View:      out = Category::Views;      return true;
    case ObjectKind::Function:  out = Category::Functions;  return true;
    case ObjectKind::Procedure: out = Category::Procedures; return true;
    case ObjectKind::Package:   break;
    }
    return false;
}
} // namespace

void ConnectionTree::GroupTrackRename(ConnEntry* e, const wxString& db, ObjectKind kind,
                                      const wxString& from, const wxString& to)
{
    Category cat;
    if (CatForKind(kind, cat)) GroupTrackRename(e, db, cat, from, to);
}

void ConnectionTree::GroupTrackDrop(ConnEntry* e, const wxString& db, ObjectKind kind,
                                    const wxString& name)
{
    Category cat;
    if (CatForKind(kind, cat)) GroupTrackDrop(e, db, cat, name);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void ConnectionTree::AppendGroupMenu(wxMenu& menu, const wxTreeItemId& item,
                                     NodeData* data)
{
    if (!data) return;

    // 触发器 leaves carry no NodeData (see ConnectionTree.cpp's Triggers arm), so
    // they cannot be identified after a rebuild and are honestly left out rather
    // than offered a menu item that would drop them.
    auto groupable = [](Category c) { return c != Category::Triggers; };

    switch (data->kind) {
    case NodeData::Group: {
        menu.AppendSeparator();
        wxMenuItem* mr = menu.Append(wxID_ANY, tr(L"重命名分组"));
        menu.Bind(wxEVT_MENU, [this, item](wxCommandEvent&) { RenameGroupUi(item); },
                  mr->GetId());
        wxMenuItem* mc = menu.Append(wxID_ANY, tr(L"清空分组"));
        menu.Bind(wxEVT_MENU, [this, item](wxCommandEvent&) { ClearGroupUi(item); },
                  mc->GetId());
        wxMenuItem* md = menu.Append(wxID_ANY, tr(L"删除分组"));
        menu.Bind(wxEVT_MENU, [this, item](wxCommandEvent&) { DeleteGroupUi(item); },
                  md->GetId());
        const bool connLevel = (data->entry == nullptr);
        ConnEntry* e = data->entry;
        const wxString db = data->db;
        const Category cat = data->cat;
        wxMenuItem* mn = menu.Append(wxID_ANY, tr(L"新建分组"));
        menu.Bind(wxEVT_MENU, [this, e, db, cat, connLevel](wxCommandEvent&) {
            NewGroupUi(e, db, cat, connLevel);
        }, mn->GetId());
        break;
    }
    case NodeData::Connection: {
        menu.AppendSeparator();
        AppendMoveToGroupSub(menu, ConnScope(),
                             core::GroupStore::GroupOf(ConnScope(), data->entry->profile.name),
                             [this, e = data->entry](const wxString& g) {
                                 MoveConnectionToGroup(e, g);
                             });
        wxMenuItem* mn = menu.Append(wxID_ANY, tr(L"新建分组"));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            NewGroupUi(nullptr, wxString(), Category::Tables, /*connLevel*/ true);
        }, mn->GetId());
        break;
    }
    case NodeData::CategoryNode: {
        if (!groupable(data->cat)) break;
        menu.AppendSeparator();
        ConnEntry* e = data->entry;
        const wxString db = data->db;
        const Category cat = data->cat;
        wxMenuItem* mn = menu.Append(wxID_ANY, tr(L"新建分组"));
        menu.Bind(wxEVT_MENU, [this, e, db, cat](wxCommandEvent&) {
            NewGroupUi(e, db, cat, /*connLevel*/ false);
        }, mn->GetId());
        break;
    }
    case NodeData::Table: {
        if (!groupable(data->cat) || !data->entry) break;
        const wxString scope = ObjScope(data->entry, data->db, data->cat);
        menu.AppendSeparator();
        AppendMoveToGroupSub(menu, scope, core::GroupStore::GroupOf(scope, data->table),
                             [this, item](const wxString& g) { MoveObjectToGroup(item, g); });
        break;
    }
    default:
        break;
    }
}

// The 移动到分组 ▶ submenu, identical everywhere it appears: every group in the
// scope with the current one checked, then 移出分组, then 新建分组…
//
// 新建分组… IS WHAT MAKES THIS USABLE ON A FRESH SCOPE. This submenu used to be
// disabled outright when the scope had no groups yet, which is a dead end
// precisely at the moment the user is trying to make their first one: the menu
// names the thing they want and refuses to open. Creating-and-moving in one
// step is also the gesture people actually want ("put these in a new folder"),
// so it earns its place even once groups exist.
void ConnectionTree::AppendMoveToGroupSub(wxMenu& menu, const wxString& scope,
                                          const wxString& current,
                                          std::function<void(const wxString&)> move)
{
    auto* sub = new wxMenu;
    for (const wxString& g : core::GroupStore::Groups(scope)) {
        wxMenuItem* mi = sub->AppendCheckItem(wxID_ANY, g);
        mi->Check(g == current);
        sub->Bind(wxEVT_MENU, [move, g](wxCommandEvent&) { move(g); }, mi->GetId());
    }
    if (sub->GetMenuItemCount() > 0) sub->AppendSeparator();
    wxMenuItem* mo = sub->Append(wxID_ANY, tr(L"移出分组"));
    mo->Enable(!current.IsEmpty());
    sub->Bind(wxEVT_MENU, [move](wxCommandEvent&) { move(wxString()); }, mo->GetId());
    wxMenuItem* mn = sub->Append(wxID_ANY, tr(L"新建分组…"));
    sub->Bind(wxEVT_MENU, [this, scope, move](wxCommandEvent&) {
        const wxString g = PromptNewGroup(scope);
        if (!g.IsEmpty()) move(g);   // created → move straight into it
    }, mn->GetId());
    menu.AppendSubMenu(sub, tr(L"移动到分组"));
}

// ---------------------------------------------------------------------------
// Batch move — the 表信息 list's multi-selection
// ---------------------------------------------------------------------------

// The group a whole SELECTION is in, or "" when they disagree. Used to decide
// which submenu item gets the checkmark: a mixed selection must show none,
// because ticking one of several would claim something untrue about the rest.
wxString ConnectionTree::CommonGroupOf(const wxString& scope,
                                       const std::vector<wxString>& members) const
{
    if (members.empty()) return wxString();
    const std::map<wxString, wxString> m = core::GroupStore::Memberships(scope);
    auto groupOf = [&](const wxString& name) {
        auto it = m.find(name);
        return it == m.end() ? wxString() : it->second;
    };
    const wxString first = groupOf(members.front());
    for (const wxString& n : members)
        if (groupOf(n) != first) return wxString();
    return first;
}

bool ConnectionTree::MoveTablesToGroup(ConnEntry* e, const wxString& db,
                                       const std::vector<wxString>& tables,
                                       const wxString& group)
{
    if (!e || tables.empty()) return false;
    const wxString scope = ObjScope(e, db, Category::Tables);
    for (const wxString& t : tables)
        core::GroupStore::SetMemberGroup(scope, t, group);
    // ONE repaint for the whole batch, not one per table: RegroupCategory
    // rebuilds the level, so calling it inside the loop would rebuild N times
    // and make a 200-table move visibly crawl.
    RegroupCategory(FindCategoryNode(e, db, Category::Tables));
    hooks_.status(group.IsEmpty()
        ? wxString::Format(tr(L"已将 %zu 张表移出分组"), tables.size())
        : wxString::Format(tr(L"已将 %zu 张表移动到分组 "), tables.size()) + group);
    return true;
}

std::vector<ConnEntry*> ConnectionTree::allEntries() const
{
    std::vector<ConnEntry*> out;
    out.reserve(conns_.size());
    for (const auto& up : conns_) out.push_back(up.get());
    return out;
}

bool ConnectionTree::RemoveConnectionsFromGroupUi(const std::vector<wxString>& names)
{
    if (names.empty()) return false;
    bool any = false;
    for (const wxString& n : names) {
        for (const auto& up : conns_)
            if (up->profile.name == n) {
                // Per connection, because each one's node has to be re-created
                // where it now belongs (a connection node owns a subtree — see
                // the file header). There is no cheaper batch form of that.
                MoveConnectionToGroup(up.get(), wxString());
                any = true;
                break;
            }
    }
    if (any)
        hooks_.status(wxString::Format(tr(L"已将 %zu 个连接移出分组"), names.size()));
    return any;
}

bool ConnectionTree::RemoveTablesFromGroupUi(ConnEntry* e, const wxString& db,
                                             const std::vector<wxString>& tables)
{
    // An empty group name IS the "no group" value throughout GroupStore, so
    // 从分组移除 is the same batch operation as a move — no second code path,
    // and therefore no second chance to forget the sidebar repaint.
    return MoveTablesToGroup(e, db, tables, wxString());
}

void ConnectionTree::AppendTableGroupMenuUi(wxMenu& menu, ConnEntry* e,
                                            const wxString& db,
                                            const std::vector<wxString>& tables,
                                            std::function<void()> after)
{
    if (!e || tables.empty()) return;
    const wxString scope = ObjScope(e, db, Category::Tables);
    // The SAME submenu the tree uses, on the same scope — so a table moved from
    // the 表信息 list and one moved from the sidebar land in the same folder,
    // and neither view can grow its own idea of what the groups are.
    AppendMoveToGroupSub(menu, scope, CommonGroupOf(scope, tables),
                         [this, e, db, tables, after](const wxString& g) {
                             if (MoveTablesToGroup(e, db, tables, g) && after) after();
                         });
}

// Right-click on empty sidebar space. Without this, a user whose tree is empty
// (or who clicks below the last node) has no way to create the first group.
void ConnectionTree::OnTreeRightDown(wxMouseEvent& ev)
{
    int flags = 0;
    const wxTreeItemId hit = tree_->HitTest(ev.GetPosition(), flags);
    if (hit.IsOk()) { ev.Skip(); return; }   // a real node → the tree's own event

    wxMenu menu;
    wxMenuItem* mn = menu.Append(wxID_ANY, tr(L"新建分组"));
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        NewGroupUi(nullptr, wxString(), Category::Tables, /*connLevel*/ true);
    }, mn->GetId());
    tree_->PopupMenu(&menu);
}

// ---------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------
void ConnectionTree::OnBeginDrag(wxTreeEvent& ev)
{
    dragItem_ = wxTreeItemId();
    auto* d = dynamic_cast<NodeData*>(tree_->GetItemData(ev.GetItem()));
    if (!d) return;                       // no Allow() → the drag never starts
    // Only the two things a group can hold are draggable. A database or a
    // category folder has no group concept, and letting them be picked up would
    // promise a drop that cannot mean anything.
    const bool ok = (d->kind == NodeData::Connection) ||
                    (d->kind == NodeData::Table && d->entry &&
                     d->cat != Category::Triggers);
    if (!ok) return;
    dragItem_ = ev.GetItem();
    ev.Allow();
}

void ConnectionTree::OnEndDrag(wxTreeEvent& ev)
{
    const wxTreeItemId src = dragItem_;
    dragItem_ = wxTreeItemId();
    if (!src.IsOk()) return;
    auto* sd = dynamic_cast<NodeData*>(tree_->GetItemData(src));
    if (!sd) return;

    const wxTreeItemId dst = ev.GetItem();
    auto* dd = dst.IsOk() ? dynamic_cast<NodeData*>(tree_->GetItemData(dst)) : nullptr;

    // RESOLVING A DROP TARGET TO A GROUP NAME. Dropping ON a folder means "into
    // it"; dropping on a SIBLING means "into whatever holds that sibling", which
    // is what makes dropping onto a top-level connection mean "move out of the
    // group". An unrecognized target is treated as top level rather than as an
    // error — the gesture already happened, and refusing it silently would look
    // like a bug.
    auto groupAt = [&](NodeData* t) -> wxString {
        if (!t) return wxString();
        if (t->kind == NodeData::Group) return t->group;
        if (t->kind == NodeData::Connection && t->entry)
            return core::GroupStore::GroupOf(ConnScope(), t->entry->profile.name);
        if (t->kind == NodeData::Table && t->entry)
            return core::GroupStore::GroupOf(ObjScope(t->entry, t->db, t->cat), t->table);
        return wxString();
    };

    if (sd->kind == NodeData::Connection) {
        // A connection may only land among connections. Dropping one onto a
        // TABLE's group would otherwise write a connection name into an object
        // scope, where nothing would ever render it again.
        if (dd && dd->kind == NodeData::Group && dd->entry != nullptr) return;
        if (dd && (dd->kind == NodeData::Database || dd->kind == NodeData::CategoryNode ||
                   dd->kind == NodeData::Table))
            return;
        MoveConnectionToGroup(sd->entry, groupAt(dd));
        return;
    }

    // An object leaf may only move within its OWN scope — same connection, same
    // database, same category. Anything else is a different group namespace.
    if (sd->kind == NodeData::Table) {
        if (!dd) { MoveObjectToGroup(src, wxString()); return; }   // dropped on nothing
        const bool sameScope =
            dd->entry == sd->entry && dd->db == sd->db &&
            ((dd->kind == NodeData::Group && dd->cat == sd->cat) ||
             (dd->kind == NodeData::Table && dd->cat == sd->cat) ||
             (dd->kind == NodeData::CategoryNode && dd->cat == sd->cat));
        if (!sameScope) return;
        MoveObjectToGroup(src, dd->kind == NodeData::CategoryNode ? wxString()
                                                                  : groupAt(dd));
    }
}

} // namespace ui
