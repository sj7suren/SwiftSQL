// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/ErDiagramView.h"

#include <wx/wx.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/scrolwin.h>
#include <map>

#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

namespace ui {
namespace {

constexpr int   kCardW    = 210;
constexpr int   kHeaderH  = 30;
constexpr int   kRowH     = 22;
constexpr int   kMaxRows  = 6;     // columns shown per entity
constexpr int   kGapX     = 70;
constexpr int   kGapY     = 46;
constexpr int   kMargin   = 36;
constexpr int   kCols     = 3;     // entities per row (masonry)
constexpr int   kMaxEntities = 40;

const wxColour kPalette[] = {
    theme::kPrimary, theme::kGreen, theme::kDotAmber,
    theme::kDotPurple, theme::kAccent, theme::kDotPink,
};

wxFont Mono(double pt, bool bold = false)
{
#if defined(__WXMSW__)
    const wxString face = L"Consolas";
#elif defined(__WXOSX__)
    const wxString face = L"Menlo";
#else
    const wxString face = L"Monospace";
#endif
    wxFontInfo fi(pt);
    fi.FaceName(face);
    if (bold) fi.Bold();
    return wxFont(fi);
}

int CardHeight(const db::Entity& e)
{
    const int shown = std::min(static_cast<int>(e.columns.size()), kMaxRows);
    const int extra = (static_cast<int>(e.columns.size()) > kMaxRows) ? kRowH : 0;
    return kHeaderH + shown * kRowH + extra + 8;
}

} // namespace

// ===========================================================================
// ErCanvas — owns the entity list, computes a masonry layout, paints cards +
// foreign-key connectors, and supports a zoom factor.
// ===========================================================================
class ErCanvas : public wxScrolledCanvas {
public:
    explicit ErCanvas(wxWindow* parent)
        : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxFULL_REPAINT_ON_RESIZE)
    {
        SetBackgroundColour(wxColour(0xF3, 0xF5, 0xF8));
        SetScrollRate(16, 16);
        Bind(wxEVT_PAINT, &ErCanvas::OnPaint, this);
    }

    void SetData(std::vector<db::Entity> entities, std::vector<db::ForeignKey> fks)
    {
        entities_ = std::move(entities);
        fks_ = std::move(fks);
        Relayout();
    }

    void SetZoom(double z) { zoom_ = z; ApplyVirtualSize(); Refresh(); }

    void Relayout()
    {
        layout_.clear();
        int colY[kCols] = { kMargin, kMargin, kMargin };
        int i = 0;
        for (const auto& e : entities_) {
            const int col = i % kCols;
            const int x = kMargin + col * (kCardW + kGapX);
            const int y = colY[col];
            layout_[e.name] = wxRect(x, y, kCardW, CardHeight(e));
            colY[col] += CardHeight(e) + kGapY;
            ++i;
        }
        int maxY = kMargin;
        for (int c = 0; c < kCols; ++c) maxY = std::max(maxY, colY[c]);
        contentW_ = kMargin * 2 + kCols * kCardW + (kCols - 1) * kGapX;
        contentH_ = maxY + kMargin;
        ApplyVirtualSize();
        Refresh();
    }

private:
    void ApplyVirtualSize()
    {
        SetVirtualSize(static_cast<int>(contentW_ * zoom_),
                       static_cast<int>(contentH_ * zoom_));
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->Scale(zoom_, zoom_);

        DrawGridDots(gc.get());
        DrawConnectors(gc.get());
        for (size_t i = 0; i < entities_.size(); ++i)
            DrawCard(gc.get(), entities_[i], kPalette[i % WXSIZEOF(kPalette)]);

        if (entities_.empty()) {
            gc->SetFont(Mono(12), theme::kTextFaint);
            gc->DrawText(tr(L"连接数据库并选择库后,此处显示 ER 关系图"), kMargin, kMargin);
        }
    }

    void DrawGridDots(wxGraphicsContext* gc)
    {
        gc->SetBrush(wxBrush(wxColour(0xD5, 0xDA, 0xE2)));
        gc->SetPen(*wxTRANSPARENT_PEN);
        for (int y = 0; y < contentH_; y += 22)
            for (int x = 0; x < contentW_; x += 22)
                gc->DrawEllipse(x, y, 1.4, 1.4);
    }

    void DrawConnectors(wxGraphicsContext* gc)
    {
        int i = 0;
        for (const auto& fk : fks_) {
            auto a = layout_.find(fk.fromTable);
            auto b = layout_.find(fk.toTable);
            if (a == layout_.end() || b == layout_.end()) continue;
            const wxRect& r1 = a->second;
            const wxRect& r2 = b->second;
            const wxColour col = kPalette[(i++) % WXSIZEOF(kPalette)];

            // exit from the side of r1 that faces r2
            const bool toRight = r2.GetLeft() >= r1.GetRight();
            double x1 = toRight ? r1.GetRight() : r1.GetLeft();
            double y1 = r1.GetTop() + kHeaderH / 2.0;
            double x2 = (r2.GetLeft() >= r1.GetRight()) ? r2.GetLeft()
                       : (r2.GetRight() <= r1.GetLeft()) ? r2.GetRight()
                       : r2.GetLeft();
            double y2 = r2.GetTop() + kHeaderH / 2.0;

            wxGraphicsPath p = gc->CreatePath();
            p.MoveToPoint(x1, y1);
            const double dx = (x2 - x1) * 0.5;
            p.AddCurveToPoint(x1 + dx, y1, x2 - dx, y2, x2, y2);
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(col).Width(1.8)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->StrokePath(p);

            // endpoints: dot at parent, crow's-foot-ish tick at child
            gc->SetBrush(wxBrush(col));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawEllipse(x1 - 3, y1 - 3, 6, 6);
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(col).Width(1.8)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            wxGraphicsPath tick = gc->CreatePath();
            const double s = toRight ? 6 : -6;
            tick.MoveToPoint(x2, y2 - 5); tick.AddLineToPoint(x2 + s, y2);
            tick.AddLineToPoint(x2, y2 + 5);
            gc->StrokePath(tick);
        }
    }

    void DrawCard(wxGraphicsContext* gc, const db::Entity& e, const wxColour& accent)
    {
        auto it = layout_.find(e.name);
        if (it == layout_.end()) return;
        const wxRect r = it->second;

        // body
        wxGraphicsPath body = gc->CreatePath();
        body.AddRoundedRectangle(r.x, r.y, r.width, r.height, 11);
        gc->SetBrush(*wxWHITE_BRUSH);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kBorderInput).Width(1)));
        gc->DrawPath(body);

        // header tint
        wxGraphicsPath head = gc->CreatePath();
        head.AddRoundedRectangle(r.x, r.y, r.width, kHeaderH, 11);
        wxColour tint(accent.Red(), accent.Green(), accent.Blue(), 32);
        gc->SetBrush(wxBrush(tint));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawPath(head);

        // header dot + table name
        gc->SetBrush(wxBrush(accent));
        gc->DrawRoundedRectangle(r.x + 12, r.y + kHeaderH / 2 - 3, 7, 7, 2);
        gc->SetFont(Mono(10, true), theme::kText);
        gc->DrawText(e.name, r.x + 26, r.y + kHeaderH / 2 - 8);

        // columns
        const wxFont colFont = Mono(9);
        int y = r.y + kHeaderH + 4;
        const int shown = std::min(static_cast<int>(e.columns.size()), kMaxRows);
        for (int i = 0; i < shown; ++i) {
            const db::ColumnInfo& c = e.columns[i];
            double nameX = r.x + 13;

            if (c.key == L"PK") {
                gc->SetBrush(wxBrush(theme::kDotAmber));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->DrawEllipse(r.x + 13, y + 4, 6, 6);
                nameX = r.x + 24;
            } else if (c.key == L"FK") {
                gc->SetBrush(wxBrush(theme::kPrimary));
                gc->SetPen(*wxTRANSPARENT_PEN);
                wxGraphicsPath d = gc->CreatePath();   // diamond
                d.MoveToPoint(r.x + 16, y + 3); d.AddLineToPoint(r.x + 19, y + 7);
                d.AddLineToPoint(r.x + 16, y + 11); d.AddLineToPoint(r.x + 13, y + 7);
                d.CloseSubpath();
                gc->FillPath(d);
                nameX = r.x + 24;
            }

            const wxColour nameCol = (c.key == L"PK") ? theme::kDotAmber
                                   : (c.key == L"FK") ? theme::kPrimary
                                   : theme::kTextBody;
            gc->SetFont(colFont, nameCol);
            gc->DrawText(c.name, nameX, y);

            gc->SetFont(colFont, theme::kTextFaint);
            double tw = 0, th = 0;
            gc->GetTextExtent(c.type, &tw, &th);
            gc->DrawText(c.type, r.x + r.width - tw - 12, y);
            y += kRowH;
        }
        if (static_cast<int>(e.columns.size()) > kMaxRows) {
            gc->SetFont(Mono(8), theme::kTextGhost);
            gc->DrawText(wxString::Format(L"+%zu more…", e.columns.size() - kMaxRows),
                         r.x + 13, y + 2);
        }
    }

    std::vector<db::Entity>       entities_;
    std::vector<db::ForeignKey>   fks_;
    std::map<wxString, wxRect>    layout_;
    double zoom_ = 0.8;
    int contentW_ = 400, contentH_ = 300;
};

// ===========================================================================
ErDiagramView::ErDiagramView(wxWindow* parent)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kWhite);
    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- toolbar ----
    auto* bar = new wxPanel(this);
    bar->SetBackgroundColour(theme::kMenuBg);
    auto* hs = new wxBoxSizer(wxHORIZONTAL);

    auto* title = new wxStaticText(bar, wxID_ANY, tr(L"ER 关系图"));
    title->SetFont(wxFontInfo(11).Bold().Family(wxFONTFAMILY_SWISS));
    title->SetForegroundColour(theme::kText);
    hs->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 18);

    subtitle_ = new wxStaticText(bar, wxID_ANY, tr(L"未连接"));
    subtitle_->SetFont(wxFontInfo(9).Family(wxFONTFAMILY_SWISS));
    subtitle_->SetForegroundColour(theme::kTextFaint);
    hs->Add(subtitle_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    hs->AddStretchSpacer();

    auto* zoomOut = new wxButton(bar, wxID_ANY, L"−", wxDefaultPosition, wxSize(30, 26));
    zoomLabel_ = new wxStaticText(bar, wxID_ANY, L"80%");
    zoomLabel_->SetFont(Mono(9));
    zoomLabel_->SetForegroundColour(theme::kTextBody);
    auto* zoomIn = new wxButton(bar, wxID_ANY, L"+", wxDefaultPosition, wxSize(30, 26));
    auto* autoBtn = new wxButton(bar, wxID_ANY, tr(L" 自动布局 "));
    autoBtn->SetFont(wxFontInfo(9).Family(wxFONTFAMILY_SWISS));

    hs->Add(zoomOut, 0, wxALIGN_CENTER_VERTICAL);
    hs->Add(zoomLabel_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
    hs->Add(zoomIn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    hs->Add(autoBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    bar->SetSizer(hs);
    bar->SetMinSize(wxSize(-1, 44));
    root->Add(bar, 0, wxEXPAND);

    // ---- canvas ----
    canvas_ = new ErCanvas(this);
    root->Add(canvas_, 1, wxEXPAND);
    SetSizer(root);

    zoomOut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetZoom(zoom_ - 0.1); });
    zoomIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetZoom(zoom_ + 0.1); });
    autoBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { canvas_->Relayout(); });
}

void ErDiagramView::SetZoom(double z)
{
    zoom_ = std::max(0.4, std::min(1.5, z));
    zoomLabel_->SetLabel(wxString::Format(L"%d%%", static_cast<int>(zoom_ * 100 + 0.5)));
    canvas_->SetZoom(zoom_);
}

// ---------------------------------------------------------------------------
void ErDiagramView::Load(db::IConnection* conn, const wxString& database)
{
    if (!conn || !conn->IsConnected()) {
        subtitle_->SetLabel(tr(L"未连接"));
        canvas_->SetData({}, {});
        return;
    }

    wxString err;
    std::vector<db::TableInfo> tables;
    if (!conn->ListTables(database, tables, err)) {
        subtitle_->SetLabel(tr(L"读取表失败:") + err);
        canvas_->SetData({}, {});
        return;
    }

    std::vector<db::Entity> entities;
    const size_t limit = std::min<size_t>(tables.size(), kMaxEntities);
    for (size_t i = 0; i < limit; ++i) {
        db::Entity e;
        e.name = tables[i].name;
        conn->GetColumns(database, e.name, e.columns, err);
        entities.push_back(std::move(e));
    }

    std::vector<db::ForeignKey> fks;
    conn->GetForeignKeys(database, wxString(), fks, err);

    canvas_->SetData(std::move(entities), std::move(fks));

    wxString note = wxString::Format(L"%s · %zu entities", database, limit);
    if (tables.size() > limit)
        note += wxString::Format(tr(L"(共 %zu,仅显示前 %zu)"), tables.size(), limit);
    subtitle_->SetLabel(note);
}

} // namespace ui
