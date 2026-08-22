#include "ui/IconFactory.h"

#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/dcmemory.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace icons {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Transparent canvas + graphics context; drawing happens in a `design`-unit
// space (default 24) regardless of the requested pixel size.
struct Canvas {
    wxImage img;
    std::unique_ptr<wxGraphicsContext> gc;

    explicit Canvas(int size, double design = 24.0) : img(size, size)
    {
        img.InitAlpha();
        unsigned char* a = img.GetAlpha();
        memset(a, 0, static_cast<size_t>(size) * size);
        gc.reset(wxGraphicsContext::Create(img));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            const double s = size / design;
            gc->Scale(s, s);
        }
    }

    wxBitmap Finish()
    {
        gc.reset();          // flush before reading pixels back
        return wxBitmap(img);
    }
};

void StrokePath(wxGraphicsContext* gc, const wxGraphicsPath& p,
                const wxColour& c, double w)
{
    gc->SetPen(gc->CreatePen(
        wxGraphicsPenInfo(c).Width(w).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->StrokePath(p);
}

// --- minimal SVG path parser (M L H V C S Q T Z, absolute + relative) ---
// Lets brand logos be pasted straight from the design library's SVG `d` data.
// Arcs (A/a) are unsupported and stop parsing.
double ReadNum(const char*& s)
{
    while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t' || *s == '\r') ++s;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    s = end;
    return v;
}

void AddSvgPath(wxGraphicsPath& p, const char* s)
{
    double cx = 0, cy = 0, sx = 0, sy = 0, rcx = 0, rcy = 0;
    char cmd = 0, prev = 0;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t' || *s == '\r') ++s;
        if (!*s) break;
        if (std::isalpha(static_cast<unsigned char>(*s))) cmd = *s++;
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        switch (std::toupper(cmd)) {
        case 'M': { double x = ReadNum(s), y = ReadNum(s); if (rel) { x += cx; y += cy; }
                    p.MoveToPoint(x, y); cx = x; cy = y; sx = x; sy = y;
                    cmd = rel ? 'l' : 'L'; break; }
        case 'L': { double x = ReadNum(s), y = ReadNum(s); if (rel) { x += cx; y += cy; }
                    p.AddLineToPoint(x, y); cx = x; cy = y; break; }
        case 'H': { double x = ReadNum(s); if (rel) x += cx; p.AddLineToPoint(x, cy); cx = x; break; }
        case 'V': { double y = ReadNum(s); if (rel) y += cy; p.AddLineToPoint(cx, y); cy = y; break; }
        case 'C': { double x1 = ReadNum(s), y1 = ReadNum(s), x2 = ReadNum(s), y2 = ReadNum(s),
                           x = ReadNum(s), y = ReadNum(s);
                    if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
                    p.AddCurveToPoint(x1, y1, x2, y2, x, y);
                    rcx = x2; rcy = y2; cx = x; cy = y; break; }
        case 'S': { double x2 = ReadNum(s), y2 = ReadNum(s), x = ReadNum(s), y = ReadNum(s);
                    if (rel) { x2 += cx; y2 += cy; x += cx; y += cy; }
                    double x1 = cx, y1 = cy;
                    if (std::toupper(prev) == 'C' || std::toupper(prev) == 'S') {
                        x1 = 2 * cx - rcx; y1 = 2 * cy - rcy; }
                    p.AddCurveToPoint(x1, y1, x2, y2, x, y);
                    rcx = x2; rcy = y2; cx = x; cy = y; break; }
        case 'Q': { double x1 = ReadNum(s), y1 = ReadNum(s), x = ReadNum(s), y = ReadNum(s);
                    if (rel) { x1 += cx; y1 += cy; x += cx; y += cy; }
                    p.AddQuadCurveToPoint(x1, y1, x, y);
                    rcx = x1; rcy = y1; cx = x; cy = y; break; }
        case 'T': { double x = ReadNum(s), y = ReadNum(s); if (rel) { x += cx; y += cy; }
                    double x1 = cx, y1 = cy;
                    if (std::toupper(prev) == 'Q' || std::toupper(prev) == 'T') {
                        x1 = 2 * cx - rcx; y1 = 2 * cy - rcy; }
                    p.AddQuadCurveToPoint(x1, y1, x, y);
                    rcx = x1; rcy = y1; cx = x; cy = y; break; }
        case 'Z': p.CloseSubpath(); cx = sx; cy = sy; break;
        default: return;   // unsupported command (e.g. arcs) — stop
        }
        prev = cmd;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Database brand icons
// ---------------------------------------------------------------------------
wxBitmap DbBrand(db::DbType type, int size, const wxColour& mono)
{
    const db::DbTypeInfo& info = db::InfoOf(type);
    Canvas cv(size, 40.0);   // vendor logos are authored in a 40x40 viewBox
    wxGraphicsContext* gc = cv.gc.get();
    if (!gc) return wxBitmap(size, size);

    const bool m = mono.IsOk();          // single-colour silhouette mode
    const wxColour white(255, 255, 255);
    auto fill = [&](const char* d, const wxColour& col) {
        wxGraphicsPath p = gc->CreatePath(); AddSvgPath(p, d);
        gc->SetBrush(wxBrush(m ? mono : col)); gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(p);
    };
    auto stroke = [&](const char* d, const wxColour& col, double w) {
        wxGraphicsPath p = gc->CreatePath(); AddSvgPath(p, d);
        StrokePath(gc, p, m ? mono : col, w);
    };
    auto ring = [&](double x, double y, double r, const wxColour& col, double w) {
        wxGraphicsPath p = gc->CreatePath(); p.AddCircle(x, y, r);
        StrokePath(gc, p, m ? mono : col, w);
    };
    auto dot = [&](double x, double y, double r, const wxColour& col) {
        wxGraphicsPath p = gc->CreatePath(); p.AddCircle(x, y, r);
        gc->SetBrush(wxBrush(col)); gc->SetPen(*wxTRANSPARENT_PEN); gc->FillPath(p);
    };

    // full-colour mode gets a soft brand-tint chip; silhouette mode is bare so
    // the status colour (green/grey) reads cleanly.
    if (!m) {
        wxGraphicsPath bg = gc->CreatePath();
        bg.AddRoundedRectangle(0.5, 0.5, 39.0, 39.0, 10.0);
        gc->SetBrush(wxBrush(wxColour(info.brandColor.Red(), info.brandColor.Green(),
                                      info.brandColor.Blue(), 28)));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(bg);
    }

    switch (type) {
    case db::DbType::PostgreSQL: {   // Slonik elephant (64×64 viewBox → rescale)
        gc->Scale(40.0 / 64.0, 40.0 / 64.0);
        const wxColour el(0x33, 0x67, 0x91);
        fill("M20 20C10 16 4 22 6 32c2 8 9 10 15 7z", el);   // left ear
        fill("M44 20c10-4 16 2 14 12-2 8-9 10-15 7z", el);   // right ear
        fill("M32 8c-9 0-16 6-16 16 0 7 2 12 5 17 2 3 5 5 7 8 1 2 4 2 4-1v-5"
             "c0-3 0-5 0-5s0 2 0 5v5c0 3 3 3 4 1 2-3 5-5 7-8 3-5 5-10 5-17 "
             "0-10-7-16-16-16z", el);                         // head
        if (!m) {
            dot(25, 25, 2.6, white); dot(25, 25.6, 1.2, wxColour(0x1B, 0x2A, 0x3A));
            dot(39, 25, 2.6, white); dot(39, 25.6, 1.2, wxColour(0x1B, 0x2A, 0x3A));
        }
        stroke("M32 34c0 4 0 8 0 12 0 4-4 4-4 8", wxColour(0x28, 0x52, 0x7A), 3.4);
        break;
    }
    case db::DbType::OceanBase: {    // green ring + blue wave
        ring(20, 20, 14, wxColour(0x0B, 0xA0, 0x5C), 3.4);
        stroke("M8 20c3-4 6-4 9 0s6 4 9 0 6-4 6-4", wxColour(0x1B, 0x84, 0xD6), 2.6);
        break;
    }
    case db::DbType::Oracle: {        // database cylinder + red ellipse (design library)
        const wxColour orc = info.brandColor;    // Oracle brand red (metadata)
        auto oell = [&](double ecx, double ecy, double rx, double ry) {
            wxGraphicsPath pe = gc->CreatePath();
            pe.AddEllipse(ecx - rx, ecy - ry, rx * 2, ry * 2);
            gc->SetBrush(wxBrush(m ? mono : orc)); gc->SetPen(*wxTRANSPARENT_PEN);
            gc->FillPath(pe);
        };
        oell(20, 10, 12, 4.5);                   // top disc
        stroke("M8 10v16c0 2.5 5.4 4.5 12 4.5s12-2 12-4.5V10", orc, 3.0);  // cylinder body
        {   // Oracle's signature horizontal ellipse ring, centred on the body
            wxGraphicsPath oval = gc->CreatePath();
            oval.AddEllipse(13, 16.2, 14, 5.6);
            StrokePath(gc, oval, m ? mono : white, 2.4);
        }
        break;
    }
    case db::DbType::KingBase:       // domestic DBs: text mark (per design library)
    case db::DbType::DM: {
        const wxString mark = info.shortMark;
        gc->SetFont(wxFontInfo(mark.length() >= 3 ? 13 : 16)
                        .Family(wxFONTFAMILY_SWISS).Bold(),
                    m ? mono : info.brandColor);
        double tw = 0, th = 0;
        gc->GetTextExtent(mark, &tw, &th);
        gc->DrawText(mark, 20 - tw / 2, 20 - th / 2);
        break;
    }
    case db::DbType::Sqlite: {        // feather (design library, 48×48 viewBox)
        gc->Scale(40.0 / 48.0, 40.0 / 48.0);
        const char* kOuter =
            "M36 6c-11 1-20 9-23 21-1 5-1 10 1 15l4-2c-2-9 2-19 11-25 3-2 5-5 6-9z";
        const char* kInner = "M33 11c-8 4-14 12-16 21l7 4c1-11 4-19 9-25z";
        if (m) { fill(kOuter, mono); }
        else {
            fill(kOuter, wxColour(0x0F, 0x80, 0xCC));
            fill(kInner, wxColour(0x00, 0x3B, 0x57));
            stroke("M15 38l-4 4", wxColour(0x0F, 0x80, 0xCC), 2.4);
        }
        break;
    }
    case db::DbType::MariaDB: {       // sea lion (design library, 64×64 viewBox)
        gc->Scale(40.0 / 64.0, 40.0 / 64.0);
        const char* kBody =
            "M13 22c-2-4 0-9 4-11 4-2 8 0 9 4 7-1 14 1 20 6 6 5 9 11 8 17-1 5-5 8-10 8 "
            "0 2-1 4-3 4-3 0-4-3-4-6-3 1-6 0-8-3-2 3-5 4-8 4-5 0-9-4-9-10 0-5 2-9 6-12 "
            "3-2 3-5 1-7-2-2-4-2-6-1z";
        const char* kFlip1 = "M24 48c-2 6-7 9-13 9 3-5 8-8 12-10z";
        const char* kFlip2 = "M43 52c4 4 10 4 15 1-3 4-9 6-14 4z";
        if (m) { fill(kBody, mono); fill(kFlip1, mono); fill(kFlip2, mono); }
        else {
            fill(kBody,  wxColour(0xB2, 0x5A, 0x44));
            fill(kFlip1, wxColour(0x9A, 0x4B, 0x38));
            fill(kFlip2, wxColour(0x9A, 0x4B, 0x38));
            dot(24, 18, 2.3, white);
            dot(24, 18.5, 1.0, wxColour(0x00, 0x35, 0x45));   // eye
            dot(15, 22, 1.8, wxColour(0x00, 0x35, 0x45));     // nose
            stroke("M15 22c-4 0-8 1-11 3M15 24c-4 0-7 2-10 4",
                   wxColour(0x8E, 0x3F, 0x2C), 1.3);          // whiskers
        }
        break;
    }
    case db::DbType::SqlServer: {     // database cylinder (design library, 40×40)
        auto ell = [&](double ecx, double ecy, double rx, double ry, const wxColour& col) {
            wxGraphicsPath pe = gc->CreatePath();
            pe.AddEllipse(ecx - rx, ecy - ry, rx * 2, ry * 2);
            gc->SetBrush(wxBrush(m ? mono : col)); gc->SetPen(*wxTRANSPARENT_PEN);
            gc->FillPath(pe);
        };
        ell(20, 9, 12, 4.5, wxColour(0xCC, 0x29, 0x27));      // top disc
        stroke("M8 9v9c0 2.5 5.4 4.5 12 4.5s12-2 12-4.5V9",
               wxColour(0xCC, 0x29, 0x27), 3.0);
        stroke("M8 18v9c0 2.5 5.4 4.5 12 4.5s12-2 12-4.5v-9",
               wxColour(0x7D, 0x1B, 0x1A), 3.0);
        break;
    }
    default: {   // MySQL: dolphin (authored in a 64×64 viewBox → rescale to 40)
        gc->Scale(40.0 / 64.0, 40.0 / 64.0);
        const char* kDorsal = "M27 17 C21 10 15 6 11 7 C14 12 18 16 20 19 Z";
        const char* kTail   = "M9 43 C5 45 1 48 2 52 C5 50 7 48 9 47 C8 50 8 53 10 56 "
                              "C12 51 13 47 12 43 Z";
        const char* kBody   = "M62 22 C58 20 54 19 51 19 C40 14 27 14 18 21 "
                              "C12 26 8 36 6 45 C6 46 6 47 7 47 C8 46 9 45 10 44 "
                              "C17 37 25 33 34 31 C43 29 51 27 56 24 C58 23 61 23 62 22 Z";
        const char* kPec    = "M41 31 C37 39 34 44 31 47 C35 44 40 39 44 34 Z";
        if (m) {                       // status silhouette (green/grey)
            fill(kBody, mono); fill(kDorsal, mono); fill(kTail, mono); fill(kPec, mono);
        } else {
            fill(kDorsal, wxColour(0x3E, 0x5E, 0x70));
            fill(kTail,   wxColour(0x3E, 0x5E, 0x70));
            fill(kBody,   wxColour(0x55, 0x78, 0x8C));
            fill("M10 44 C17 37 25 33 34 31 C43 29 51 27 56 24 "
                 "C51 30 41 33 31 36 C23 38 16 41 10 44 Z", wxColour(0xC2, 0xD9, 0xE3));
            fill(kPec, wxColour(0x43, 0x65, 0x77));
            stroke("M56 24 C58 24 60 23 62 22", wxColour(0x33, 0x50, 0x5F), 1.1);
            dot(52, 21, 2.0, wxColour(0x16, 0x30, 0x3C));
            dot(52.8, 20.3, 0.7, white);
        }
        break;
    }
    }
    return cv.Finish();
}

// ---------------------------------------------------------------------------
// App logo (brand board: gradient tile + lightning bolt + cylinder ring)
// ---------------------------------------------------------------------------
wxBitmap AppLogo(int size)
{
    Canvas cv(size);
    wxGraphicsContext* gc = cv.gc.get();
    if (!gc) return wxBitmap(size, size);

    // gradient tile #3B8AF0 -> #18C0DE
    wxGraphicsPath tile = gc->CreatePath();
    tile.AddRoundedRectangle(0.3, 0.3, 23.4, 23.4, 5.9);
    gc->SetBrush(gc->CreateLinearGradientBrush(
        0, 0, 24, 24, wxColour(0x3B, 0x8A, 0xF0), wxColour(0x18, 0xC0, 0xDE)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->FillPath(tile);

    // lightning bolt (98-space coords / 98 * 24)
    auto X = [](double v) { return v * 24.0 / 98.0; };
    wxGraphicsPath bolt = gc->CreatePath();
    bolt.MoveToPoint(X(56), X(18));
    bolt.AddLineToPoint(X(30), X(54));
    bolt.AddLineToPoint(X(46), X(54));
    bolt.AddLineToPoint(X(40), X(80));
    bolt.AddLineToPoint(X(70), X(42));
    bolt.AddLineToPoint(X(52), X(42));
    bolt.CloseSubpath();
    gc->SetBrush(*wxWHITE_BRUSH);
    gc->FillPath(bolt);

    // cylinder ring hint
    wxGraphicsPath ring = gc->CreatePath();
    ring.AddEllipse(X(49 - 20), X(30 - 6.5), X(40), X(13));
    StrokePath(gc, ring, wxColour(255, 255, 255, 140), 0.65);

    return cv.Finish();
}

// ---------------------------------------------------------------------------
// Stroke glyph set (24x24 design space, matches docs/UI svg paths)
// ---------------------------------------------------------------------------
wxBitmap Stroke(Glyph g, int size, const wxColour& c, double w)
{
    Canvas cv(size);
    wxGraphicsContext* gc = cv.gc.get();
    if (!gc) return wxBitmap(size, size);

    wxGraphicsPath p = gc->CreatePath();

    switch (g) {
    case Glyph::Link: // two chain hooks + bridge
        p.MoveToPoint(9, 15);  p.AddLineToPoint(15, 9);
        p.MoveToPoint(8, 13);  p.AddLineToPoint(6, 15);
        p.AddCurveToPoint(4.9, 16.1, 4.9, 17.9, 6, 19);
        p.AddCurveToPoint(7.1, 20.1, 8.9, 20.1, 10, 19);
        p.AddLineToPoint(12, 17);
        p.MoveToPoint(16, 11); p.AddLineToPoint(18, 9);
        p.AddCurveToPoint(19.1, 7.9, 19.1, 6.1, 18, 5);
        p.AddCurveToPoint(16.9, 3.9, 15.1, 3.9, 14, 5);
        p.AddLineToPoint(12, 7);
        break;

    case Glyph::Code: // </>
        p.MoveToPoint(8, 4);  p.AddLineToPoint(4, 12);  p.AddLineToPoint(8, 20);
        p.MoveToPoint(16, 4); p.AddLineToPoint(20, 12); p.AddLineToPoint(16, 20);
        break;

    case Glyph::Table:
        p.AddRoundedRectangle(3, 4, 18, 16, 2);
        p.MoveToPoint(3, 9);  p.AddLineToPoint(21, 9);
        p.MoveToPoint(3, 14); p.AddLineToPoint(21, 14);
        p.MoveToPoint(9, 4);  p.AddLineToPoint(9, 20);
        break;

    case Glyph::Eye:
        p.MoveToPoint(2, 12);
        p.AddCurveToPoint(5.5, 7, 8.5, 5, 12, 5);
        p.AddCurveToPoint(15.5, 5, 18.5, 7, 22, 12);
        p.AddCurveToPoint(18.5, 17, 15.5, 19, 12, 19);
        p.AddCurveToPoint(8.5, 19, 5.5, 17, 2, 12);
        p.CloseSubpath();
        p.AddCircle(12, 12, 3);
        break;

    case Glyph::Function: // stylized f + crossbar
        p.MoveToPoint(13.5, 4);
        p.AddCurveToPoint(10.5, 4, 9, 5.5, 9, 8);
        p.AddLineToPoint(9, 16);
        p.AddCurveToPoint(9, 18.5, 7.5, 20, 5, 20);
        p.MoveToPoint(6, 11); p.AddLineToPoint(14, 11);
        break;

    case Glyph::Model: // two linked boxes
        p.AddRoundedRectangle(3, 4, 7, 6, 1);
        p.AddRoundedRectangle(14, 14, 7, 6, 1);
        p.MoveToPoint(7, 10); p.AddLineToPoint(7, 14); p.AddLineToPoint(14, 14);
        break;

    case Glyph::Backup: // archive box
        p.AddRoundedRectangle(3, 4, 18, 4, 1);
        p.MoveToPoint(5, 8);  p.AddLineToPoint(5, 19);
        p.AddCurveToPoint(5, 19.6, 5.4, 20, 6, 20);
        p.AddLineToPoint(18, 20);
        p.AddCurveToPoint(18.6, 20, 19, 19.6, 19, 19);
        p.AddLineToPoint(19, 8);
        p.MoveToPoint(10, 12); p.AddLineToPoint(14, 12);
        break;

    case Glyph::Import: // arrow down onto line
        p.MoveToPoint(12, 3);  p.AddLineToPoint(12, 15);
        p.MoveToPoint(7, 10);  p.AddLineToPoint(12, 15); p.AddLineToPoint(17, 10);
        p.MoveToPoint(4, 21);  p.AddLineToPoint(20, 21);
        break;

    case Glyph::Export: // arrow up off line
        p.MoveToPoint(12, 15); p.AddLineToPoint(12, 3);
        p.MoveToPoint(7, 8);   p.AddLineToPoint(12, 3);  p.AddLineToPoint(17, 8);
        p.MoveToPoint(4, 21);  p.AddLineToPoint(20, 21);
        break;

    case Glyph::Timer:
        p.AddCircle(12, 13, 7.6);
        p.MoveToPoint(12, 9.5); p.AddLineToPoint(12, 13); p.AddLineToPoint(14, 15);
        p.MoveToPoint(9.5, 2.5); p.AddLineToPoint(14.5, 2.5);
        break;

    case Glyph::Star: { // filled 5-point star
        const double cx = 12, cy = 12.6, R = 9.6, r = 3.9;
        for (int i = 0; i < 10; ++i) {
            const double rad = (i % 2 == 0) ? R : r;
            const double a = -kPi / 2 + i * kPi / 5;
            const double x = cx + rad * std::cos(a), y = cy + rad * std::sin(a);
            if (i == 0) p.MoveToPoint(x, y); else p.AddLineToPoint(x, y);
        }
        p.CloseSubpath();
        gc->SetBrush(wxBrush(c));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(p);
        return cv.Finish();
    }

    case Glyph::Play: { // filled triangle
        p.MoveToPoint(7, 4.5); p.AddLineToPoint(20, 12); p.AddLineToPoint(7, 19.5);
        p.CloseSubpath();
        gc->SetBrush(wxBrush(c));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(p);
        return cv.Finish();
    }

    case Glyph::Cylinder:
        p.AddEllipse(4, 2, 16, 6);
        p.MoveToPoint(4, 5);  p.AddLineToPoint(4, 19);
        p.AddQuadCurveToPoint(12, 23.4, 20, 19);
        p.AddLineToPoint(20, 5);
        p.MoveToPoint(4, 12);
        p.AddQuadCurveToPoint(12, 16.4, 20, 12);
        break;

    case Glyph::Search:
        p.AddCircle(11, 11, 7);
        p.MoveToPoint(16.2, 16.2); p.AddLineToPoint(21, 21);
        break;

    case Glyph::Format:
        p.MoveToPoint(4, 6);  p.AddLineToPoint(20, 6);
        p.MoveToPoint(4, 12); p.AddLineToPoint(14, 12);
        p.MoveToPoint(4, 18); p.AddLineToPoint(17, 18);
        break;

    case Glyph::Explain: // pulse line
        p.MoveToPoint(3, 12);  p.AddLineToPoint(7, 12);
        p.AddLineToPoint(10, 20); p.AddLineToPoint(14, 4);
        p.AddLineToPoint(17, 12); p.AddLineToPoint(21, 12);
        break;

    case Glyph::Beautify: {  // { } braces (stroked) + a sparkle (filled)
        wxGraphicsPath br = gc->CreatePath();
        br.MoveToPoint(8, 4.5);
        br.AddCurveToPoint(5, 4.5, 5.5, 7, 5.5, 9);
        br.AddCurveToPoint(5.5, 10.5, 4.5, 11.5, 3, 12);
        br.AddCurveToPoint(4.5, 12.5, 5.5, 13.5, 5.5, 15);
        br.AddCurveToPoint(5.5, 17, 5, 19.5, 8, 19.5);
        br.MoveToPoint(16, 4.5);
        br.AddCurveToPoint(19, 4.5, 18.5, 7, 18.5, 9);
        br.AddCurveToPoint(18.5, 10.5, 19.5, 11.5, 21, 12);
        br.AddCurveToPoint(19.5, 12.5, 18.5, 13.5, 18.5, 15);
        br.AddCurveToPoint(18.5, 17, 19, 19.5, 16, 19.5);
        StrokePath(gc, br, c, w);
        // 4-point sparkle
        const double cx = 12, cy = 12, R = 3.6, r = 1.3;
        for (int i = 0; i < 8; ++i) {
            const double rad = (i % 2 == 0) ? R : r;
            const double a = -kPi / 2 + i * kPi / 4;
            const double x = cx + rad * std::cos(a), y = cy + rad * std::sin(a);
            if (i == 0) p.MoveToPoint(x, y); else p.AddLineToPoint(x, y);
        }
        p.CloseSubpath();
        gc->SetBrush(wxBrush(c));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(p);
        return cv.Finish();
    }

    case Glyph::Plus:
        p.MoveToPoint(12, 5); p.AddLineToPoint(12, 19);
        p.MoveToPoint(5, 12); p.AddLineToPoint(19, 12);
        break;

    case Glyph::Stop: {  // filled rounded square
        p.AddRoundedRectangle(5, 5, 14, 14, 2.5);
        gc->SetBrush(wxBrush(c));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(p);
        return cv.Finish();
    }

    case Glyph::TxBegin:  // flag = begin
        p.MoveToPoint(6, 3.5); p.AddLineToPoint(6, 20.5);
        p.MoveToPoint(6, 4.5); p.AddLineToPoint(17, 4.5);
        p.AddLineToPoint(14, 8); p.AddLineToPoint(17, 11.5);
        p.AddLineToPoint(6, 11.5);
        break;

    case Glyph::Commit:  // circle + check
        p.AddCircle(12, 12, 8);
        p.MoveToPoint(8, 12.2); p.AddLineToPoint(11, 15.2); p.AddLineToPoint(16.2, 9);
        break;

    case Glyph::Rollback:  // counter-clockwise undo arrow
        p.AddArc(12, 12, 7.5, -0.5, 4.2, false);
        p.MoveToPoint(4.5, 8.3); p.AddLineToPoint(4.9, 4.4);
        p.AddLineToPoint(8.7, 5.2);
        break;

    case Glyph::ChevronRight:
        p.MoveToPoint(9, 6); p.AddLineToPoint(15, 12); p.AddLineToPoint(9, 18);
        break;

    case Glyph::ErDiagram:  // two boxes joined by an elbow
        p.AddRoundedRectangle(3, 4, 7, 6, 1.2);
        p.AddRoundedRectangle(14, 14, 7, 6, 1.2);
        p.MoveToPoint(7, 10); p.AddLineToPoint(7, 14); p.AddLineToPoint(14, 14);
        break;

    case Glyph::ViewLayers:  // stacked cards = a (virtual) view over base tables
        p.AddRoundedRectangle(4, 4, 12.5, 12.5, 2.2);   // back card
        p.AddRoundedRectangle(8, 8, 12, 12, 2.2);       // front card (offset)
        p.MoveToPoint(11, 13.5); p.AddLineToPoint(17, 13.5);  // a row line
        p.MoveToPoint(11, 16.5); p.AddLineToPoint(17, 16.5);
        break;

    case Glyph::StarLine: {  // outline 5-point star
        const double cx = 12, cy = 12.6, R = 9.6, r = 3.9;
        for (int i = 0; i < 10; ++i) {
            const double rad = (i % 2 == 0) ? R : r;
            const double a = -kPi / 2 + i * kPi / 5;
            const double x = cx + rad * std::cos(a), y = cy + rad * std::sin(a);
            if (i == 0) p.MoveToPoint(x, y); else p.AddLineToPoint(x, y);
        }
        p.CloseSubpath();
        break;
    }

    case Glyph::ChevronDown:
        p.MoveToPoint(6, 9); p.AddLineToPoint(12, 15); p.AddLineToPoint(18, 9);
        break;

    case Glyph::Filter:  // funnel
        p.MoveToPoint(4, 6);  p.AddLineToPoint(20, 6);
        p.AddLineToPoint(14, 13); p.AddLineToPoint(14, 19);
        p.AddLineToPoint(10, 16.5); p.AddLineToPoint(10, 13);
        p.CloseSubpath();
        break;

    case Glyph::Sort:  // up-arrow rule + descending lines (A→Z sort)
        p.MoveToPoint(6, 5);   p.AddLineToPoint(6, 19);           // stem
        p.MoveToPoint(3.4, 7.6); p.AddLineToPoint(6, 5); p.AddLineToPoint(8.6, 7.6);  // arrow
        p.MoveToPoint(11, 6);  p.AddLineToPoint(20, 6);           // descending rows
        p.MoveToPoint(11, 12); p.AddLineToPoint(17, 12);
        p.MoveToPoint(11, 18); p.AddLineToPoint(14, 18);
        break;

    case Glyph::Refresh: {  // circular reload arrow (manual arc; content kept ≤ ~19)
        const double gap0 = -kPi * 0.30;                 // arrow-head end (upper right)
        const double gap1 =  kPi * 0.20;                 // open-gap other edge
        const int N = 36;
        const double start = gap1, sweep = (2 * kPi) - (gap1 - gap0);
        for (int i = 0; i <= N; ++i) {
            const double a = start + sweep * i / N;
            const double x = 12 + 6.8 * std::cos(a), y = 12 + 6.8 * std::sin(a);
            if (i == 0) p.MoveToPoint(x, y); else p.AddLineToPoint(x, y);
        }
        const double ax = 12 + 6.8 * std::cos(gap0), ay = 12 + 6.8 * std::sin(gap0);
        p.AddLineToPoint(ax - 2.9, ay - 1.3);            // arrowhead barbs (pulled in)
        p.MoveToPoint(ax, ay);
        p.AddLineToPoint(ax + 0.9, ay - 2.9);
        break;
    }

    case Glyph::RowAdd:  // 增加行: table rows + ⊕ badge (lower-right, per docs/UI icon lib)
        p.AddRoundedRectangle(2.5, 4, 13, 12, 1.8);
        p.MoveToPoint(2.5, 8.2);  p.AddLineToPoint(15.5, 8.2);   // header rule
        p.MoveToPoint(2.5, 12.1); p.AddLineToPoint(15.5, 12.1);  // row rule
        p.AddCircle(17.6, 17.6, 4);                              // ⊕ badge
        p.MoveToPoint(17.6, 15.4); p.AddLineToPoint(17.6, 19.8);
        p.MoveToPoint(15.4, 17.6); p.AddLineToPoint(19.8, 17.6);
        break;

    case Glyph::RowDelete:  // 删除行: table rows + ⊖ badge (lower-right)
        p.AddRoundedRectangle(2.5, 4, 13, 12, 1.8);
        p.MoveToPoint(2.5, 8.2);  p.AddLineToPoint(15.5, 8.2);   // header rule
        p.MoveToPoint(2.5, 12.1); p.AddLineToPoint(15.5, 12.1);  // row rule
        p.AddCircle(17.6, 17.6, 4);                              // ⊖ badge
        p.MoveToPoint(15.4, 17.6); p.AddLineToPoint(19.8, 17.6);
        break;

    case Glyph::Save:  // floppy disk
        p.MoveToPoint(4, 6);
        p.AddCurveToPoint(4, 4.9, 4.9, 4, 6, 4);
        p.AddLineToPoint(16, 4); p.AddLineToPoint(20, 8);
        p.AddLineToPoint(20, 18);
        p.AddCurveToPoint(20, 19.1, 19.1, 20, 18, 20);
        p.AddLineToPoint(6, 20);
        p.AddCurveToPoint(4.9, 20, 4, 19.1, 4, 18);
        p.CloseSubpath();
        p.MoveToPoint(8, 4);  p.AddLineToPoint(8, 9);  p.AddLineToPoint(15, 9);
        p.AddLineToPoint(15, 4);                                  // top slot
        p.AddRoundedRectangle(8, 13, 8, 5, 0.6);                  // label
        break;

    case Glyph::PageFirst:  // ‖ + two left chevrons
        p.MoveToPoint(5, 6);  p.AddLineToPoint(5, 18);
        p.MoveToPoint(13, 6); p.AddLineToPoint(8, 12);  p.AddLineToPoint(13, 18);
        p.MoveToPoint(20, 6); p.AddLineToPoint(15, 12); p.AddLineToPoint(20, 18);
        break;

    case Glyph::PagePrev:  // ◀
        p.MoveToPoint(15, 5); p.AddLineToPoint(8, 12); p.AddLineToPoint(15, 19);
        break;

    case Glyph::PageNext:  // ▶
        p.MoveToPoint(9, 5); p.AddLineToPoint(16, 12); p.AddLineToPoint(9, 19);
        break;

    case Glyph::PageLast:  // two right chevrons + ‖
        p.MoveToPoint(4, 6);  p.AddLineToPoint(9, 12);  p.AddLineToPoint(4, 18);
        p.MoveToPoint(11, 6); p.AddLineToPoint(16, 12); p.AddLineToPoint(11, 18);
        p.MoveToPoint(19, 6); p.AddLineToPoint(19, 18);
        break;

    // ---- transaction family (docs/UI icon lib 事务管理): shared database
    // cylinder + a distinguishing badge at the lower-right (▶ begin / ✓ commit
    // / ↺ rollback). Single-colour stroke; the button supplies the semantic hue.
    // Shared cylinder (x 3..11.5, y 3..13) + a badge at (15.8, 15.8) r=4.0 so the
    // whole glyph stays within ~3..20 — no edge clipping when scaled to 16 px.
    case Glyph::TxDatabase: {  // 开始事务: cylinder + ▶ begin badge
        p.AddEllipse(3, 3, 8.5, 3);
        p.MoveToPoint(3, 4.5); p.AddLineToPoint(3, 11.6);
        p.AddQuadCurveToPoint(7.25, 13.7, 11.5, 11.6);
        p.AddLineToPoint(11.5, 4.5);
        p.MoveToPoint(3, 7.3); p.AddQuadCurveToPoint(7.25, 9.4, 11.5, 7.3);
        p.MoveToPoint(3, 9.5); p.AddQuadCurveToPoint(7.25, 11.6, 11.5, 9.5);
        p.AddCircle(15.8, 15.8, 4.0);
        p.MoveToPoint(14.4, 13.8); p.AddLineToPoint(17.9, 15.8);
        p.AddLineToPoint(14.4, 17.8); p.CloseSubpath();
        break;
    }

    case Glyph::TxCommit: {  // 提交事务: cylinder + ✓ commit badge
        p.AddEllipse(3, 3, 8.5, 3);
        p.MoveToPoint(3, 4.5); p.AddLineToPoint(3, 11.6);
        p.AddQuadCurveToPoint(7.25, 13.7, 11.5, 11.6);
        p.AddLineToPoint(11.5, 4.5);
        p.MoveToPoint(3, 7.3); p.AddQuadCurveToPoint(7.25, 9.4, 11.5, 7.3);
        p.MoveToPoint(3, 9.5); p.AddQuadCurveToPoint(7.25, 11.6, 11.5, 9.5);
        p.AddCircle(15.8, 15.8, 4.0);
        p.MoveToPoint(13.8, 15.9); p.AddLineToPoint(15.2, 17.3);
        p.AddLineToPoint(17.9, 14.2);
        break;
    }

    case Glyph::TxRollback: {  // 回滚事务: cylinder + ↺ rollback badge
        p.AddEllipse(3, 3, 8.5, 3);
        p.MoveToPoint(3, 4.5); p.AddLineToPoint(3, 11.6);
        p.AddQuadCurveToPoint(7.25, 13.7, 11.5, 11.6);
        p.AddLineToPoint(11.5, 4.5);
        p.MoveToPoint(3, 7.3); p.AddQuadCurveToPoint(7.25, 9.4, 11.5, 7.3);
        p.MoveToPoint(3, 9.5); p.AddQuadCurveToPoint(7.25, 11.6, 11.5, 9.5);
        p.AddCircle(15.8, 15.8, 4.0);
        p.AddArc(15.8, 15.8, 2.1, -kPi * 0.15, kPi * 1.25, false);
        const double ra = -kPi * 0.15;
        const double rx = 15.8 + 2.1 * std::cos(ra), ry = 15.8 + 2.1 * std::sin(ra);
        p.MoveToPoint(rx - 1.3, ry - 0.3);
        p.AddLineToPoint(rx, ry);
        p.AddLineToPoint(rx + 0.2, ry - 1.5);
        break;
    }

    case Glyph::Close:  // X
        p.MoveToPoint(6, 6);  p.AddLineToPoint(18, 18);
        p.MoveToPoint(18, 6); p.AddLineToPoint(6, 18);
        break;

    case Glyph::Minus:  // −  (single centered horizontal line)
        p.MoveToPoint(5, 12); p.AddLineToPoint(19, 12);
        break;

    case Glyph::ArrowUp:  // ↑ 上移
        p.MoveToPoint(12, 19); p.AddLineToPoint(12, 6);      // stem
        p.MoveToPoint(7, 11);  p.AddLineToPoint(12, 6);      // head (left)
        p.AddLineToPoint(17, 11);                            // head (right)
        break;

    case Glyph::ArrowDown:  // ↓ 下移
        p.MoveToPoint(12, 5);  p.AddLineToPoint(12, 18);
        p.MoveToPoint(7, 13);  p.AddLineToPoint(12, 18);
        p.AddLineToPoint(17, 13);
        break;

    case Glyph::Key:  // 主键: bow ring (left) + shaft + two teeth (right)
        p.AddCircle(7.5, 12, 3.4);
        p.MoveToPoint(10.9, 12); p.AddLineToPoint(20, 12);   // shaft
        p.MoveToPoint(16.5, 12); p.AddLineToPoint(16.5, 15.2); // tooth 1
        p.MoveToPoint(19, 12);   p.AddLineToPoint(19, 14.6);   // tooth 2
        break;

    case Glyph::RowInsert:  // 插入字段: table rows + ⊕ badge upper-right (= insert above)
        p.AddRoundedRectangle(2.5, 6, 13, 12, 1.8);
        p.MoveToPoint(2.5, 10.2); p.AddLineToPoint(15.5, 10.2);
        p.MoveToPoint(2.5, 14.1); p.AddLineToPoint(15.5, 14.1);
        p.AddCircle(17.6, 6.0, 4);                            // ⊕ badge
        p.MoveToPoint(17.6, 3.8); p.AddLineToPoint(17.6, 8.2);
        p.MoveToPoint(15.4, 6.0); p.AddLineToPoint(19.8, 6.0);
        break;

    case Glyph::Copy:  // 复制: two overlapping sheets
        p.AddRoundedRectangle(8, 8, 12, 12, 2);               // front sheet
        p.AddRoundedRectangle(4, 4, 12, 12, 2);               // back sheet
        break;

    case Glyph::Paste:  // 粘贴: clipboard + clip + lines
        p.AddRoundedRectangle(5, 5, 14, 16, 2);               // board
        p.AddRoundedRectangle(9, 3, 6, 3.5, 1);               // clip
        p.MoveToPoint(8, 12); p.AddLineToPoint(16, 12);       // text lines
        p.MoveToPoint(8, 15); p.AddLineToPoint(16, 15);
        break;

    case Glyph::Users:  // 用户组: a back figure + a front figure (two people)
        // back person (offset up-right, drawn smaller)
        p.AddCircle(15.5, 8, 2.4);                            // back head
        p.MoveToPoint(12.4, 19);                              // back shoulders arc
        p.AddCurveToPoint(12.4, 14.6, 14.0, 12.8, 16.6, 12.8);
        p.AddCurveToPoint(19.0, 12.8, 20.6, 14.4, 20.8, 17.6);
        // front person (larger, foreground)
        p.AddCircle(9, 8.5, 3.2);                             // front head
        p.MoveToPoint(3.4, 20);                               // front shoulders arc
        p.AddCurveToPoint(3.4, 15.2, 5.6, 13.0, 9.0, 13.0);
        p.AddCurveToPoint(12.4, 13.0, 14.6, 15.2, 14.6, 20);
        break;

    case Glyph::Folder:  // 分组: a tabbed folder outline
        // The tab is drawn as part of the same outline rather than as a second
        // subpath, so the corner where it meets the body joins cleanly instead
        // of showing two stroked edges overlapping at small sizes.
        p.MoveToPoint(3, 18.5);
        p.AddLineToPoint(3, 6);
        p.AddLineToPoint(9.4, 6);
        p.AddLineToPoint(11.4, 8.8);
        p.AddLineToPoint(21, 8.8);
        p.AddLineToPoint(21, 18.5);
        p.CloseSubpath();
        break;
    }

    StrokePath(gc, p, c, w);
    return cv.Finish();
}

} // namespace icons
