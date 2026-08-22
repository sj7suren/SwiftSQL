// UiFonts.h — shared UI / monospace font helpers used across the ui layer.
//
// Fonts are referenced by NAME only; no font files are bundled or redistributed
// with SwiftSQL. Every face below ships with its target OS (Microsoft YaHei UI /
// Consolas on Windows, PingFang / Menlo on macOS) or is an open font (Noto on
// Linux), so there is no licensing concern — the OS supplies the glyphs.
#pragma once

#include <wx/font.h>

namespace ui {

// Proportional UI text. A concrete, screen-optimised face is requested so CJK
// text stays crisp — the default SWISS family falls back to SimSun/宋体, which is
// blurry at small sizes. Falls back to the SWISS family if the face is missing.
inline wxFont Ui(double pt, bool bold = false)
{
#if defined(__WXMSW__)
    const wxString face = L"Microsoft YaHei UI";   // 微软雅黑 — crisp CJK + Latin
#elif defined(__WXOSX__)
    const wxString face = L"PingFang SC";
#else
    const wxString face = L"Noto Sans CJK SC";
#endif
    wxFontInfo fi(pt);
    fi.FaceName(face);
    if (bold) fi.Bold();
    wxFont f(fi);
    if (!f.IsOk()) {
        wxFontInfo fb(pt);
        if (bold) fb.Bold();
        f = wxFont(fb.Family(wxFONTFAMILY_SWISS));
    }
    return f;
}

// Monospace (SQL editor, code, breadcrumbs).
inline wxFont Mono(double pt, bool bold = false)
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

} // namespace ui
