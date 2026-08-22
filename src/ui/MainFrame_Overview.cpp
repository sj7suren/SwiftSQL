// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/MainFrame.h"
#include "core/CrashLog.h"
#include "core/GroupStore.h"   // 分组 filter over the table grid

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
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/PreferencesDialog.h"
#include "ui/TableDesignView.h"
#include "ui/NewTableView.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {

// Empty-state main visual: a magnifier framing a table grid, drawn per
// docs/design/empty-state-spec.md in a 96×96 design space, whitened blues so it
// "recedes" on white. Self-drawn with wxGraphicsContext; centred in its client.
class EmptyArtPanel : public wxWindow {
public:
    explicit EmptyArtPanel(wxWindow* parent)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(96, 96))
    {
        SetBackgroundColour(theme::kWhite);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(96, 96));
        Bind(wxEVT_PAINT, &EmptyArtPanel::OnPaint, this);
    }
private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(theme::kWhite));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        // 96×96 design space, centred in the client.
        const wxSize sz = GetClientSize();
        const double s = 1.0;                         // @1x
        const double ox = (sz.x - 96 * s) / 2.0;
        const double oy = (sz.y - 96 * s) / 2.0;
        gc->Translate(ox, oy);
        gc->Scale(s, s);

        auto strokePen = [&](const wxColour& c, double w) {
            return gc->CreatePen(wxGraphicsPenInfo(c).Width(w)
                                     .Cap(wxCAP_ROUND).Join(wxJOIN_ROUND));
        };

        // 1) lens interior fill (circle, no stroke)
        gc->SetPen(wxNullGraphicsPen);
        gc->SetBrush(wxBrush(theme::kEmptyLensFill));
        {
            wxGraphicsPath p = gc->CreatePath();
            p.AddCircle(42, 42, 30);
            gc->FillPath(p);
        }

        // 3) handle (drawn before the ring so the ring overlaps it cleanly)
        gc->SetPen(strokePen(theme::kEmptyStrokeStrong, 6));
        {
            wxGraphicsPath p = gc->CreatePath();
            p.MoveToPoint(63.2, 63.2);
            p.AddLineToPoint(86, 86);
            gc->StrokePath(p);
        }

        // 4) table card + grid lines (recede one layer: thinner, lighter)
        gc->SetPen(strokePen(theme::kEmptyStroke, 2.4));
        gc->SetBrush(wxNullGraphicsBrush);
        {
            wxGraphicsPath card = gc->CreatePath();
            card.AddRoundedRectangle(25, 29, 34, 26, 3);
            gc->StrokePath(card);

            wxGraphicsPath grid = gc->CreatePath();
            grid.MoveToPoint(25, 37); grid.AddLineToPoint(59, 37);   // header rule
            grid.MoveToPoint(25, 46); grid.AddLineToPoint(59, 46);   // row rule
            grid.MoveToPoint(36, 29); grid.AddLineToPoint(36, 55);   // first column
            gc->StrokePath(grid);
        }

        // 5) lens outer ring (drawn last — sharpest edge)
        gc->SetPen(strokePen(theme::kEmptyStrokeStrong, 3.4));
        gc->SetBrush(wxNullGraphicsBrush);
        {
            wxGraphicsPath ring = gc->CreatePath();
            ring.AddCircle(42, 42, 30);
            gc->StrokePath(ring);
        }
    }
};

// Human-readable byte size ("—" for unknown/-1). Binary units to match du/df.
wxString FmtBytes(long long b)
{
    if (b < 0) return L"—";
    const wchar_t* unit[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    return u == 0 ? wxString::Format(L"%lld B", b)
                  : wxString::Format(L"%.1f %s", v, unit[u]);
}

// Thousands-grouped integer ("—" for unknown/-1).
wxString FmtCount(long long n)
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

// ---------------------------------------------------------------------------
// Connection overview (slot 0 of the fixed "总览" tab): a per-engine database grid
// on the left and a fixed connection-info panel on the right. Shown when a
// connection node is opened; the columns come from the driver so each engine shows
// its own metadata.
wxWindow* MainFrame::BuildOverviewView(wxWindow* parent)
{
    auto* split = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize,
                                       wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxBORDER_NONE);
    split->SetMinimumPaneSize(180);

    overviewList_ = new wxListCtrl(split, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    // A small database icon per row lifts scannability of the database grid.
    // Image 0 = the cylinder glyph; every data row references it (see ShowConnectionOverview).
    auto* imgs = new wxImageList(16, 16, /*mask*/ true);
    imgs->Add(icons::Stroke(icons::Glyph::Cylinder, 16, theme::kPrimary));
    overviewList_->AssignImageList(imgs, wxIMAGE_LIST_SMALL);

    auto* info = new wxPanel(split);
    info->SetBackgroundColour(theme::kSidebarBg);
    auto* v = new wxBoxSizer(wxVERTICAL);
    // Header: engine brand logo + panel title, so the active engine is legible at
    // a glance (MySQL vs PostgreSQL vs SQLite…) the moment a node is clicked.
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    ovBrand_ = new wxStaticBitmap(info, wxID_ANY, wxNullBitmap);
    head->Add(ovBrand_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    auto* title = new wxStaticText(info, wxID_ANY, tr(L"连接信息"));
    title->SetFont(Ui(11, true));
    title->SetForegroundColour(theme::kText);
    head->Add(title, 0, wxALIGN_CENTER_VERTICAL);
    v->Add(head, 0, wxALL, 14);

    auto* grid = new wxFlexGridSizer(2, 8, 10);
    grid->AddGrowableCol(1, 1);
    auto field = [&](const wxString& label) -> wxStaticText* {
        auto* l = new wxStaticText(info, wxID_ANY, label);
        l->SetForegroundColour(theme::kTextSecondary);
        auto* val = new wxStaticText(info, wxID_ANY, L"—");
        val->SetForegroundColour(theme::kText);
        val->SetFont(Ui(9.5, true));
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(val, 1, wxEXPAND);
        return val;
    };
    ovName_    = field(tr(L"连接名"));
    ovEngine_  = field(tr(L"引擎"));
    ovVersion_ = field(tr(L"版本"));
    ovHost_    = field(tr(L"地址"));
    ovPort_    = field(tr(L"端口"));
    ovStatus_  = field(tr(L"状态"));
    v->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    info->SetSizer(v);

    split->SplitVertically(overviewList_, info, -280);   // info pane ~280px, on the right
    split->SetSashGravity(1.0);                           // list grows on resize
    return split;
}

void MainFrame::ShowConnectionOverview(ConnEntry* e)
{
    if (!e) return;
    SetOverviewSlot(0);   // 确保固定标签存在 + 内容切到"连接库列表",标签名 → 总览
    if (!overviewList_) return;
    ovEntry_ = e;         // 记住这半正显示哪个连接(断连时清空用)
    const db::DbTypeInfo& info = db::InfoOf(e->profile.type);
    const bool connected = e->IsConnected();
    const bool fileBased = e->profile.host.IsEmpty();   // SQLite carries a file path

    if (ovBrand_) ovBrand_->SetBitmap(icons::DbBrand(e->profile.type, 36));
    ovName_->SetLabel(e->profile.name);
    ovEngine_->SetLabel(info.name);
    ovVersion_->SetLabel(connected ? e->conn->ServerVersion() : wxString(L"—"));
    ovHost_->SetLabel(fileBased ? e->profile.database : e->profile.host);
    ovPort_->SetLabel(e->profile.port > 0 ? wxString::Format(L"%d", e->profile.port)
                                          : wxString(L"—"));
    ovStatus_->SetLabel(connected ? tr(L"已连接") : tr(L"未连接"));
    ovStatus_->SetForegroundColour(connected ? theme::kGreen : theme::kTextSecondary);

    overviewList_->ClearAll();
    if (connected) {
        JoinCountWorker();   // serialize with any in-flight pager COUNT on the same conn
        db::DbOverview ov; wxString err;
        if (e->conn->GetDatabaseOverview(ov, err)) {
            for (size_t c = 0; c < ov.columns.size(); ++c)
                overviewList_->InsertColumn(static_cast<long>(c), tr(ov.columns[c].label),
                                            wxLIST_FORMAT_LEFT, 170);
            for (size_t r = 0; r < ov.rows.size(); ++r) {
                const auto& row = ov.rows[r];
                const long idx = overviewList_->InsertItem(
                    static_cast<long>(r), row.empty() ? wxString() : row[0],
                    /*imageIndex*/ 0);   // cylinder icon on every database row
                for (size_t c = 1; c < row.size() && c < ov.columns.size(); ++c)
                    overviewList_->SetItem(idx, static_cast<long>(c), row[c]);
            }
        } else {
            overviewList_->InsertColumn(0, tr(L"错误"), wxLIST_FORMAT_LEFT, 500);
            overviewList_->InsertItem(0, err);
        }
    }

    if (overviewView_) overviewView_->Layout();   // relabelled fields → relayout
    // 打开连接是明确"打开"手势(仅由双击/连接成功触发)→ 把固定标签切到前面。
    SelectDbOverviewTab();
}

// ---------------------------------------------------------------------------
// Database overview (shared "表列表" tab in editors_): a table-metadata grid on the
// left and a database-info panel on the right. Shown when a database node is activated; the
// table metadata is loaded off the GUI thread (a schema with thousands of tables
// would otherwise freeze the window) with an activity spinner while it loads.
wxWindow* MainFrame::BuildDatabaseView(wxWindow* parent)
{
    auto* split = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize,
                                       wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxBORDER_NONE);
    split->SetMinimumPaneSize(200);

    // ---- left: header (title + spinner + count) over the table grid ----
    auto* left = new wxPanel(split);
    left->SetBackgroundColour(theme::kWhite);
    auto* lv = new wxBoxSizer(wxVERTICAL);

    // ---- toolbar: table operations (icon + label) + a name filter ----
    auto* tb = new wxBoxSizer(wxHORIZONTAL);
    auto toolBtn = [&](const wxString& label, icons::Glyph g, std::function<void()> fn) {
        auto* b = new wxButton(left, wxID_ANY, label, wxDefaultPosition,
                               wxDefaultSize, wxBORDER_NONE);
        b->SetBitmap(icons::Stroke(g, 16, theme::kTextSecondary, 1.8));
        b->SetBitmapMargins(2, 0);       // tight gap between icon and label
        b->SetFont(Ui(9.5));
        b->SetBackgroundColour(theme::kWhite);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        tb->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        return b;
    };
    toolBtn(tr(L"打开表"), icons::Glyph::Table,   [this] { OverviewOpenTable(); });
    toolBtn(tr(L"新建表"), icons::Glyph::RowAdd,  [this] { OverviewNewTable(); });
    toolBtn(tr(L"设计表"), icons::Glyph::Model,   [this] { OverviewDesignTable(); });
    toolBtn(tr(L"删除表"), icons::Glyph::Stop,    [this] { OverviewDeleteTables(); });
    toolBtn(tr(L"导入"),   icons::Glyph::Import,  [this] {
        if (dbovEntry_) connTree_->ImportSqlFileUi(dbovEntry_, dbovDb_);
    });
    toolBtn(tr(L"导出"),   icons::Glyph::Export,  [this] {
        if (!dbovEntry_) return;
        wxMenu m;   // structure+data / structure only (async dump with progress)
        wxMenuItem* a = m.Append(wxID_ANY, tr(L"结构和数据"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            connTree_->ExportDatabaseUi(dbovEntry_, dbovDb_, /*withData*/ true);
        }, a->GetId());
        wxMenuItem* s = m.Append(wxID_ANY, tr(L"仅结构"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            connTree_->ExportDatabaseUi(dbovEntry_, dbovDb_, /*withData*/ false);
        }, s->GetId());
        PopupMenu(&m);
    });
    toolBtn(tr(L"刷新"),   icons::Glyph::Rollback,[this] {
        if (dbovEntry_) ShowDatabaseOverview(dbovEntry_, dbovDb_, /*force*/ true);
    });
    tb->AddStretchSpacer(1);
    dbovSearch_ = new wxSearchCtrl(left, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   FromDIP(wxSize(180, -1)));
    dbovSearch_->SetDescriptiveText(tr(L"搜索表名"));
    dbovSearch_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        RenderTableList(dbovSearch_->GetValue());
    });
    // The 共 N 张表 count row above the grid is removed per request; the loading
    // spinner it hosted moves into the toolbar so loading is still indicated.
    tableSpinner_ = new wxActivityIndicator(left, wxID_ANY);
    tb->Add(tableSpinner_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    tb->Add(dbovSearch_, 0, wxALIGN_CENTER_VERTICAL);
    lv->Add(tb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    // Multi-select (Shift/Ctrl) so several tables can be dropped at once — no
    // wxLC_SINGLE_SEL. The last column stretches to fill the pane (StretchTableColumns).
    tableList_ = new wxListCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxLC_REPORT | wxBORDER_NONE);
    tableList_->AppendColumn(tr(L"表名"),     wxLIST_FORMAT_LEFT,  FromDIP(200));
    tableList_->AppendColumn(tr(L"引擎"),     wxLIST_FORMAT_LEFT,  FromDIP(80));
    tableList_->AppendColumn(tr(L"行数"),     wxLIST_FORMAT_RIGHT, FromDIP(100));
    tableList_->AppendColumn(tr(L"字段数"),   wxLIST_FORMAT_RIGHT, FromDIP(70));
    tableList_->AppendColumn(tr(L"体积"),     wxLIST_FORMAT_RIGHT, FromDIP(100));
    tableList_->AppendColumn(tr(L"自增"),     wxLIST_FORMAT_CENTER,FromDIP(60));
    tableList_->AppendColumn(tr(L"修改日期"), wxLIST_FORMAT_LEFT,  FromDIP(150));
    tableList_->AppendColumn(tr(L"注释"),     wxLIST_FORMAT_LEFT,  FromDIP(240));
    // Right-click → table operations menu (respects multi-selection for delete).
    tableList_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& ev) {
        // RIGHT-CLICK RETARGETS THE SELECTION when the clicked row is not part
        // of it (Explorer / Navicat behaviour). wxListCtrl does not do this on
        // its own, so without it the menu acts on whatever was selected BEFORE
        // the click. That was survivable for 删除表 — it confirms with a count
        // first — but 移动到分组 applies immediately, and would silently move
        // tables the user never pointed at.
        const long hit = ev.GetIndex();
        if (hit >= 0 &&
            !(tableList_->GetItemState(hit, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED)) {
            // Collect first, then clear: deselecting while walking GetNextItem
            // mutates the very state the walk is filtering on.
            std::vector<long> prev;
            for (long s = tableList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
                 s >= 0;
                 s = tableList_->GetNextItem(s, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
                prev.push_back(s);
            for (long s : prev) tableList_->SetItemState(s, 0, wxLIST_STATE_SELECTED);
            tableList_->SetItemState(hit, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                     wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        }
        wxMenu m;
        wxMenuItem* mo = m.Append(wxID_ANY, tr(L"打开表"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OverviewOpenTable(); }, mo->GetId());
        wxMenuItem* md = m.Append(wxID_ANY, tr(L"设计表"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OverviewDesignTable(); }, md->GetId());
        m.AppendSeparator();
        const std::vector<wxString> sel = OverviewSelectedTables();
        const size_t n = sel.size();
        // 移动到分组 ▶ — honours the multi-selection, same submenu (and same
        // group scope) the sidebar tree uses. The grid itself shows no group
        // column, so the visible result lands in the sidebar; the status line
        // reports the count so the gesture is not silent from here.
        if (connTree_ && dbovEntry_)
            connTree_->AppendTableGroupMenuUi(m, dbovEntry_, dbovDb_, sel, nullptr);
        // 从分组移除 — only while this grid IS a group view; on the whole-database
        // view there is no single group to remove from, and offering it there
        // would silently mean "remove from whatever group each row happens to be
        // in", which is not what the words say.
        if (!dbovGroup_.IsEmpty() && connTree_ && dbovEntry_) {
            wxMenuItem* ma = m.Append(wxID_ANY, tr(L"全选"));
            m.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
                // wxListCtrl has no built-in Ctrl+A; without this, "全选移除"
                // would depend on the user rubber-banding the whole list.
                for (long i = 0; i < tableList_->GetItemCount(); ++i)
                    tableList_->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            }, ma->GetId());
            wxMenuItem* mr = m.Append(wxID_ANY, n > 1
                ? wxString::Format(tr(L"从分组移除选中的 %zu 项"), n) : tr(L"从分组移除"));
            m.Bind(wxEVT_MENU, [this, sel](wxCommandEvent&) {
                if (!connTree_ || !dbovEntry_) return;
                if (connTree_->RemoveTablesFromGroupUi(dbovEntry_, dbovDb_, sel))
                    // Re-render from the SAME cache: the rows just removed are no
                    // longer members, so they drop out of this view immediately.
                    RenderTableList(dbovSearch_ ? dbovSearch_->GetValue() : wxString());
            }, mr->GetId());
        }
        m.AppendSeparator();
        wxMenuItem* mx = m.Append(wxID_ANY, n > 1
            ? wxString::Format(tr(L"删除选中的 %zu 张表"), n) : tr(L"删除表"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OverviewDeleteTables(); }, mx->GetId());
        PopupMenu(&m);
    });
    // Double-click a row opens the table (same as 打开表).
    tableList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { OverviewOpenTable(); });
    // Keep the last column filling the remaining width on resize.
    tableList_->Bind(wxEVT_SIZE, [this](wxSizeEvent& ev) { StretchTableColumns(); ev.Skip(); });
    lv->Add(tableList_, 1, wxEXPAND);
    left->SetSizer(lv);

    // ---- right: database info panel ----
    auto* info = new wxPanel(split);
    info->SetBackgroundColour(theme::kSidebarBg);
    auto* v = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(info, wxID_ANY, tr(L"数据库信息"));
    title->SetFont(Ui(11, true));
    title->SetForegroundColour(theme::kText);
    v->Add(title, 0, wxALL, 14);

    auto* grid = new wxFlexGridSizer(2, 8, 10);
    grid->AddGrowableCol(1, 1);
    auto field = [&](const wxString& label) -> wxStaticText* {
        auto* l = new wxStaticText(info, wxID_ANY, label);
        l->SetForegroundColour(theme::kTextSecondary);
        auto* val = new wxStaticText(info, wxID_ANY, L"—");
        val->SetForegroundColour(theme::kText);
        val->SetFont(Ui(9.5, true));
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(val, 1, wxEXPAND);
        return val;
    };
    dbovName_      = field(tr(L"数据库"));
    dbovCharset_   = field(tr(L"字符集"));
    dbovCollation_ = field(tr(L"排序规则"));
    dbovTables_    = field(tr(L"表数量"));
    dbovSize_      = field(tr(L"总体积"));
    v->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    info->SetSizer(v);

    split->SplitVertically(left, info, -260);
    split->SetSashGravity(1.0);
    return split;
}

// ---------------------------------------------------------------------------
// Slot 2 — the connections inside one 分组.
//
// A SEPARATE VIEW, not a filter over slot 0 or 1. Slot 0 lists one connection's
// DATABASES and slot 1 lists one database's TABLES; neither is a list of
// connections, and a connection has none of the columns either is built from.
// So this is the one group kind that genuinely needs its own grid — table groups
// deliberately reuse slot 1 instead (see ShowTableGroupOverview).
// ---------------------------------------------------------------------------
wxWindow* MainFrame::BuildConnGroupView(wxWindow* parent)
{
    auto* split = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);
    split->SetMinimumPaneSize(FromDIP(180));

    auto* left = new wxPanel(split);
    left->SetBackgroundColour(theme::kWhite);
    auto* lv = new wxBoxSizer(wxVERTICAL);

    // Multi-select for the same reason the table grid has it: 全选移除 must be a
    // real gesture, not "remove them one at a time".
    connGroupList_ = new wxListCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxBORDER_NONE);
    connGroupList_->AppendColumn(tr(L"名称"), wxLIST_FORMAT_LEFT, FromDIP(200));
    connGroupList_->AppendColumn(tr(L"引擎"), wxLIST_FORMAT_LEFT, FromDIP(110));
    connGroupList_->AppendColumn(tr(L"主机"), wxLIST_FORMAT_LEFT, FromDIP(200));
    connGroupList_->AppendColumn(tr(L"状态"), wxLIST_FORMAT_LEFT, FromDIP(90));

    connGroupList_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& ev) {
        // Same retarget-on-right-click rule as the table grid: without it the
        // menu acts on the previous selection, and 从分组移除 applies instantly.
        const long hit = ev.GetIndex();
        if (hit >= 0 && !(connGroupList_->GetItemState(hit, wxLIST_STATE_SELECTED) &
                          wxLIST_STATE_SELECTED)) {
            std::vector<long> prev;
            for (long s = connGroupList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
                 s >= 0;
                 s = connGroupList_->GetNextItem(s, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
                prev.push_back(s);
            for (long s : prev) connGroupList_->SetItemState(s, 0, wxLIST_STATE_SELECTED);
            connGroupList_->SetItemState(hit, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                         wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        }
        const std::vector<wxString> sel = ConnGroupSelectedNames();
        wxMenu m;
        wxMenuItem* ma = m.Append(wxID_ANY, tr(L"全选"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            for (long i = 0; i < connGroupList_->GetItemCount(); ++i)
                connGroupList_->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }, ma->GetId());
        m.AppendSeparator();
        wxMenuItem* mr = m.Append(wxID_ANY, sel.size() > 1
            ? wxString::Format(tr(L"从分组移除选中的 %zu 项"), sel.size())
            : tr(L"从分组移除"));
        mr->Enable(!sel.empty());
        m.Bind(wxEVT_MENU, [this, sel](wxCommandEvent&) {
            if (connTree_ && connTree_->RemoveConnectionsFromGroupUi(sel))
                RenderConnGroupList();   // they are no longer members → rows go
        }, mr->GetId());
        PopupMenu(&m);
    });
    lv->Add(connGroupList_, 1, wxEXPAND);
    left->SetSizer(lv);

    auto* info = new wxPanel(split);
    info->SetBackgroundColour(theme::kSidebarBg);
    auto* v = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(info, wxID_ANY, tr(L"分组信息"));
    title->SetFont(Ui(11, true));
    title->SetForegroundColour(theme::kText);
    v->Add(title, 0, wxALL, 14);
    auto* grid = new wxFlexGridSizer(2, 8, 10);
    grid->AddGrowableCol(1, 1);
    auto field = [&](const wxString& label) -> wxStaticText* {
        auto* l = new wxStaticText(info, wxID_ANY, label);
        l->SetForegroundColour(theme::kTextSecondary);
        auto* val = new wxStaticText(info, wxID_ANY, L"—");
        val->SetForegroundColour(theme::kText);
        val->SetFont(Ui(9.5, true));
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(val, 1, wxEXPAND);
        return val;
    };
    cgName_  = field(tr(L"分组"));
    cgCount_ = field(tr(L"连接数"));
    v->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    info->SetSizer(v);

    split->SplitVertically(left, info, -260);
    split->SetSashGravity(1.0);
    return split;
}

std::vector<wxString> MainFrame::ConnGroupSelectedNames() const
{
    std::vector<wxString> out;
    if (!connGroupList_) return out;
    for (long i = connGroupList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
         i >= 0;
         i = connGroupList_->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
        out.push_back(connGroupList_->GetItemText(i));
    return out;
}

// Rendered from GroupStore + the tree's live entries every time, never cached:
// membership and connected-state both change underneath this view, and re-reading
// is what makes a removed connection vanish from the list immediately.
void MainFrame::RenderConnGroupList()
{
    if (!connGroupList_) return;
    connGroupList_->DeleteAllItems();
    if (cgroupName_.IsEmpty() || !connTree_) return;

    const std::vector<wxString> members =
        core::GroupStore::MembersOf(core::GroupStore::ConnScope(), cgroupName_);
    const std::vector<ConnEntry*> all = connTree_->allEntries();

    long row = 0;
    for (const wxString& name : members) {
        ConnEntry* e = nullptr;
        for (ConnEntry* c : all) if (c->profile.name == name) { e = c; break; }
        // A membership with no live entry means the connection was deleted
        // outside this view; skip it rather than drawing a row that opens
        // nothing. GroupStore prunes it on the next mutation.
        if (!e) continue;
        const long idx = connGroupList_->InsertItem(row++, e->profile.name);
        connGroupList_->SetItem(idx, 1, db::InfoOf(e->profile.type).name);
        connGroupList_->SetItem(idx, 2, e->profile.host.IsEmpty()
            ? wxString(L"—")
            : e->profile.host + wxString::Format(L":%d", e->profile.port));
        connGroupList_->SetItem(idx, 3, e->IsConnected() ? tr(L"已连接") : tr(L"未连接"));
    }
    if (cgName_)  cgName_->SetLabel(cgroupName_);
    if (cgCount_) cgCount_->SetLabel(wxString::Format(L"%ld", row));
    if (connGroupView_) connGroupView_->Layout();
}

void MainFrame::ShowConnGroupOverview(const wxString& group)
{
    if (group.IsEmpty()) return;
    cgroupName_ = group;
    SetOverviewSlot(2);
    RenderConnGroupList();
}

// 惰性创建 editors_ 中唯一、不可关闭的固定"总览"标签(始终位于最左 index 0)。
// 该标签是一个跟随上下文的容器 overviewStack_:slot 0 = 连接的数据库列表
// (overviewView_),slot 1 = 库的表列表(databaseView_)。已存在则原样返回;首次
// 调用(首个 ShowConnectionOverview / ShowDatabaseOverview)才建,保证首启动是空态。
wxWindow* MainFrame::EnsureDbOverviewTab()
{
    if (overviewTab_ && editors_->GetPageIndex(overviewTab_) != wxNOT_FOUND)
        return overviewTab_;
    overviewStack_ = new wxSimplebook(editors_, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_NONE);
    overviewStack_->SetBackgroundColour(theme::kWhite);
    overviewView_  = BuildOverviewView(overviewStack_);    // slot 0:连接库列表 + 连接信息
    databaseView_  = BuildDatabaseView(overviewStack_);    // slot 1:库表列表 + 库信息
    connGroupView_ = BuildConnGroupView(overviewStack_);   // slot 2:分组内连接列表 + 分组信息
    overviewStack_->AddPage(overviewView_,  L"conn");      // slot 0
    overviewStack_->AddPage(databaseView_,  L"db");        // slot 1
    overviewStack_->AddPage(connGroupView_, L"cgroup");    // slot 2
    overviewTab_ = overviewStack_;                       // 固定标签身份(art/veto 判定用)
    editors_->InsertPage(0, overviewStack_, tr(L"总览"), false,
                         icons::Stroke(icons::Glyph::Table, 14, theme::kPrimary));
    editors_->SetPageToolTip(0, tr(L"总览标签，不可关闭"));   // 提示这是常驻标签
    UpdateQueryView();   // 有页了 → 从空态占位翻到 editors_
    return overviewTab_;
}

// 切换固定"总览"标签的内容页并同步标签名(0 = 连接库列表 → "总览";
// 1 = 库表列表 → "表列表")。只改内容,不抢焦点(是否切到前面由调用方决定)。
void MainFrame::SetOverviewSlot(int slot)
{
    EnsureDbOverviewTab();
    overviewStack_->SetSelection(slot);
    RefreshOverviewTabTitle();   // 总览 / 表信息 / 分组：X — one place decides
}

// 选中固定"总览"标签并切到查询视图(editors_ 位于查询视图内)。
void MainFrame::SelectDbOverviewTab()
{
    if (!overviewTab_) return;
    const int idx = editors_->GetPageIndex(overviewTab_);
    if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
    SwitchView(View::Query);
}

// 关闭连接时清空共享"表列表"标签(惰性标签可能尚未创建,逐一判空)。
void MainFrame::ClearDbOverview()
{
    ++dbLoadGen_;                 // 作废在飞的加载 CallAfter
    dbovEntry_ = nullptr;
    dbovDb_.clear();
    dbovGroup_.clear();           // 分组过滤器属于那个库,库没了它也没了
    dbovMetas_.clear();
    if (tableSpinner_)   { tableSpinner_->Stop(); tableSpinner_->Hide(); }
    if (tableList_)      tableList_->DeleteAllItems();
    if (dbovName_)       dbovName_->SetLabel(L"—");
    if (dbovCharset_)    dbovCharset_->SetLabel(L"—");
    if (dbovCollation_)  dbovCollation_->SetLabel(L"—");
    if (dbovTables_)     dbovTables_->SetLabel(L"—");
    if (dbovSize_)       dbovSize_->SetLabel(L"—");
    if (dbovSearch_)     dbovSearch_->ChangeValue(wxEmptyString);
    if (databaseView_)   databaseView_->Layout();
}

// 断开连接时清空固定标签的"连接库列表"半(overviewList_ + 连接信息)。
void MainFrame::ClearConnectionOverview()
{
    ovEntry_ = nullptr;
    if (overviewList_) overviewList_->ClearAll();
    if (ovBrand_)   ovBrand_->SetBitmap(wxNullBitmap);
    if (ovName_)    ovName_->SetLabel(L"—");
    if (ovEngine_)  ovEngine_->SetLabel(L"—");
    if (ovVersion_) ovVersion_->SetLabel(L"—");
    if (ovHost_)    ovHost_->SetLabel(L"—");
    if (ovPort_)    ovPort_->SetLabel(L"—");
    if (ovStatus_)  { ovStatus_->SetLabel(tr(L"未连接"));
                      ovStatus_->SetForegroundColour(theme::kTextSecondary); }
    if (overviewView_) overviewView_->Layout();
}

void MainFrame::ShowDatabaseOverview(ConnEntry* e, const wxString& db, bool force)
{
    if (!e || !e->IsConnected()) return;
    // CLICKING A DATABASE ALWAYS RETURNS TO THE WHOLE-DATABASE VIEW. Dropping the
    // 分组 filter here — before the "already showing this database" shortcut
    // below — is what makes 点分组 → 点数据库 a round trip instead of a one-way
    // door: the same database with a group filter still on would otherwise keep
    // showing just that group.
    const bool hadGroup = !dbovGroup_.IsEmpty();
    dbovGroup_.clear();
    SetOverviewSlot(1);         // 惰性建好固定标签 + 内容切到"库表列表",标签名 → 表信息
    // 内容已是这个库就别重载(spinner + 重查);焦点是否切到表列表由调用方按 focus
    // 决定(见 showDatabase 钩子),这里只管内容,绝不主动抢焦点。
    if (!force && e == dbovEntry_ && db == dbovDb_) {
        // …but a filter that was just dropped still needs one cheap re-render
        // from the SAME cache. No query, no spinner.
        if (hadGroup) RenderTableList(dbovSearch_ ? dbovSearch_->GetValue() : wxString());
        return;
    }

    dbovEntry_ = e;             // remembered for the toolbar (open/import/export/refresh)
    dbovDb_    = db;
    dbovMetas_.clear();
    if (dbovSearch_) dbovSearch_->ChangeValue(wxEmptyString);   // reset filter for the new db

    // Reset to a loading state and switch to the view immediately, so the click
    // feels instant even while the metadata query is still running.
    dbovName_->SetLabel(db);
    dbovCharset_->SetLabel(L"—");
    dbovCollation_->SetLabel(L"—");
    dbovTables_->SetLabel(L"—");
    dbovSize_->SetLabel(L"—");
    tableList_->DeleteAllItems();
    tableSpinner_->Show();
    tableSpinner_->Start();
    if (databaseView_) databaseView_->Layout();

    JoinDbWorker();                 // cancel + join any previous load (single-threaded conn)
    const int gen = ++dbLoadGen_;
    db::IConnection* conn = e->conn.get();
    dbLoadConn_ = conn;
    dbWorker_ = core::CrashLog::GuardedThread(L"数据库概览加载", [this, conn, db, gen]() {
        std::vector<db::TableMeta> metas; db::DbInfo dinfo;
        wxString errT, errI;
        const bool okT = conn->GetTableList(db, metas, errT);
        const bool okI = conn->GetDatabaseInfo(db, dinfo, errI);
        CallAfter([this, gen, metas, dinfo, okT, okI, errT]() {
            if (gen != dbLoadGen_) return;   // a newer load superseded this one
            dbLoadConn_ = nullptr;
            tableSpinner_->Stop();
            tableSpinner_->Hide();

            // db-info panel
            dbovCharset_->SetLabel(dinfo.charset.IsEmpty() ? L"—" : dinfo.charset);
            dbovCollation_->SetLabel(dinfo.collation.IsEmpty() ? L"—" : dinfo.collation);
            dbovTables_->SetLabel(okI ? FmtCount(dinfo.tableCount)
                                      : wxString(L"—"));
            dbovSize_->SetLabel(okI ? FmtBytes(dinfo.sizeBytes) : wxString(L"—"));

            // table grid
            tableList_->DeleteAllItems();
            if (!okT) {
                dbovMetas_.clear();
                tableList_->InsertItem(0, tr(L"加载失败: ") + errT);
                if (databaseView_) databaseView_->Layout();
                return;
            }
            dbovMetas_ = metas;                         // cache for the search filter
            // 默认按表名 A-Z 升序(大小写不敏感);过滤/刷新都从这份已排序缓存渲染,
            // 所以顺序始终保持。
            std::sort(dbovMetas_.begin(), dbovMetas_.end(),
                      [](const db::TableMeta& a, const db::TableMeta& b) {
                          return a.name.CmpNoCase(b.name) < 0;
                      });
            RenderTableList(dbovSearch_ ? dbovSearch_->GetValue() : wxString());
            if (databaseView_) databaseView_->Layout();
        });
    });
}

// Clicking a 表 group folder: the same grid, the same cached metadata, narrowed
// to that group. Deliberately NOT a separate view — the group's tables want
// exactly the columns (行数 / 体积 / 引擎), the search box, and the context menu
// the database view already has, and a parallel view would have to grow all of
// it again and then drift.
void MainFrame::ShowTableGroupOverview(ConnEntry* e, const wxString& db,
                                       const wxString& group)
{
    if (!e || !e->IsConnected() || group.IsEmpty()) return;
    // Is the metadata for this database already in hand? If so the group view
    // costs one render and no query at all.
    const bool cached = (e == dbovEntry_ && db == dbovDb_ && !dbovMetas_.empty());
    ShowDatabaseOverview(e, db);   // ensures the slot, and loads the db if needed
    dbovGroup_ = group;            // AFTER: ShowDatabaseOverview clears the filter
    if (cached) RenderTableList(dbovSearch_ ? dbovSearch_->GetValue() : wxString());
    // Not cached → a load is in flight; its completion calls RenderTableList,
    // which will read the filter we just set.
    RefreshOverviewTabTitle();
}

// The fixed tab's caption follows what it is actually showing: 总览 (a
// connection's databases), 表信息 (a database's tables), or 分组：X (one group).
void MainFrame::RefreshOverviewTabTitle()
{
    if (!overviewStack_ || !overviewTab_) return;
    const int idx = editors_->GetPageIndex(overviewTab_);
    if (idx == wxNOT_FOUND) return;
    const int slot = overviewStack_->GetSelection();
    wxString title = (slot == 0) ? tr(L"总览") : tr(L"表信息");
    if (slot == 1 && !dbovGroup_.IsEmpty()) title = tr(L"分组：") + dbovGroup_;
    if (slot == 2)                          title = tr(L"分组：") + cgroupName_;
    editors_->SetPageText(idx, title);
}

// (Re)populate the table grid from the cached metadata, keeping only rows whose
// name contains `filter` (case-insensitive) AND — when a 分组 is being shown —
// which belong to that group. Runs on the GUI thread; it just re-renders
// already-fetched data, so typing in the filter never re-queries.
void MainFrame::RenderTableList(const wxString& filter)
{
    if (!tableList_) return;
    const wxString f = filter.Lower();
    // The 分组 filter is read fresh on every render rather than cached alongside
    // dbovMetas_, because a 移动到分组 / 从分组移除 changes membership WITHOUT
    // touching the metadata — re-rendering has to see the new answer, and that is
    // exactly how a removed table disappears from this list immediately.
    std::vector<wxString> inGroup;
    if (!dbovGroup_.IsEmpty() && dbovEntry_)
        inGroup = core::GroupStore::MembersOf(
            core::GroupStore::ObjectScope(dbovEntry_->profile.name, dbovDb_, L"tables"),
            dbovGroup_);
    auto memberOfGroup = [&](const wxString& name) {
        return std::find(inGroup.begin(), inGroup.end(), name) != inGroup.end();
    };

    tableList_->DeleteAllItems();
    long row = 0;
    for (const db::TableMeta& m : dbovMetas_) {
        if (!dbovGroup_.IsEmpty() && !memberOfGroup(m.name)) continue;
        if (!f.IsEmpty() && !m.name.Lower().Contains(f)) continue;
        const long idx = tableList_->InsertItem(row++, m.name);
        tableList_->SetItem(idx, 1, m.engine.IsEmpty() ? L"—" : m.engine);
        tableList_->SetItem(idx, 2, FmtCount(m.rows));
        tableList_->SetItem(idx, 3, m.columns < 0 ? L"—"
                                    : wxString::Format(L"%d", m.columns));
        tableList_->SetItem(idx, 4, FmtBytes(m.sizeBytes));
        tableList_->SetItem(idx, 5, m.autoIncrement >= 0 ? tr(L"是") : tr(L"否"));
        tableList_->SetItem(idx, 6, m.updatedAt.IsEmpty() ? L"—" : m.updatedAt);
        tableList_->SetItem(idx, 7, m.comment);
    }
    StretchTableColumns();
}

// Widen the last column (注释) so the columns fill the whole list width — no empty
// grey gutter to the right. Recomputed on every resize and re-render.
void MainFrame::StretchTableColumns()
{
    if (!tableList_) return;
    const int cols = tableList_->GetColumnCount();
    if (cols == 0) return;
    int used = 0;
    for (int c = 0; c < cols - 1; ++c) used += tableList_->GetColumnWidth(c);
    int last = tableList_->GetClientSize().GetWidth() - used;
    const int minLast = FromDIP(140);
    if (last < minLast) last = minLast;
    tableList_->SetColumnWidth(cols - 1, last);
}

std::vector<wxString> MainFrame::OverviewSelectedTables() const
{
    std::vector<wxString> out;
    if (!tableList_) return out;
    long i = tableList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    while (i >= 0) {
        out.push_back(tableList_->GetItemText(i));
        i = tableList_->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    }
    return out;
}

void MainFrame::OverviewOpenTable()
{
    const std::vector<wxString> sel = OverviewSelectedTables();
    if (sel.empty()) { SetStatusText(tr(L"请先在列表中选择一张表")); return; }
    if (dbovEntry_) OpenTableTab(dbovEntry_, dbovDb_, sel.front());   // dedicated data tab
}

void MainFrame::OverviewDesignTable()
{
    const std::vector<wxString> sel = OverviewSelectedTables();
    if (sel.empty()) { SetStatusText(tr(L"请先在列表中选择一张表")); return; }
    if (dbovEntry_) OpenDesignTab(dbovEntry_, dbovDb_, sel.front());   // per-table closable design tab
}

void MainFrame::OverviewNewTable()
{
    if (dbovEntry_) OpenNewTableTab(dbovEntry_, dbovDb_);   // a fresh CREATE-TABLE designer
}

void MainFrame::OverviewDeleteTables()
{
    if (!dbovEntry_) return;
    const std::vector<wxString> sel = OverviewSelectedTables();
    if (sel.empty()) { SetStatusText(tr(L"请先在列表中选择要删除的表")); return; }
    if (connTree_->DropTablesUi(dbovEntry_, dbovDb_, sel))
        ShowDatabaseOverview(dbovEntry_, dbovDb_, /*force*/ true);   // refresh the grid after the drops
}

// ---------------------------------------------------------------------------
// Empty-state placeholder for the query editor: white background, a centred
// magnifier-over-table illustration, a title, a hint, and a "新建SQL" CTA.
// Per docs/design/empty-state-spec.md.
wxWindow* MainFrame::BuildEmptyState(wxWindow* parent)
{
    auto* p = new wxPanel(parent);
    p->SetBackgroundColour(theme::kWhite);

    auto* col = new wxBoxSizer(wxVERTICAL);
    col->AddStretchSpacer(4);                         // vertical centre, slight upward bias

    auto* art = new EmptyArtPanel(p);                 // 96×96 illustration
    col->Add(art, 0, wxALIGN_CENTER_HORIZONTAL);

    auto* title = new wxStaticText(p, wxID_ANY, tr(L"开始你的查询"));
    title->SetForegroundColour(theme::kTextBody);     // #3F4652
    title->SetBackgroundColour(theme::kWhite);
    title->SetFont(Ui(16, true));                     // 16pt Semibold
    col->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 20);   // 96px art → 20px → title

    auto* hint = new wxStaticText(
        p, wxID_ANY, tr(L"双击左侧表名可生成查询，或点「新建SQL」手写 SQL"));
    hint->SetForegroundColour(theme::kSynOperator);   // #6B7280 (AA on white)
    hint->SetBackgroundColour(theme::kWhite);
    hint->SetFont(Ui(12.5, false));
    col->Add(hint, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 8);     // title → 8px → hint

    // CTA — primary blue, white text, ≥14pt Semibold (AA at large/bold: 3:1).
    // hover → kPrimaryDark for extra safety.
    auto* cta = new wxButton(p, wxID_ANY, tr(L"  新建SQL  "),
                             wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
    cta->SetBackgroundColour(theme::kPrimary);
    cta->SetForegroundColour(theme::kWhite);
    cta->SetFont(Ui(14, true));
    cta->SetBitmap(icons::Stroke(icons::Glyph::Code, 14, theme::kWhite, 1.9));
    cta->SetBitmapMargins(6, 0);
    cta->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { wxCommandEvent e; OnNewQuery(e); });
    cta->Bind(wxEVT_ENTER_WINDOW, [cta](wxMouseEvent& e) {
        cta->SetBackgroundColour(theme::kPrimaryDark); cta->Refresh(); e.Skip();
    });
    cta->Bind(wxEVT_LEAVE_WINDOW, [cta](wxMouseEvent& e) {
        cta->SetBackgroundColour(theme::kPrimary); cta->Refresh(); e.Skip();
    });
    col->Add(cta, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 20);     // hint → 20px → CTA

    col->AddStretchSpacer(5);
    p->SetSizer(col);
    return p;
}

} // namespace ui
