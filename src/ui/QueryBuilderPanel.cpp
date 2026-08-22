// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// QueryBuilderPanel.cpp — the visual query builder UI. QbCanvas (below) is the
// self-drawn, scrolled table-box surface; QueryBuilderPanel wires the left table
// list + bottom SQL preview around it. All geometry is in logical (unscrolled)
// canvas coordinates; mouse device coords are converted via CalcUnscrolledPosition.
#include "ui/QueryBuilderPanel.h"

#include <wx/wx.h>
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/listbox.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <algorithm>

#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {

constexpr int kBoxW    = 208;   // table box width
constexpr int kHeaderH = 30;    // box title bar height
constexpr int kRowH    = 24;    // per-column row height
constexpr int kMargin  = 26;    // canvas edge padding
constexpr int kCheckX  = 9;     // checkbox left inset within a row
constexpr int kCheckSz = 15;    // checkbox side

int BoxHeight(const QbTable& t)
{
    return kHeaderH + static_cast<int>(t.columns.size()) * kRowH + 6;
}

// Index of a column by name within a table (-1 if gone).
int ColIndex(const QbTable& t, const wxString& col)
{
    for (size_t i = 0; i < t.columns.size(); ++i)
        if (t.columns[i].name == col) return static_cast<int>(i);
    return -1;
}

} // namespace

// ===========================================================================
// QbCanvas — draws the table boxes + join connectors, and owns all pointer
// interaction (drag a box, toggle a column checkbox, pick two columns to join,
// delete a box / join). Mutates the shared model and calls onChanged_ so the
// panel can refresh the SQL preview.
// ===========================================================================
class QbCanvas : public wxScrolledCanvas {
public:
    QbCanvas(wxWindow* parent, QueryBuilderModel* model, std::function<void()> onChanged)
        : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxFULL_REPAINT_ON_RESIZE),
          model_(model), onChanged_(std::move(onChanged))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(0xF3, 0xF5, 0xF8));
        SetScrollRate(16, 16);
        UpdateVirtualSize();
        Bind(wxEVT_PAINT,      &QbCanvas::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN,  &QbCanvas::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP,    &QbCanvas::OnLeftUp, this);
        Bind(wxEVT_MOTION,     &QbCanvas::OnMotion, this);
        Bind(wxEVT_RIGHT_DOWN, &QbCanvas::OnRightDown, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { if (!dragId_) { hoverId_ = 0; Refresh(); } });
        // If the OS steals the capture mid-drag (Alt+Tab, a popup), end the drag
        // cleanly rather than leaving dragId_ set (and tripping a capture assertion).
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragId_ = 0; });
    }

    void ModelChanged()          // panel added/removed a table → relayout + repaint
    {
        pendingId_ = 0;
        UpdateVirtualSize();
        Refresh();
    }

    // Cascade position for the Nth box so fresh boxes don't stack exactly.
    wxRect NextBoxRect(int n, int colCount) const
    {
        const int col = n % 3;
        const int band = n / 3;
        const int x = kMargin + col * (kBoxW + 46);
        const int y = kMargin + band * 60 + col * 22;
        return wxRect(x, y, kBoxW, kHeaderH + colCount * kRowH + 6);
    }

private:
    // ---- coordinate helpers ----
    wxPoint ToLogical(const wxPoint& dev) const
    {
        int x = 0, y = 0;
        CalcUnscrolledPosition(dev.x, dev.y, &x, &y);
        return wxPoint(x, y);
    }
    wxRect CheckRect(const QbTable& t, int rowIdx) const
    {
        const int y = t.rect.y + kHeaderH + rowIdx * kRowH + (kRowH - kCheckSz) / 2;
        return wxRect(t.rect.x + kCheckX, y, kCheckSz, kCheckSz);
    }
    wxRect CloseRect(const QbTable& t) const
    {
        return wxRect(t.rect.GetRight() - 24, t.rect.y + (kHeaderH - 16) / 2, 16, 16);
    }
    // Two connector endpoints for a join (false if either side is gone).
    bool JoinEndpoints(const QbJoin& j, wxPoint& p1, wxPoint& p2) const
    {
        const QbTable* a = model_->TableById(j.leftId);
        const QbTable* b = model_->TableById(j.rightId);
        if (!a || !b) return false;
        const int ai = ColIndex(*a, j.leftCol), bi = ColIndex(*b, j.rightCol);
        if (ai < 0 || bi < 0) return false;
        const int ay = a->rect.y + kHeaderH + ai * kRowH + kRowH / 2;
        const int by = b->rect.y + kHeaderH + bi * kRowH + kRowH / 2;
        const bool aRight = (b->rect.x + b->rect.width / 2) >= (a->rect.x + a->rect.width / 2);
        p1 = wxPoint(aRight ? a->rect.GetRight() : a->rect.x, ay);
        p2 = wxPoint(aRight ? b->rect.x : b->rect.GetRight(), by);
        return true;
    }
    // Join whose midpoint marker is near `pt` (logical), or -1.
    int JoinAt(const wxPoint& pt) const
    {
        for (size_t i = 0; i < model_->joins.size(); ++i) {
            wxPoint p1, p2;
            if (!JoinEndpoints(model_->joins[i], p1, p2)) continue;
            const wxPoint mid((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
            if (std::abs(pt.x - mid.x) <= 9 && std::abs(pt.y - mid.y) <= 9)
                return static_cast<int>(i);
        }
        return -1;
    }

    void UpdateVirtualSize()
    {
        int w = 600, h = 400;
        for (const auto& t : model_->tables) {
            w = std::max(w, t.rect.GetRight() + kMargin);
            h = std::max(h, t.rect.GetBottom() + kMargin);
        }
        SetVirtualSize(w, h);
    }

    // ---- painting ----
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        DrawDots(gc.get());
        DrawConnectors(gc.get());
        for (const auto& t : model_->tables) DrawBox(gc.get(), t);

        if (model_->tables.empty()) {
            gc->SetFont(Ui(11), theme::kTextFaint);
            gc->DrawText(tr(L"双击左侧表名,把表添加到画布"), kMargin + 6, kMargin + 4);
            gc->SetFont(Ui(9.5), theme::kTextGhost);
            gc->DrawText(tr(L"勾选列 → SELECT · 点一列再点另一表的列 → 建立 JOIN"),
                         kMargin + 6, kMargin + 30);
        }
    }

    void DrawDots(wxGraphicsContext* gc)
    {
        const wxSize v = GetVirtualSize();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(0xD8, 0xDD, 0xE4)));
        for (int y = 0; y < v.y; y += 24)
            for (int x = 0; x < v.x; x += 24)
                gc->DrawEllipse(x, y, 1.3, 1.3);
    }

    void DrawConnectors(wxGraphicsContext* gc)
    {
        for (const auto& j : model_->joins) {
            wxPoint p1, p2;
            if (!JoinEndpoints(j, p1, p2)) continue;
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(p1.x, p1.y);
            const double dx = std::max(30.0, std::abs(p2.x - p1.x) * 0.5);
            const double c1 = (p1.x <= p2.x) ? p1.x + dx : p1.x - dx;
            const double c2 = (p1.x <= p2.x) ? p2.x - dx : p2.x + dx;
            path.AddCurveToPoint(c1, p1.y, c2, p2.y, p2.x, p2.y);
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kPrimary).Width(2.0)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->StrokePath(path);
            // endpoint dots
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(theme::kPrimary));
            gc->DrawEllipse(p1.x - 3, p1.y - 3, 6, 6);
            gc->DrawEllipse(p2.x - 3, p2.y - 3, 6, 6);
            // midpoint badge: join type initial, click to delete / right-click to switch
            const wxPoint mid((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
            gc->SetBrush(wxBrush(theme::kWhite));
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kPrimary).Width(1.4)));
            gc->DrawEllipse(mid.x - 9, mid.y - 9, 18, 18);
            const wxString badge = (j.type == QbJoinType::Left)  ? L"L"
                                 : (j.type == QbJoinType::Right) ? L"R" : L"I";
            gc->SetFont(Ui(8.5, true), theme::kPrimary);
            double tw = 0, th = 0; gc->GetTextExtent(badge, &tw, &th);
            gc->DrawText(badge, mid.x - tw / 2, mid.y - th / 2);
        }
    }

    void DrawBox(wxGraphicsContext* gc, const QbTable& t)
    {
        const wxRect r = t.rect;
        const bool hovered = (hoverId_ == t.id);
        // body
        wxGraphicsPath body = gc->CreatePath();
        body.AddRoundedRectangle(r.x, r.y, r.width, r.height, 10);
        gc->SetBrush(*wxWHITE_BRUSH);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(
            hovered ? theme::kPrimary : theme::kBorderInput).Width(hovered ? 1.6 : 1.0)));
        gc->DrawPath(body);
        // header tint
        wxGraphicsPath head = gc->CreatePath();
        head.AddRoundedRectangle(r.x, r.y, r.width, kHeaderH, 10);
        gc->SetBrush(wxBrush(wxColour(0xE8, 0xF0, 0xFE)));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawPath(head);
        // title: name + alias
        gc->SetFont(Ui(9.5, true), theme::kText);
        gc->DrawText(t.name, r.x + 12, r.y + kHeaderH / 2 - 8);
        double nw = 0, nh = 0; gc->GetTextExtent(t.name, &nw, &nh);
        gc->SetFont(Ui(8.5), theme::kPrimary);
        gc->DrawText(t.alias, r.x + 12 + nw + 7, r.y + kHeaderH / 2 - 7);
        // close ×
        const wxRect cr = CloseRect(t);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kTextMuted).Width(1.4)));
        gc->StrokeLine(cr.x + 3, cr.y + 3, cr.GetRight() - 3, cr.GetBottom() - 3);
        gc->StrokeLine(cr.GetRight() - 3, cr.y + 3, cr.x + 3, cr.GetBottom() - 3);

        // columns
        for (size_t i = 0; i < t.columns.size(); ++i) {
            const QbColumn& c = t.columns[i];
            const int rowY = r.y + kHeaderH + static_cast<int>(i) * kRowH;
            const bool anchor = (pendingId_ == t.id && pendingCol_ == c.name);
            if (anchor) {   // highlight the pending join anchor row
                gc->SetBrush(wxBrush(wxColour(0xFF, 0xF3, 0xD6)));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->DrawRectangle(r.x + 1, rowY, r.width - 2, kRowH);
            }
            // checkbox
            const wxRect ch = CheckRect(t, static_cast<int>(i));
            gc->SetBrush(c.selected ? wxBrush(theme::kPrimary) : *wxWHITE_BRUSH);
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(
                c.selected ? theme::kPrimary : theme::kBorderInput).Width(1.2)));
            gc->DrawRoundedRectangle(ch.x, ch.y, ch.width, ch.height, 3);
            if (c.selected) {   // white check mark
                gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kWhite).Width(1.6)));
                gc->StrokeLine(ch.x + 3, ch.y + 8, ch.x + 6, ch.y + 11);
                gc->StrokeLine(ch.x + 6, ch.y + 11, ch.x + 12, ch.y + 4);
            }
            // pk dot
            double nameX = r.x + kCheckX + kCheckSz + 8;
            if (c.isPk) {
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(theme::kDotAmber));
                gc->DrawEllipse(nameX, rowY + kRowH / 2 - 3, 6, 6);
                nameX += 11;
            }
            gc->SetFont(Ui(9), c.isPk ? theme::kDotAmber : theme::kTextBody);
            gc->DrawText(c.name, nameX, rowY + kRowH / 2 - 8);
            // type, faint, right-aligned (clipped by close/edge)
            if (!c.type.IsEmpty()) {
                gc->SetFont(Ui(8), theme::kTextFaint);
                double tw = 0, th = 0; gc->GetTextExtent(c.type, &tw, &th);
                gc->DrawText(c.type, r.GetRight() - tw - 12, rowY + kRowH / 2 - 7);
            }
        }
    }

    // ---- interaction ----
    void OnLeftDown(wxMouseEvent& ev)
    {
        const wxPoint p = ToLogical(ev.GetPosition());
        // 1) a join midpoint badge → delete that join
        const int jhit = JoinAt(p);
        if (jhit >= 0) {
            model_->RemoveJoin(static_cast<size_t>(jhit));
            Notify();
            return;
        }
        // 2) topmost box first (tables draw in order → iterate in reverse)
        for (size_t k = model_->tables.size(); k-- > 0; ) {
            QbTable& t = model_->tables[k];
            if (!t.rect.Contains(p)) continue;
            if (CloseRect(t).Contains(p)) {          // × → remove box
                model_->RemoveTable(t.id);
                Notify();
                return;
            }
            if (p.y < t.rect.y + kHeaderH) {          // header → start drag
                dragId_ = t.id;
                dragOff_ = wxPoint(p.x - t.rect.x, p.y - t.rect.y);
                if (!HasCapture()) CaptureMouse();
                return;
            }
            const int row = (p.y - (t.rect.y + kHeaderH)) / kRowH;
            if (row < 0 || row >= static_cast<int>(t.columns.size())) return;
            if (CheckRect(t, row).Contains(p)) {      // checkbox → toggle SELECT
                t.columns[row].selected = !t.columns[row].selected;
                Notify();
                return;
            }
            PickJoinColumn(t, t.columns[row].name);   // row body → join anchor
            return;
        }
        // clicked empty space → cancel a pending join pick
        if (pendingId_) { pendingId_ = 0; Refresh(); }
    }

    void PickJoinColumn(const QbTable& t, const wxString& col)
    {
        if (pendingId_ == 0) {                 // first pick
            pendingId_ = t.id; pendingCol_ = col; Refresh();
            return;
        }
        if (pendingId_ == t.id) {              // same box → re-pick (or cancel if same col)
            if (pendingCol_ == col) pendingId_ = 0;
            else                    pendingCol_ = col;
            Refresh();
            return;
        }
        model_->AddJoin(pendingId_, pendingCol_, t.id, col);   // complete the join
        pendingId_ = 0;
        Notify();
    }

    void OnMotion(wxMouseEvent& ev)
    {
        const wxPoint p = ToLogical(ev.GetPosition());
        if (dragId_ && ev.Dragging() && ev.LeftIsDown()) {
            QbTable* t = model_->TableById(dragId_);
            if (t) {
                t->rect.x = std::max(0, p.x - dragOff_.x);
                t->rect.y = std::max(0, p.y - dragOff_.y);
                UpdateVirtualSize();
                Refresh();
            }
            return;
        }
        // hover feedback on boxes
        int hv = 0;
        for (size_t k = model_->tables.size(); k-- > 0; )
            if (model_->tables[k].rect.Contains(p)) { hv = model_->tables[k].id; break; }
        if (hv != hoverId_) { hoverId_ = hv; Refresh(); }
    }

    void OnLeftUp(wxMouseEvent&)
    {
        if (dragId_) {
            dragId_ = 0;
            if (HasCapture()) ReleaseMouse();
            UpdateVirtualSize();
        }
    }

    void OnRightDown(wxMouseEvent& ev)
    {
        const wxPoint p = ToLogical(ev.GetPosition());
        const int jhit = JoinAt(p);
        if (jhit < 0) return;
        wxMenu m;
        const int idInner = wxWindow::NewControlId();
        const int idLeft  = wxWindow::NewControlId();
        const int idRight = wxWindow::NewControlId();
        const int idDel   = wxWindow::NewControlId();
        m.AppendRadioItem(idInner, tr(L"INNER JOIN"))->Check(model_->joins[jhit].type == QbJoinType::Inner);
        m.AppendRadioItem(idLeft,  tr(L"LEFT JOIN"))->Check(model_->joins[jhit].type == QbJoinType::Left);
        m.AppendRadioItem(idRight, tr(L"RIGHT JOIN"))->Check(model_->joins[jhit].type == QbJoinType::Right);
        m.AppendSeparator();
        m.Append(idDel, tr(L"删除连接"));
        m.Bind(wxEVT_MENU, [this, jhit, idInner, idLeft, idRight, idDel](wxCommandEvent& e) {
            if (jhit >= static_cast<int>(model_->joins.size())) return;
            const int id = e.GetId();
            if (id == idDel) model_->RemoveJoin(static_cast<size_t>(jhit));
            else if (id == idInner) model_->joins[jhit].type = QbJoinType::Inner;
            else if (id == idLeft)  model_->joins[jhit].type = QbJoinType::Left;
            else if (id == idRight) model_->joins[jhit].type = QbJoinType::Right;
            Notify();
        });
        PopupMenu(&m);
    }

    void Notify() { Refresh(); if (onChanged_) onChanged_(); }

    QueryBuilderModel*    model_;
    std::function<void()> onChanged_;
    int      dragId_    = 0;        // table id being dragged (0 = none)
    wxPoint  dragOff_;
    int      hoverId_   = 0;
    int      pendingId_ = 0;        // pending join first-pick table id (0 = none)
    wxString pendingCol_;
};

// ===========================================================================
// QueryBuilderPanel
// ===========================================================================
QueryBuilderPanel::QueryBuilderPanel(wxWindow* parent)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kWhite);
    auto* root = new wxBoxSizer(wxHORIZONTAL);

    // ---- left: table list ----
    auto* left = new wxPanel(this);
    left->SetBackgroundColour(theme::kWhite);
    left->SetMinSize(wxSize(210, -1));
    auto* lv = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(left, wxID_ANY, tr(L"表"));
    title->SetFont(Ui(10.5, true));
    title->SetForegroundColour(theme::kText);
    lv->Add(title, 0, wxLEFT | wxTOP | wxRIGHT, 12);
    search_ = new wxSearchCtrl(left, wxID_ANY);
    search_->SetDescriptiveText(tr(L"搜索表…"));
    lv->Add(search_, 0, wxEXPAND | wxALL, 10);
    tableList_ = new wxListBox(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               0, nullptr, wxLB_SINGLE | wxBORDER_SIMPLE);
    tableList_->SetFont(Ui(9.5));
    lv->Add(tableList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    hint_ = new wxStaticText(left, wxID_ANY, tr(L"双击添加到画布"));
    hint_->SetFont(Ui(8.5));
    hint_->SetForegroundColour(theme::kTextFaint);
    lv->Add(hint_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    left->SetSizer(lv);
    root->Add(left, 0, wxEXPAND);

    // vertical hairline between list and canvas
    auto* sep = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    sep->SetBackgroundColour(theme::kBorder);
    root->Add(sep, 0, wxEXPAND);

    // ---- right: canvas over SQL preview (draggable sash) ----
    auto* split = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       wxSP_LIVE_UPDATE | wxSP_3DSASH);
    split->SetMinimumPaneSize(120);
    canvas_ = new QbCanvas(split, &model_, [this] { RegenSql(); });

    auto* bottom = new wxPanel(split);
    bottom->SetBackgroundColour(theme::kWhite);
    auto* bv = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* plabel = new wxStaticText(bottom, wxID_ANY, tr(L"SQL 预览"));
    plabel->SetFont(Ui(9.5, true));
    plabel->SetForegroundColour(theme::kText);
    bar->Add(plabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    bar->AddStretchSpacer();

    auto* clearBtn = new wxButton(bottom, wxID_ANY, tr(L"清空画布"),
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* copyBtn  = new wxButton(bottom, wxID_ANY, tr(L"复制 SQL"),
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* applyBtn = new wxButton(bottom, wxID_ANY, tr(L"应用到编辑器"),
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    applyBtn->SetBackgroundColour(theme::kPrimary);
    applyBtn->SetForegroundColour(theme::kWhite);
    applyBtn->SetFont(Ui(9.5, true));
    for (wxButton* b : { clearBtn, copyBtn })
        bar->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    bar->Add(applyBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    bv->Add(bar, 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    preview_ = new wxTextCtrl(bottom, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxBORDER_NONE);
    preview_->SetFont(Mono(10));
    preview_->SetBackgroundColour(theme::kEditorBg);
    bv->Add(preview_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    bottom->SetSizer(bv);

    split->SplitHorizontally(canvas_, bottom, -170);
    root->Add(split, 1, wxEXPAND);
    SetSizer(root);

    // ---- events ----
    tableList_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent& e) {
        const wxString name = e.GetString();
        if (!name.IsEmpty()) AddTableToCanvas(name);
    });
    search_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        RefreshTableList(search_->GetValue());
    });
    clearBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        model_.tables.clear();
        model_.joins.clear();
        canvas_->ModelChanged();
        RegenSql();
    });
    copyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const wxString sql = model_.BuildSql();
        if (sql.IsEmpty()) return;
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(sql));
            wxTheClipboard->Close();
        }
    });
    applyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const wxString sql = model_.BuildSql();
        if (sql.IsEmpty()) {
            wxMessageBox(tr(L"画布上还没有表,先从左侧添加一张表。"),
                         tr(L"构建查询"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (apply_) apply_(sql);
    });

    RegenSql();
}

void QueryBuilderPanel::Load(db::IConnection* conn, const wxString& database,
                             db::Dialect dialect)
{
    conn_ = conn;
    db_   = database;
    model_.dialect   = dialect;
    model_.database  = database;
    // MySQL keeps a session "current database"; qualify FROM/JOIN with `db`. so the
    // generated statement resolves even if it is pasted into a differently-scoped
    // editor. Other engines are single-schema per connection → bare table names.
    model_.qualifyDb = (dialect == db::Dialect::MySQL);

    allTables_.clear();
    if (!conn_ || !conn_->IsConnected()) {
        hint_->SetLabel(tr(L"未连接"));
        return;
    }
    std::vector<db::TableInfo> tabs; wxString err;
    if (!conn_->ListTables(database, tabs, err)) {
        hint_->SetLabel(tr(L"读取表失败"));
        wxMessageBox(tr(L"读取表列表失败:") + L"\n" + err, tr(L"构建查询"),
                     wxOK | wxICON_ERROR, this);
        return;
    }
    for (const auto& ti : tabs) allTables_.push_back(ti.name);
    RefreshTableList(wxString());
    hint_->SetLabel(wxString::Format(tr(L"%zu 张表 · 双击添加"), allTables_.size()));
}

void QueryBuilderPanel::RefreshTableList(const wxString& filter)
{
    if (!tableList_) return;
    const wxString f = filter.Lower();
    tableList_->Freeze();
    tableList_->Clear();
    for (const auto& name : allTables_)
        if (f.IsEmpty() || name.Lower().Contains(f))
            tableList_->Append(name);
    tableList_->Thaw();
}

void QueryBuilderPanel::AddTableToCanvas(const wxString& table)
{
    if (!conn_ || !conn_->IsConnected()) {
        wxMessageBox(tr(L"连接已断开。"), tr(L"构建查询"), wxOK | wxICON_WARNING, this);
        return;
    }
    std::vector<db::ColumnInfo> cols; wxString err;
    {
        wxBusyCursor busy;
        if (!conn_->GetColumns(db_, table, cols, err)) {
            wxMessageBox(tr(L"读取列失败:") + L"\n" + err, tr(L"构建查询"),
                         wxOK | wxICON_ERROR, this);
            return;
        }
    }
    std::vector<QbColumn> qcols;
    qcols.reserve(cols.size());
    for (const auto& ci : cols) {
        QbColumn qc;
        qc.name = ci.name;
        qc.type = ci.type;
        qc.isPk = (ci.key == L"PK");
        qcols.push_back(std::move(qc));
    }
    const wxRect rect = canvas_->NextBoxRect(static_cast<int>(model_.tables.size()),
                                             static_cast<int>(qcols.size()));
    model_.AddTable(table, qcols, rect);
    canvas_->ModelChanged();
    RegenSql();
}

void QueryBuilderPanel::RegenSql()
{
    if (!preview_) return;
    const wxString sql = model_.BuildSql();
    if (sql.IsEmpty()) {
        preview_->SetValue(tr(L"-- 添加表并勾选列后,这里会实时生成 SELECT 语句"));
        return;
    }
    wxString text = sql + L";";
    // hint when nothing is checked → the query defaults to SELECT *
    bool anyCol = false;
    for (const auto& t : model_.tables)
        for (const auto& c : t.columns)
            if (c.selected) { anyCol = true; break; }
    if (!anyCol)
        text += L"\n\n-- " + tr(L"未勾选任何列,默认 SELECT *(勾选列以精确选择字段)");
    preview_->SetValue(text);
}

} // namespace ui
