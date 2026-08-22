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
#include <wx/ffile.h>
#include <wx/filedlg.h>
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
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/PreferencesDialog.h"
#include "ui/ServerMonitorDialog.h"
#include "db/ProcessMonitor.h"
#include "ui/TableDesignView.h"
#include "ui/NewTableView.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/MainFrameInternal.h"

namespace ui {
namespace {

// ID_TX_BEGIN / ID_TX_COMMIT / ID_TX_ROLLBACK now live in EditorPage.h (shared),
// so the data-grid toolbar's txn buttons bubble up here the same way run/stop do.
// ID_STOP_QUERY lives in EditorPage.h (shared with the editor strip).
// ID_RUN_QUERY / ID_FORMAT_SQL / ID_EXPLAIN_SQL live in EditorPage.h (shared).

#ifdef __WXMSW__
// ---- custom menu-bar keyboard navigation (← / → between open menus) ----------
// A native wxMenu popup (Win32 TrackPopupMenu) runs its own modal message loop,
// so our frame-level CHAR_HOOK never sees keys while a menu is open. To give the
// custom menu row the ← / → "walk to the adjacent menu" behaviour of a real
// wxMenuBar, we install a thread-local WH_KEYBOARD hook only while a top menu is
// up: on Left/Right it records the direction, calls EndMenu() to dismiss the
// current popup (so PopupMenu returns), and OpenTopMenu re-opens the neighbour.
static HHOOK g_menuNavHook = nullptr;
static int   g_menuNavDir  = 0;    // 0 = closed normally; -1 = ←, +1 = →

static LRESULT CALLBACK MenuNavHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && !(lParam & 0x80000000)) {   // bit31=0 → key DOWN
        if (wParam == VK_LEFT || wParam == VK_RIGHT) {
            g_menuNavDir = (wParam == VK_RIGHT) ? +1 : -1;
            ::EndMenu();       // close the active popup; the PopupMenu call returns
            return 1;          // swallow so the menu itself doesn't also act on it
        }
    }
    return ::CallNextHookEx(g_menuNavHook, code, wParam, lParam);
}

static void InstallMenuNavHook()
{
    g_menuNavDir = 0;
    if (!g_menuNavHook)
        g_menuNavHook = ::SetWindowsHookExW(WH_KEYBOARD, MenuNavHookProc,
                                            nullptr, ::GetCurrentThreadId());
}

static void RemoveMenuNavHook()
{
    if (g_menuNavHook) { ::UnhookWindowsHookEx(g_menuNavHook); g_menuNavHook = nullptr; }
}
#endif // __WXMSW__

const wchar_t* kWelcomeSql =
    L"-- SwiftSQL:连接数据库后,双击左侧表名生成查询,或直接书写 SQL\n"
    L"-- Ctrl+N 新建连接 · 选中文本可只执行选中部分\n"
    L"SELECT 1 AS hello;\n";

} // namespace

// ===========================================================================
MainFrame::MainFrame()
    // Frameless: drop wxCAPTION (removes the OS title bar AND its system buttons —
    // we redraw those ourselves in the header). Keep wxRESIZE_BORDER + the box
    // flags so edge-resize, Aero Snap and the taskbar entry all keep working.
    : wxFrame(nullptr, wxID_ANY, L"SwiftSQL",
              wxDefaultPosition, wxSize(1440, 920),
              (wxDEFAULT_FRAME_STYLE & ~wxCAPTION) | wxCLIP_CHILDREN)
{
    wxIcon appIcon;
    appIcon.CopyFromBitmap(icons::AppLogo(32));
    SetIcon(appIcon);

#ifdef __WXMSW__
    // Collapse the DWM window-frame extension so DWM stops rendering the top
    // non-client border band. Re-asserted on WM_ACTIVATE (see MSWWindowProc).
    {
        MARGINS m = { 0, 0, 0, 0 };
        ::DwmExtendFrameIntoClientArea((HWND)GetHandle(), &m);
    }
#endif

    BuildMenuBar();   // creates the wxMenu objects + accelerators (no native bar)

    // Three stacked full-width rows above the body, mirroring the design's
    // fully-custom top chrome (no native menu bar):
    //   header row = SwiftSQL logo + centred view-switch pill + search  (ABOVE menus)
    //   menu  row  = 文件/编辑/查看/收藏/工具/窗口/帮助 (custom, pop up wxMenus)
    //   tool  row  = tool buttons (left) + AI 生成 SQL (right)
    // The pill lives ALONE in the header row (no tools/menus beside it), so it
    // centres cleanly against the whole window and can never be pushed aside.
    wxWindow* header  = BuildHeaderBar(this);   // sets viewBar_; paints the pill
    wxWindow* menuRow = BuildMenuRow(this);
    wxWindow* toolRow = BuildToolRow(this);

    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize,
                                     wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxBORDER_NONE);
    wxWindow* sidebar = BuildSidebar(splitter_);
    wxWindow* main = BuildMainArea(splitter_);
    splitter_->SplitVertically(sidebar, main, 288);
    splitter_->SetMinimumPaneSize(220);

    // header (top) → toolbar → split body. A frame sizer is required so the
    // three children stack instead of overlapping (a frame auto-fills one child).
    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(header,    0, wxEXPAND);
    frameSizer->Add(menuRow,   0, wxEXPAND);
    frameSizer->Add(toolRow,   0, wxEXPAND);
    frameSizer->Add(splitter_, 1, wxEXPAND);
    SetSizer(frameSizer);

    CreateStatusBar();
    SetStatusText(tr(L"就绪 — 点击工具栏「新建连接」开始"));

    connTree_->LoadSavedConnections();

    StartAutomationTimer();   // in-app scheduler: run due automation jobs once a minute

    Centre();
    CallAfter([this]() {
        MaybeAutoConnect();
        // Scriptable: open the connection manager on startup (verification hook).
        if (wxGetEnv(L"SWIFTSQL_AUTODIALOG", nullptr)) {
            connTree_->NewConnection(db::DbType::MySQL);
        }
        // Scriptable: pop the new-connection type menu (verification hook).
        if (wxGetEnv(L"SWIFTSQL_AUTOMENU", nullptr)) {
            connTree_->ShowNewConnectionMenu();
        }
        // Scriptable: open Preferences (verification hook).
        if (wxGetEnv(L"SWIFTSQL_AUTOPREFS", nullptr)) {
            wxCommandEvent e;
            OnPreferences(e);
        }
    });
}

MainFrame::~MainFrame()
{
    automationTimer_.Stop();   // no scheduler ticks during teardown
    JoinWorker(true);
    JoinDbWorker();
    JoinCountWorker();
    // These menus are not owned by a native wxMenuBar, so delete them ourselves.
    delete fileMenu_;
    delete editMenu_;
    delete viewMenu_;
    delete favMenu_;
    delete toolsMenu_;
    delete windowMenu_;
    delete helpMenu_;
}

#ifdef __WXMSW__
// Frameless + WS_THICKFRAME windows maximize to the physical monitor bounds,
// letting the invisible ~7px sizing border spill past every edge (header top
// clipped, caption buttons pushed off-right, bottom over the taskbar). Pin the
// maximize position/size to the work area of the monitor the window is on.
WXLRESULT MainFrame::MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam)
{
    HWND hwnd = (HWND)GetHandle();

    // --- Kill the DWM-rendered top non-client border band (the ~5px grey line
    // only visible via the DWM composited layer, repainted whenever the window is
    // activated / uncovered — the "flash then grey line" repro). Two levers:
    //   • WM_NCACTIVATE with lParam = -1 tells DefWindowProc NOT to repaint the
    //     non-client border on focus changes → the band never gets redrawn.
    //   • WM_ACTIVATE re-asserts the DWM frame extension so the collapse sticks.
    if (msg == WM_NCACTIVATE) {
        return ::DefWindowProc(hwnd, WM_NCACTIVATE, wParam, static_cast<LPARAM>(-1));
    }
    if (msg == WM_ACTIVATE) {
        MARGINS m = { 0, 0, 0, 0 };
        ::DwmExtendFrameIntoClientArea(hwnd, &m);
        // fall through to default handling below
    }

    // --- Kill the DWM top border line of the captionless WS_THICKFRAME window ---
    // The 1px line is the top non-client edge. We reclaim JUST the top strip into
    // the client area (client top = window top → no line), and deliberately KEEP
    // the left/right/bottom sizing borders. That matters for our layout: the
    // header/menu/tool rows are child wxPanels; native edge-resize lives in the
    // NON-client border AROUND them. If we zeroed all four insets (client=window)
    // the panels would cover every edge and — because a child HWND swallows the
    // mouse before the frame's WM_NCHITTEST is consulted — native resize would be
    // lost on all sides. Keeping L/R/B borders preserves native resize with no
    // NCHITTEST juggling; only the top edge (rarely used) is given up.
    if (msg == WM_NCCALCSIZE && wParam == TRUE) {
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        if (::IsZoomed(hwnd)) {
            // Maximized: make the client exactly the monitor work area so nothing
            // spills over the taskbar and the top/right stay flush (no clip).
            HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (::GetMonitorInfo(mon, &mi)) p->rgrc[0] = mi.rcWork;
            return 0;
        }
        // Restored: let the default proc compute the standard client rect (borders
        // on all four sides), then pull the top edge back up to the window top.
        WXLRESULT r = wxFrame::MSWWindowProc(msg, wParam, lParam);
        const int cyTop = ::GetSystemMetrics(SM_CYSIZEFRAME) +
                          ::GetSystemMetrics(SM_CXPADDEDBORDER);
        p->rgrc[0].top -= cyTop;
        return r;
    }

    // Double insurance for maximize sizing (also clamped in WM_NCCALCSIZE above).
    if (msg == WM_GETMINMAXINFO) {
        HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (::GetMonitorInfo(mon, &mi)) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            // ptMaxPosition/ptMaxSize are relative to the monitor origin.
            mmi->ptMaxPosition.x  = mi.rcWork.left   - mi.rcMonitor.left;
            mmi->ptMaxPosition.y  = mi.rcWork.top    - mi.rcMonitor.top;
            mmi->ptMaxSize.x      = mi.rcWork.right  - mi.rcWork.left;
            mmi->ptMaxSize.y      = mi.rcWork.bottom - mi.rcWork.top;
            mmi->ptMaxTrackSize.x = mmi->ptMaxSize.x;
            mmi->ptMaxTrackSize.y = mmi->ptMaxSize.y;
        }
        return 0;
    }

    return wxFrame::MSWWindowProc(msg, wParam, lParam);
}
#endif

// ---------------------------------------------------------------------------
// Builds the wxMenu objects (kept as members) and their command bindings, but
// does NOT create a native wxMenuBar — the design puts the view switch ABOVE the
// menus, which is impossible with a native menu bar (it is welded under the OS
// title bar). Instead these menus are popped up from the custom menu row
// (BuildMenuRow). Accelerators that the native bar used to provide are restored
// via a frame-level wxAcceleratorTable.
void MainFrame::BuildMenuBar()
{
    fileMenu_ = new wxMenu;
    fileMenu_->Append(ID_NEW_CONNECTION, tr(L"新建连接(&N)\tCtrl+N"));
    fileMenu_->Append(ID_NEW_QUERY, tr(L"新建SQL(&Q)\tCtrl+T"));
    fileMenu_->AppendSeparator();
    fileMenu_->Append(wxID_OPEN, tr(L"打开 SQL 文件(&O)\tCtrl+O"));
    fileMenu_->Append(wxID_SAVE, tr(L"保存(&S)\tCtrl+S"));
    fileMenu_->AppendSeparator();
    fileMenu_->Append(wxID_EXIT, tr(L"退出(&X)"));

    editMenu_ = new wxMenu;
    editMenu_->Append(wxID_UNDO, tr(L"撤销(&U)\tCtrl+Z"));
    editMenu_->Append(wxID_REDO, tr(L"重做(&R)\tCtrl+Y"));
    editMenu_->AppendSeparator();
    editMenu_->Append(wxID_CUT, tr(L"剪切(&T)\tCtrl+X"));
    editMenu_->Append(wxID_COPY, tr(L"复制(&C)\tCtrl+C"));
    editMenu_->Append(wxID_PASTE, tr(L"粘贴(&P)\tCtrl+V"));

    // 查看: focus the SQL editor, then run / stop the active query (表设计 / ER 图
    // entries were removed — those tabs still open from the connection tree / overview).
    viewMenu_ = new wxMenu;
    viewMenu_->Append(ID_VIEW_QUERY, tr(L"SQL 编辑器"));
    viewMenu_->AppendSeparator();
    viewMenu_->Append(ID_RUN_QUERY, tr(L"运行\tF5"));
    viewMenu_->Append(ID_STOP_QUERY, tr(L"停止"));

    favMenu_ = new wxMenu;
    favMenu_->Append(wxID_ADD, tr(L"收藏当前查询"));
    favMenu_->AppendSeparator();

    toolsMenu_ = new wxMenu;
    toolsMenu_->Append(ID_TOOL_IMPORT, tr(L"导入向导"));
    toolsMenu_->Append(ID_TOOL_EXPORT, tr(L"导出向导"));
    toolsMenu_->Append(ID_TOOL_BACKUP, tr(L"备份"));
    toolsMenu_->AppendSeparator();
    // 同步到… flattened to first-level items (was a ▶ submenu) — active database as
    // source; the bindings (OpenSyncForActive 0/1/2) are unchanged.
    toolsMenu_->Append(ID_TOOL_SYNC_STRUCT, tr(L"结构同步"));
    toolsMenu_->Append(ID_TOOL_SYNC_DATA,   tr(L"数据同步"));
    toolsMenu_->Append(ID_TOOL_SYNC_BOTH,   tr(L"结构和数据同步"));
    toolsMenu_->AppendSeparator();
    toolsMenu_->Append(ID_TOOL_SERVER_MONITOR, tr(L"服务器监控"));
    toolsMenu_->AppendSeparator();
    toolsMenu_->Append(ID_PREFERENCES, tr(L"偏好设置"));

    windowMenu_ = new wxMenu;
    windowMenu_->Append(ID_NEXT_TAB, tr(L"下一个标签页\tCtrl+Tab"));

    helpMenu_ = new wxMenu;
    helpMenu_->Append(wxID_ABOUT, tr(L"关于 SwiftSQL(&A)"));

    // Frame-level accelerators. Only the genuinely frame-scoped commands are
    // registered (新建连接 / 新建SQL); the editor's own Ctrl+Z/X/C/V/S are
    // handled internally by wxSTC, so we deliberately do NOT intercept them here
    // (that would steal keys from the focused editor).
    wxAcceleratorEntry accels[4];
    accels[0].Set(wxACCEL_CTRL, (int)'N', ID_NEW_CONNECTION);
    accels[1].Set(wxACCEL_CTRL, (int)'T', ID_NEW_QUERY);
    accels[2].Set(wxACCEL_NORMAL, WXK_F5, ID_RUN_QUERY);   // F5 → run the active query
    accels[3].Set(wxACCEL_CTRL, WXK_TAB, ID_NEXT_TAB);     // Ctrl+Tab → next editor tab
    SetAcceleratorTable(wxAcceleratorTable(4, accels));

    Bind(wxEVT_MENU, &MainFrame::OnNewConnection, this, ID_NEW_CONNECTION);
    Bind(wxEVT_MENU, &MainFrame::OnNewQuery, this, ID_NEW_QUERY);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::OnQuit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnPreferences, this, ID_PREFERENCES);
    // Tools ▸ 同步到… — active database as source (0 struct / 1 data / 2 both).
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (connTree_) connTree_->OpenSyncForActive(0); }, ID_TOOL_SYNC_STRUCT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (connTree_) connTree_->OpenSyncForActive(1); }, ID_TOOL_SYNC_DATA);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (connTree_) connTree_->OpenSyncForActive(2); }, ID_TOOL_SYNC_BOTH);
    // 工具 ▸ 服务器监控 — live session list for the ACTIVE connection. Engines that
    // can't enumerate sessions (SQLite) report SupportsProcessList()==false, so we
    // show a friendly notice instead of an empty grid.
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        ConnEntry* e = connTree_ ? connTree_->active() : nullptr;
        if (!e || !e->IsConnected() || !e->conn) {
            SetStatusText(tr(L"请先连接数据库"));
            return;
        }
        if (!db::SupportsProcessList(e->conn->GetDialect())) {
            wxMessageBox(tr(L"当前数据库暂不支持会话监控"), tr(L"服务器监控"),
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        ServerMonitorDialog dlg(this, e->conn.get(), e->profile.name);
        dlg.ShowModal();
    }, ID_TOOL_SERVER_MONITOR);
    Bind(wxEVT_MENU, &MainFrame::OnFavorite, this, wxID_ADD);
    Bind(wxEVT_MENU_OPEN, &MainFrame::OnMenuOpen, this);
    // Alt+F/E/V/B/T/W/H opens the matching custom top menu (native menu-bar feel).
    Bind(wxEVT_CHAR_HOOK, &MainFrame::OnMenuNavCharHook, this);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SwitchView(View::Query); }, ID_VIEW_QUERY);
    // 查看 ▸ 停止 — cancel the running query (mirrors the toolbar 停止 button, which
    // is wired for wxEVT_BUTTON in BuildToolRow; here we add the wxEVT_MENU path).
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (queryRunning_ && workerConn_) {
            workerConn_->Cancel();
            SetStatusText(tr(L"已请求停止查询"));
        }
    }, ID_STOP_QUERY);
    // 查看 ▸ 运行 (ID_RUN_QUERY) is already bound to OnRunQuery in BuildToolRow.

    // ---- 文件 ▸ 打开 SQL 文件 / 保存 --------------------------------------------
    Bind(wxEVT_MENU, &MainFrame::OnOpenSqlFile, this, wxID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::OnSaveSqlFile, this, wxID_SAVE);

    // ---- 编辑 ▸ 撤销/重做/剪切/复制/粘贴 → active SQL editor's wxSTC -------------
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (EditorPage* p = ActivePage()) p->EditUndo();  }, wxID_UNDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (EditorPage* p = ActivePage()) p->EditRedo();  }, wxID_REDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (EditorPage* p = ActivePage()) p->EditCut();   }, wxID_CUT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (EditorPage* p = ActivePage()) p->EditCopy();  }, wxID_COPY);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (EditorPage* p = ActivePage()) p->EditPaste(); }, wxID_PASTE);

    // ---- 工具 ▸ 导入向导 / 导出向导 / 备份 → active connection + database ---------
    Bind(wxEVT_MENU, &MainFrame::OnToolImport, this, ID_TOOL_IMPORT);
    Bind(wxEVT_MENU, &MainFrame::OnToolExport, this, ID_TOOL_EXPORT);
    Bind(wxEVT_MENU, &MainFrame::OnToolBackup, this, ID_TOOL_BACKUP);

    // ---- 窗口 ▸ 下一个标签页 (Ctrl+Tab) -----------------------------------------
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (editors_ && editors_->GetPageCount() > 1) editors_->AdvanceSelection();
    }, ID_NEXT_TAB);
}

// ---------------------------------------------------------------------------
// 文件 ▸ 打开 SQL 文件: pick a .sql, read it, and load it into a fresh SQL editor
// tab (reuses OnNewQuery + EditorPage::InsertQuery).
void MainFrame::OnOpenSqlFile(wxCommandEvent&)
{
    wxFileDialog dlg(this, tr(L"打开 SQL 文件"), wxEmptyString, wxEmptyString,
                     tr(L"SQL 文件 (*.sql)|*.sql|所有文件 (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    wxFFile f(dlg.GetPath(), "rb");
    wxString text;
    if (!f.IsOpened() || !f.ReadAll(&text, wxConvUTF8)) {
        SetStatusText(tr(L"无法读取文件: ") + dlg.GetPath());
        return;
    }
    wxCommandEvent e;
    OnNewQuery(e);
    if (EditorPage* p = ActivePage()) p->InsertQuery(text);
    SetStatusText(tr(L"已打开: ") + dlg.GetPath());
}

// ---------------------------------------------------------------------------
// 文件 ▸ 保存: write the active SQL editor's whole buffer to a chosen .sql file.
void MainFrame::OnSaveSqlFile(wxCommandEvent&)
{
    EditorPage* p = ActivePage();
    if (!p) { SetStatusText(tr(L"没有可保存的 SQL 编辑器")); return; }
    wxFileDialog dlg(this, tr(L"保存 SQL 文件"), wxEmptyString, "query.sql",
                     tr(L"SQL 文件 (*.sql)|*.sql|所有文件 (*.*)|*.*"),
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxFFile f(dlg.GetPath(), "wb");
    if (!f.IsOpened() || !f.Write(p->FullText(), wxConvUTF8)) {
        SetStatusText(tr(L"无法写入文件: ") + dlg.GetPath());
        return;
    }
    SetStatusText(tr(L"已保存: ") + dlg.GetPath());
}

// ---------------------------------------------------------------------------
// 工具 ▸ 导入向导: run a .sql file against the active connection + database
// (reuses ConnectionTree::ImportSqlFileUi, same path as the overview 导入 button).
void MainFrame::OnToolImport(wxCommandEvent&)
{
    ConnEntry* e = connTree_ ? connTree_->active() : nullptr;
    if (!e || !e->IsConnected() || e->currentDb.IsEmpty()) {
        SetStatusText(tr(L"请先连接并打开数据库"));
        return;
    }
    connTree_->ImportSqlFileUi(e, e->currentDb);
}

// ---------------------------------------------------------------------------
// 工具 ▸ 导出向导: dump the active database (structure + data) to a .sql file
// (reuses ConnectionTree::ExportDatabaseUi, same path as the overview 导出 button).
void MainFrame::OnToolExport(wxCommandEvent&)
{
    ConnEntry* e = connTree_ ? connTree_->active() : nullptr;
    if (!e || !e->IsConnected() || e->currentDb.IsEmpty()) {
        SetStatusText(tr(L"请先连接并打开数据库"));
        return;
    }
    connTree_->ExportDatabaseUi(e, e->currentDb, /*withData*/ true);
}

// ---------------------------------------------------------------------------
// 工具 ▸ 备份: a full-database backup is a structure+data dump — same code path as
// 导出向导 (ExportDatabaseUi withData=true writes a restorable .sql of the whole db).
void MainFrame::OnToolBackup(wxCommandEvent&)
{
    ConnEntry* e = connTree_ ? connTree_->active() : nullptr;
    if (!e || !e->IsConnected() || e->currentDb.IsEmpty()) {
        SetStatusText(tr(L"请先连接并打开数据库"));
        return;
    }
    connTree_->ExportDatabaseUi(e, e->currentDb, /*withData*/ true);
}

// ---------------------------------------------------------------------------
// Hand a press on empty chrome (menu row / tool row blank space) to the OS as a
// title-bar drag — the same native WM_NCLBUTTONDOWN+HTCAPTION hand-off the header
// uses, so window move + Aero Snap come for free. Child buttons over these rows
// keep their own LEFT_DOWN handlers, so only blank areas reach this.
void MainFrame::BeginWindowDrag()
{
#ifdef __WXMSW__
    ::ReleaseCapture();
    ::SendMessage((HWND)GetHandle(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
#endif
}

// ---------------------------------------------------------------------------
// Custom menu row (replaces the native wxMenuBar): a slim strip of clickable
// text labels that pop up the wxMenu objects built in BuildMenuBar. Sits BELOW
// the header (so the view-switch pill stays above the menus, per the design).
wxWindow* MainFrame::BuildMenuRow(wxWindow* parent)
{
    auto* bar = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 30));
    bar->SetMinSize(wxSize(-1, 30));
    bar->SetBackgroundColour(theme::kChromeBg);
    // Blank space in the menu row drags the window (menu labels are child buttons
    // with their own handlers, so this only fires on the empty strip).
    bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { BeginWindowDrag(); });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->AddSpacer(6);

    auto addMenu = [&](const wxString& label, wxMenu* menu, wchar_t mnemonic) {
        const int index = static_cast<int>(topMenus_.size());
        auto* b = new wxButton(bar, wxID_ANY, label, wxDefaultPosition,
                               wxDefaultSize, wxBU_EXACTFIT | wxBORDER_NONE);
        b->SetBackgroundColour(theme::kChromeBg);
        b->SetForegroundColour(theme::kText);
        b->SetFont(Ui(9.5, false));
        // Flat buttons on a custom bg don't self-highlight on MSW — do it manually.
        b->Bind(wxEVT_ENTER_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kChromeBtnHover); b->Refresh(); e.Skip();
        });
        b->Bind(wxEVT_LEAVE_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kChromeBg); b->Refresh(); e.Skip();
        });
        // Route the click through OpenTopMenu so ← / → menu-walking works whether
        // the menu was opened by mouse or by Alt+mnemonic.
        b->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { OpenTopMenu(index); });
        row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 2);
        topMenus_.push_back({ b, menu, mnemonic });
    };

    // Mnemonics mirror the LangPack menu-bar titles: 文件(&F) 编辑(&E) 查看(&V)
    // 收藏(&B) 工具(&T) 窗口(&W) 帮助(&H) — driven by Alt+letter in the CHAR_HOOK.
    topMenus_.clear();
    addMenu(tr(L"文件"), fileMenu_,   L'F');
    addMenu(tr(L"编辑"), editMenu_,   L'E');
    addMenu(tr(L"查看"), viewMenu_,   L'V');
    addMenu(tr(L"收藏"), favMenu_,    L'B');
    addMenu(tr(L"工具"), toolsMenu_,  L'T');
    addMenu(tr(L"窗口"), windowMenu_, L'W');
    addMenu(tr(L"帮助"), helpMenu_,   L'H');

    bar->SetSizer(row);
    return bar;
}

// ---------------------------------------------------------------------------
// Pop up top menu #index at its button. On MSW, ← / → while the menu is open
// walk to the neighbouring menu (wrapping around), mirroring a native menu bar:
// the WH_KEYBOARD hook records the direction + EndMenu()s the popup, then this
// loop re-opens the adjacent one. Elsewhere it's a plain single popup.
void MainFrame::OpenTopMenu(int index)
{
    const int n = static_cast<int>(topMenus_.size());
    if (index < 0 || index >= n) return;

    int cur = index;
    for (;;) {
        const TopMenu& tm = topMenus_[cur];
        if (tm.menu == favMenu_) RebuildFavorites();   // popups don't fire MENU_OPEN reliably
        wxWindow* b = tm.btn;
#ifdef __WXMSW__
        InstallMenuNavHook();
        b->PopupMenu(tm.menu, wxPoint(0, b->GetSize().y));
        RemoveMenuNavHook();
        if (g_menuNavDir == 0) break;                  // closed normally (pick / Esc / click-away)
        cur = (cur + g_menuNavDir + n) % n;            // ← / → → walk to the neighbour, wrapping
#else
        b->PopupMenu(tm.menu, wxPoint(0, b->GetSize().y));
        break;
#endif
    }
}

int MainFrame::TopMenuForMnemonic(wchar_t upper) const
{
    for (size_t i = 0; i < topMenus_.size(); ++i)
        if (topMenus_[i].mnemonic == upper) return static_cast<int>(i);
    return -1;
}

// Alt+letter opens the matching top menu (like a native menu bar). The native
// popup then handles ↑/↓/Enter/Esc itself; ← / → are handled by the hook above.
void MainFrame::OnMenuNavCharHook(wxKeyEvent& ev)
{
    if (ev.AltDown() && !ev.ControlDown() && !ev.ShiftDown()) {
        const int kc = ev.GetKeyCode();              // letters arrive upper-cased
        if (kc >= 'A' && kc <= 'Z') {
            const int idx = TopMenuForMnemonic(static_cast<wchar_t>(kc));
            if (idx >= 0) { OpenTopMenu(idx); return; }   // consume — don't Skip()
        }
    }
    ev.Skip();
}

} // namespace ui
