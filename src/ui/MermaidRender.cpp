// MermaidRender.cpp — the GUI half of the Mermaid engine: turns a parsed +
// laid-out graph into a wxBitmap with wxGraphicsContext. The pure logic
// (parse/rank/layout) lives in MermaidDiagram.cpp; this TU only draws.
//
// Rendering notes
//   * one-shot GC onto a wxImage (never per-frame) — cost is acceptable.
//   * node shape → path: rect (small radius) · round (pill) · diamond · circle.
//   * edges: centre-to-centre line clipped to each node's border, triangular
//     arrow head, dash pen for dashed links, pill-backed mid-point label.
//   * fit-width: if the drawing exceeds maxWidth it is scaled down uniformly.
//   * robustness: empty / unparseable → small placeholder bitmap; every path
//     guards the GC pointer, so this never throws or crashes.

#include "ui/MermaidDiagram.h"

#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/dcmemory.h>
#include <cmath>
#include <memory>

#include "ui/I18n.h"
#include "ui/Theme.h"

namespace ui {
namespace mmd {
namespace {

constexpr double kFontPt = 10.5;

wxFont LabelFont(bool bold = false)
{
    wxFontInfo fi(kFontPt);
    fi.Family(wxFONTFAMILY_SWISS);
    if (bold) fi.Bold();
    return wxFont(fi);
}

// Head-less-safe text measurer (memory DC, no window needed).
MMSize MeasureLabel(const wxString& text)
{
    static wxBitmap probe(8, 8);
    wxMemoryDC dc(probe);
    dc.SetFont(LabelFont());
    wxString t = text.empty() ? wxString(L" ") : text;
    wxSize e = dc.GetTextExtent(t);
    return { static_cast<double>(e.x), static_cast<double>(e.y) };
}

struct ShapeStyle { wxColour fill, border, text; };

ShapeStyle StyleFor(MMShape s)
{
    switch (s) {
        case MMShape::Diamond:
            return { theme::kDiffModBg,   theme::kDotAmber, theme::kTextStrong };
        case MMShape::Circle:
            return { theme::kDiffAddedBg, theme::kGreen,    theme::kTextStrong };
        case MMShape::Round:
        case MMShape::Rect:
        default:
            return { theme::kEmptyLensFill, theme::kPrimary, theme::kTextStrong };
    }
}

// Clip the segment centre→toward against a node's border, per shape, so the
// line/arrow lands cleanly on the outline instead of the centre.
void BorderPoint(const MMRect& r, MMShape shape, double tx, double ty,
                 double& ox, double& oy)
{
    double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    double dx = tx - cx, dy = ty - cy;
    if (dx == 0 && dy == 0) { ox = cx; oy = cy; return; }
    double hw = r.w / 2, hh = r.h / 2;
    double t;
    if (shape == MMShape::Circle) {
        double a = dx / hw, b = dy / hh;
        t = 1.0 / std::sqrt(a * a + b * b);
    } else if (shape == MMShape::Diamond) {
        t = 1.0 / (std::fabs(dx) / hw + std::fabs(dy) / hh);
    } else { // rectangle border
        double sx = dx != 0 ? hw / std::fabs(dx) : 1e9;
        double sy = dy != 0 ? hh / std::fabs(dy) : 1e9;
        t = std::min(sx, sy);
    }
    ox = cx + dx * t; oy = cy + dy * t;
}

void DrawNode(wxGraphicsContext* gc, const MMNode& node, const MMRect& r)
{
    ShapeStyle st = StyleFor(node.shape);
    gc->SetBrush(wxBrush(st.fill));
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(st.border).Width(1.4)));

    wxGraphicsPath p = gc->CreatePath();
    switch (node.shape) {
        case MMShape::Round:
            p.AddRoundedRectangle(r.x, r.y, r.w, r.h, r.h / 2.0);
            break;
        case MMShape::Circle:
            p.AddEllipse(r.x, r.y, r.w, r.h);
            break;
        case MMShape::Diamond: {
            double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            p.MoveToPoint(cx, r.y);
            p.AddLineToPoint(r.x + r.w, cy);
            p.AddLineToPoint(cx, r.y + r.h);
            p.AddLineToPoint(r.x, cy);
            p.CloseSubpath();
            break;
        }
        case MMShape::Rect:
        default:
            p.AddRoundedRectangle(r.x, r.y, r.w, r.h, 5);
            break;
    }
    gc->DrawPath(p);

    // centred label
    gc->SetFont(LabelFont(), st.text);
    double tw = 0, th = 0, desc = 0, lead = 0;
    wxString label = node.label.empty() ? node.id : node.label;
    gc->GetTextExtent(label, &tw, &th, &desc, &lead);
    gc->DrawText(label, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2);
}

void DrawArrowHead(wxGraphicsContext* gc, double x, double y, double dx, double dy,
                   const wxColour& col)
{
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6) return;
    dx /= len; dy /= len;
    const double a = 9, w = 4.5;               // arrow length / half-width
    double bx = x - dx * a, by = y - dy * a;    // base centre
    double px = -dy, py = dx;                    // perpendicular
    wxGraphicsPath p = gc->CreatePath();
    p.MoveToPoint(x, y);
    p.AddLineToPoint(bx + px * w, by + py * w);
    p.AddLineToPoint(bx - px * w, by - py * w);
    p.CloseSubpath();
    gc->SetBrush(wxBrush(col));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->FillPath(p);
}

void DrawEdge(wxGraphicsContext* gc, const MMGraph& g, const MMLayout& lay,
              const MMEdge& e)
{
    if (e.from < 0 || e.to < 0) return;
    const MMRect& a = lay.nodes[e.from];
    const MMRect& b = lay.nodes[e.to];
    double acx = a.x + a.w / 2, acy = a.y + a.h / 2;
    double bcx = b.x + b.w / 2, bcy = b.y + b.h / 2;

    double x1, y1, x2, y2;
    BorderPoint(a, g.nodes[e.from].shape, bcx, bcy, x1, y1);
    BorderPoint(b, g.nodes[e.to].shape,   acx, acy, x2, y2);

    const wxColour col = theme::kTextSecondary;
    wxGraphicsPenInfo pi(col);
    pi.Width(1.5).Cap(wxCAP_ROUND);
    if (e.dashed) { wxDash d[] = { 4, 3 }; pi.Dashes(2, d).Style(wxPENSTYLE_USER_DASH); }
    gc->SetPen(gc->CreatePen(pi));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);

    wxGraphicsPath line = gc->CreatePath();
    line.MoveToPoint(x1, y1);
    line.AddLineToPoint(x2, y2);
    gc->StrokePath(line);

    if (e.arrow) DrawArrowHead(gc, x2, y2, x2 - x1, y2 - y1, col);

    if (!e.label.empty()) {
        gc->SetFont(LabelFont(), theme::kTextBody);
        double tw = 0, th = 0, ds = 0, ld = 0;
        gc->GetTextExtent(e.label, &tw, &th, &ds, &ld);
        double mx = (x1 + x2) / 2 - tw / 2, my = (y1 + y2) / 2 - th / 2;
        gc->SetBrush(wxBrush(wxColour(0xFF, 0xFF, 0xFF, 230)));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(mx - 4, my - 1, tw + 8, th + 2, 4);
        gc->SetFont(LabelFont(), theme::kTextBody);
        gc->DrawText(e.label, mx, my);
    }
}

wxBitmap Placeholder(const wxString& msg)
{
    wxImage img(240, 60);
    img.SetRGB(wxRect(0, 0, 240, 60), 0xFC, 0xFD, 0xFE);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(img));
    if (gc) {
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(theme::kBorderInput).Width(1)));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(1, 1, 238, 58, 6);
        gc->SetFont(LabelFont(), theme::kTextFaint);
        double tw = 0, th = 0, ds = 0, ld = 0;
        gc->GetTextExtent(msg, &tw, &th, &ds, &ld);
        gc->DrawText(msg, (240 - tw) / 2, (60 - th) / 2);
    }
    gc.reset();
    return wxBitmap(img);
}

} // namespace

// ==========================================================================
wxBitmap RenderGraphToBitmap(const MMGraph& g, int maxWidth)
{
    if (g.empty())
        return Placeholder(tr(L"（无可渲染的流程图）"));

    MMLayout lay = LayoutGraph(g, &MeasureLabel);
    int w = static_cast<int>(std::ceil(lay.width));
    int h = static_cast<int>(std::ceil(lay.height));
    if (w < 8 || h < 8) return Placeholder(tr(L"（无可渲染的流程图）"));

    double scale = 1.0;
    if (maxWidth > 0 && w > maxWidth) scale = static_cast<double>(maxWidth) / w;
    int bw = std::max(1, static_cast<int>(std::ceil(w * scale)));
    int bh = std::max(1, static_cast<int>(std::ceil(h * scale)));

    wxImage img(bw, bh);
    img.SetRGB(wxRect(0, 0, bw, bh), 0xFC, 0xFD, 0xFE);   // kEditorBg wash
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(img));
    if (!gc) return Placeholder(tr(L"（渲染上下文创建失败）"));

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    if (scale != 1.0) gc->Scale(scale, scale);

    for (const auto& e : g.edges) DrawEdge(gc.get(), g, lay, e);
    for (size_t i = 0; i < g.nodes.size(); ++i)
        DrawNode(gc.get(), g.nodes[i], lay.nodes[i]);

    gc.reset();                                          // flush before readback
    return wxBitmap(img);
}

wxBitmap RenderToBitmap(const wxString& src, int maxWidth)
{
    return RenderGraphToBitmap(ParseMermaid(src), maxWidth);
}

} // namespace mmd
} // namespace ui
