#include "ui/ConnectionTree.h"
#include "ui/ConnectionTreeInternal.h"
#include "core/CrashLog.h"

#include <wx/wx.h>
#include <wx/busyinfo.h>
#include <wx/dcmemory.h>
#include <wx/imaglist.h>
#include <wx/stopwatch.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>
#include <algorithm>

#include "core/ConnectionStore.h"
#include "db/OciConfig.h"
#include "net/SshTunnel.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

// Out-of-line so unique_ptr<net::SshTunnel> (incomplete in the header) is
// destroyed here, where net::SshTunnel is complete.
ConnEntry::ConnEntry() = default;
ConnEntry::~ConnEntry() {
    // Tear down the DB connection before the SSH tunnel it flows through
    // (mirrors DisconnectEntry). Default member order would destroy tunnel
    // first, sending the DB's close packets through an already-dead socket.
    conn.reset();
    tunnel.reset();
}

namespace {

// A dedicated DB connection that flows through its OWN private SSH tunnel (not the
// shared ConnEntry::tunnel). It bundles the driver connection and the tunnel so the
// pair travels as a single unique_ptr<db::IConnection> and is torn down in the right
// order (the connection first — its close packets flow through the tunnel — then the
// tunnel). Built by MakeDedicatedConnection for the AI knowledge base's off-GUI-
// thread introspection, which must never share a connection with the UI. Every pure
// virtual is forwarded to the inner driver; the composed non-pure IConnection helpers
// (GetTableList, GetPrimaryKey, …) route through these overrides, so they work too.
class TunnelledConnection : public db::IConnection {
public:
    TunnelledConnection(std::unique_ptr<db::IConnection> inner,
                        std::unique_ptr<net::SshTunnel>   tunnel)
        : inner_(std::move(inner)), tunnel_(std::move(tunnel)) {}
    // Explicit order: kill the connection before the tunnel it dialed through
    // (mirrors ConnEntry::~ConnEntry; default member order would reverse it).
    ~TunnelledConnection() override { inner_.reset(); tunnel_.reset(); }

    db::Dialect GetDialect() const override { return inner_->GetDialect(); }
    bool Connect(const core::ConnectionProfile& p, wxString& err) override
    { return inner_->Connect(p, err); }
    void Disconnect() override { inner_->Disconnect(); }
    bool IsConnected() const override { return inner_->IsConnected(); }
    const core::ConnectionProfile& EffectiveProfile() const override
    { return inner_->EffectiveProfile(); }
    void Cancel() override { inner_->Cancel(); }
    bool Execute(const wxString& sql, db::QueryResult& out, wxString& err) override
    { return inner_->Execute(sql, out, err); }
    bool ListDatabases(std::vector<wxString>& out, wxString& err) override
    { return inner_->ListDatabases(out, err); }
    bool ListTables(const wxString& db, std::vector<db::TableInfo>& out, wxString& err) override
    { return inner_->ListTables(db, out, err); }
    bool GetColumns(const wxString& db, const wxString& table,
                    std::vector<db::ColumnInfo>& out, wxString& err) override
    { return inner_->GetColumns(db, table, out, err); }
    bool GetForeignKeys(const wxString& db, const wxString& table,
                        std::vector<db::ForeignKey>& out, wxString& err) override
    { return inner_->GetForeignKeys(db, table, out, err); }
    bool GetIndexes(const wxString& db, const wxString& table,
                    std::vector<db::IndexInfo>& out, wxString& err) override
    { return inner_->GetIndexes(db, table, out, err); }
    bool GetCreateDdl(const wxString& db, const wxString& table,
                      wxString& ddl, wxString& err) override
    { return inner_->GetCreateDdl(db, table, ddl, err); }
    wxString ServerVersion() const override { return inner_->ServerVersion(); }

private:
    std::unique_ptr<db::IConnection> inner_;
    std::unique_ptr<net::SshTunnel>  tunnel_;   // torn down AFTER inner_ (see dtor)
};

// The image-list layout (kConnImgCount / IMG_* / ConnImg) now lives in
// ConnectionTreeInternal.h — ConnectionTree_Groups.cpp needs it too.

// Centre a glyph bitmap inside a larger transparent `cell`×`cell` bitmap, so a
// smaller child icon can live in the same image list as bigger connection icons.
wxBitmap PadIcon(const wxBitmap& bm, int cell)
{
    wxImage src = bm.ConvertToImage();
    if (!src.HasAlpha()) src.InitAlpha();
    wxImage dst(cell, cell);
    dst.InitAlpha();
    memset(dst.GetAlpha(), 0, static_cast<size_t>(cell) * cell);
    const int off = (cell - src.GetWidth()) / 2;
    dst.Paste(src, off, off, wxIMAGE_ALPHA_BLEND_OVER);
    return wxBitmap(dst);
}

} // namespace

// ===========================================================================
ConnectionTree::ConnectionTree(wxWindow* parent, wxWindow* dialogParent, Hooks hooks)
    : dialogParent_(dialogParent), hooks_(std::move(hooks))
{
    // wxTR_FULL_ROW_HIGHLIGHT: selection highlights the whole row, not just the
    //   label text.
    tree_ = new wxGenericTreeCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxTR_HIDE_ROOT | wxTR_NO_LINES | wxTR_HAS_BUTTONS |
                                  wxTR_FULL_ROW_HIGHLIGHT | wxBORDER_NONE);
    tree_->SetBackgroundColour(theme::kWhite);   // brighter sidebar (was kSidebarBg grey)
    tree_->SetForegroundColour(theme::kTextBody);
    tree_->SetFont(Mono(10));

    // Custom expand/collapse glyphs: chevron ▸ (collapsed) / ▾ (expanded).
    // CRITICAL: the generic tree indexes the buttons image list by the
    // wxTreeItemIcon enum — Normal(0), Selected(1), Expanded(2), SelectedExpanded(3)
    // — so it needs ALL FOUR slots. With only 2, an *expanded* node asks for
    // index 2, gets wxNullBitmap, and draws no chevron.
    constexpr int kBtn = 14;
    const wxBitmap chvCollapsed = icons::Stroke(icons::Glyph::ChevronRight, kBtn, theme::kTextMuted, 1.8);
    const wxBitmap chvExpanded  = icons::Stroke(icons::Glyph::ChevronDown,  kBtn, theme::kTextMuted, 1.8);
    auto* btns = new wxImageList(kBtn, kBtn);
    btns->Add(chvCollapsed);   // 0 = Normal            (collapsed)
    btns->Add(chvCollapsed);   // 1 = Selected          (collapsed, selected)
    btns->Add(chvExpanded);    // 2 = Expanded          (expanded)
    btns->Add(chvExpanded);    // 3 = SelectedExpanded  (expanded, selected)
    tree_->AssignButtonsImageList(btns);

    // First-level connection icons fill the (larger) cell; database / table
    // glyphs are rendered smaller and centred in the same cell so only the
    // top-level connection icons grow.
    constexpr int kIco = 24;     // image-list cell size (connection icons fill it)
    constexpr int kChild = 19;   // db/table glyph size within the cell
    auto* images = new wxImageList(kIco, kIco);
    for (const db::DbTypeInfo& info : db::AllDbTypes()) {
        images->Add(icons::DbBrand(info.type, kIco, theme::kGreen));      // connected
        images->Add(icons::DbBrand(info.type, kIco, theme::kTextFaint));  // disconnected
    }
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Cylinder, kChild, theme::kPrimary, 1.8), kIco));   // db open
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Cylinder, kChild, theme::kTextFaint, 1.8), kIco)); // db closed
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Table,    kChild, theme::kGreen,    1.8), kIco));   // table
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Eye,      kChild, theme::kAccent,   1.8), kIco));   // view
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Function, kChild, theme::kDotAmber, 1.8), kIco));   // function
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Code,     kChild, theme::kDotPurple,1.8), kIco));   // procedure
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Timer,    kChild, theme::kDotPink,  1.8), kIco));   // trigger
    images->Add(PadIcon(icons::Stroke(icons::Glyph::Folder,   kChild, theme::kDotAmber, 1.8), kIco));   // group (IMG_GROUP)
    tree_->AssignImageList(images);

    tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, &ConnectionTree::OnActivated, this);
    tree_->Bind(wxEVT_TREE_ITEM_EXPANDING, &ConnectionTree::OnItemExpanding, this);
    tree_->Bind(wxEVT_TREE_SEL_CHANGED,    &ConnectionTree::OnSelChanged, this);
    tree_->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, &ConnectionTree::OnRightClick, this);
    tree_->Bind(wxEVT_TREE_KEY_DOWN,         &ConnectionTree::OnTreeKey, this);   // F2 = rename table
    // 分组拖拽: BEGIN_DRAG must Allow() or the generic tree never sends END_DRAG.
    // Both handlers are strict about WHAT may be dragged and WHERE it may land —
    // see ConnectionTree_Groups.cpp.
    tree_->Bind(wxEVT_TREE_BEGIN_DRAG, &ConnectionTree::OnBeginDrag, this);
    tree_->Bind(wxEVT_TREE_END_DRAG,   &ConnectionTree::OnEndDrag,   this);
    // Right-click on EMPTY sidebar space (below the last node) — the tree's own
    // item right-click event cannot fire there, so the "新建分组" entry point for
    // a user with no groups yet would otherwise be unreachable from the tree.
    tree_->Bind(wxEVT_RIGHT_DOWN, &ConnectionTree::OnTreeRightDown, this);
    root_ = tree_->AddRoot(L"connections");
}

ConnectionTree::~ConnectionTree()
{
    AbortCatWorker();   // never let the loader thread outlive the tree
}

// ---------------------------------------------------------------------------
void ConnectionTree::LoadSavedConnections()
{
    // Group folders FIRST, in the order the store holds them, so the sidebar's
    // top-level order is the user's group order — not the order in which the
    // first connection of each group happens to load. It also makes an EMPTY
    // group visible at startup, which is the whole point of groups existing
    // independently of their members (see core/GroupStore.h).
    SyncConnGroupNodes();
    for (const core::ConnectionProfile& p : core::ConnectionStore::LoadAll())
        AddConnection(p);   // grey nodes, not connected yet
}

ConnEntry* ConnectionTree::AutoConnect(const core::ConnectionProfile& p)
{
    ConnEntry* e = AddConnection(p);
    ConnectEntry(e);
    return e;
}

void ConnectionTree::SetConnIcon(ConnEntry* e)
{
    if (!e || !e->node.IsOk()) return;
    const int img = ConnImg(e->profile.type, e->IsConnected());
    // set BOTH the normal and selected images — otherwise a selected node keeps
    // its old (grey) selected icon and looks disconnected right after connecting.
    tree_->SetItemImage(e->node, img, wxTreeItemIcon_Normal);
    tree_->SetItemImage(e->node, img, wxTreeItemIcon_Selected);
}

ConnEntry* ConnectionTree::AddConnection(const core::ConnectionProfile& p)
{
    for (auto& up : conns_)
        if (up->profile.name == p.name) {           // dedupe by name → update
            up->profile = p;
            tree_->SetItemText(up->node, p.name);
            return up.get();
        }
    auto entry = std::make_unique<ConnEntry>();
    entry->profile = p;
    ConnEntry* e = entry.get();
    // Parented under its saved group folder when it has one, else under the root
    // exactly as before. CreateConnectionNode also sets the bold label and the
    // always-present expand chevron (a disconnected connection has no children
    // yet, and without the chevron it would look like a leaf).
    CreateConnectionNode(e);
    conns_.push_back(std::move(entry));
    return e;
}

void ConnectionTree::ConnectEntry(ConnEntry* e)
{
    if (!e) return;
    if (e->IsConnected()) { LoadDatabases(e); return; }

    // Oracle loads its OCI client library (oci.dll) at runtime. If it isn't
    // found/configured, guide the user to Preferences instead of attempting a
    // doomed connect that fails with a cryptic driver error.
    if (e->profile.type == db::DbType::Oracle) {
        wxString ociResolved;
        if (!db::OciAvailable(ociResolved)) {
            const int r = wxMessageBox(
                tr(L"连接 Oracle 需要 OCI 客户端库(oci.dll),当前未找到。是否现在去偏好设置配置?"),
                tr(L"未配置 Oracle 客户端"),
                wxYES_NO | wxICON_INFORMATION, dialogParent_);
            if (r == wxYES && hooks_.openPreferences) hooks_.openPreferences();
            return;
        }
    }

    // Effective connect target — rewritten to the local end of an SSH tunnel
    // when the profile enables SSH.
    core::ConnectionProfile eff = e->profile;
    if (e->profile.sshEnabled) {
        e->tunnel = std::make_unique<net::SshTunnel>();
        wxString terr;
        wxBusyCursor busy;
        if (!e->tunnel->Open(e->profile, terr)) {
            e->tunnel.reset();
            hooks_.status(tr(L"SSH 隧道失败:") + terr);
            wxMessageBox(tr(L"SSH 隧道失败:") + L"\n\n" + terr, e->profile.name,
                         wxOK | wxICON_ERROR, dialogParent_);
            return;
        }
        eff.host = L"127.0.0.1";
        eff.port = e->tunnel->localPort();
    }

    e->conn = db::CreateConnection(e->profile.type);
    wxString err;
    bool ok = false;
    long ms = 0;
    {
        wxBusyCursor busy;
        wxStopWatch sw;
        ok = e->conn->Connect(eff, err);
        ms = sw.Time();
    }
    if (!ok) {
        e->conn.reset();
        e->tunnel.reset();
        SetConnIcon(e);
        hooks_.status(tr(L"连接失败:") + err);
        wxMessageBox(tr(L"连接失败:") + L"\n\n" + err, e->profile.name,
                     wxOK | wxICON_ERROR, dialogParent_);
        return;
    }
    active_ = e;
    SetConnIcon(e);   // → green
    hooks_.status(wxString::Format(L"%s %s(%s %s · %ldms)", tr(L"已连接"),
                  e->profile.name, db::InfoOf(e->profile.type).name,
                  e->conn->ServerVersion(), ms));
    LoadDatabases(e);
    if (hooks_.showOverview) hooks_.showOverview(e);   // → connection overview view
}

std::unique_ptr<db::IConnection>
ConnectionTree::MakeDedicatedConnection(const core::ConnectionProfile& p, wxString& err)
{
    // Same Oracle OCI gate as ConnectEntry, but headless: report via err, no dialog.
    if (p.type == db::DbType::Oracle) {
        wxString oci;
        if (!db::OciAvailable(oci)) { err = tr(L"未配置 Oracle 客户端"); return nullptr; }
    }

    wxBusyCursor busy;   // connect + SSH block the GUI thread briefly

    // A PRIVATE tunnel for this one connection (never ConnEntry::tunnel) — it must
    // outlive the connection, so it's bundled into the returned wrapper below.
    std::unique_ptr<net::SshTunnel> tunnel;
    core::ConnectionProfile eff = p;
    if (p.sshEnabled) {
        tunnel = std::make_unique<net::SshTunnel>();
        wxString terr;
        if (!tunnel->Open(p, terr)) { err = tr(L"SSH 隧道失败:") + terr; return nullptr; }
        eff.host = L"127.0.0.1";
        eff.port = tunnel->localPort();
    }

    std::unique_ptr<db::IConnection> conn = db::CreateConnection(p.type);
    if (!conn) { err = tr(L"数据库连接不可用"); return nullptr; }
    if (!conn->Connect(eff, err)) return nullptr;   // Connect fills err on failure

    // No SSH → the driver connection is self-contained; otherwise bundle both so the
    // tunnel lives exactly as long as the connection and tears down after it.
    if (!tunnel) return conn;
    return std::make_unique<TunnelledConnection>(std::move(conn), std::move(tunnel));
}

std::vector<ConnEntry*> ConnectionTree::connectedEntries() const
{
    std::vector<ConnEntry*> out;
    for (const auto& up : conns_)
        if (up && up->IsConnected()) out.push_back(up.get());
    return out;
}

bool ConnectionTree::isLive(ConnEntry* e) const
{
    if (!e) return false;
    for (const auto& up : conns_)
        if (up.get() == e) return up->IsConnected();   // pointer valid → check live
    return false;                                       // stale pointer (entry gone)
}

// Headless connect-by-name for unattended callers (automation): no dialogs — the SSH /
// OCI / connect failures that ConnectEntry pops as message boxes are returned via `err`.
ConnEntry* ConnectionTree::EnsureConnectedByName(const wxString& name, wxString& err)
{
    ConnEntry* e = nullptr;
    for (const auto& up : conns_)
        if (up && up->profile.name == name) { e = up.get(); break; }
    if (!e) { err = wxString::Format(tr(L"未找到连接: %s"), name); return nullptr; }
    if (e->IsConnected()) return e;

    if (e->profile.type == db::DbType::Oracle) {
        wxString oci;
        if (!db::OciAvailable(oci)) { err = tr(L"未配置 Oracle 客户端"); return nullptr; }
    }
    core::ConnectionProfile eff = e->profile;
    if (e->profile.sshEnabled) {
        e->tunnel = std::make_unique<net::SshTunnel>();
        wxString terr;
        if (!e->tunnel->Open(e->profile, terr)) {
            e->tunnel.reset();
            err = tr(L"SSH 隧道失败:") + terr;
            return nullptr;
        }
        eff.host = L"127.0.0.1";
        eff.port = e->tunnel->localPort();
    }
    e->conn = db::CreateConnection(e->profile.type);
    if (!e->conn->Connect(eff, err)) {
        e->conn.reset();
        e->tunnel.reset();
        SetConnIcon(e);
        return nullptr;
    }
    SetConnIcon(e);       // → green
    LoadDatabases(e);
    return e;
}

void ConnectionTree::LoadDatabases(ConnEntry* e)
{
    if (!e || !e->IsConnected()) return;
    AbortCatWorker();   // a pending member load may reference nodes we're about to delete
    tree_->DeleteChildren(e->node);

    wxString err;
    std::vector<wxString> dbs;
    if (!e->conn->ListDatabases(dbs, err)) {
        hooks_.status(tr(L"获取数据库列表失败:") + err);
        return;
    }

    // A logical default target for queries — but we do NOT auto-open (expand +
    // load tables of) this database; the user picks which database to open. This
    // just seeds currentDb so a query run before any pick has a sensible target.
    const wxString current = e->profile.database;
    if (e->currentDb.IsEmpty())
        e->currentDb = current.IsEmpty() ? (dbs.empty() ? wxString() : dbs.front())
                                         : current;

    for (const wxString& dbName : dbs) {
        wxTreeItemId dbNode = tree_->AppendItem(
            e->node, dbName, IMG_DB_GREY, IMG_DB_GREY,
            new NodeData(NodeData::Database, e, dbName));
        tree_->SetItemImage(dbNode, IMG_DB, wxTreeItemIcon_Expanded);
        tree_->SetItemImage(dbNode, IMG_DB, wxTreeItemIcon_SelectedExpanded);
        tree_->AppendItem(dbNode, tr(L"加载中…"));   // lazy: real categories load on expand
    }
    tree_->Expand(e->node);   // reveal the database LIST (each db still collapsed)
}

// A database node's children are per-engine category folders (表/视图/函数/…),
// each lazily populated when expanded. Which categories appear is gated by DbCaps.
void ConnectionTree::LoadCategories(const wxTreeItemId& dbNode, ConnEntry* e,
                                    const wxString& db)
{
    auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(dbNode));
    if (data && data->loaded) return;
    AbortCatWorker();   // invalidate any pending member load before rebuilding children
    tree_->DeleteChildren(dbNode);

    const DbCaps caps = CapsFor(e->conn->GetDialect());
    auto addCat = [&](const wxString& label, Category cat, int img) {
        auto* cd = new NodeData(NodeData::CategoryNode, e, db);
        cd->cat = cat;
        wxTreeItemId cn = tree_->AppendItem(dbNode, label, img, img, cd);
        tree_->AppendItem(cn, tr(L"加载中…"));   // lazy: real members load on expand
    };

    addCat(tr(L"表"), Category::Tables, IMG_TABLE);
    if (caps.hasViews)    addCat(tr(L"视图"), Category::Views, IMG_VIEW);
    if (caps.hasRoutines) addCat(tr(L"函数"), Category::Functions, IMG_FUNC);
    if (caps.hasRoutines && caps.splitProcFunc)
        addCat(tr(L"存储过程"), Category::Procedures, IMG_PROC);
    if (caps.hasTriggers) addCat(tr(L"触发器"), Category::Triggers, IMG_TRIGGER);

    if (data) data->loaded = true;
}

// Populate one category folder with its objects. The ListTables/Views/Routines/
// Triggers call runs on a WORKER THREAD (a schema with thousands of objects would
// otherwise freeze the tree while blocking on the server); the "加载中…"
// placeholder stays until the result marshals back via CallAfter. Only one load
// runs at a time — the connection is single-threaded — and a superseded or
// structurally-invalidated load is dropped by the generation guard.
void ConnectionTree::LoadCategoryMembers(const wxTreeItemId& catNode)
{
    auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(catNode));
    if (!data || data->loaded) return;
    ConnEntry* e = data->entry;
    const wxString db = data->db;
    const Category cat = data->cat;

    tree_->DeleteChildren(catNode);
    tree_->AppendItem(catNode, tr(tr(L"加载中…")));   // stays until the async fetch returns

    JoinCatWorker();                 // serialize on the single-threaded connection
    const int gen = ++catGen_;
    db::IConnection* conn = e->conn.get();
    catConn_ = conn;
    catWorker_ = core::CrashLog::GuardedThread(L"对象树加载", [this, conn, db, cat, e, catNode, gen]() {
        wxString err; bool ok = true;
        std::vector<db::TableInfo>   tables;
        std::vector<wxString>        views;
        std::vector<db::RoutineInfo> routines;
        std::vector<db::TriggerInfo> triggers;
        switch (cat) {
        case Category::Tables:
            ok = conn->ListTables(db, tables, err);
            if (ok)   // 树里的表节点按表名 A-Z 升序(大小写不敏感)
                std::sort(tables.begin(), tables.end(),
                          [](const db::TableInfo& a, const db::TableInfo& b) {
                              return a.name.CmpNoCase(b.name) < 0;
                          });
            break;
        case Category::Views:      ok = conn->ListViews(db, views, err); break;
        case Category::Functions:
        case Category::Procedures: ok = conn->ListRoutines(db, routines, err); break;
        case Category::Triggers:   ok = conn->ListTriggers(db, triggers, err); break;
        }
        const DbCaps caps = CapsFor(conn->GetDialect());   // safe: worker owns conn until joined

        tree_->CallAfter([this, gen, catNode, e, db, cat, caps, ok,
                          tables, views, routines, triggers]() {
            if (gen != catGen_) return;   // superseded / structure changed → discard
            catConn_ = nullptr;
            auto* d = dynamic_cast<NodeData*>(tree_->GetItemData(catNode));
            if (!d) return;

            tree_->DeleteChildren(catNode);   // drop the "加载中…" placeholder
            if (!ok) { tree_->AppendItem(catNode, tr(L"(读取失败)")); return; }

            switch (cat) {
            case Category::Tables:
                for (const auto& t : tables)
                    tree_->AppendItem(catNode,
                        wxString::Format(L"%s · %s", t.name, t.approxRows),
                        IMG_TABLE, IMG_TABLE, new NodeData(NodeData::Table, e, db, t.name));
                break;
            case Category::Views:
                for (const auto& v : views) {   // views open like tables
                    auto* nd = new NodeData(NodeData::Table, e, db, v);
                    nd->cat = Category::Views;   // marks the leaf for the 修改视图 menu
                    tree_->AppendItem(catNode, v, IMG_VIEW, IMG_VIEW, nd);
                }
                break;
            case Category::Functions:
            case Category::Procedures: {
                const bool wantProc = (cat == Category::Procedures);
                const int img = wantProc ? IMG_PROC : IMG_FUNC;
                for (const auto& r : routines) {
                    const bool isProc = (r.type == L"PROCEDURE");
                    if (caps.splitProcFunc && isProc != wantProc) continue;
                    // Tag the leaf (kind=Table, cat=Functions/Procedures) so its
                    // context menu / double-click can drive the 修改 object editor.
                    auto* nd = new NodeData(NodeData::Table, e, db, r.name);
                    nd->cat = cat;
                    tree_->AppendItem(catNode, r.name, img, img, nd);
                }
                break;
            }
            case Category::Triggers:
                // NOTE: trigger leaves carry NO NodeData, so they are the one
                // object kind RegroupCategory cannot move into a folder (it
                // identifies a leaf by its NodeData). Grouping is offered for
                // 表/视图/函数/存储过程 only — see AppendGroupMenu.
                for (const auto& t : triggers)
                    tree_->AppendItem(catNode, t.name, IMG_TRIGGER, IMG_TRIGGER);
                break;
            }
            // The members were appended FLAT above; this re-parents them into
            // the user's group folders. Done as a separate pass rather than
            // inline in each case so all four object kinds get identical
            // grouping behaviour from one implementation.
            //
            // ORDER IS LOAD-BEARING: RegroupCategory refuses to touch a folder
            // whose members are still loading, and it reads exactly this flag to
            // decide. Setting it after the call would make the FIRST render of
            // every category skip grouping entirely — the folders would only
            // appear on a later refresh.
            d->loaded = true;
            RegroupCategory(catNode);
        });
    });
}

// Cancel any in-flight member query and join the loader. The single-threaded
// connection means a new load must wait for the previous one; Cancel() keeps that
// wait short. Does NOT bump the generation (LoadCategoryMembers does that for its
// own new load).
void ConnectionTree::JoinCatWorker()
{
    if (catWorker_.joinable()) {
        if (catConn_) catConn_->Cancel();
        catWorker_.join();
    }
    catConn_ = nullptr;
}

// Stop the loader AND invalidate any result it already queued, so a pending
// CallAfter can't touch tree nodes we are about to delete. Call before any
// structural change (disconnect, delete, reload).
void ConnectionTree::AbortCatWorker()
{
    JoinCatWorker();
    ++catGen_;
}

} // namespace ui
