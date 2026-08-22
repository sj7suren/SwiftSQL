// MainFrame.h — SwiftSQL main window. The sidebar connection tree + all
// connection lifecycle live in ConnectionTree; MainFrame owns the frame chrome,
// the query/data editor, and the table-design / ER views. The tree delegates
// view-level actions (open table, new query, table design) back here via hooks.
#pragma once

#include <wx/frame.h>
#include <wx/gdicmn.h>   // wxRect
#include <wx/colour.h>   // wxColour (per-tab colour map)
#include <wx/timer.h>    // automation scheduler tick
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "db/DbDriver.h"   // db::IConnection, db::TableMeta (cached for the overview filter)
#include "ui/AiChatPanel.h"   // AiChatHost (MakeAiHost returns one by value)

class wxAuiNotebook;
class wxActivityIndicator;
class wxListCtrl;
class wxPanel;
class wxSearchCtrl;
class wxSimplebook;
class wxSplitterWindow;
class wxStaticBitmap;
class wxStaticText;

namespace ui {

class EditorPage;
class TableDataPage;
class ResultGridPanel;
class IQueryTab;
class TableDesignView;
class ErDiagramView;
class ConnectionTree;
class ScriptLibraryPanel;
class ObjectListPanel;
struct ConnEntry;

// Which kind of database object a toolbar-launched object-list tab lists. Each maps
// to a distinct set of columns + toolbar actions in ConfigureObjectList.
enum class ObjectListKind { Tables, Views, Functions, Procedures };

// mainBook_ now holds a single page: Query (the editors_ notebook — SQL editors,
// data tabs, design tabs, ER tabs, and the fixed 总览 tab all live here). The 总览
// tab itself is a context-following container (overviewStack_) whose slots are the
// per-connection database grid and the per-database table grid. Design/ER/Overview/
// Database are legacy enum values kept for source compatibility but no longer map to
// a distinct mainBook_ page (SwitchView always selects page 0). The old view-switch
// pill is retired.
enum class View { Query = 0, Design = 1, ER = 2, Overview = 3, Database = 4 };

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    // ---- construction ----
#ifdef __WXMSW__
    // Clamp the frameless-window maximize box to the monitor work area so the
    // invisible WS_THICKFRAME resize border (~7px) does not overflow the screen
    // (which cut the header top / pushed the caption buttons off, and covered
    // the taskbar).
    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

    void BeginWindowDrag();                     // hand a press on empty chrome to the OS as a caption drag (Aero Snap)
    void BuildMenuBar();                        // builds the wxMenu objects + accelerators (no native bar)
    wxWindow* BuildHeaderBar(wxWindow* parent); // row 1: centred view-switch pill + logo
    wxWindow* BuildMenuRow(wxWindow* parent);   // row 2: custom clickable 文件/编辑/… menus
    wxWindow* BuildToolRow(wxWindow* parent);   // row 3: tool buttons + AI (replaces wxToolBar)
    wxWindow* BuildSidebar(wxWindow* parent);
    wxWindow* BuildMainArea(wxWindow* parent);
    wxWindow* BuildOverviewView(wxWindow* parent);   // DB-list grid + info panel (slot 0 of the overview tab)
    void ShowConnectionOverview(ConnEntry* e);       // fixed tab → connection db-list, bring to front
    void ClearConnectionOverview();                  // reset the connection db-list slot (its connection was closed)
    wxWindow* BuildDatabaseView(wxWindow* parent);   // table-list grid + db-info panel (slot 1 of the overview tab)
    wxWindow* BuildConnGroupView(wxWindow* parent);  // 分组 connection list + group info (slot 2)
    wxWindow* EnsureDbOverviewTab();                 // lazily create the shared, un-closable "总览" tab in editors_
    void SetOverviewSlot(int slot);                  // flip the fixed tab's content (0=conn db-list, 1=table-list) + relabel
    void SelectDbOverviewTab();                      // select the 总览 tab + switch to the query view
    void ClearDbOverview();                          // reset the table-list slot (its connection was closed)
    void ShowDatabaseOverview(ConnEntry* e, const wxString& db, bool force = false);  // async load + switch
    // Same grid, restricted to one 分组's members — clicking a 表 group folder in
    // the sidebar. Reuses the database's cached metadata (no extra query when the
    // database is already shown), so this is a FILTER, not a second view.
    // Clicking a database afterwards clears the filter and shows the whole
    // database again; that round trip is the whole point of the gesture.
    void ShowTableGroupOverview(ConnEntry* e, const wxString& db, const wxString& group);
    // Slot 2: the connections inside one connection 分组.
    void ShowConnGroupOverview(const wxString& group);
    void RenderConnGroupList();                        // (re)populate slot 2's list
    std::vector<wxString> ConnGroupSelectedNames() const;
    void RenderTableList(const wxString& filter);    // (re)populate the grid, name+group filtered
    // The fixed tab's caption for what it currently shows: 总览 / 表信息 / 分组：X.
    void RefreshOverviewTabTitle();
    // Reload every surface that lists TABLES for (e, db) after a script created /
    // dropped / altered one. GUI thread only; each half no-ops when it doesn't apply.
    void RefreshAfterTableDdl(ConnEntry* e, const wxString& db);
    void StretchTableColumns();                      // last column fills the remaining width
    std::vector<wxString> OverviewSelectedTables() const;  // names of selected rows
    void OverviewOpenTable();                        // open the (first) selected table
    void OverviewNewTable();                         // open a fresh 新建表 designer tab
    void OverviewDesignTable();                      // design the (first) selected table
    void OverviewDeleteTables();                     // drop all selected tables (confirm) + refresh
    void JoinDbWorker();                             // cancel + join the overview loader
    void JoinCountWorker();                          // cancel + join the pager row-counter
    void MaybeAutoConnect();

    // ---- views ----
    void PaintViewSwitch();                // custom pill segmented control (SQL 编辑器 / 表设计 / ER 图)
    int  ActivePillSegment() const;        // which pill segment matches the active editors_ tab (0/1/2, -1 none)
    void ActivatePillSegment(int i);       // pill click → open/focus the matching tab (not a view switch)
    wxWindow* BuildEmptyState(wxWindow* parent);  // white placeholder shown when no query tabs
    void UpdateQueryView();                // toggle editors_ ↔ placeholder by tab count
    void RefreshEditorTabBar();            // force the editors_ tab strip (child wxAuiTabCtrl) to repaint
    void SwitchView(View v);
    EditorPage* ActivePage() const;        // active SQL editor tab (nullptr for a data tab)
    IQueryTab*  ActiveTab() const;         // active tab as a runnable query surface
    EditorPage* EnsurePage();              // active page, creating one if there are none
    void JoinWorker(bool cancelInFlight = false);

    // ---- events ----
    void OnNewConnection(wxCommandEvent&);
    void OnNewQuery(wxCommandEvent&);
    // ---- saved-SQL script library (MainFrame_Scripts.cpp) ----
    void OpenScriptLibrary();                   // top-bar entry → open/focus the library tab
    wxWindow* EnsureScriptLibraryTab();         // lazily create the reusable singleton library tab
    void OpenScriptInEditor(const wxString& name); // load a script into a fresh SQL editor tab
    void RefreshScriptLibrary();                // re-list the library grid if the tab is open
    // ---- automation jobs (MainFrame_Automation.cpp) ----
    void OpenAutomationTab();                    // top-bar Automation button → open/focus the list tab
    wxWindow* EnsureAutomationTab();             // lazily create the reusable singleton automation tab
    // ---- AI placeholder ("功能建设中") tab (MainFrame_Tabs.cpp) ----
    void OpenAiPlaceholderTab();                 // AI pill segment / AI 生成 SQL button → open/focus the placeholder tab
    wxWindow* EnsureAiPlaceholderTab();          // lazily create the reusable singleton "AI 功能建设中" tab
    void ConfigureAutomationList(ObjectListPanel* panel);  // wire columns / actions / loader
    void ReloadAutomation();                     // re-list the automation grid if the tab is open
    void AutomationNew();                        // 新建 job dialog → persist → reload
    void AutomationEdit(const wxString& name);   // 修改 job dialog → persist → reload
    void AutomationDelete(const wxString& name); // 删除 (confirm) → reload
    void AutomationRun(const wxString& name, bool interactive);      // 开始: run headless now
    void AutomationSetSchedule(const wxString& name);   // 设定自动计划 dialog
    void AutomationClearSchedule(const wxString& name); // 删除自动计划
    void StartAutomationTimer();                 // start the once-a-minute scheduler tick
    void OnAutomationTick(wxTimerEvent&);        // due-job check (runs on the GUI thread)
    // Ctrl+S save hook wired onto every plain SQL editor: prompt (first save),
    // write via ScriptStore with the active connection user as owner/exec, refresh
    // the library, rename the tab. Returns the saved name ("" = cancelled/failed).
    wxString SaveEditorScript(EditorPage* page, const wxString& fullSql,
                              const wxString& curName);
    void ConfigurePage(EditorPage* page);      // wire completion (SQL editor) + grid handlers
    // The connection/database/knowledge-base half of an AiChatHost, shared by the
    // AI tab and every editor's own right-side panel. Its SQL-routing callbacks
    // target the active editor; EditorPage::SetAiHost replaces them so a docked
    // panel writes into the editor it lives in.
    AiChatHost MakeAiHost();
    void ConfigureGrid(ResultGridPanel* grid); // wire apply/count/detail handlers on a result grid
    void OpenTableTab(ConnEntry* e, const wxString& db, const wxString& table); // dedicated data tab
    TableDesignView* OpenDesignTab(ConnEntry* e, const wxString& db, const wxString& table); // per-table closable design tab (dedup); returns its view
    void OpenNewTableTab(ConnEntry* e, const wxString& db);   // fresh 新建表 (CREATE) designer tab
    void OpenErTab(ConnEntry* e, const wxString& db);   // per-(conn,db) closable ER-diagram tab (dedup)
    void OpenQueryBuilder();                            // visual query-builder tab (multi-open); auto-closed with its connection
    void OpenObjectListTab(ObjectListKind kind);        // toolbar 表/视图/函数/存储过程 → object-list tab on the active conn+db (dedup)
    void ConfigureObjectList(ObjectListPanel* panel, ConnEntry* e, const wxString& db,
                             ObjectListKind kind);      // wire the columns / actions / loader for a kind
    void OpenUserListTab();                             // 用户组 pill → user-management list tab for the active conn (dedup)
    void ConfigureUserList(ObjectListPanel* panel, ConnEntry* e);  // wire the columns/actions/loader for the user list
    void EditUserDialog(ConnEntry* e, const wxString& user, const wxString& host,
                        bool isNew, int startPage);     // multi-tab create/edit-user dialog
    void CloseTabsForEntry(ConnEntry* e);               // close this entry's design/ER tabs before its conn dies (avoids dangling conn_)
    void CountRowsForPager(const wxString& table, const wxString& where,
                           wxWindow* owner,
                           std::function<void(long long)> done);   // data-grid pager (async)
    void OnRunQuery(wxCommandEvent&);
    void OnExplain(wxCommandEvent&);
    void RunSql(IQueryTab* tab, const wxString& sql);     // async multi-statement run
    void RunTxn(const wxString& sql, const wxString& okMsg);   // BEGIN/COMMIT/ROLLBACK
    void UpdateRunStopButtons();                               // enable/disable 运行/停止 by run state
    void RefreshBottomBar();                              // data-browser rows/db/SQL in the status bar
    void ApplyDataChanges(const wxString& dml);           // apply in-grid edits + autocommit
    void ApplyDataChangesInTxn(const wxString& dml);      // apply inside an open txn (no commit)
    bool ExecuteDml(const wxString& dml, wxString& err);  // atomic txn, no UI
    void OnAbout(wxCommandEvent&);
    void OnQuit(wxCommandEvent&);
    void OnPreferences(wxCommandEvent&);
    void OnFavorite(wxCommandEvent&);      // save current query to favourites
    void OnOpenSqlFile(wxCommandEvent&);   // 文件 ▸ 打开 SQL 文件 → new editor tab
    void OnSaveSqlFile(wxCommandEvent&);   // 文件 ▸ 保存 → write active editor buffer to .sql
    void OnToolImport(wxCommandEvent&);    // 工具 ▸ 导入向导 → run a .sql into the active db
    void OnToolExport(wxCommandEvent&);    // 工具 ▸ 导出向导 → dump active db (structure+data)
    void OnToolBackup(wxCommandEvent&);    // 工具 ▸ 备份 → full structure+data dump
    void OnMenuOpen(wxMenuEvent&);         // rebuild the favourites submenu
    void RebuildFavorites();

    // ---- custom menu-bar keyboard navigation ----
    // The menu row is a strip of buttons (no native wxMenuBar), so Alt+mnemonic
    // and ← / → menu-walking are wired by hand. topMenus_ pairs each top label
    // with its wxMenu and Alt mnemonic (F/E/V/B/T/W/H), populated in BuildMenuRow.
    struct TopMenu { wxWindow* btn; wxMenu* menu; wchar_t mnemonic; };
    void OpenTopMenu(int index);           // pop up menu #index; ← / → walk neighbours
    int  TopMenuForMnemonic(wchar_t upper) const;   // index for an Alt+letter, or -1
    void OnMenuNavCharHook(wxKeyEvent&);   // Alt+letter opens the matching top menu

    // ---- state ----
    std::unique_ptr<ConnectionTree> connTree_;
    std::thread       worker_;
    std::atomic<bool> queryRunning_{ false };
    db::IConnection*  workerConn_ = nullptr;   // conn the SQL worker_ holds (for join-on-disconnect),
                                               // mirrors dbLoadConn_/countConn_; main-thread only
    bool              bottomBarBrowser_ = false;   // status bar currently in 4-field browser mode
    wxString          selTable_;
    View              view_ = View::Query;

    // ---- widgets ----
    wxSplitterWindow* splitter_ = nullptr;
    // Menus are no longer attached to a native wxMenuBar; they are popped up from
    // the custom menu row (BuildMenuRow) and owned by the frame (deleted in dtor).
    wxMenu*           fileMenu_ = nullptr;
    wxMenu*           editMenu_ = nullptr;
    wxMenu*           viewMenu_ = nullptr;
    wxMenu*           favMenu_ = nullptr;   // 收藏 menu (dynamic favourites)
    wxMenu*           toolsMenu_ = nullptr;
    wxMenu*           windowMenu_ = nullptr;
    wxMenu*           helpMenu_ = nullptr;
    wxSimplebook*     mainBook_ = nullptr;
    wxSimplebook*     queryStack_ = nullptr;        // query slot: [0]=editors_, [1]=placeholder
    wxWindow*         placeholder_ = nullptr;       // empty-state panel (white + illustration)
    wxAuiNotebook*    editors_ = nullptr;
    // Design / ER are per-tab now (OpenDesignTab / OpenErTab), created into editors_
    // on demand — no shared singletons. These maps register each such tab for dedupe
    // (focus the existing one instead of opening a duplicate), for cleanup on close,
    // and for closing an entry's tabs when its connection is torn down (else the tab
    // keeps a dangling conn_ → save/reload crashes). The dedupe key carries the
    // ConnEntry* so same-named tables on two connections don't collide.
    struct DesignRef { ConnEntry* entry; wxString db; wxString table; };
    std::map<wxWindow*, DesignRef>                       designTabs_;  // page → (conn, db, table)
    std::map<wxWindow*, std::pair<ConnEntry*, wxString>> erTabs_;      // page → (conn entry, db)
    // Data tabs (TableDataPage from OpenTableTab) register their owning connection so
    // CloseTabsForEntry can close them when that connection is torn down — otherwise a
    // closed connection leaves data tabs showing stale rows.
    std::map<wxWindow*, ConnEntry*>                      dataTabs_;    // page → owning conn
    // Object-list tabs (ObjectListPanel from OpenObjectListTab). Keyed like designTabs_
    // by (conn, db, kind) for dedupe (re-focus instead of duplicating) and closed with
    // their connection in CloseTabsForEntry (the tab holds no conn, but its action
    // closures capture the ConnEntry* — close it before the connection is freed).
    struct ObjListRef { ConnEntry* entry; wxString db; ObjectListKind kind; };
    std::map<wxWindow*, ObjListRef>                      objListTabs_; // page → (conn, db, kind)
    // User-management list tabs (ObjectListPanel from OpenUserListTab). One per
    // connection (dedup); closed with their connection in CloseTabsForEntry, and
    // identify the 用户组 pill segment's highlight (ActivePillSegment).
    std::map<wxWindow*, ConnEntry*>                      userTabs_;    // page → owning conn
    // The saved-SQL script library tab — a reusable singleton (like the 总览 tab),
    // but closable. scriptLibTab_ is the page identity for dedupe / close cleanup;
    // scriptLib_ is the same object typed for Reload(). Both nulled on close.
    wxWindow*            scriptLibTab_ = nullptr;
    ScriptLibraryPanel*  scriptLib_    = nullptr;
    // The Automation job list tab — a reusable singleton like the script library
    // (closable; both pointers nulled on close). The scheduler timer ticks once a
    // minute for the whole app lifetime (independent of the tab being open) and
    // runs due jobs via CallAfter; runningJobs_ guards against a job being launched
    // twice (a still-queued run must not be re-scheduled by the next tick).
    wxWindow*            automationTab_  = nullptr;
    ObjectListPanel*     automationList_ = nullptr;
    // The "AI 功能建设中" placeholder tab — a reusable singleton (closable; nulled on
    // close). Identifies the AI pill segment's highlight (ActivePillSegment → 2).
    wxWindow*            aiTab_          = nullptr;
    wxTimer              automationTimer_;
    std::set<wxString>   runningJobs_;
    // The fixed, un-closable "总览" tab (index 0 of editors_) is a context-following
    // container: overviewStack_ is a 3-page wxSimplebook whose slots are
    // overviewView_ (connection database list), databaseView_ (database table
    // list) and connGroupView_ (one 分组's connections). ShowConnectionOverview /
    // ShowDatabaseOverview / ShowConnGroupOverview flip the slot; the tab
    // itself never closes. overviewTab_ mirrors overviewStack_ as a plain wxWindow*
    // so the tab-art special-styling / close-veto identity checks (which need a
    // stable wxWindow* const*) keep working through lazy creation.
    wxSimplebook*     overviewStack_ = nullptr;
    wxWindow*         overviewTab_   = nullptr;   // == overviewStack_; fixed-tab identity
    // Connection overview: left = per-engine database grid, right = info panel.
    wxWindow*         overviewView_ = nullptr;
    wxListCtrl*       overviewList_ = nullptr;
    ConnEntry*        ovEntry_ = nullptr;           // connection the db-list slot is showing (clear-on-disconnect)
    wxStaticBitmap*   ovBrand_ = nullptr;           // engine brand logo atop the info panel
    wxStaticText*     ovName_ = nullptr;
    wxStaticText*     ovEngine_ = nullptr;
    wxStaticText*     ovVersion_ = nullptr;
    wxStaticText*     ovHost_ = nullptr;
    wxStaticText*     ovPort_ = nullptr;
    wxStaticText*     ovStatus_ = nullptr;
    // Connection-group view (slot 2): the connections inside one 分组. Its own
    // view rather than a filter over anything existing, because a connection has
    // none of the columns a table grid is made of.
    wxWindow*         connGroupView_ = nullptr;
    wxListCtrl*       connGroupList_ = nullptr;
    wxStaticText*     cgName_  = nullptr;
    wxStaticText*     cgCount_ = nullptr;
    wxString          cgroupName_;                  // 分组 the slot is showing ("" = none)
    // Database overview: left = table-metadata grid (async), right = db-info panel.
    wxWindow*            databaseView_ = nullptr;
    wxListCtrl*          tableList_ = nullptr;
    wxActivityIndicator* tableSpinner_ = nullptr;   // animated while the metadata loads
    wxStaticText*        dbovName_ = nullptr;
    wxStaticText*        dbovCharset_ = nullptr;
    wxStaticText*        dbovCollation_ = nullptr;
    wxStaticText*        dbovTables_ = nullptr;
    wxStaticText*        dbovSize_ = nullptr;
    wxSearchCtrl*        dbovSearch_ = nullptr;      // toolbar filter over the table grid
    ConnEntry*           dbovEntry_ = nullptr;       // connection the overview is showing
    wxString             dbovDb_;                    // database the overview is showing
    // Active 分组 filter over the table grid ("" = the whole database, the
    // original behaviour). Held rather than derived because the grid must keep
    // showing one group across a re-render, a search-box keystroke and a reload.
    wxString             dbovGroup_;
    std::vector<db::TableMeta> dbovMetas_;           // cached rows, re-rendered on filter
    std::thread          dbWorker_;                 // overview metadata loader (off the GUI thread)
    db::IConnection*     dbLoadConn_ = nullptr;      // connection the loader is querying (for Cancel)
    int                  dbLoadGen_ = 0;             // bumps per load; stale CallAfter results are dropped
    std::thread          countWorker_;              // pager COUNT(*) off the GUI thread (mirrors dbWorker_)
    db::IConnection*     countConn_ = nullptr;       // connection the counter is querying (for Cancel)
    int                  countGen_ = 0;              // bumps per count; stale CallAfter results are dropped
    wxWindow*         viewBar_ = nullptr;          // custom top bar; paints the centred view-switch pill
    std::array<wxRect, 3> tabRects_{};             // hit-test rects for the pill segments
    std::map<wxWindow*, wxColour> tabColors_;      // per-tab colour (editors_ tab right-click 设置颜色)
    std::vector<TopMenu> topMenus_;                // top-label ↔ wxMenu ↔ Alt mnemonic
};

} // namespace ui
