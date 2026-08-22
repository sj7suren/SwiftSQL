// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MermaidChat.cpp — see MermaidChat.h. Line-based fence scanner + block renderer.
#include "ui/MermaidChat.h"

#include "ui/MermaidDiagram.h"

namespace ui {

namespace {

// Split `s` into logical lines on '\n', dropping a trailing '\r' from each (so CRLF
// replies scan the same as LF). No line ever contains a newline; the final segment
// (even if empty) is included so trailing content is not lost.
std::vector<wxString> SplitLines(const wxString& s)
{
    std::vector<wxString> lines;
    wxString cur;
    for (size_t i = 0, n = s.length(); i < n; ++i) {
        const wxUniChar c = s[i];
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    lines.push_back(cur);
    return lines;
}

// True when `line` opens a code fence; on success `lang` is the trimmed language tag
// (empty for a bare ```). Leading whitespace before the fence is tolerated.
bool IsFence(const wxString& line, wxString& lang)
{
    wxString t = line;
    t.Trim(false);                       // left-trim only
    if (!t.StartsWith(L"```")) return false;
    lang = t.Mid(3);
    lang.Trim(true).Trim(false);
    return true;
}

} // namespace

MermaidScan ScanMermaid(const wxString& full)
{
    MermaidScan scan;
    const std::vector<wxString> lines = SplitLines(full);

    wxString disp;
    for (size_t i = 0; i < lines.size();) {
        wxString lang;
        if (IsFence(lines[i], lang) && lang.Lower() == L"mermaid") {
            // Collect the block body up to the closing fence (or end of text).
            wxString block;
            size_t j = i + 1;
            bool closed = false;
            for (; j < lines.size(); ++j) {
                wxString dummy;
                if (IsFence(lines[j], dummy)) { closed = true; break; }
                if (!block.empty()) block += L"\n";
                block += lines[j];
            }
            scan.blocks.push_back(block);
            i = closed ? j + 1 : j;      // skip past the closing fence when present
        } else {
            if (!disp.empty()) disp += L"\n";
            disp += lines[i];
            ++i;
        }
    }

    disp.Trim(true).Trim(false);
    scan.displayText = disp;
    return scan;
}

std::vector<wxBitmap> RenderMermaidBlocks(const std::vector<wxString>& blocks, int maxWidth)
{
    std::vector<wxBitmap> out;
    for (const wxString& b : blocks) {
        wxString t = b;
        t.Trim(true).Trim(false);
        if (t.IsEmpty()) continue;                       // skip an empty ```mermaid``` block
        wxBitmap bmp = mmd::RenderToBitmap(b, maxWidth);
        if (bmp.IsOk()) out.push_back(bmp);
    }
    return out;
}

} // namespace ui
