// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/MainFrame.h"
#include "core/CrashLog.h"

#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/aui/framemanager.h>
#include <wx/aui/dockart.h>
#include <wx/busyinfo.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/grid.h>
#include <wx/imaglist.h>
#include <wx/notebook.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/activityindicator.h>
#include <wx/srchctrl.h>
#include <wx/simplebook.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/bmpbuttn.h>
#include <wx/statbmp.h>
#include <wx/accel.h>

#ifdef __WXMSW__
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <uxtheme.h>   // MARGINS
#  include <dwmapi.h>    // DwmExtendFrameIntoClientArea
#  pragma comment(lib, "dwmapi.lib")
#  undef DrawText        // windows.h maps DrawText→DrawTextW; we call wxDC/gc DrawText
#endif
#include <wx/stopwatch.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <vector>
#include <wx/srchctrl.h>
#include <wx/stc/stc.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>   // connTree_->widget() is a wxGenericTreeCtrl

#include "core/ConnectionStore.h"
#include "core/FavoriteStore.h"
#include "db/SqlScript.h"          // db::SplitSqlScript (dialect-aware statement splitter, R2)
#include "ui/CenteredDialog.h"
#include "ui/ConnectionDialog.h"
#include "ui/ConnectionTree.h"
#include "ui/EditorPage.h"
#include "ui/ObjectTemplates.h"
#include "ui/ExportDialog.h"
#include "ui/ImportDialog.h"
#include "ui/ResultGridPanel.h"
#include "ui/TableDataPage.h"
#include "ui/ErDiagramView.h"
#include "ui/QueryBuilderPanel.h"
#include "ui/ObjectListPanel.h"
#include "ui/UserEditDialog.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/PreferencesDialog.h"
#include "ui/TableDesignView.h"
#include "ui/NewTableView.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/MainFrameInternal.h"   // ui::MakeObjectTarget (defined in MainFrame_Chrome.cpp)
#include "ui/ConnectionTreeInternal.h"   // ConnEntry (full type for the AI chat host)
#include "ui/AiChatPanel.h"              // AiChatPanel + AiChatHost (the AI 助手 chat page)

namespace ui {
namespace {

// Autocompletion source bound to whatever connection is active at call time.
EditorPage::CompletionSource MakeCompletion(ConnectionTree* ct)
{
    EditorPage::CompletionSource s;
    s.dialect = [ct]() -> db::Dialect {
        ConnEntry* a = ct->active();
        return (a && a->IsConnected()) ? a->conn->GetDialect() : db::Dialect::MySQL;
    };
    s.databases = [ct]() -> std::vector<wxString> {
        std::vector<wxString> out;
        ConnEntry* a = ct->active();
        if (a && a->IsConnected()) {
            std::vector<wxString> dbs; wxString e;
            if (a->conn->ListDatabases(dbs, e)) out = std::move(dbs);
        }
        return out;
    };
    s.tables = [ct]() -> std::vector<wxString> {
        std::vector<wxString> out;
        ConnEntry* a = ct->active();
        if (a && a->IsConnected()) {
            std::vector<db::TableInfo> ts; wxString e;
            if (a->conn->ListTables(a->currentDb, ts, e))
                for (const auto& t : ts) out.push_back(t.name);
        }
        return out;
    };
    s.columns = [ct](const wxString& table) -> std::vector<wxString> {
        std::vector<wxString> out;
        ConnEntry* a = ct->active();
        if (a && a->IsConnected()) {
            std::vector<db::ColumnInfo> cs; wxString e;
            if (a->conn->GetColumns(a->currentDb, table, cs, e))
                for (const auto& c : cs) out.push_back(c.name);
        }
        return out;
    };
    // db-qualified variants: resolve tables/columns against an explicit database
    // (e.g. "mydb." / "mydb.mytable.") so completion fires with no active DB.
    s.tablesOf = [ct](const wxString& db) -> std::vector<wxString> {
        std::vector<wxString> out;
        ConnEntry* a = ct->active();
        if (a && a->IsConnected()) {
            std::vector<db::TableInfo> ts; wxString e;
            if (a->conn->ListTables(db, ts, e))
                for (const auto& t : ts) out.push_back(t.name);
        }
        return out;
    };
    s.columnsOf = [ct](const wxString& db, const wxString& table) -> std::vector<wxString> {
        std::vector<wxString> out;
        ConnEntry* a = ct->active();
        if (a && a->IsConnected()) {
            std::vector<db::ColumnInfo> cs; wxString e;
            if (a->conn->GetColumns(db, table, cs, e))
                for (const auto& c : cs) out.push_back(c.name);
        }
        return out;
    };
    return s;
}

} // namespace

// Show the editor notebook when there is at least one query tab, otherwise the
// empty-state placeholder.
void MainFrame::UpdateQueryView()
{
    if (!queryStack_ || !editors_) return;
    const bool hasTabs = editors_->GetPageCount() > 0;
    queryStack_->SetSelection(hasTabs ? 0 : 1);
}

// 强制标签条重绘。editors_ 的标签条是子窗口(wxAuiTabCtrl),父 Refresh 不递归到
// 子窗口,自定义色可能要 hover/切页才生效;逐个找出并立即重绘。
void MainFrame::RefreshEditorTabBar()
{
    if (!editors_) return;
    editors_->Refresh();
    for (wxWindow* c : editors_->GetChildren())
        if (dynamic_cast<wxAuiTabCtrl*>(c)) { c->Refresh(); c->Update(); }
}

// Return the active editor page, creating a fresh one if there are no tabs.
EditorPage* MainFrame::EnsurePage()
{
    if (EditorPage* p = ActivePage()) return p;
    wxCommandEvent e; OnNewQuery(e);
    return ActivePage();
}

// ---------------------------------------------------------------------------
// A centred pill segmented control (SQL 编辑器 / 表设计 / ER 图), matching the
// design mock: grey pill, white rounded highlight behind the active segment, each
// segment an icon + label. The segments now OPEN/FOCUS the matching editors_ tab
// (ActivatePillSegment); the highlight follows the active tab's type, not view_.
void MainFrame::PaintViewSwitch()
{
    wxAutoBufferedPaintDC dc(viewBar_);
    dc.SetBackground(wxBrush(theme::kWhite));   // header is white
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxString labs[3] = { tr(L"SQL脚本"), tr(L"用户组"), tr(L"AI") };
    const icons::Glyph icon[3] = { icons::Glyph::Save, icons::Glyph::Users,
                                   icons::Glyph::StarLine };
    const double iconW = 16, gap = 7, padX = 14, ph = 34, ih = 28;

    gc->SetFont(Ui(9.5, true), theme::kText);
    double tw[3] = {}, th = 0, total = 0;
    for (int i = 0; i < 3; ++i) {
        double w = 0, h = 0; gc->GetTextExtent(labs[i], &w, &h);
        tw[i] = w; th = h; total += iconW + gap + w + padX * 2;
    }
    const wxSize sz = viewBar_->GetClientSize();
    const double pillPad = 4;
    const double pillW = total + pillPad * 2;
    const double x0 = (sz.x - pillW) / 2, y0 = (sz.y - ph) / 2;

    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(0xD5, 0xDA, 0xE1)).Width(1)));
    gc->SetBrush(wxBrush(wxColour(0xE1, 0xE5, 0xEB)));
    wxGraphicsPath pill = gc->CreatePath();
    pill.AddRoundedRectangle(x0, y0, pillW, ph, 11);
    gc->DrawPath(pill);

    const int seg = ActivePillSegment();   // 高亮跟随活动标签类型(-1 = 不高亮)
    double x = x0 + pillPad;
    for (int i = 0; i < 3; ++i) {
        const double tabW = iconW + gap + tw[i] + padX * 2;
        const bool on = (i == seg);
        tabRects_[i] = wxRect(static_cast<int>(x), static_cast<int>(y0),
                              static_cast<int>(tabW), static_cast<int>(ph));
        if (on) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(theme::kWhite));
            wxGraphicsPath t = gc->CreatePath();
            t.AddRoundedRectangle(x + 1, y0 + (ph - ih) / 2, tabW - 2, ih, 8);
            gc->FillPath(t);
        }
        const wxColour fg = on ? theme::kText : theme::kTextSecondary;
        gc->DrawBitmap(icons::Stroke(icon[i], 16, fg, 1.9),
                       x + padX, y0 + (ph - iconW) / 2, iconW, iconW);
        gc->SetFont(Ui(9.5, on), fg);
        gc->DrawText(labs[i], x + padX + iconW + gap, y0 + (ph - th) / 2);
        x += tabW;
    }
    // (no bottom separator — the whole top chrome is seamless white)
}

// Which pill segment matches the currently-active editors_ tab, for the highlight:
// the script-library tab → 0 (SQL脚本), the user-management tab → 1 (用户组), the
// "AI 功能建设中" placeholder tab → 2 (AI), anything else (editor / 表列表 / 设计标签 /
// no tabs) → -1 (no highlight).
int MainFrame::ActivePillSegment() const
{
    if (!editors_) return -1;
    wxWindow* cur = editors_->GetCurrentPage();
    if (!cur) return -1;
    if (cur == scriptLibTab_)   return 0;   // SQL脚本 段高亮 = 脚本库标签在前
    if (userTabs_.count(cur))   return 1;   // 用户组 段高亮 = 用户列表标签在前
    if (cur == aiTab_)          return 2;   // AI 段高亮 = "AI 功能建设中"标签在前
    return -1;
}

// Pill click: each segment opens/focuses its kind of tab (it is an action, not a
// view switch). 0 = open the saved-SQL script library; 1 = open the user-management
// (用户组) list for the active connection; 2 = open the "AI 功能建设中" placeholder.
void MainFrame::ActivatePillSegment(int i)
{
    if (i == 0) {
        OpenScriptLibrary();   // SQL脚本 段 → 打开已保存 SQL 脚本库标签
        return;
    } else if (i == 1) {
        OpenUserListTab();     // 用户组 段 → 打开用户列表标签(仍可从树/对象列表设计表)
        return;
    } else if (i == 2) {
        OpenAiPlaceholderTab();   // AI 段 → 打开"AI 功能建设中"占位标签
        return;
    }
    if (viewBar_) viewBar_->Refresh();   // 动作后刷新高亮
}

// ---------------------------------------------------------------------------
void MainFrame::SwitchView(View v)
{
    view_ = v;
    // mainBook_ 只剩一页(查询,editors_ 记事本);连接总览/库表列表/表设计/ER 都在
    // editors_ 里,通过各自的 Open*Tab / EnsureDbOverviewTab 打开,不走这里。
    mainBook_->SetSelection(0);
    if (viewBar_) viewBar_->Refresh();   // pill 高亮跟随活动标签,视图变化也刷一次
    RefreshBottomBar();   // the rows/db/SQL bar only shows in the query view (§5)
}

EditorPage* MainFrame::ActivePage() const
{
    return editors_ ? dynamic_cast<EditorPage*>(editors_->GetCurrentPage()) : nullptr;
}

// The active tab as a runnable query surface — an SQL editor tab or a dedicated
// open-table data tab (both implement IQueryTab).
IQueryTab* MainFrame::ActiveTab() const
{
    return editors_ ? dynamic_cast<IQueryTab*>(editors_->GetCurrentPage()) : nullptr;
}

// Wire the caller-supplied handlers a result grid needs: edit-apply, pager
// row-count, table-info. Shared by SQL editor tabs and open-table tabs.
void MainFrame::ConfigureGrid(ResultGridPanel* grid)
{
    grid->SetApplyHandler([this](const wxString& dml) { ApplyDataChanges(dml); });
    grid->SetApplyInTxnHandler([this](const wxString& dml) { ApplyDataChangesInTxn(dml); });
    grid->SetStatusChanged([this] { RefreshBottomBar(); });   // rows/db/SQL → global bar (§5)
    grid->SetCountHandler([this, grid](const wxString& t, const wxString& w,
                                 std::function<void(long long)> done) {
        // Resolve the notebook page owning this grid so the async COUNT result can be
        // dropped if the tab is closed before it lands (comparing a possibly-dangling
        // wxWindow* against the live page list is pointer-safe, as in RunSql).
        wxWindow* owner = grid;
        while (owner && editors_->GetPageIndex(owner) == wxNOT_FOUND)
            owner = owner->GetParent();
        CountRowsForPager(t, w, owner, std::move(done));
    });
    grid->SetTableDetailHandler([this](const wxString& table,
                                       std::function<void(const db::TableDetail&)> done) {
        ConnEntry* a = connTree_->active();
        db::TableDetail d;
        if (a && a->IsConnected() && !table.IsEmpty()) {
            wxString err; wxBusyCursor busy;
            a->conn->GetTableDetail(a->currentDb, table, d, err);
        }
        done(d);
    });
    // 导出 / 将数据另存为… — package the current view into an ExportDialog against
    // the live connection (streams the whole table chunk-by-chunk in the worker).
    grid->SetExportHandler([this](const ui::ExportRequest& req) {
        ConnEntry* a = connTree_->active();
        if (!a || !a->IsConnected()) {
            wxMessageBox(tr(L"尚未连接数据库。"), tr(L"导出数据"),
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        ExportDialog dlg(this, a->conn.get(), req);
        dlg.ShowModal();
    });
    // 导入 — an ImportDialog into the target table (falls back to the active db).
    grid->SetImportHandler([this](const wxString& db, const wxString& table, db::Dialect) {
        ConnEntry* a = connTree_->active();
        if (!a || !a->IsConnected()) {
            wxMessageBox(tr(L"尚未连接数据库。"), tr(L"导入数据"),
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        const wxString tdb = db.IsEmpty() ? a->currentDb : db;
        ImportDialog dlg(this, a->conn.get(), tdb, table);
        dlg.ShowModal();
    });
}

// Wire an SQL editor tab: schema-aware completion (editor-only) + the shared
// result-grid handlers.
void MainFrame::ConfigurePage(EditorPage* page)
{
    page->SetCompletionSource(MakeCompletion(connTree_.get()));
    ConfigureGrid(page->Grid());
    // The right-side AI panel's host. Given to EVERY editor page (plain SQL and
    // object editors alike) but nothing is built until the user expands the
    // panel — SetAiHost only stores the callbacks.
    page->SetAiHost(MakeAiHost());
    // Ctrl+S on a plain SQL editor saves the buffer to the script library. The
    // editor tracks its own saved name; MainFrame owns the prompt + ScriptStore +
    // library refresh (and renaming this tab to the script name).
    page->SetSaveScriptHook([this, page](const wxString& sql, const wxString& cur) {
        return SaveEditorScript(page, sql, cur);
    });
    // Top 连接/数据库 selector on every editor tab (plain SQL + object). Default to
    // the connection + db active when this tab was created; empty when there is no
    // active connection (selector shows nothing, never crashes). An object editor
    // overrides this with its own object's connection/db via a second SetObjectTarget
    // call right after ConfigurePage (see newObjectEditor in MainFrame_Chrome.cpp).
    void* h = connTree_->active() ? static_cast<void*>(connTree_->active()) : nullptr;
    wxString db = connTree_->active() ? connTree_->active()->currentDb : wxString();
    page->SetObjectTarget(MakeObjectTarget(connTree_.get()), h, db);
}

// Open a table as a dedicated Navicat-style data tab (browse mode: no SQL editor,
// just toolbar + grid + right info panel + pager). One tab per table; loads its
// rows immediately.
void MainFrame::OpenTableTab(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return;
    connTree_->setActive(e);
    e->currentDb = db;
    selTable_ = table;

    auto* page = new TableDataPage(editors_, db, table, e->conn->GetDialect());
    // Wire the shared result-grid handlers (edit-apply, pager row-count via the
    // db-qualified COUNT(*), table-info) exactly like an SQL editor tab. The paged
    // SELECT runs through the same ID_RUN_QUERY → RunSql → QueryText path.
    ConfigureGrid(page->Grid());
    editors_->AddPage(page, db + L"." + table, true,
                      icons::Stroke(icons::Glyph::Table, 14, theme::kGreen));
    const int idx = editors_->GetPageIndex(page);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + db + L" / " + table);
    dataTabs_[page] = e;       // 登记归属连接 → 断连时随该连接一并关闭
    UpdateQueryView();
    SwitchView(View::Query);   // the editors notebook lives in the query view
    page->Load();              // count + load the first page
}

// Open (or focus) a table's structure-design tab. One closable tab per (db,table);
// clicking 设计表 again just re-focuses the existing tab. Returns the tab's view so
// scripted verification (SWIFTSQL_AUTOADDCOL) can drive it.
TableDesignView* MainFrame::OpenDesignTab(ConnEntry* e, const wxString& db, const wxString& table)
{
    if (!e || !e->IsConnected() || table.IsEmpty()) return nullptr;
    connTree_->setActive(e);
    e->currentDb = db;
    selTable_ = table;

    // 去重:按 (conn,db,table) 三元组比较(含连接身份,两连接同名表不误聚焦)。
    for (const auto& kv : designTabs_) {
        if (kv.second.entry == e && kv.second.db == db && kv.second.table == table) {
            const int idx = editors_->GetPageIndex(kv.first);
            if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
            SwitchView(View::Query);
            return dynamic_cast<TableDesignView*>(kv.first);
        }
    }

    auto* page = new TableDesignView(editors_);
    page->Load(e->conn, db, table, e->profile.type);
    // 关闭 toolbar button → remove this tab. Programmatic DeletePage does not fire the
    // AUINOTEBOOK_PAGE_CLOSE veto, so the view's own confirm (already run) isn't asked
    // twice. Deferred so the button click finishes before the page is destroyed.
    page->SetOnRequestClose([this, page] {
        CallAfter([this, page] {
            tabColors_.erase(page);
            dataTabs_.erase(page);
            designTabs_.erase(page);
            erTabs_.erase(page);
            const int i = editors_->GetPageIndex(page);
            if (i != wxNOT_FOUND) editors_->DeletePage(i);
            UpdateQueryView();
            RefreshBottomBar();
        });
    });
    editors_->AddPage(page, tr(L"设计: ") + db + L"." + table, /*select*/ true,
                      icons::Stroke(icons::Glyph::Model, 14, theme::kPrimary));
    const int idx = editors_->GetPageIndex(page);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + db + L" / " + table);
    designTabs_[page] = { e, db, table };
    UpdateQueryView();
    SwitchView(View::Query);   // editors_ 位于查询视图内
    return page;
}

// A fresh 新建表 (create-table) designer tab. Unlike OpenDesignTab this is not
// deduplicated (each is a brand-new unnamed table) and is NOT tracked in designTabs_.
void MainFrame::OpenNewTableTab(ConnEntry* e, const wxString& db)
{
    if (!e || !e->IsConnected()) return;
    connTree_->setActive(e);
    e->currentDb = db;

    auto* page = new NewTableView(editors_);
    page->BeginNew(e->conn, db, e->profile.type);
    page->SetOnRequestClose([this, page] {
        CallAfter([this, page] {
            tabColors_.erase(page);
            designTabs_.erase(page);
            const int i = editors_->GetPageIndex(page);
            if (i != wxNOT_FOUND) editors_->DeletePage(i);
            UpdateQueryView();
            RefreshBottomBar();
        });
    });
    page->SetOnTableCreated([this, e, db](const wxString&) {
        if (dbovEntry_ == e && dbovDb_ == db)
            ShowDatabaseOverview(e, db, /*force*/ true);   // the new table appears in 表信息
    });
    editors_->AddPage(page, tr(L"新建表: ") + db, /*select*/ true,
                      icons::Stroke(icons::Glyph::RowAdd, 14, theme::kPrimary));
    const int idx = editors_->GetPageIndex(page);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + db + L" / " + tr(L"新建表"));
    // 归属连接 → 断连时随该连接一并关闭。表名留空:OpenDesignTab 的去重只查非空表名
    // 的三元组,所以这条记录永远不会被误当成某张已有表的设计标签。这个登记以前缺失,
    // 于是"新建表"标签在断开连接后仍活着,握着一个已释放的连接。
    designTabs_[page] = { e, db, wxString() };
    UpdateQueryView();
    SwitchView(View::Query);
}

// Open (or focus) a database's ER-diagram tab. One closable tab per (conn,db).
void MainFrame::OpenErTab(ConnEntry* e, const wxString& db)
{
    if (!e || !e->IsConnected()) return;
    connTree_->setActive(e);
    e->currentDb = db;

    // 去重:同一 (conn,db) 已有 ER 标签就聚焦它。
    for (const auto& kv : erTabs_) {
        if (kv.second.first == e && kv.second.second == db) {
            const int idx = editors_->GetPageIndex(kv.first);
            if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
            SwitchView(View::Query);
            return;
        }
    }

    auto* page = new ErDiagramView(editors_);
    page->Load(e->conn.get(), db);
    editors_->AddPage(page, tr(L"ER: ") + db, /*select*/ true,
                      icons::Stroke(icons::Glyph::ErDiagram, 14, theme::kPrimary));
    const int idx = editors_->GetPageIndex(page);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + db);
    erTabs_[page] = { e, db };
    UpdateQueryView();
    SwitchView(View::Query);
}

// ---------------------------------------------------------------------------
// "AI 功能建设中" placeholder tab — a friendly centred panel (big brand-blue star
// + title + subtitle) shown while the AI features (generate / optimize SQL, smart
// diagnostics) are still being built. Reusable singleton like the script library
// (closable; aiTab_ nulled on close in the editors_ PAGE_CLOSE handler).

// Lazily create the single placeholder tab (dedupe-focus it if already open).
wxWindow* MainFrame::EnsureAiPlaceholderTab()
{
    if (aiTab_ && editors_->GetPageIndex(aiTab_) != wxNOT_FOUND)
        return aiTab_;

    // Real AI chat page (replaces the old "建设中" placeholder). The host wires the
    // panel to the connection tree (connection / database enumeration, same source as
    // MakeObjectTarget) and to the editor for SQL routing:
    //   runQuery(sql)       → a read (SELECT/…): open a query tab and RUN it (jump to results)
    //   insertToEditor(sql) → a write (UPDATE/DELETE/DDL): insert but DON'T run (user reviews)
    AiChatHost host = MakeAiHost();
    auto* p = new AiChatPanel(editors_, std::move(host));
    aiTab_ = p;
    editors_->AddPage(p, tr(L"AI"), /*select*/ true,
                      icons::Stroke(icons::Glyph::StarLine, 14, theme::kPrimary));
    UpdateQueryView();
    return aiTab_;
}

// The connection / database / knowledge-base half of an AiChatHost. Extracted so
// the AI TAB and every SQL editor's own right-side panel share one definition —
// two copies of "how the AI enumerates connections" is how the tab and the panel
// would start disagreeing about which databases exist.
//
// The two SQL-routing callbacks are filled in here for the tab (route to the
// active editor); EditorPage::SetAiHost REPLACES them so a docked panel writes
// into the editor it lives in.
AiChatHost MainFrame::MakeAiHost()
{
    ConnectionTree* ct = connTree_.get();
    AiChatHost host;
    host.connections = [ct]() -> std::vector<std::pair<wxString, void*>> {
        std::vector<std::pair<wxString, void*>> out;
        for (ConnEntry* e : ct->connectedEntries())
            out.emplace_back(e->profile.name, static_cast<void*>(e));
        return out;
    };
    host.databases = [ct](void* h) -> std::vector<wxString> {
        std::vector<wxString> out;
        auto* e = static_cast<ConnEntry*>(h);
        if (ct->isLive(e)) {
            std::vector<wxString> dbs; wxString err;
            if (e->conn->ListDatabases(dbs, err)) out = std::move(dbs);
        }
        return out;
    };
    host.connection = [ct](void* h) -> db::IConnection* {
        auto* e = static_cast<ConnEntry*>(h);
        return ct->isLive(e) ? e->conn.get() : nullptr;
    };
    // A dedicated connection for the knowledge base — same handle→ConnEntry mapping,
    // but a fresh independent connection (its own SSH tunnel if the profile needs
    // one) so the KB's worker thread never touches the UI's shared connection.
    host.makeConnection = [ct](void* h, wxString& err) -> std::unique_ptr<db::IConnection> {
        auto* e = static_cast<ConnEntry*>(h);
        if (!ct->isLive(e)) { err = tr(L"数据库连接不可用"); return nullptr; }
        return ct->MakeDedicatedConnection(e->profile, err);
    };
    // Beautify the AI-generated statement ONCE before it reaches the editor: the model
    // emits SQL/DDL as one dense line, so run the same dialect-aware pretty-printer the
    // "美化" button uses (ScriptedFormat) so the user gets a formatted query, not a wall.
    host.runQuery = [this](const wxString& sql) {
        if (EditorPage* p = EnsurePage()) { const wxString q = p->ScriptedFormat(sql); p->InsertQuery(q); RunSql(p, q); }
    };
    host.insertToEditor = [this](const wxString& sql) {
        if (EditorPage* p = EnsurePage()) p->InsertQuery(p->ScriptedFormat(sql));
    };
    return host;
}

// AI entry point (pill AI segment + toolbar "AI 生成 SQL" button): open the
// placeholder tab (creating it once) and bring it to front.
void MainFrame::OpenAiPlaceholderTab()
{
    if (!connTree_ || connTree_->connectedEntries().empty()) {
        wxMessageBox(tr(L"AI 助手需要先打开一个数据库连接。\n\n请在左侧连接列表双击连接，或点击“新建连接”完成连接后再使用 AI。"),
                     tr(L"AI 助手"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    EnsureAiPlaceholderTab();
    const int idx = editors_->GetPageIndex(aiTab_);
    if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
    SwitchView(View::Query);
    if (viewBar_) viewBar_->Refresh();   // AI 段高亮
}

// Open a fresh visual query-builder tab on the active connection's current database.
// Multi-open (each click is a new builder — no dedupe; builders are cheap and users
// may want several side by side). The panel holds a raw db::IConnection*, so it is
// registered in dataTabs_ under its owning connection: CloseTabsForEntry closes it
// before that connection is torn down, so the pointer never dangles. No SQL runs
// here — the builder only generates text and hands it to a new SQL editor tab via
// its apply hook (the user still runs it manually, so it is safe and editable).
void MainFrame::OpenQueryBuilder()
{
    ConnEntry* e = connTree_->active();
    if (!e || !e->IsConnected()) {
        wxMessageBox(tr(L"请先连接并选择数据库,再构建查询。"), tr(L"构建查询"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (e->currentDb.IsEmpty()) {
        wxMessageBox(tr(L"请先选择一个数据库(在左侧展开并点击一个库)。"), tr(L"构建查询"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    connTree_->setActive(e);

    auto* page = new QueryBuilderPanel(editors_);
    // 应用到编辑器: open a fresh SQL editor tab prefilled with the generated SQL
    // (same path as 新建SQL). We do NOT execute it — the editor is the safe, editable
    // handoff surface, exactly like the CREATE-template flow.
    page->SetApplyHook([this](const wxString& sql) {
        wxCommandEvent d; OnNewQuery(d);
        if (EditorPage* p = ActivePage()) p->InsertQuery(sql);
        SwitchView(View::Query);
    });
    page->Load(e->conn, e->currentDb, e->conn->GetDialect());
    editors_->AddPage(page, tr(L"构建查询: ") + e->currentDb, /*select*/ true,
                      icons::Stroke(icons::Glyph::ErDiagram, 14, theme::kAccent));
    const int idx = editors_->GetPageIndex(page);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + e->currentDb);
    dataTabs_[page] = e;       // 归属连接 → 断连时随该连接一并关闭(避免悬空 conn_)
    UpdateQueryView();
    SwitchView(View::Query);
}

// ---------------------------------------------------------------------------
// Object-list tabs (表 / 视图 / 函数 / 存储过程) launched from the top toolbar.
namespace {

// Human-readable byte size / thousands-grouped count for the table-list columns
// ("—" for unknown/-1). Local copies of the overview's formatters (file-local there).
wxString ObjFmtBytes(long long b)
{
    if (b < 0) return L"—";
    const wchar_t* unit[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    return u == 0 ? wxString::Format(L"%lld B", b)
                  : wxString::Format(L"%.1f %s", v, unit[u]);
}
wxString ObjFmtCount(long long n)
{
    if (n < 0) return L"—";
    wxString s = wxString::Format(L"%lld", n), out;
    int c = 0;
    for (int i = static_cast<int>(s.length()) - 1; i >= 0; --i) {
        out.Prepend(s[i]);
        if (++c % 3 == 0 && i > 0) out.Prepend(L',');
    }
    return out;
}

} // namespace

// Configure an object-list panel's columns, toolbar/context actions, and data loader
// for one object kind. Every action re-validates the connection via isLive before use
// and reuses ConnectionTree's existing object flows (new/modify/drop/execute); a
// destructive op reloads the list and syncs the sidebar tree folder afterwards.
void MainFrame::ConfigureObjectList(ObjectListPanel* panel, ConnEntry* e,
                                    const wxString& db, ObjectListKind kind)
{
    using Row    = ObjectListPanel::Row;
    using Action = ObjectListPanel::Action;
    ConnectionTree* ct = connTree_.get();

    // ---- loader: one metadata query per kind (guarded by isLive) ----
    panel->SetLoader([ct, e, db, kind](std::vector<Row>& out, wxString& err) -> bool {
        if (!ct->isLive(e)) { err = tr(L"连接已断开"); return false; }
        wxBusyCursor busy;
        db::IConnection* c = e->conn.get();
        switch (kind) {
        case ObjectListKind::Tables: {
            std::vector<db::TableMeta> metas;
            if (!c->GetTableList(db, metas, err)) return false;
            for (const auto& m : metas)
                out.push_back({ { m.name, m.engine, ObjFmtCount(m.rows),
                                  ObjFmtBytes(m.sizeBytes), m.comment } });
            return true;
        }
        case ObjectListKind::Views: {
            std::vector<wxString> views;
            if (!c->ListViews(db, views, err)) return false;
            for (const auto& v : views) out.push_back({ { v } });
            return true;
        }
        case ObjectListKind::Functions:
        case ObjectListKind::Procedures: {
            std::vector<db::RoutineInfo> routines;
            if (!c->ListRoutines(db, routines, err)) return false;
            const bool wantProc = (kind == ObjectListKind::Procedures);
            for (const auto& r : routines) {
                const bool isProc = (r.type.CmpNoCase(L"PROCEDURE") == 0);
                if (isProc != wantProc) continue;
                out.push_back({ { r.name, r.type } });
            }
            return true;
        }
        }
        return true;
    });

    // ---- columns + actions per kind ----
    std::vector<ObjectListPanel::Column> cols;
    std::vector<Action> actions;
    int dbl = -1;

    auto reloadAfter = [panel] { panel->Reload(); };

    if (kind == ObjectListKind::Tables) {
        cols = { { tr(L"名称"), 220 }, { tr(L"引擎"), 90 },
                 { tr(L"行数"), 100, true }, { tr(L"大小"), 100, true },
                 { tr(L"注释"), 220 } };
        actions = {
            { tr(L"新建表"), icons::Glyph::Plus, theme::kGreen, false,
              [this, e, db](const wxString&, const std::vector<wxString>&) {
                  OpenNewTableTab(e, db); } },
            { tr(L"打开表"), icons::Glyph::Table, theme::kPrimary, true,
              [this, e, db](const wxString& n, const std::vector<wxString>&) {
                  if (!n.IsEmpty()) OpenTableTab(e, db, n); } },
            { tr(L"设计表"), icons::Glyph::Model, theme::kAccent, true,
              [this, e, db](const wxString& n, const std::vector<wxString>&) {
                  if (!n.IsEmpty()) OpenDesignTab(e, db, n); } },
            { tr(L"删除表"), icons::Glyph::Close, theme::kDotRed, true,
              [this, ct, e, db, reloadAfter](const wxString&, const std::vector<wxString>& all) {
                  if (all.empty() || !ct->isLive(e)) return;
                  if (ct->DropTablesUi(e, db, all)) { reloadAfter(); ct->RefreshTablesUi(e, db); } } },
            { tr(L"刷新"), icons::Glyph::Refresh, theme::kTextSecondary, false,
              [reloadAfter](const wxString&, const std::vector<wxString>&) { reloadAfter(); } },
        };
        dbl = 1;   // double-click → 打开表
    } else if (kind == ObjectListKind::Views) {
        cols = { { tr(L"名称"), 320 } };
        actions = {
            { tr(L"新建视图"), icons::Glyph::Plus, theme::kAccent, false,
              [ct, e, db](const wxString&, const std::vector<wxString>&) {
                  ct->NewObjectUi(e, db, ObjectKind::View); } },
            { tr(L"打开视图"), icons::Glyph::Table, theme::kPrimary, true,
              [this, e, db](const wxString& n, const std::vector<wxString>&) {
                  if (!n.IsEmpty()) OpenTableTab(e, db, n); } },
            { tr(L"修改视图"), icons::Glyph::Code, theme::kDotPurple, true,
              [ct, e, db](const wxString& n, const std::vector<wxString>&) {
                  if (!n.IsEmpty()) ct->ModifyObjectUi(e, db, n, ObjectKind::View); } },
            { tr(L"删除视图"), icons::Glyph::Close, theme::kDotRed, true,
              [ct, e, db, reloadAfter](const wxString& n, const std::vector<wxString>&) {
                  if (n.IsEmpty() || !ct->isLive(e)) return;
                  ct->DropObjectUi(e, db, n, ObjectKind::View);
                  reloadAfter(); ct->RefreshCategory(e, db, ObjectKind::View); } },
            { tr(L"刷新"), icons::Glyph::Refresh, theme::kTextSecondary, false,
              [reloadAfter](const wxString&, const std::vector<wxString>&) { reloadAfter(); } },
        };
        dbl = 2;   // double-click → 修改视图
    } else {   // Functions or Procedures
        const bool isProc = (kind == ObjectListKind::Procedures);
        const ObjectKind ok = isProc ? ObjectKind::Procedure : ObjectKind::Function;
        cols = { { tr(L"名称"), 300 }, { tr(L"类型"), 120 } };
        actions = {
            { isProc ? tr(L"新增存储过程") : tr(L"新增函数"), icons::Glyph::Plus,
              theme::kDotAmber, false,
              [ct, e, db, ok](const wxString&, const std::vector<wxString>&) {
                  ct->NewObjectUi(e, db, ok); } },
            { isProc ? tr(L"修改存储过程") : tr(L"修改函数"), icons::Glyph::Code,
              theme::kDotPurple, true,
              [ct, e, db, ok](const wxString& n, const std::vector<wxString>&) {
                  if (!n.IsEmpty()) ct->ModifyObjectUi(e, db, n, ok); } },
            { isProc ? tr(L"删除存储过程") : tr(L"删除函数"), icons::Glyph::Close,
              theme::kDotRed, true,
              [ct, e, db, ok, reloadAfter](const wxString& n, const std::vector<wxString>&) {
                  if (n.IsEmpty() || !ct->isLive(e)) return;
                  ct->DropObjectUi(e, db, n, ok);
                  reloadAfter(); ct->RefreshCategory(e, db, ok); } },
            { isProc ? tr(L"执行存储过程") : tr(L"执行函数"), icons::Glyph::Play,
              theme::kGreen, true,
              [ct, e, db, isProc](const wxString& n, const std::vector<wxString>&) {
                  if (!n.IsEmpty()) ct->ExecuteRoutineUi(e, db, n, isProc); } },
            { tr(L"刷新"), icons::Glyph::Refresh, theme::kTextSecondary, false,
              [reloadAfter](const wxString&, const std::vector<wxString>&) { reloadAfter(); } },
        };
        dbl = 1;   // double-click → 修改
    }

    panel->Setup(std::move(cols), std::move(actions), dbl);
}

// Open (or focus) an object-list tab for `kind` on the active connection's current
// database. One tab per (conn, db, kind): re-clicking the toolbar button re-focuses
// the existing tab. Requires an active, connected connection with a selected database
// — otherwise a hint (no empty tab, no crash).
void MainFrame::OpenObjectListTab(ObjectListKind kind)
{
    ConnEntry* e = connTree_->active();
    if (!e || !e->IsConnected() || e->currentDb.IsEmpty()) {
        wxMessageBox(tr(L"请先连接并打开一个数据库。"), tr(L"对象列表"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const wxString db = e->currentDb;

    // 去重:同一 (conn, db, kind) 已有标签就聚焦它。
    for (const auto& kv : objListTabs_) {
        if (kv.second.entry == e && kv.second.db == db && kv.second.kind == kind) {
            const int idx = editors_->GetPageIndex(kv.first);
            if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
            SwitchView(View::Query);
            return;
        }
    }

    struct KindMeta { const wchar_t* label; icons::Glyph glyph; wxColour color; };
    const KindMeta meta =
        kind == ObjectListKind::Tables     ? KindMeta{ L"表",       icons::Glyph::Table,      theme::kGreen }
      : kind == ObjectListKind::Views      ? KindMeta{ L"视图",     icons::Glyph::ViewLayers, theme::kAccent }
      : kind == ObjectListKind::Functions  ? KindMeta{ L"函数",     icons::Glyph::Function,   theme::kDotAmber }
                                           : KindMeta{ L"存储过程", icons::Glyph::Code,       theme::kDotPurple };

    auto* panel = new ObjectListPanel(editors_);
    ConfigureObjectList(panel, e, db, kind);
    editors_->AddPage(panel, tr(meta.label) + L": " + db, /*select*/ true,
                      icons::Stroke(meta.glyph, 14, meta.color));
    const int idx = editors_->GetPageIndex(panel);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + db + L" / " + tr(meta.label));
    objListTabs_[panel] = { e, db, kind };
    UpdateQueryView();
    SwitchView(View::Query);
    panel->Reload();
}

// ---------------------------------------------------------------------------
// User-management (用户组) list tab — reuses the generic ObjectListPanel with a
// user-specific column set + toolbar/context actions, exactly like the object-list
// tabs. cells[0] is the account "user@host" (split at the last '@' by the actions).

// Open a modal create/edit-user dialog on `e`'s live connection (any dialect that
// SupportsUserAdmin; the caller has already checked). startPage picks the tab
// (1 = the server-level privilege/role checklist, used by Privilege Manager).
void MainFrame::EditUserDialog(ConnEntry* e, const wxString& user, const wxString& host,
                               bool isNew, int startPage)
{
    if (!e || !e->IsConnected()) return;
    UserEditDialog dlg(this, e->conn.get(), user, host, isNew, startPage);
    dlg.ShowModal();
}

// Wire the user-list panel: loader (accounts via ListUsers) + toolbar/context
// actions (新用户/编辑/删除/复制/Privilege Manager/刷新), double-click = 编辑用户.
// Columns + account display adapt to the dialect's user-admin model (host-based
// dialects show user@host; the rest show a plain name).
void MainFrame::ConfigureUserList(ObjectListPanel* panel, ConnEntry* e)
{
    using Row    = ObjectListPanel::Row;
    using Action = ObjectListPanel::Action;
    ConnectionTree* ct = connTree_.get();

    const db::UserAdminModel model = e->conn->GetUserAdminModel();
    const bool hasHost = model.hasHost;

    panel->SetLoader([ct, e, hasHost](std::vector<Row>& out, wxString& err) -> bool {
        if (!ct->isLive(e)) { err = tr(L"连接已断开"); return false; }
        wxBusyCursor busy;
        std::vector<db::UserInfo> users;
        if (!e->conn->ListUsers(users, err)) return false;
        for (const auto& u : users)
            out.push_back({ { hasHost ? (u.name + L"@" + u.host) : u.name, u.authPlugin } });
        return true;
    });

    auto reload = [panel] { panel->Reload(); };
    auto split  = [hasHost](const wxString& acct, wxString& u, wxString& h) {
        if (!hasHost) { u = acct; h = wxString(); return; }
        const int at = acct.Find(L'@', /*fromEnd*/ true);
        if (at == wxNOT_FOUND) { u = acct; h = L"%"; }
        else { u = acct.Left(at); h = acct.Mid(at + 1); }
    };

    std::vector<Action> actions = {
        { tr(L"新用户"), icons::Glyph::Plus, theme::kGreen, false,
          [this, e, reload](const wxString&, const std::vector<wxString>&) {
              EditUserDialog(e, wxString(), L"%", /*isNew*/ true, 0); reload(); } },
        { tr(L"编辑用户"), icons::Glyph::Model, theme::kPrimary, true,
          [this, e, reload, split](const wxString& n, const std::vector<wxString>&) {
              if (n.IsEmpty()) return; wxString u, h; split(n, u, h);
              EditUserDialog(e, u, h, /*isNew*/ false, 0); reload(); } },
        { tr(L"删除用户"), icons::Glyph::Close, theme::kDotRed, true,
          [this, ct, e, reload, split](const wxString&, const std::vector<wxString>& all) {
              if (all.empty() || !ct->isLive(e)) return;
              if (wxMessageBox(wxString::Format(tr(L"确定删除选中的 %zu 个用户?此操作不可撤销。"),
                                                all.size()),
                               tr(L"删除用户"), wxYES_NO | wxICON_WARNING, this) != wxYES)
                  return;
              wxBusyCursor busy;
              for (const auto& acct : all) {
                  wxString u, h; split(acct, u, h); wxString err;
                  if (!e->conn->DropUser(u, h, err))
                      wxMessageBox(tr(L"删除失败: ") + acct + L"\n" + err,
                                   tr(L"删除用户"), wxOK | wxICON_ERROR, this);
              }
              reload(); } },
        { tr(L"复制"), icons::Glyph::Copy, theme::kAccent, true,
          [this, ct, e, reload, split, hasHost](const wxString& n, const std::vector<wxString>&) {
              if (n.IsEmpty() || !ct->isLive(e)) return;
              wxString su, sh; split(n, su, sh);
              const wxString prompt = hasHost
                  ? tr(L"新用户名 (主机沿用 ") + sh + L"):" : tr(L"新用户名:");
              const wxString nn = GetTextCentered(this, prompt, tr(L"复制用户"), su + L"_copy");
              if (nn.IsEmpty()) return;
              wxBusyCursor busy;
              db::UserGrants g; wxString err;
              if (!e->conn->GetUserGrants(su, sh, g, err)) {
                  wxMessageBox(err, tr(L"复制用户"), wxOK | wxICON_ERROR, this); return; }
              db::UserSpec spec;
              spec.name = nn; spec.host = sh;
              spec.globalPrivs = g.globalPrivs; spec.globalGrantOption = g.globalGrantOption;
              spec.dbPrivs = g.dbPrivs; spec.roles = g.roles;
              if (!e->conn->SaveUser(spec, /*isNew*/ true, err)) {
                  wxMessageBox(err, tr(L"复制用户"), wxOK | wxICON_ERROR, this); return; }
              reload(); } },
        { tr(L"Privilege Manager"), icons::Glyph::Key, theme::kDotPurple, true,
          [this, e, reload, split](const wxString& n, const std::vector<wxString>&) {
              if (n.IsEmpty()) return; wxString u, h; split(n, u, h);
              EditUserDialog(e, u, h, /*isNew*/ false, /*server checklist tab*/ 1); reload(); } },
        { tr(L"刷新"), icons::Glyph::Refresh, theme::kTextSecondary, false,
          [reload](const wxString&, const std::vector<wxString>&) { reload(); } },
    };

    const wxString nameCol = hasHost ? tr(L"用户 (User@Host)") : tr(L"用户");
    const wxString col2    = model.userListSecondLabel.IsEmpty()
                                 ? tr(L"认证插件") : tr(model.userListSecondLabel);
    panel->Setup({ { nameCol, 300 }, { col2, 220 } },
                 std::move(actions), /*double-click →*/ 1);   // 编辑用户
}

// Open (or focus) the user-management list tab for the active connection. One tab
// per connection (dedup). Requires an active connection whose driver supports user
// admin (MySQL / PostgreSQL / SQL Server / Oracle・达梦); engines without it get a
// "not supported" notice (no tab, no crash). SQLite has no user concept.
void MainFrame::OpenUserListTab()
{
    ConnEntry* e = connTree_->active();
    if (!e || !e->IsConnected()) {
        wxMessageBox(tr(L"请先连接一个数据库。"), tr(L"用户管理"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (!e->conn->SupportsUserAdmin()) {
        wxMessageBox(tr(L"当前数据库暂不支持用户管理。"), tr(L"用户管理"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    for (const auto& kv : userTabs_) {           // 去重:同一连接已有用户标签就聚焦它
        if (kv.second == e) {
            const int idx = editors_->GetPageIndex(kv.first);
            if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
            SwitchView(View::Query);
            return;
        }
    }

    auto* panel = new ObjectListPanel(editors_);
    ConfigureUserList(panel, e);
    editors_->AddPage(panel, tr(L"用户: ") + e->profile.name, /*select*/ true,
                      icons::Stroke(icons::Glyph::Users, 14, theme::kPrimary));
    const int idx = editors_->GetPageIndex(panel);
    if (idx != wxNOT_FOUND)
        editors_->SetPageToolTip(idx, e->profile.name + L" / " + tr(L"用户管理"));
    userTabs_[panel] = e;
    UpdateQueryView();
    SwitchView(View::Query);
    panel->Reload();
    if (viewBar_) viewBar_->Refresh();           // 用户组 段高亮
}

// Close EVERY tab that belongs to `e` (data / design / ER), before its connection is
// torn down. Leaving them open would either dangle (TableDesignView holds a raw conn_
// with no post-teardown guard → save/reload use-after-free) or show stale rows
// (TableDataPage). The fixed "总览" tab is shared and non-closable — it is never
// deleted here; its content is cleared separately (ClearDbOverview /
// ClearConnectionOverview in the entryClosing hook).
void MainFrame::CloseTabsForEntry(ConnEntry* e)
{
    if (!editors_) return;
    std::vector<wxWindow*> victims;   // collect first (don't mutate maps mid-iterate)
    for (const auto& kv : dataTabs_)    if (kv.second       == e) victims.push_back(kv.first);
    for (const auto& kv : designTabs_)  if (kv.second.entry == e) victims.push_back(kv.first);
    for (const auto& kv : erTabs_)      if (kv.second.first == e) victims.push_back(kv.first);
    for (const auto& kv : objListTabs_) if (kv.second.entry == e) victims.push_back(kv.first);
    for (const auto& kv : userTabs_)    if (kv.second       == e) victims.push_back(kv.first);
    if (victims.empty()) return;

    for (wxWindow* w : victims) {
        // 断连是强制关闭(连接即将销毁,无法保存也无法真正取消):仍对有未存改动的
        // 设计标签弹一次确认,让用户知道会丢改动——但无论选择都关(否则悬空崩溃)。
        if (auto* dv = dynamic_cast<TableDesignView*>(w)) dv->ConfirmClose();
        const int idx = editors_->GetPageIndex(w);
        if (idx != wxNOT_FOUND) editors_->DeletePage(idx);
        tabColors_.erase(w);
        dataTabs_.erase(w);
        designTabs_.erase(w);
        erTabs_.erase(w);
        objListTabs_.erase(w);
        userTabs_.erase(w);
    }
    UpdateQueryView();
    RefreshBottomBar();
}

} // namespace ui
