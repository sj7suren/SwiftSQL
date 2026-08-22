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
#include <memory>
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
#include "ui/EditorTabArt.h"
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
#include "ui/MainFrameInternal.h"

namespace ui {

// 连接/数据库 selector data source, bound to the connection tree. Declared in
// MainFrameInternal.h (linkage widened from file-local to ui:: like DeriveEditable)
// so ConfigurePage in MainFrame_Tabs.cpp can wire it onto every editor tab.
// Connection handles are opaque ConnEntry* re-validated via ct->isLive() on every
// use, so an editor tab that outlived its connection can never dereference a
// dangling pointer.
EditorPage::ObjectTarget MakeObjectTarget(ConnectionTree* ct)
{
    EditorPage::ObjectTarget t;
    t.connections = [ct]() -> std::vector<std::pair<wxString, void*>> {
        std::vector<std::pair<wxString, void*>> out;
        for (ConnEntry* e : ct->connectedEntries())
            out.emplace_back(e->profile.name, static_cast<void*>(e));
        return out;
    };
    t.databases = [ct](void* h) -> std::vector<wxString> {
        std::vector<wxString> out;
        auto* e = static_cast<ConnEntry*>(h);
        if (ct->isLive(e)) {
            std::vector<wxString> dbs; wxString err;
            if (e->conn->ListDatabases(dbs, err)) out = std::move(dbs);
        }
        return out;
    };
    t.setTarget = [ct](void* h, const wxString& dbName, wxString& err) -> bool {
        auto* e = static_cast<ConnEntry*>(h);
        if (!ct->isLive(e)) { err = tr(L"连接已断开"); return false; }
        ct->setActive(e);      // reuse the existing run path (active conn + currentDb)
        e->currentDb = dbName;
        // MySQL / SQL Server keep a session "current database"; the object template's
        // CREATE is unqualified, so without a USE it errors "No database selected".
        // (PG/SQLite/Oracle can't switch db mid-session — db is per-connection / schema.)
        if (!dbName.IsEmpty()) {
            const db::Dialect d = e->conn->GetDialect();
            if (d == db::Dialect::MySQL || d == db::Dialect::SqlServer) {
                db::QueryResult r; wxString uerr;
                e->conn->Execute(L"USE " + db::QuoteIdent(dbName, d), r, uerr);
            }
        }
        return true;
    };
    return t;
}

namespace {

// ---------------------------------------------------------------------------
// Windows-style caption button (min / max-restore / close) for the frameless
// header. Self-drawn so we control the hover states precisely: the close button
// turns red-on-white on hover (Windows convention); the others get a light grey
// hover. Kept tiny and dependency-free — just fires a callback on click.
class CaptionButton : public wxWindow {
public:
    enum Kind { Min, Max, Close };
    CaptionButton(wxWindow* parent, Kind kind, std::function<void()> onClick)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(46, 44)),
          kind_(kind), onClick_(std::move(onClick))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(46, 44));
        Bind(wxEVT_PAINT, &CaptionButton::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true;  Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; pressed_ = false; Refresh(); });
        Bind(wxEVT_LEFT_DOWN,    [this](wxMouseEvent&) { pressed_ = true;  Refresh(); });
        Bind(wxEVT_LEFT_UP,      [this](wxMouseEvent& e) {
            const bool wasPressed = pressed_;
            pressed_ = false; Refresh();
            if (wasPressed && GetClientRect().Contains(e.GetPosition()) && onClick_) onClick_();
        });
    }
    void SetMaximized(bool m) { if (maxed_ != m) { maxed_ = m; Refresh(); } }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        wxColour bg = theme::kWhite;   // header is white
        wxColour fg = theme::kText;
        if (hover_ || pressed_) {
            if (kind_ == Close) { bg = wxColour(0xE8, 0x11, 0x23); fg = *wxWHITE; }
            else                 bg = pressed_ ? wxColour(0xCF, 0xD3, 0xD9)
                                              : wxColour(0xE1, 0xE5, 0xEB);
        }
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bg));
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        dc.SetPen(wxPen(fg, 1));
        const int cx = sz.x / 2, cy = sz.y / 2;
        if (kind_ == Min) {
            dc.DrawLine(cx - 5, cy, cx + 5, cy);
        } else if (kind_ == Max) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            if (maxed_) {                        // restore glyph: two offset squares
                dc.DrawRectangle(cx - 2, cy - 5, 7, 7);   // back square
                dc.SetBrush(wxBrush(bg));                  // front square masks the overlap
                dc.DrawRectangle(cx - 5, cy - 2, 7, 7);
            } else {
                dc.DrawRectangle(cx - 5, cy - 5, 10, 10);
            }
        } else { // Close — an X
            dc.DrawLine(cx - 5, cy - 5, cx + 5, cy + 5);
            dc.DrawLine(cx - 5, cy + 5, cx + 5, cy - 5);
        }
    }

    Kind kind_;
    std::function<void()> onClick_;
    bool hover_ = false, pressed_ = false, maxed_ = false;
};

// ---------------------------------------------------------------------------
// "AI 生成 SQL" action button — a filled, gradient rounded-pill matching the
// prototype (docs/UI/SwiftSql.dc.html:274): a vertical blue gradient
// (#3585EE → #2C7BE5), 10px radius, a soft blue drop shadow, a white filled star +
// white bold label. wxButton cannot paint gradients or rounded corners, so this is
// a self-drawn wxWindow (like CaptionButton above), sized to its text. hover
// brightens the gradient, press darkens it; a click fires the supplied callback.
class AiButton : public wxWindow {
public:
    AiButton(wxWindow* parent, const wxString& label, std::function<void()> onClick)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          label_(label), onClick_(std::move(onClick)),
          star_(icons::Stroke(icons::Glyph::Star, kIcon, theme::kWhite))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        wxClientDC dc(this);
        dc.SetFont(LabelFont());
        const wxSize ts = dc.GetTextExtent(label_);
        const int w = kPadX * 2 + kIcon + kGap + ts.x;
        SetMinSize(wxSize(w, kHeight));
        SetInitialSize(wxSize(w, kHeight));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &AiButton::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true;  Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; pressed_ = false; Refresh(); });
        Bind(wxEVT_LEFT_DOWN,    [this](wxMouseEvent&) { pressed_ = true;  Refresh(); });
        Bind(wxEVT_LEFT_UP,      [this](wxMouseEvent& e) {
            const bool wasPressed = pressed_;
            pressed_ = false; Refresh();
            if (wasPressed && GetClientRect().Contains(e.GetPosition()) && onClick_) onClick_();
        });
    }

private:
    static constexpr int kIcon = 15, kGap = 8, kPadX = 16, kHeight = 36, kRadius = 10;

    static wxFont LabelFont() { return Ui(10, /*bold*/ true); }   // ~700 weight, ~12px
    static wxColour Lighten(const wxColour& c) {
        return wxColour(std::min(255, c.Red() + 18), std::min(255, c.Green() + 18),
                        std::min(255, c.Blue() + 18));
    }
    static wxColour Darken(const wxColour& c) {
        return wxColour(int(c.Red() * 0.88), int(c.Green() * 0.88), int(c.Blue() * 0.88));
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(theme::kChromeBg));   // blend into the toolbar grey
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        const wxSize sz = GetClientSize();
        const double x = 0.5, top = 1.0, bodyH = sz.y - 6.0, w = sz.x - 1.0;

        // Soft blue shadow — an approximation of box-shadow 0 4px 12px -3px
        // rgba(44,123,229,.5): a translucent blue rounded rect nudged 4px down, under
        // the pill. Two stacked translucent layers fake the blur's soft falloff.
        {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(0x2C, 0x7B, 0xE5, 40)));
            wxGraphicsPath s1 = gc->CreatePath();
            s1.AddRoundedRectangle(x - 1, top + 5, w + 2, bodyH, kRadius + 1);
            gc->DrawPath(s1);
            gc->SetBrush(wxBrush(wxColour(0x2C, 0x7B, 0xE5, 60)));
            wxGraphicsPath s2 = gc->CreatePath();
            s2.AddRoundedRectangle(x + 1, top + 4, w - 2, bodyH, kRadius);
            gc->DrawPath(s2);
        }

        // Vertical gradient body #3585EE → #2C7BE5 (brighten on hover / darken on press).
        wxColour cTop(0x35, 0x85, 0xEE), cBot(0x2C, 0x7B, 0xE5);
        if (pressed_)      { cTop = Darken(cTop);  cBot = Darken(cBot); }
        else if (hover_)   { cTop = Lighten(cTop); cBot = Lighten(cBot); }
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateLinearGradientBrush(0, top, 0, top + bodyH, cTop, cBot));
        wxGraphicsPath body = gc->CreatePath();
        body.AddRoundedRectangle(x, top, w, bodyH, kRadius);
        gc->DrawPath(body);

        // Content: white star + gap + white bold label, centred as a group in the body.
        gc->SetFont(LabelFont(), theme::kWhite);
        double tw = 0, th = 0; gc->GetTextExtent(label_, &tw, &th);
        const double contentW = kIcon + kGap + tw;
        const double cx = (sz.x - contentW) / 2;
        gc->DrawBitmap(star_, cx, top + (bodyH - kIcon) / 2, kIcon, kIcon);
        gc->DrawText(label_, cx + kIcon + kGap, top + (bodyH - th) / 2);
    }

    wxString label_;
    std::function<void()> onClick_;
    wxBitmap star_;
    bool hover_ = false, pressed_ = false;
};

// The editor notebook's tab art (flat grey strip, blue-accented selected tab, a
// close X only on the hovered tab) lives in EditorTabArt.h/.cpp — it grew too long
// to keep inline here (single-file ≤1000-line charter rule).

// 标签右键"设置颜色"用的预设色板 + 色块(与连接树 kConnColors 同风格,那份在
// ConnectionTree.cpp 的匿名命名空间里取不到,这里另置一份)。
struct TabColor { const wchar_t* name; const wchar_t* hex; };
constexpr TabColor kTabColors[] = {
    { L"红色", L"#E5484D" }, { L"橙色", L"#F76B15" }, { L"黄色", L"#FFB224" },
    { L"绿色", L"#30A46C" }, { L"蓝色", L"#2C7BE5" }, { L"紫色", L"#8E4EC6" },
    { L"灰色", L"#8B8D98" },
};
wxBitmap TabColorSwatch(const wxColour& c, int size = 14)
{
    wxBitmap bm(size, size);
    wxMemoryDC dc(bm);
    dc.SetBackground(wxBrush(c));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(0, 0, 0, 40)));   // faint outline so light swatches read
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, size, size);
    dc.SelectObject(wxNullBitmap);
    return bm;
}

} // namespace

// ---------------------------------------------------------------------------
// Header row (topmost — this IS the title bar now that the window is frameless):
// SwiftSQL logo on the left, the view-switch pill PAINTED centred against the
// whole window width, then search + the Windows-style min/max/close buttons on
// the right. Empty areas act as the OS caption (drag / double-click-maximize /
// Aero Snap) via a native WM_NCLBUTTONDOWN hand-off. The pill holds the centre
// alone, so it can never be pushed aside. Matches docs/UI/SwiftSql.dc.html:184.
// (Pill segments now OPEN/FOCUS the matching editors_ tab — see ActivatePillSegment
// — rather than switch a mainBook_ view; 表设计/ER 图 are per-tab now.)
wxWindow* MainFrame::BuildHeaderBar(wxWindow* parent)
{
    auto* bar = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 44));
    bar->SetMinSize(wxSize(-1, 44));
    bar->SetBackgroundColour(theme::kWhite);
    bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
    viewBar_ = bar;

    // Hand a press on empty header space to the OS as a title-bar drag: this is
    // what preserves native move + Aero Snap (edge-snap / shake) for free.
    auto beginDrag = [this]() {
#ifdef __WXMSW__
        ::ReleaseCapture();
        ::SendMessage((HWND)GetHandle(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
#endif
    };

    // The pill is drawn (and hit-tested) by the header itself; the centre region
    // has no child windows over it, so the painted pill shows and clicks land.
    bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { PaintViewSwitch(); });
    bar->Bind(wxEVT_LEFT_DOWN, [this, beginDrag](wxMouseEvent& ev) {
        for (int i = 0; i < 3; ++i)
            if (tabRects_[i].Contains(ev.GetPosition())) {   // pill segment → open/focus its tab
                ActivatePillSegment(i);
                return;
            }
        beginDrag();                                          // else → drag the window
    });
    bar->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { Maximize(!IsMaximized()); });
    // Repaint the centred pill whenever the row is resized (its x0 depends on width).
    bar->Bind(wxEVT_SIZE, [bar](wxSizeEvent& ev) { bar->Refresh(); ev.Skip(); });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->AddSpacer(14);
    auto* logo = new wxStaticBitmap(bar, wxID_ANY, icons::AppLogo(18));
    logo->SetBackgroundColour(theme::kWhite);
    row->Add(logo, 0, wxALIGN_CENTER_VERTICAL);

    // Wordmark: "Swift" in ink + a colourful "SQL" (blue / green / amber).
    struct Part { const wchar_t* t; wxColour c; };
    const Part parts[] = {
        { L"Swift", theme::kText },
        { L"S", theme::kPrimary },
        { L"Q", theme::kGreen },
        { L"L", theme::kDotAmber },
    };
    std::vector<wxWindow*> dragKids = { logo };
    for (size_t i = 0; i < 4; ++i) {
        auto* seg = new wxStaticText(bar, wxID_ANY, parts[i].t);
        seg->SetForegroundColour(parts[i].c);
        seg->SetBackgroundColour(theme::kWhite);
        seg->SetFont(Ui(10.5, true));
        row->Add(seg, 0, wxALIGN_CENTER_VERTICAL | (i == 0 ? wxLEFT : 0), i == 0 ? 8 : 0);
        dragKids.push_back(seg);
    }
    // Logo + title segments are child windows that would otherwise swallow the
    // press, so let dragging (and double-click maximize) start from them too.
    for (wxWindow* w : dragKids) {
        w->Bind(wxEVT_LEFT_DOWN,  [beginDrag](wxMouseEvent&) { beginDrag(); });
        w->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { Maximize(!IsMaximized()); });
    }

    row->AddStretchSpacer(1);   // empty centre — the pill is painted here, no children

    auto* search = new wxBitmapButton(
        bar, wxID_ANY, icons::Stroke(icons::Glyph::Search, 18, theme::kTextSecondary),
        wxDefaultPosition, wxSize(34, 34), wxBORDER_NONE);
    search->SetBackgroundColour(theme::kWhite);
    search->SetToolTip(tr(L"搜索"));
    row->Add(search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    // Windows-style caption buttons, flush to the top-right corner (close last).
    auto* minBtn = new CaptionButton(bar, CaptionButton::Min,   [this] { Iconize(true); });
    auto* maxBtn = new CaptionButton(bar, CaptionButton::Max,   [this] { Maximize(!IsMaximized()); });
    auto* clsBtn = new CaptionButton(bar, CaptionButton::Close, [this] { Close(); });
    maxBtn->SetMaximized(IsMaximized());
    row->Add(minBtn, 0, wxEXPAND);
    row->Add(maxBtn, 0, wxEXPAND);
    row->Add(clsBtn, 0, wxEXPAND);

    // Keep the max/restore glyph in sync when the state changes elsewhere
    // (double-click, Aero Snap, Win+Up, taskbar).
    Bind(wxEVT_SIZE, [this, maxBtn](wxSizeEvent& ev) {
        maxBtn->SetMaximized(IsMaximized());
        ev.Skip();
    });

    bar->SetSizer(row);
    return bar;
}

// ---------------------------------------------------------------------------
// Toolbar row (below the header): compact icon tool buttons on the left, AI
// button on the right. No pill here — that lives one row up in the header.
wxWindow* MainFrame::BuildToolRow(wxWindow* parent)
{
    auto* bar = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 66));
    bar->SetMinSize(wxSize(-1, 66));
    bar->SetBackgroundColour(theme::kChromeBg);
    bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
    // light-grey fill (same as the menu row) + a 1px bottom border.
    bar->Bind(wxEVT_PAINT, [bar](wxPaintEvent&) {
        wxPaintDC dc(bar);
        const wxSize s = dc.GetSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(theme::kChromeBg));
        dc.DrawRectangle(0, 0, s.x, s.y);
        dc.SetPen(wxPen(theme::kBorder, 1));
        dc.DrawLine(0, s.y - 1, s.x, s.y - 1);
    });
    // Blank space in the tool row drags the window (tool buttons are children with
    // their own handlers, so this only fires on the empty strip). Not Skip()ed — the
    // native drag is a synchronous hand-off, so it can't conflict with the PAINT above.
    bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { BeginWindowDrag(); });

    using icons::Glyph;
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    // Navicat-style tool button: icon on top, text label below. `dropdown` adds a
    // ▾ caret so buttons that pop a menu read as dropdowns.
    auto iconBtn = [&](int id, const wxString& label, Glyph g, const wxColour& c,
                       bool dropdown = false) {
        auto* b = new wxButton(bar, id, dropdown ? (label + L" ▾") : label,
                               wxDefaultPosition, wxDefaultSize,
                               wxBU_EXACTFIT | wxBORDER_NONE);
        b->SetBitmap(icons::Stroke(g, 25, c));   // larger icon
        b->SetBitmapPosition(wxTOP);             // icon above text
        b->SetBitmapMargins(0, 2);
        b->SetBackgroundColour(theme::kChromeBg);
        b->SetForegroundColour(theme::kText);    // brighter / clearer than kTextSecondary
        b->SetFont(Ui(9.5));                     // larger label
        b->SetToolTip(label);
        // hover feedback coordinated with the chrome grey.
        b->Bind(wxEVT_ENTER_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kChromeBtnHover); b->Refresh(); e.Skip();
        });
        b->Bind(wxEVT_LEAVE_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(theme::kChromeBg); b->Refresh(); e.Skip();
        });
        row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    };
    auto sep = [&]() {
        auto* line = new wxStaticLine(bar, wxID_ANY, wxDefaultPosition,
                                      wxSize(1, 40), wxLI_VERTICAL);
        row->Add(line, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
    };

    row->AddSpacer(8);
    iconBtn(ID_NEW_CONNECTION, tr(L"新建连接"), Glyph::Link, theme::kPrimary, true);
    iconBtn(ID_NEW_QUERY, tr(L"新建SQL"), Glyph::Code, theme::kDotPurple);
    iconBtn(ID_QUERY_BUILDER, tr(L"构建查询"), Glyph::ErDiagram, theme::kAccent);
    sep();
    iconBtn(ID_RUN_QUERY, tr(L"运行"), Glyph::Play, theme::kGreen);
    iconBtn(ID_STOP_QUERY, tr(L"停止"), Glyph::Stop, theme::kDotRed);
    if (wxWindow* s = bar->FindWindow(ID_STOP_QUERY)) s->Enable(false);   // nothing to stop yet
    iconBtn(ID_FORMAT_SQL, tr(L"美化"), Glyph::Beautify, theme::kDotPurple);
    iconBtn(ID_EXPLAIN_SQL, tr(L"解释"), Glyph::Explain, theme::kTextSecondary);
    // 性能分析 — next to 解释 because they answer neighbouring questions:
    // 解释 runs a raw EXPLAIN into the grid, this one has the model READ that
    // plan (plus the schema) and say what is slow and what index would fix it.
    iconBtn(ID_AI_PERF, tr(L"性能分析"), Glyph::StarLine, theme::kDotAmber);
    iconBtn(ID_TOOL_AI, tr(L"AI 助手"), Glyph::Star, theme::kPrimary);   // → OpenAiPlaceholderTab (AI 聊天页)
    sep();
    iconBtn(ID_TOOL_TABLE, tr(L"表"), Glyph::Table, theme::kGreen);
    iconBtn(ID_TOOL_VIEW, tr(L"视图"), Glyph::ViewLayers, theme::kAccent);
    iconBtn(ID_TOOL_FUNC, tr(L"函数"), Glyph::Function, theme::kDotAmber);
    iconBtn(ID_TOOL_PROC, tr(L"存储过程"), Glyph::Code, theme::kDotPurple);
    sep();
    // Automation: opens the scheduled-jobs list tab (sync / export jobs with an
    // in-app timer schedule).
    iconBtn(ID_TOOL_AUTORUN, tr(L"Automation"), Glyph::Timer, theme::kAccent);

    row->AddStretchSpacer(1);   // empty centre — the floating pill is painted here

    // Self-drawn gradient pill (see AiButton): opens the "AI 功能建设中" placeholder
    // tab, same as the AI pill segment (AI generation is still under construction).
    auto* ai = new AiButton(bar, tr(L"AI 生成 SQL"), [this] { OpenAiPlaceholderTab(); });
    row->Add(ai, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    bar->SetSizer(row);

    // ---- bindings: same handlers as the old toolbar, now on wxEVT_BUTTON ----
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { connTree_->ShowNewConnectionMenu(); },
         ID_NEW_CONNECTION);
    Bind(wxEVT_BUTTON, &MainFrame::OnNewQuery, this, ID_NEW_QUERY);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenQueryBuilder(); }, ID_QUERY_BUILDER);
    // 表/视图/函数/存储过程 → an object-list tab on the active connection + database.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenObjectListTab(ObjectListKind::Tables); },     ID_TOOL_TABLE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenObjectListTab(ObjectListKind::Views); },      ID_TOOL_VIEW);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenObjectListTab(ObjectListKind::Functions); },  ID_TOOL_FUNC);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenObjectListTab(ObjectListKind::Procedures); }, ID_TOOL_PROC);
    Bind(wxEVT_BUTTON, &MainFrame::OnRunQuery, this, ID_RUN_QUERY);
    Bind(wxEVT_MENU,   &MainFrame::OnRunQuery, this, ID_RUN_QUERY);   // F5 accelerator fires MENU
    Bind(wxEVT_BUTTON, &MainFrame::OnExplain, this, ID_EXPLAIN_SQL);
    // 性能分析 acts on the active editor, which owns its own AI panel — the whole
    // flow (EXPLAIN, prompt, streaming answer) lives there, so MainFrame only
    // forwards. Works for an object editor (函数/存储过程) too: it is the same class.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (EditorPage* p = ActivePage()) p->AnalyzePerformance();
    }, ID_AI_PERF);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenAiPlaceholderTab(); }, ID_TOOL_AI);   // AI 聊天页
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (EditorPage* p = ActivePage()) p->FormatActive();
    }, ID_FORMAT_SQL);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!queryRunning_) return;
        // Cancel the connection the WORKER actually runs on (workerConn_), not
        // connTree_->active(): they diverge once the user switches connection/tab while
        // a query is stuck, and cancelling active() would leave the stuck query running.
        if (workerConn_) { workerConn_->Cancel(); SetStatusText(tr(L"已请求停止查询")); }
    }, ID_STOP_QUERY);

    // Automation button → open the scheduled-jobs list tab.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenAutomationTab(); }, ID_TOOL_AUTORUN);

    // Transaction handlers — fire from the overflow menu (wxEVT_MENU) AND from the
    // data-grid toolbar buttons (wxEVT_BUTTON), which share the same IDs.
    Bind(wxEVT_MENU,   [this](wxCommandEvent&) { RunTxn(L"BEGIN", tr(L"事务已开始")); }, ID_TX_BEGIN);
    Bind(wxEVT_MENU,   [this](wxCommandEvent&) { RunTxn(L"COMMIT", tr(L"事务已提交")); }, ID_TX_COMMIT);
    Bind(wxEVT_MENU,   [this](wxCommandEvent&) { RunTxn(L"ROLLBACK", tr(L"事务已回滚")); }, ID_TX_ROLLBACK);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunTxn(L"BEGIN", tr(L"事务已开始")); }, ID_TX_BEGIN);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunTxn(L"COMMIT", tr(L"事务已提交")); }, ID_TX_COMMIT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunTxn(L"ROLLBACK", tr(L"事务已回滚")); }, ID_TX_ROLLBACK);

    return bar;
}

// ---------------------------------------------------------------------------
wxWindow* MainFrame::BuildSidebar(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    panel->SetBackgroundColour(theme::kWhite);   // brighter sidebar (was kSidebarBg grey)
    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- search ----
    auto* search = new wxSearchCtrl(panel, wxID_ANY);
    search->SetDescriptiveText(tr(L"搜索表、视图…"));
    root->Add(search, 0, wxEXPAND | wxALL, 10);

    // ---- connection tree — owned by ConnectionTree; view-level actions come
    // back to us via hooks ----
    ConnectionTree::Hooks hooks;
    hooks.status = [this](const wxString& s) { SetStatusText(s); };
    hooks.beforeClose = [this](ConnEntry*) { JoinWorker(true); JoinDbWorker(); JoinCountWorker(); };
    // 任何断开/删除都会先调这个(不受 active_ 约束):表列表可能正显示这个 entry。
    // 先停掉可能正查它 conn 的 worker,再清空引用,避免 conn 释放后 UAF。
    hooks.entryClosing = [this](ConnEntry* e) {
        db::IConnection* c = e ? e->conn.get() : nullptr;
        // Join EVERY background worker still bound to THIS connection before its conn
        // is freed — keyed on the conn the worker actually holds, NOT on which entry is
        // active_/dbovEntry_. Those can diverge (a query runs on A while the tree/overview
        // shows B); the old dbovEntry_==e gate missed workers on non-displayed conns and
        // never joined the SQL worker_ at all → use-after-free on e->conn.reset().
        if (c && workerConn_ == c) JoinWorker(true);   // async SQL 执行 worker
        if (c && dbLoadConn_ == c) JoinDbWorker();     // 总览元数据加载 worker
        if (c && countConn_  == c) JoinCountWorker();  // 分页 COUNT worker
        // 固定"总览"标签的两半都可能正显示这个 entry —— 分别清空:
        if (dbovEntry_ == e) ClearDbOverview();          // 表列表半
        if (ovEntry_ == e)   ClearConnectionOverview();  // 连接库列表半
        // 分组内连接列表(slot 2)可能正列着这个 entry。它按名字重新读取,所以只
        // 需重绘:那一行会消失,而不是留着一行点了什么都不会发生的死行。
        if (!cgroupName_.IsEmpty()) RenderConnGroupList();
        CloseTabsForEntry(e);   // 关掉属于该连接的设计/ER 标签,否则它们持悬空 conn_ → 崩
    };
    hooks.selectTable = [this](ConnEntry*, const wxString&, const wxString& table) {
        selTable_ = table;   // 树里单击选表 → 同步当前表,供"查看→表设计"菜单用
    };
    hooks.showOverview = [this](ConnEntry* e) { ShowConnectionOverview(e); };
    hooks.openPreferences = [this]() { wxCommandEvent e; OnPreferences(e); };
    hooks.showDatabase = [this](ConnEntry* e, const wxString& db, bool focus) {
        ShowDatabaseOverview(e, db);            // 只更新共享"表列表"标签内容
        if (focus) SelectDbOverviewTab();        // 双击/展开才把标签切到前面
    };
    // 点表分组 → 同一个固定标签只显示该分组的表;点数据库会清掉这个过滤器回到整库。
    hooks.showTableGroup = [this](ConnEntry* e, const wxString& db,
                                  const wxString& group, bool focus) {
        ShowTableGroupOverview(e, db, group);
        if (focus) SelectDbOverviewTab();
    };
    // 点连接分组 → 固定标签切到"分组内的连接列表"(slot 2)。
    hooks.showConnGroup = [this](const wxString& group, bool focus) {
        ShowConnGroupOverview(group);
        if (focus) SelectDbOverviewTab();
    };
    hooks.clearTables = [this](ConnEntry* e, const wxString& db, bool force) {
        // 单击未打开的库(force) → 无条件清空;关闭某个库(!force) → 仅当它正是当前
        // 显示的库时才清,免得关闭 B 却把正在看的 A 也清了。
        if (force || (dbovEntry_ == e && dbovDb_ == db))
            ClearDbOverview();
    };
    hooks.newQuery = [this](ConnEntry*, const wxString&) {
        wxCommandEvent d; OnNewQuery(d);
    };
    hooks.newQueryWith = [this](ConnEntry*, const wxString&, const wxString& sql) {
        wxCommandEvent d; OnNewQuery(d);           // fresh tab (becomes active)
        if (EditorPage* p = ActivePage()) p->InsertQuery(sql);
        SwitchView(View::Query);
    };
    hooks.newQueryRun = [this](ConnEntry*, const wxString&, const wxString& sql) {
        // 执行 a routine: open a fresh query tab, prefill the CALL/SELECT, and run it
        // so the outcome shows in the shared results grid (same path as F5).
        wxCommandEvent d; OnNewQuery(d);
        if (EditorPage* p = ActivePage()) {
            p->InsertQuery(sql);
            SwitchView(View::Query);
            RunSql(p, sql);
        }
    };
    hooks.newObjectEditor = [this](ConnEntry* e, const wxString& db, const wxString& title,
                                   const wxString& sql, ObjectKind kind) {
        // A SQL editor tab prefilled with an object CREATE template / DDL; flagged
        // so a successful run renames the tab to the object name (see RunSql).
        auto* page = new EditorPage(editors_, sql);
        page->MarkObjectEditor(true, kind);
        ConfigurePage(page);
        // F10 in a 函数/存储过程 editor → parameter dialog + execute. Kept off the
        // editor's back so it never depends on ConnectionTree: it hands the chosen
        // (connection, db, kind, name) here and we reuse ExecuteRoutine (dialog+run).
        page->SetRunRoutineHook([this](void* conn, const wxString& tgtDb,
                                       ObjectKind k, const wxString& name) {
            connTree_->ExecuteRoutineUi(static_cast<ConnEntry*>(conn), tgtDb, name,
                                        k == ObjectKind::Procedure);
        });
        // Top 连接/数据库 selector; default to the connection + db it was opened on.
        page->SetObjectTarget(MakeObjectTarget(connTree_.get()),
                              static_cast<void*>(e), db);
        editors_->AddPage(page, title, /*select*/ true,
                          icons::Stroke(icons::Glyph::Code, 14, theme::kPrimary));
        UpdateQueryView();
        SwitchView(View::Query);
    };
    hooks.openTable = [this](ConnEntry* e, const wxString& db, const wxString& table) {
        OpenTableTab(e, db, table);   // dedicated data tab (no SQL editor)
    };
    hooks.designTable = [this](ConnEntry* e, const wxString& db, const wxString& table) {
        OpenDesignTab(e, db, table);   // per-table closable design tab
    };
    connTree_ = std::make_unique<ConnectionTree>(panel, this, std::move(hooks));
    root->Add(connTree_->widget(), 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    // Wire the search box to the tree's incremental search. `search` was created
    // above (before the tree), so bind here — now that connTree_ exists. Typing
    // (EVT_TEXT) jumps to the first match; Enter / the 🔍 button (EVT_SEARCH)
    // advances to the next match (wraps); the × cancel button clears + restores.
    // Every lambda re-checks connTree_ so a null can never be dereferenced. Only
    // already-loaded nodes are searched (lazy load — see ConnectionTree::SearchJump).
    search->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        if (connTree_) connTree_->SearchJump(e.GetString(), /*next*/false);
    });
    search->Bind(wxEVT_SEARCH, [this, search](wxCommandEvent&) {
        if (connTree_) connTree_->SearchJump(search->GetValue(), /*next*/true);
    });
    search->Bind(wxEVT_SEARCH_CANCEL, [this, search](wxCommandEvent&) {
        search->ChangeValue(wxString{});                 // clear without re-firing EVT_TEXT
        if (connTree_) connTree_->SearchJump(wxString{}, /*next*/false);
    });

    panel->SetSizer(root);
    return panel;
}

// ---------------------------------------------------------------------------
wxWindow* MainFrame::BuildMainArea(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    panel->SetBackgroundColour(theme::kWhite);
    auto* v = new wxBoxSizer(wxVERTICAL);

    // (view switch now lives centred in the top toolbar — see BuildToolBar)

    // three stacked views
    mainBook_ = new wxSimplebook(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxBORDER_NONE);
    mainBook_->SetBackgroundColour(theme::kWhite);

    // Query slot is itself a 2-page stack: [0] the editor notebook, [1] the empty
    // -state placeholder. UpdateQueryView() flips between them by tab count.
    queryStack_ = new wxSimplebook(mainBook_, wxID_ANY, wxDefaultPosition,
                                   wxDefaultSize, wxBORDER_NONE);
    queryStack_->SetBackgroundColour(theme::kWhite);

    editors_ = new wxAuiNotebook(queryStack_, wxID_ANY, wxDefaultPosition,
                                 wxDefaultSize,
                                 wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE |
                                 wxAUI_NB_CLOSE_ON_ALL_TABS | wxAUI_NB_SCROLL_BUTTONS |
                                 wxBORDER_NONE);
    editors_->SetBackgroundColour(theme::kWhite);
    // Fully self-drawn tab art (EditorTabArt): flat grey #EEF0F4 strip with a 1px
    // bottom rule, rectangular tabs, a 2px blue top accent + blue dot on the selected
    // tab, grey icon/text otherwise, and a close X that shows only on the hovered tab.
    auto* tabArt = new EditorTabArt;
    tabArt->SetColorMap(&tabColors_);              // 项4:每标签自定义色(转为强调色/图标着色)
    tabArt->SetSpecialTabPtr(&overviewTab_);       // 固定"总览"标签:不可关闭,永不给命中关闭矩形
    editors_->SetArtProvider(tabArt);

    // THE real full-width (165) hairline: wxAuiSimpleTabArt only draws a base line
    // at the *bottom* of the strip (suppressed by EditorTabArt) plus per-tab
    // outlines (tab-width only) — neither is a full-width line at the *top*. That
    // line is the pane border drawn by wxAuiNotebook's INTERNAL wxAuiManager
    // (dock art), which frames the tab-frame pane and lands right at the top seam.
    // Zero the dock-art pane-border metric to remove it completely.
    {
        wxAuiManager& mgr = const_cast<wxAuiManager&>(editors_->GetAuiManager());
        if (wxAuiDockArt* da = mgr.GetArtProvider()) {
            da->SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 0);
            da->SetColour(wxAUI_DOCKART_BORDER_COLOUR, theme::kWhite);
        }
        mgr.Update();
    }
    // No default query tab — first launch shows the empty state. A tab is created
    // on demand (CTA / 新建SQL / double-click a table).
    placeholder_ = BuildEmptyState(queryStack_);
    queryStack_->AddPage(editors_, L"editors");      // page 0
    queryStack_->AddPage(placeholder_, L"empty");    // page 1

    // Unsaved-edit guard (design decision B): veto a tab close when it has a
    // pending edit diff and the user chooses 取消.
    editors_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& ev) {
        wxWindow* w = editors_->GetPage(ev.GetSelection());
        if (w == overviewTab_) {                           // 固定"总览"标签不可关闭
            ev.Veto();
            SetStatusText(tr(L"总览标签不可关闭"));         // 给个即时反馈,而非静默
            return;
        }
        if (auto* t = dynamic_cast<IQueryTab*>(w)) {
            if (!t->ConfirmClose()) { ev.Veto(); return; }
        } else if (auto* dv = dynamic_cast<TableDesignView*>(w)) {
            if (!dv->ConfirmClose()) { ev.Veto(); return; }   // 未存结构改动 → 确认
        }
        // 标签要关了,顺手清掉它的登记(自定义色 / 数据 / 设计 / ER 标签归属表)。
        tabColors_.erase(w);
        dataTabs_.erase(w);
        designTabs_.erase(w);
        erTabs_.erase(w);
        objListTabs_.erase(w);
        userTabs_.erase(w);
        if (w == scriptLibTab_) { scriptLibTab_ = nullptr; scriptLib_ = nullptr; }  // 脚本库单例可复用
        if (w == automationTab_) { automationTab_ = nullptr; automationList_ = nullptr; }  // Automation 单例可复用
        if (w == aiTab_) aiTab_ = nullptr;   // "AI 功能建设中"单例可复用
    });
    // 项4:标签右键菜单 —— 关闭当前/右侧/左侧/其他 + 设置颜色。
    editors_->Bind(wxEVT_AUINOTEBOOK_TAB_RIGHT_UP, [this](wxAuiNotebookEvent& ev) {
        const int clicked = ev.GetSelection();
        if (clicked < 0) return;
        wxWindow* clickedWin = editors_->GetPage(clicked);

        // 关闭一批页(下标集合):跳过"表列表"标签,关前确认未存改动。
        auto closePages = [this](std::vector<int> idxs) {
            std::sort(idxs.begin(), idxs.end(), std::greater<int>());  // 从高到低,避免下标位移
            for (int i : idxs) {
                wxWindow* w = editors_->GetPage(i);
                if (w == overviewTab_) continue;               // 永不关闭固定总览标签
                if (auto* t = dynamic_cast<IQueryTab*>(w)) {
                    if (!t->ConfirmClose()) continue;          // 用户取消 → 跳过
                } else if (auto* dv = dynamic_cast<TableDesignView*>(w)) {
                    if (!dv->ConfirmClose()) continue;         // 未存结构改动 → 确认
                }
                tabColors_.erase(w);
                dataTabs_.erase(w);
                designTabs_.erase(w);
                erTabs_.erase(w);
                objListTabs_.erase(w);
                userTabs_.erase(w);
                if (w == scriptLibTab_) { scriptLibTab_ = nullptr; scriptLib_ = nullptr; }
                if (w == automationTab_) { automationTab_ = nullptr; automationList_ = nullptr; }
                if (w == aiTab_) aiTab_ = nullptr;
                editors_->DeletePage(i);
            }
            UpdateQueryView();
            RefreshBottomBar();
        };

        const int count = static_cast<int>(editors_->GetPageCount());
        wxMenu m;

        wxMenuItem* mCur = m.Append(wxID_ANY, tr(L"关闭当前标签"));
        mCur->Enable(clickedWin != overviewTab_);
        m.Bind(wxEVT_MENU, [closePages, clicked](wxCommandEvent&) {
            closePages({ clicked });
        }, mCur->GetId());

        wxMenuItem* mRight = m.Append(wxID_ANY, tr(L"关闭右侧所有"));
        m.Bind(wxEVT_MENU, [closePages, clicked, count](wxCommandEvent&) {
            std::vector<int> v; for (int i = clicked + 1; i < count; ++i) v.push_back(i);
            closePages(v);
        }, mRight->GetId());

        wxMenuItem* mLeft = m.Append(wxID_ANY, tr(L"关闭左侧所有"));
        m.Bind(wxEVT_MENU, [closePages, clicked](wxCommandEvent&) {
            std::vector<int> v; for (int i = 0; i < clicked; ++i) v.push_back(i);
            closePages(v);
        }, mLeft->GetId());

        wxMenuItem* mOther = m.Append(wxID_ANY, tr(L"关闭其他标签"));
        m.Bind(wxEVT_MENU, [closePages, clicked, count](wxCommandEvent&) {
            std::vector<int> v; for (int i = 0; i < count; ++i) if (i != clicked) v.push_back(i);
            closePages(v);
        }, mOther->GetId());

        m.AppendSeparator();

        // 设置颜色 ▶ —— 预设色板 + 清除颜色,写入 tabColors_ 后强制标签条重绘。
        auto* sub = new wxMenu;
        for (const TabColor& cc : kTabColors) {
            wxMenuItem* mi = sub->Append(wxID_ANY, tr(cc.name));
            mi->SetBitmap(TabColorSwatch(wxColour(cc.hex)));
            const wxString hex = cc.hex;
            sub->Bind(wxEVT_MENU, [this, clickedWin, hex](wxCommandEvent&) {
                tabColors_[clickedWin] = wxColour(hex);
                RefreshEditorTabBar();
            }, mi->GetId());
        }
        sub->AppendSeparator();
        wxMenuItem* clr = sub->Append(wxID_ANY, tr(L"清除颜色"));
        clr->Enable(tabColors_.count(clickedWin) != 0);
        sub->Bind(wxEVT_MENU, [this, clickedWin](wxCommandEvent&) {
            tabColors_.erase(clickedWin);
            RefreshEditorTabBar();
        }, clr->GetId());
        wxMenuItem* colorItem = m.AppendSubMenu(sub, tr(L"设置颜色"));
        // 固定"总览"标签有专属淡蓝样式,禁止对它设色以免盖掉(与"关闭当前"一样置灰)。
        colorItem->Enable(clickedWin != overviewTab_);

        editors_->PopupMenu(&m);
    });
    // Closing the last tab flips back to the placeholder.
    editors_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSED,
                   [this](wxAuiNotebookEvent&) {
                       UpdateQueryView(); RefreshBottomBar();
                       if (viewBar_) viewBar_->Refresh();   // pill 高亮跟随活动标签
                   });
    // Switching tabs: show/hide the data-browser rows/db/SQL in the global bar (§5)
    // and re-highlight the pill segment matching the newly-active tab.
    editors_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED,
                   [this](wxAuiNotebookEvent&) {
                       RefreshBottomBar();
                       if (viewBar_) viewBar_->Refresh();
                   });

    // 表设计 / ER 图 / 连接总览 / 库表列表 都已 tab 化,惰性建进 editors_
    // (OpenDesignTab / OpenErTab / EnsureDbOverviewTab)。连接总览(overviewView_)与
    // 库表列表(databaseView_)如今是固定"总览"标签内 overviewStack_ 的两页,按上下文
    // 切换,不再是 mainBook_ 的独立视图。mainBook_ 现在只剩查询一页。
    mainBook_->AddPage(queryStack_, L"query");     // 唯一页

    v->Add(mainBook_, 1, wxEXPAND);
    panel->SetSizer(v);

    UpdateQueryView();   // start on the placeholder (0 tabs)
    return panel;
}

} // namespace ui
