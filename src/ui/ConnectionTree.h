// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionTree.h — owns the sidebar connection tree and the connection
// lifecycle (add / connect / disconnect / edit / delete / browse schema), so
// MainFrame stays focused on the frame + query/data views. Extracted per the
// 1000-line file charter (docs/CHARTER.md).
//
// The tree drives some view-level actions (open a table, new query on a db,
// jump to table design); those are delegated back to the owner via Hooks.
#pragma once

#include <wx/treebase.h>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include "core/ConnectionProfile.h"
#include "db/DbDriver.h"

class wxGenericTreeCtrl;   // the generic tree supports the custom ▸▾ chevron buttons
class wxTreeEvent;         // (a native MSW wxTreeCtrl ignores a custom buttons image list)
class wxWindow;
class wxMenu;              // 分组 context-menu items (ConnectionTree_Groups.cpp)
class wxMouseEvent;        // right-click on empty sidebar space

namespace net { class SshTunnel; }

namespace ui {

enum class ObjectKind;   // ui/ObjectTemplates.h — view / function / procedure / package
enum class Category;     // ui/ConnectionTreeInternal.h — 表/视图/函数/存储过程/触发器 folder
// ui/ConnectionTreeInternal.h — the tree node tag. Forward-declared rather than
// included because that header includes THIS one; the .cpp files that actually
// touch a NodeData include it directly.
class NodeData;

// One connection instance: saved profile, live driver (null until connected),
// its tree node, the database currently in focus, and an optional SSH tunnel
// kept alive for the duration of the connection.
struct ConnEntry {
    core::ConnectionProfile          profile;
    // shared_ptr, not unique_ptr: long-lived tab views (TableDesignView /
    // NewTableView / QueryBuilderPanel) borrow this connection for as long as the
    // tab is open, and they hold a weak_ptr to it. Every such borrow therefore
    // fails closed once this entry drops the connection, instead of dereferencing
    // a freed driver — the crash this replaced was exactly that: reconnecting a
    // dropped connection freed the old IConnection while a 设计表 tab still held a
    // raw pointer to it, and the next focus event called conn_->IsConnected() on
    // it. Ownership is still exclusively this entry's; nobody else keeps a
    // shared_ptr alive past the call it was borrowed for.
    std::shared_ptr<db::IConnection> conn;
    std::unique_ptr<net::SshTunnel>  tunnel;
    wxTreeItemId                     node;
    wxString                         currentDb;
    ConnEntry();
    ~ConnEntry();
    // NOTE: false for a dropped-but-not-yet-freed connection (object alive, socket
    // dead). Callers that release `conn` must not gate that release on this — see
    // ConnectionTree::ReleaseConnection.
    bool IsConnected() const { return conn && conn->IsConnected(); }
};

class ConnectionTree {
public:
    // View-level actions the tree triggers but doesn't own (handled by MainFrame).
    struct Hooks {
        std::function<void(ConnEntry*, const wxString& db, const wxString& table)> openTable;
        std::function<void(ConnEntry*, const wxString& db)>                        newQuery;
        std::function<void(ConnEntry*, const wxString& db, const wxString& sql)>    newQueryWith; // new query tab prefilled with SQL (e.g. a CREATE TABLE template)
        std::function<void(ConnEntry*, const wxString& db, const wxString& sql)>    newQueryRun;  // new query tab prefilled AND executed (e.g. 执行 a routine → result in the grid)
        std::function<void(ConnEntry*, const wxString& db, const wxString& title, const wxString& sql, ObjectKind kind)> newObjectEditor; // object editor tab (视图/函数/存储过程/包): prefilled + renames to the object name after a successful run; kind lets the tab wire F10=执行 for routines
        std::function<void(ConnEntry*, const wxString& db, bool focus)>            showDatabase; // database node → refresh 表列表; focus=true also brings the tab to front
        // 表 group folder clicked → show ONLY that group's tables in the same
        // fixed tab. Clicking a database afterwards goes back to the whole
        // database (showDatabase clears the filter), which is the round trip.
        std::function<void(ConnEntry*, const wxString& db, const wxString& group, bool focus)> showTableGroup;
        // Connection group folder clicked → the group's connection list (slot 2).
        std::function<void(const wxString& group, bool focus)>                     showConnGroup;
        std::function<void(ConnEntry*, const wxString& db, bool force)>            clearTables;  // empty the 表列表: force=true unconditional (clicked an unopened db); else only if (e,db) is the one shown (a db was closed)
        std::function<void(ConnEntry*, const wxString& db, const wxString& table)> designTable;
        std::function<void(ConnEntry*, const wxString& db, const wxString& table)> selectTable; // table node selected → sync "current table" (no tab opened)
        std::function<void(const wxString&)>                                       status;
        std::function<void(ConnEntry*)>                                            beforeClose; // join workers, etc.
        std::function<void(ConnEntry*)>                                            entryClosing; // ANY disconnect/delete (NOT gated on active_): drop refs to this entry before it dies
        std::function<void(ConnEntry*)>                                            showOverview; // connection node → overview view
        std::function<void()>                                                      openPreferences; // guide user to app settings (e.g. Oracle OCI)
    };

    ConnectionTree(wxWindow* parent, wxWindow* dialogParent, Hooks hooks);
    ~ConnectionTree();   // joins the async member loader

    wxGenericTreeCtrl* widget() const { return tree_; }
    ConnEntry*  active() const { return active_; }
    void        setActive(ConnEntry* e) { active_ = e; }
    // Object-editor target selector support: every currently *connected* entry,
    // and a liveness check (an editor tab can outlive the connection it was opened
    // on, so a held ConnEntry* must be re-validated against conns_ before use).
    std::vector<ConnEntry*> connectedEntries() const;
    // EVERY saved connection in the tree, connected or not — what the 分组
    // connection list renders (a group's members are mostly disconnected, so
    // connectedEntries() would show an empty group). Pointers stay valid until
    // that connection is deleted.
    std::vector<ConnEntry*> allEntries() const;
    // Take connections out of their 分组 (batch), by name — the 分组 view's
    // 从分组移除. They return to the tree's top level; nothing is deleted.
    bool RemoveConnectionsFromGroupUi(const std::vector<wxString>& names);
    bool                    isLive(ConnEntry* e) const;   // still in conns_ AND connected
    // Find a saved connection by name and connect it if it isn't already — headless
    // (no dialogs; failure returns err). For automation jobs that run unattended.
    ConnEntry*              EnsureConnectedByName(const wxString& name, wxString& err);
    // Build a brand-new, fully independent connection for `p` — headless (no dialogs,
    // not added to conns_, no tree node, no icon). Replays the same connect path as
    // ConnectEntry (incl. an SSH tunnel when the profile needs one, bundled into the
    // returned object so it lives as long as the connection). Returns a connected
    // connection the CALLER solely owns, or nullptr + err. MUST be called on the GUI
    // thread (the connect/SSH steps block and are safest there). Used by the AI
    // knowledge base so its off-GUI-thread introspection never shares the UI's
    // connection (DB drivers are not thread-safe).
    std::unique_ptr<db::IConnection>
    MakeDedicatedConnection(const core::ConnectionProfile& p, wxString& err);
    // Tools-menu (no-context) entry into the sync wizard: syncs the active
    // database (mode: 0 struct / 1 data / 2 both). No-op with a hint if none.
    void        OpenSyncForActive(int mode);

    // Incremental sidebar search: find a tree node whose label CONTAINS `query`
    // (case-insensitive) and jump to it — expand its ancestors, select it, scroll
    // it into view, and bold-highlight it. next=false selects the FIRST match;
    // next=true selects the first match strictly AFTER the current selection,
    // wrapping to the first when past the last. Returns false (and reports "无匹配"
    // via hooks_.status) when nothing matches; an empty query clears the previous
    // highlight and returns true (no error).
    //
    // LIMITATION — lazy loading: only nodes ALREADY LOADED into the tree are
    // searched. The tree lazy-loads (a database's tables/views/... enter the model
    // only when that database node is expanded), so the contents of collapsed /
    // never-opened databases are intentionally NOT searched. Forcing an eager full
    // load of every database just to search would be slow and crash-prone, so it is
    // deliberately skipped.
    bool SearchJump(const wxString& query, bool next);

    void LoadSavedConnections();          // startup: saved → grey nodes
    void ShowNewConnectionMenu();         // engine-type dropdown → dialog
    void NewConnection(db::DbType preset); // open the new-connection dialog directly
    ConnEntry* AutoConnect(const core::ConnectionProfile& p);  // create + connect + activate
    void PopupNodeMenu(ConnEntry* e);     // scripted/verification hook

    // Public entry points so other views (e.g. the database-overview toolbar)
    // reuse the tree's import / export flows on a given connection + database.
    void ImportSqlFileUi(ConnEntry* e, const wxString& db)  { ExecuteSqlFile(e, db); }
    void ExportDatabaseUi(ConnEntry* e, const wxString& db, bool withData) { DumpDatabase(e, db, withData); }
    // Drop one or more tables (batch, with a single confirm) — used by the
    // database-overview list's multi-select delete. Returns true if it ran.
    bool DropTablesUi(ConnEntry* e, const wxString& db, const std::vector<wxString>& tables);
    // Append the 移动到分组 ▶ submenu for a BATCH of tables — the 表信息 list's
    // multi-selection. Exactly the submenu the sidebar tree uses, on the same
    // scope, so both views agree about which folders exist and where a table
    // went. A mixed selection shows no checkmark; 新建分组… inside the submenu
    // creates a folder and moves the whole selection into it. `after` runs only
    // when a move actually applied (for the caller's own refresh/feedback).
    void AppendTableGroupMenuUi(wxMenu& menu, ConnEntry* e, const wxString& db,
                                const std::vector<wxString>& tables,
                                std::function<void()> after);
    // Take `tables` out of whatever 分组 they are in (batch) — the 分组 view's
    // 从分组移除. Nothing is dropped from the database; they simply return to the
    // category's top level. Returns true if it ran.
    bool RemoveTablesFromGroupUi(ConnEntry* e, const wxString& db,
                                 const std::vector<wxString>& tables);
    // Execute a function / stored procedure with a parameter dialog — used by the
    // object editor's F10. Same flow as the tree's 执行 context item.
    void ExecuteRoutineUi(ConnEntry* e, const wxString& db, const wxString& name,
                          bool isProcedure) { ExecuteRoutine(e, db, name, isProcedure); }
    // Object-editor entry points reused by the toolbar object-list tabs
    // (ObjectListPanel), mirroring the tree's 新建/修改/删除 context items. New/Modify
    // open an object editor tab (via the newObjectEditor hook); Drop confirms then
    // DROPs. Same flows as the tree — just exposed so MainFrame can forward to them.
    void NewObjectUi(ConnEntry* e, const wxString& db, ObjectKind kind)
        { NewObject(e, db, kind); }
    void ModifyObjectUi(ConnEntry* e, const wxString& db, const wxString& name, ObjectKind kind)
        { ModifyObject(e, db, name, kind); }
    void DropObjectUi(ConnEntry* e, const wxString& db, const wxString& name, ObjectKind kind)
        { DropObject(e, db, name, kind); }
    // Reload a database's category folder (视图/函数/存储过程) in the tree so a newly
    // created / renamed object appears without a manual refresh. Walks e->node →
    // the database node matching `db` → the CategoryNode matching `cat`, marks it
    // unloaded and re-runs the async member loader. No-op (silent) if the connection
    // died, the database isn't opened in the tree yet, or that category folder was
    // never loaded/expanded (it lazy-loads fresh on the next expand anyway). MUST be
    // called on the GUI thread. The ObjectKind overload maps kind→category (View→
    // Views, Function→Functions, Procedure→Procedures; Package has no folder → no-op).
    void RefreshCategory(ConnEntry* e, const wxString& db, Category cat);
    void RefreshCategory(ConnEntry* e, const wxString& db, ObjectKind kind);
    // Refresh the 表 folder in the tree (there is no ObjectKind for a plain table, so
    // this is the table-side counterpart of the ObjectKind RefreshCategory overload;
    // Category is an internal enum, so the call is wrapped here). Used by the object-
    // list tab after a 删除表 to keep the sidebar in sync.
    void RefreshTablesUi(ConnEntry* e, const wxString& db);

private:
    // lifecycle
    void OpenConnectionDialog(db::DbType preset);
    ConnEntry* AddConnection(const core::ConnectionProfile& p);
    void ConnectEntry(ConnEntry* e);
    void DisconnectEntry(ConnEntry* e);
    void EditConnection(ConnEntry* e);
    void DuplicateConnection(ConnEntry* e);   // clone profile under a new name
    void DeleteConnection(ConnEntry* e);
    void SetConnColor(ConnEntry* e, const wxString& hex); // persist + repaint label ("" clears)
    void ApplyConnColor(ConnEntry* e);        // paint the node label from profile.color
    void NewDatabase(ConnEntry* e);           // CREATE DATABASE via a name prompt
    void EditDatabase(ConnEntry* e, const wxString& db);   // ALTER DATABASE (dialect-gated)
    void DeleteDatabase(ConnEntry* e, const wxString& db); // DROP DATABASE (with confirm)
    void DumpDatabase(ConnEntry* e, const wxString& db, bool withData); // → .sql file
    void SyncDatabase(ConnEntry* e, const wxString& db, int mode); // cross-db sync wizard (mode: 0 struct / 1 data / 2 both)
    void ExecuteSqlFile(ConnEntry* e, const wxString& db);  // run a .sql script
    void CopyTable(ConnEntry* e, const wxString& db, const wxString& table); // → table clipboard
    void PasteTable(ConnEntry* e, const wxString& db);      // recreate clipboard table into `db`
    // Table-leaf operations (context menu). Destructive ops confirm first; all
    // qualify the table name via QualifiedTable so MySQL doesn't hit the wrong db.
    void DropTable(ConnEntry* e, const wxString& db, const wxString& table);   // DROP TABLE (confirm)
    void EmptyTable(ConnEntry* e, const wxString& db, const wxString& table);  // TRUNCATE / DELETE (confirm)
    void RenameTable(ConnEntry* e, const wxString& db, const wxString& table); // prompt + RENAME/ALTER
    void ExportTable(ConnEntry* e, const wxString& db, const wxString& table, bool withData); // single-table dump
    void CopyTableName(const wxString& table);              // → system clipboard
    void CopyTableDdl(ConnEntry* e, const wxString& db, const wxString& table); // CREATE stmt → system clipboard
    void ObjectInfo(ConnEntry* e, const wxString& db, const wxString& table);  // lightweight summary dialog
    // Object editors (视图/函数/存储过程/包). New opens a dialect template; Modify
    // fetches the object's DDL and re-wraps it into a replace-in-place form. Both
    // open a normal SQL editor tab via the newObjectEditor hook.
    void NewObject(ConnEntry* e, const wxString& db, ObjectKind kind);
    void ModifyObject(ConnEntry* e, const wxString& db, const wxString& name, ObjectKind kind);
    void DropObject(ConnEntry* e, const wxString& db, const wxString& name, ObjectKind kind); // DROP view/func/proc/package (confirm)
    // Rename a view / function / procedure (NOT a table — that's RenameTable). Native
    // ALTER … RENAME where the engine has it (PG / SQL Server); otherwise a rebuild
    // (CREATE-under-new-name → DROP old) on MySQL / SQLite. Prompts for the new name.
    void RenameObject(ConnEntry* e, const wxString& db, const wxString& name, ObjectKind kind);
    // Execute a routine: prompt for parameters, then run CALL/SELECT in a new
    // query tab (result lands in the shared results grid). isProcedure picks CALL
    // vs SELECT.
    void ExecuteRoutine(ConnEntry* e, const wxString& db, const wxString& name, bool isProcedure);
    // Compile a routine/package (Oracle/DM: ALTER … COMPILE). On failure a
    // resizable error window shows the full diagnostics. MySQL has no COMPILE →
    // callers gate this off. kind is ObjectKind::Function/Procedure/Package.
    void CompileObject(ConnEntry* e, const wxString& db, const wxString& name, ObjectKind kind);
    void RefreshTableCategory(const wxTreeItemId& tableItem); // reload the table's parent category
    // Qualify a table for a raw Execute: MySQL needs `db`.`table` (session db is
    // set only at connect), other engines are single-db → bare quoted table.
    wxString QualifiedTable(db::IConnection* c, const wxString& db, const wxString& table) const;
    void LoadDatabases(ConnEntry* e);
    void LoadCategories(const wxTreeItemId& dbNode, ConnEntry* e, const wxString& db);
    void LoadCategoryMembers(const wxTreeItemId& catNode);   // async; reads NodeData::cat
    void JoinCatWorker();    // cancel + join the loader (before starting a new one)
    void AbortCatWorker();   // JoinCatWorker + bump generation (before deleting nodes)
    // The ONE place a live-or-dead `e->conn` is released. Stops the catalogue loader,
    // fires entryClosing so MainFrame joins every worker bound to this connection and
    // closes the tabs that borrowed it, then drops the driver and its tunnel.
    // DisconnectEntry, DeleteConnection and BOTH connect paths funnel through this —
    // the connect paths used to assign straight over `e->conn`, whose unique_ptr
    // destructor freed the old driver silently, with no notification to anyone.
    void ReleaseConnection(ConnEntry* e);
    void SetConnIcon(ConnEntry* e);

    // ---- 分组 (ConnectionTree_Groups.cpp) ---------------------------------
    // User-defined folders at two levels: connections under the root, and
    // objects under a database's category folder. Persistence is
    // core::GroupStore; everything here is the tree's side of it.
    //
    // The scope helpers are the ONLY place a GroupStore scope is built, so the
    // menu, the drag-drop handler and the renderer can never disagree about
    // which bucket a node belongs to.
    wxString ConnScope() const;
    wxString ObjScope(ConnEntry* e, const wxString& db, Category cat) const;

    void         SyncConnGroupNodes();          // create any missing group folder, in store order
    wxTreeItemId FindGroupNode(const wxTreeItemId& parent, const wxString& name) const;
    size_t       GroupNodeCount(const wxTreeItemId& parent) const;
    void         CreateConnectionNode(ConnEntry* e);   // parented under its group (or root)
    // The (db, cat) category folder under `e`, or an invalid id when the
    // connection is dead, the database was never opened (its category folders do
    // not exist yet), or this engine has no such folder. Says nothing about
    // whether that folder's MEMBERS are loaded — callers needing that check
    // NodeData::loaded themselves (RefreshCategory does).
    wxTreeItemId FindCategoryNode(ConnEntry* e, const wxString& db, Category cat) const;

    // Re-parent a category folder's already-loaded leaves into their group
    // folders. Works off the leaves ALREADY IN THE TREE — it never re-queries
    // the server, so moving one table does not cost a round-trip.
    void RegroupCategory(const wxTreeItemId& catNode);

    // Context-menu actions. Each prompts, mutates GroupStore, then repaints the
    // affected level; none of them touch the database.
    void NewGroupUi(ConnEntry* e, const wxString& db, Category cat, bool connLevel);
    void RenameGroupUi(const wxTreeItemId& groupNode);
    void DeleteGroupUi(const wxTreeItemId& groupNode);
    // 清空分组: empties the folder but keeps it. Members are only moved back to
    // the level above — never deleted.
    void ClearGroupUi(const wxTreeItemId& groupNode);
    // Re-home every connection inside a group folder, leaving it childless.
    // Shared by 删除分组 and 清空分组 so the rescue cannot drift between them.
    void EvacuateConnGroup(const wxTreeItemId& groupNode);
    void MoveConnectionToGroup(ConnEntry* e, const wxString& group);
    void MoveObjectToGroup(const wxTreeItemId& leaf, const wxString& group);
    // Prompt for a name and create the group. "" = cancelled or refused.
    wxString PromptNewGroup(const wxString& scope);
    // Batch: assign every table, then repaint the level ONCE. "" un-groups them.
    bool MoveTablesToGroup(ConnEntry* e, const wxString& db,
                           const std::vector<wxString>& tables, const wxString& group);
    // The group all `members` share, or "" when they disagree (mixed selection).
    wxString CommonGroupOf(const wxString& scope,
                           const std::vector<wxString>& members) const;
    // Appends the 移动到分组 ▶ / 新建分组 / 重命名分组 / 删除分组 items that apply
    // to `item`. Called from ShowNodeContextMenu so the group vocabulary lives
    // in one place instead of being re-spelled in four switch arms.
    void AppendGroupMenu(wxMenu& menu, const wxTreeItemId& item, NodeData* data);
    // The 移动到分组 ▶ submenu, shared by both levels: one check-item per group in
    // `scope` (the member's current group checked), then 移出分组. `move` receives
    // the chosen group name, or "" for 移出分组.
    void AppendMoveToGroupSub(wxMenu& menu, const wxString& scope,
                              const wxString& current,
                              std::function<void(const wxString&)> move);

    // 分组 bookkeeping for an object the DATABASE changed under us. Membership is
    // keyed by object name, so a rename must carry it (or the object drops out of
    // its folder) and a drop must clear it (or a later object created with the
    // same name inherits a folder nobody put it in). Same Category/ObjectKind
    // overload pair as RefreshCategory, for the same reason.
    void GroupTrackRename(ConnEntry* e, const wxString& db, Category cat,
                          const wxString& from, const wxString& to);
    void GroupTrackDrop(ConnEntry* e, const wxString& db, Category cat,
                        const wxString& name);
    void GroupTrackRename(ConnEntry* e, const wxString& db, ObjectKind kind,
                          const wxString& from, const wxString& to);
    void GroupTrackDrop(ConnEntry* e, const wxString& db, ObjectKind kind,
                        const wxString& name);

    // A group folder was selected (focus=false) or opened (focus=true) → route to
    // the right host hook. Connection groups and 表 groups have list views; other
    // object categories deliberately do not (see the .cpp).
    void NotifyGroupSelected(NodeData* d, bool focus);

    void OnBeginDrag(wxTreeEvent&);
    void OnEndDrag(wxTreeEvent&);
    void OnTreeRightDown(wxMouseEvent&);   // right-click on empty space → 新建分组

    // events
    void OnActivated(wxTreeEvent&);
    void OnItemExpanding(wxTreeEvent&);   // lazy-load a node's children on expand
    void OnSelChanged(wxTreeEvent&);      // single-click follow: already-open db → overview
    void OnRightClick(wxTreeEvent&);
    void OnTreeKey(wxTreeEvent&);         // F2 on a table node → rename
    void ShowNodeContextMenu(const wxTreeItemId& item);
    // SearchJump helpers: depth-first flatten of every NodeData-tagged node under
    // `parent` (in visible order; the lazy-load "加载中…" placeholders carry no
    // NodeData and are skipped), and clearing the previous search bold-highlight.
    void CollectSearchNodes(const wxTreeItemId& parent, std::vector<wxTreeItemId>& out) const;
    void ClearSearchHighlight();

    wxWindow*    dialogParent_ = nullptr;
    Hooks        hooks_;
    wxGenericTreeCtrl* tree_ = nullptr;
    wxTreeItemId root_;
    wxTreeItemId searchBold_;   // node bolded by the last SearchJump (cleared next round)
    wxTreeItemId dragItem_;     // 分组拖拽: the node picked up by OnBeginDrag ("" = none)
    std::vector<std::unique_ptr<ConnEntry>> conns_;
    ConnEntry*   active_ = nullptr;

    // Cross-connection table clipboard (复制表 → 粘贴). Source coordinates only —
    // validated against conns_ and re-fetched at paste time, so a stale entry can
    // never dangle. Empty `table` means the clipboard is empty.
    struct TableClip { ConnEntry* entry = nullptr; wxString db; wxString table; };
    TableClip    tableClip_;

    // Async category-member loader — ListTables/Views/Routines/Triggers run off the
    // GUI thread so a database with thousands of objects can't freeze the tree. One
    // load at a time (the connection is single-threaded); a superseded or
    // structurally-invalidated load is discarded via catGen_. catConn_ is the
    // connection being queried (for Cancel()). All three are touched only on the
    // GUI thread; the worker reads copies captured by value.
    std::thread      catWorker_;
    int              catGen_ = 0;
    db::IConnection* catConn_ = nullptr;
};

} // namespace ui
