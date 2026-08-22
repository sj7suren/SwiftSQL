// SplashScreen.cpp — see SplashScreen.h.
//
// Layout / colour spec mirrors docs/UI/SwiftSql.dc.html §02 (720x460 card):
//   base linear gradient #FBFCFE->#F2F5FA (~170deg) + radial highlight at (360,138),
//   dot grid, top hairline, pulsing glow ring behind the reused AppLogo, "SwiftSQL"
//   wordmark (Swift solid + SQL horizontal gradient), tagline, shimmering progress
//   bar, blinking status line, and a footer.
//
// Prototype fonts (Space Grotesk / JetBrains Mono / Plus Jakarta Sans) are not
// installed on target machines, so we approximate with Segoe UI (sans) + Consolas
// (mono) at the same pixel sizes / weights.
#include "ui/SplashScreen.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/region.h>

#include <cmath>
#include <cstring>
#include <memory>

#include "core/AppVersion.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"

namespace ui {
namespace {

constexpr int    kW        = 720;
constexpr int    kH        = 460;
constexpr int    kFrameMs  = 33;      // ~30 fps
constexpr int    kMinShow  = 1800;    // minimum on-screen time (ms)
constexpr double kPi       = 3.14159265358979323846;

// --- small colour / math helpers ------------------------------------------
inline unsigned char Lerp8(int a, int b, double t)
{
    return static_cast<unsigned char>(a + (b - a) * t + 0.5);
}
inline wxColour Mix(const wxColour& a, const wxColour& b, double t)
{
    return wxColour(Lerp8(a.Red(), b.Red(), t), Lerp8(a.Green(), b.Green(), t),
                    Lerp8(a.Blue(), b.Blue(), t));
}
inline wxColour Alpha(const wxColour& c, double a)
{
    return wxColour(c.Red(), c.Green(), c.Blue(),
                    static_cast<unsigned char>(a * 255.0 + 0.5));
}

wxFont MakeFont(int px, wxFontWeight weight, const wxString& face)
{
    return wxFont(wxSize(0, px), wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, weight,
                  false, face);
}

// Draw `text` centred on `centreX` at top `y`, with extra tracking between glyphs
// (wxGraphicsContext has no letter-spacing). Returns nothing — purely decorative.
void DrawSpaced(wxGraphicsContext* gc, const wxString& text, const wxFont& font,
                const wxColour& colour, double centreX, double y, double tracking)
{
    gc->SetFont(font, colour);
    double total = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        double w, h, d, e;
        gc->GetTextExtent(wxString(text[i]), &w, &h, &d, &e);
        total += w + (i + 1 < text.length() ? tracking : 0);
    }
    double x = centreX - total / 2.0;
    for (size_t i = 0; i < text.length(); ++i) {
        wxString ch(text[i]);
        double w, h, d, e;
        gc->GetTextExtent(ch, &w, &h, &d, &e);
        gc->DrawText(ch, x, y);
        x += w + tracking;
    }
}

// Horizontal-gradient text: render the glyphs to an offscreen alpha mask, recolour
// the RGB per column between c1 and c2, then blit. Reliable on wxMSW and cannot
// crash (all indices bounded); if the glyph mask ever comes up empty the letters
// simply appear in c1 via the transparent blit.
void DrawGradientText(wxGraphicsContext* gc, const wxString& text,
                      const wxFont& font, double x, double y, const wxColour& c1,
                      const wxColour& c2)
{
    double tw, th, td, te;
    gc->SetFont(font, *wxBLACK);
    gc->GetTextExtent(text, &tw, &th, &td, &te);
    int w = static_cast<int>(std::ceil(tw)) + 2;
    int h = static_cast<int>(std::ceil(th)) + 2;
    if (w <= 2 || h <= 2) {
        gc->SetFont(font, c1);
        gc->DrawText(text, x, y);
        return;
    }
    wxImage img(w, h);
    img.InitAlpha();
    std::memset(img.GetAlpha(), 0, static_cast<size_t>(w) * h);
    {
        std::unique_ptr<wxGraphicsContext> tgc(wxGraphicsContext::Create(img));
        if (tgc) {
            tgc->SetFont(font, *wxWHITE);
            tgc->DrawText(text, 1, 1);
        }
    }
    unsigned char* rgb = img.GetData();
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            double t = (w > 1) ? static_cast<double>(px) / (w - 1) : 0.0;
            size_t i = (static_cast<size_t>(py) * w + px) * 3;
            rgb[i]     = Lerp8(c1.Red(), c2.Red(), t);
            rgb[i + 1] = Lerp8(c1.Green(), c2.Green(), t);
            rgb[i + 2] = Lerp8(c1.Blue(), c2.Blue(), t);
        }
    }
    gc->DrawBitmap(wxBitmap(img), x - 1, y - 1, w, h);
}

// Brand palette (from the design board).
const wxColour kBlue (0x2C, 0x7B, 0xE5);
const wxColour kCyan (0x12, 0xB6, 0xD4);
const wxColour kInk  (0x1B, 0x22, 0x30);

}  // namespace

SplashScreen::SplashScreen()
    : wxFrame(nullptr, wxID_ANY, L"SwiftSQL", wxDefaultPosition, wxSize(kW, kH),
              wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP)
{
    SetClientSize(kW, kH);
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // required for wxAutoBufferedPaintDC
    Centre();
    ApplyRoundedShape();

    Bind(wxEVT_PAINT, &SplashScreen::OnPaint, this);
    Bind(wxEVT_ERASE_BACKGROUND, &SplashScreen::OnEraseBackground, this);
    Bind(wxEVT_LEFT_DOWN, &SplashScreen::OnMouseDown, this);
    Bind(wxEVT_CHAR_HOOK, &SplashScreen::OnCharHook, this);

    timer_.SetOwner(this);
    Bind(wxEVT_TIMER, &SplashScreen::OnTimer, this);
    timer_.Start(kFrameMs);
}

void SplashScreen::ApplyRoundedShape()
{
    // Paint a black canvas with a white rounded rect; the white area becomes the
    // visible window region. Degrades to a plain rectangle if SetShape is refused.
    wxBitmap mask(kW, kH, 24);
    {
        wxMemoryDC mdc(mask);
        mdc.SetBackground(*wxBLACK_BRUSH);
        mdc.Clear();
        mdc.SetBrush(*wxWHITE_BRUSH);
        mdc.SetPen(*wxWHITE_PEN);
        mdc.DrawRoundedRectangle(0, 0, kW, kH, 20);
    }
    wxRegion region(mask, *wxBLACK);
    SetShape(region);   // ignore result: rectangular fallback is fine
}

void SplashScreen::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    Draw(gc.get());
}

void SplashScreen::Draw(wxGraphicsContext* gc)
{
    const double ms = static_cast<double>(tickCount_) * kFrameMs;

    // 1) Card base: ~170deg linear gradient + radial highlight at (360,138).
    gc->SetBrush(gc->CreateLinearGradientBrush(
        120, 0, 600, kH, wxColour(0xFB, 0xFC, 0xFE), wxColour(0xF2, 0xF5, 0xFA)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRectangle(0, 0, kW, kH);

    gc->SetBrush(gc->CreateRadialGradientBrush(
        360, 138, 360, 138, 250, Alpha(kBlue, 0.12), Alpha(kBlue, 0.0)));
    gc->DrawRectangle(0, 0, kW, kH);

    // 2) Faint dot grid (1px dots, 26px pitch).
    gc->SetBrush(wxBrush(Alpha(kBlue, 0.06)));
    for (int y = 4; y < kH; y += 26)
        for (int x = 4; x < kW; x += 26)
            gc->DrawEllipse(x, y, 1.4, 1.4);

    // 3) Top accent hairline (3px, blue->cyan->blue).
    gc->SetBrush(gc->CreateLinearGradientBrush(0, 0, kW, 0, kBlue, kCyan));
    gc->DrawRectangle(0, 0, kW / 2.0, 3);
    gc->SetBrush(gc->CreateLinearGradientBrush(kW / 2.0, 0, kW, 0, kCyan, kBlue));
    gc->DrawRectangle(kW / 2.0, 0, kW / 2.0, 3);

    // 4) Pulsing glow ring behind the logo (3s sine).
    const double glowPulse = 0.5 + 0.5 * std::sin(2 * kPi * ms / 3000.0);
    const double glowR = 105 + 10 * glowPulse;
    gc->SetBrush(gc->CreateRadialGradientBrush(
        360, 118, 360, 118, glowR,
        Alpha(kBlue, 0.14 + 0.12 * glowPulse), Alpha(kBlue, 0.0)));
    gc->DrawRectangle(360 - glowR, 118 - glowR, glowR * 2, glowR * 2);

    // 5) Logo (reused AppLogo) with a soft drop shadow.
    const int logoSize = 88;
    const double logoX = (kW - logoSize) / 2.0;
    const double logoY = 58;
    gc->SetBrush(gc->CreateRadialGradientBrush(
        360, logoY + logoSize - 6, 360, logoY + logoSize - 6, 70,
        Alpha(kBlue, 0.30), Alpha(kBlue, 0.0)));
    gc->DrawRectangle(360 - 70, logoY + logoSize - 6 - 40, 140, 80);
    gc->DrawBitmap(icons::AppLogo(logoSize), logoX, logoY, logoSize, logoSize);

    // 6) Wordmark "SwiftSQL": Swift in ink, SQL in a blue->cyan gradient.
    wxFont mark = MakeFont(42, wxFONTWEIGHT_BOLD, L"Segoe UI");
    const double markY = 172;
    double sw, sh, sd, se;
    gc->SetFont(mark, kInk);
    gc->GetTextExtent(L"Swift", &sw, &sh, &sd, &se);
    double qw, qh, qd, qe;
    gc->GetTextExtent(L"SQL", &qw, &qh, &qd, &qe);
    const double markX = (kW - (sw + qw)) / 2.0;
    gc->DrawText(L"Swift", markX, markY);
    DrawGradientText(gc, L"SQL", mark, markX + sw, markY, kBlue, kCyan);

    // 7) Tagline (brand English, not translated).
    DrawSpaced(gc, L"QUERY FAST · SHIP FASTER",
               MakeFont(13, wxFONTWEIGHT_MEDIUM, L"Consolas"),
               wxColour(0x9A, 0xA1, 0xAD), 360, 226, 2.0);

    // 8) Progress bar: 320x5 track + a 38%-wide shimmer sweeping left->right (1.4s).
    const double trackX = (kW - 320) / 2.0, trackY = 285;
    gc->SetBrush(wxBrush(wxColour(0xE4, 0xE9, 0xF1)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRoundedRectangle(trackX, trackY, 320, 5, 2.5);
    const double bandW = 320 * 0.38;
    const double phase = std::fmod(ms, 1400.0) / 1400.0;
    const double bandX = trackX - bandW + phase * (320 + bandW);
    const double cx0 = std::max(trackX, bandX);
    const double cx1 = std::min(trackX + 320, bandX + bandW);
    if (cx1 > cx0) {
        gc->Clip(trackX, trackY, 320, 5);
        gc->SetBrush(gc->CreateLinearGradientBrush(
            bandX, 0, bandX + bandW, 0, Alpha(kBlue, 0.0), Mix(kBlue, kCyan, 0.5)));
        gc->DrawRoundedRectangle(bandX, trackY, bandW / 2, 5, 2.5);
        gc->SetBrush(gc->CreateLinearGradientBrush(
            bandX + bandW / 2, 0, bandX + bandW, 0, Mix(kBlue, kCyan, 0.5),
            Alpha(kCyan, 0.0)));
        gc->DrawRoundedRectangle(bandX + bandW / 2, trackY, bandW / 2, 5, 2.5);
        gc->ResetClip();
    }

    // 9) Status line: blinking dot (~1s) + translated text.
    const double blink = 0.35 + 0.65 * (0.5 + 0.5 * std::sin(2 * kPi * ms / 1000.0));
    wxFont statusFont = MakeFont(12, wxFONTWEIGHT_MEDIUM, L"Consolas");
    const wxString status = tr(L"正在初始化连接池…");
    double stw, sth, std_, ste;
    gc->SetFont(statusFont, wxColour(0x8A, 0x93, 0xA3));
    gc->GetTextExtent(status, &stw, &sth, &std_, &ste);
    const double dotD = 6, gap = 8;
    const double groupW = dotD + gap + stw;
    const double gx = 360 - groupW / 2.0;
    const double stY = 308;
    gc->SetBrush(wxBrush(Alpha(kBlue, blink)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawEllipse(gx, stY + (sth - dotD) / 2.0, dotD, dotD);
    gc->DrawText(status, gx + dotD + gap, stY);

    // 10) Footer: top border + version (left) / copyright (right).
    const double footY = kH - 44;
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(0xEA, 0xED, 0xF2)).Width(1)));
    gc->StrokeLine(0, footY, kW, footY);
    wxFont footFont = MakeFont(11, wxFONTWEIGHT_MEDIUM, L"Consolas");
    gc->SetFont(footFont, wxColour(0xA6, 0xAD, 0xB8));
    double fw, fh, fd, fe;
    const wxString version = core::kAppVersionText;
    gc->GetTextExtent(version, &fw, &fh, &fd, &fe);
    const double footTextY = footY + (44 - fh) / 2.0;
    gc->DrawText(version, 22, footTextY);
    double cw, ch, cd, ce;
    const wxString copyright = core::kCopyrightText;
    gc->GetTextExtent(copyright, &cw, &ch, &cd, &ce);
    gc->DrawText(copyright, kW - 22 - cw, footTextY);
}

void SplashScreen::OnTimer(wxTimerEvent&)
{
    ++tickCount_;
    if (static_cast<long>(tickCount_) * kFrameMs >= kMinShow) {
        Finish();
        return;
    }
    Refresh(false);
}

void SplashScreen::OnMouseDown(wxMouseEvent&)
{
    Finish();
}

void SplashScreen::OnCharHook(wxKeyEvent& e)
{
    Finish();
    e.Skip(false);
}

void SplashScreen::Finish()
{
    if (finished_)
        return;
    finished_ = true;
    timer_.Stop();
    if (onFinished_)
        onFinished_();
    Destroy();
}

}  // namespace ui
