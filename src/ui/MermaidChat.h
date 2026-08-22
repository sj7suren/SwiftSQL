// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MermaidChat.h — the thin bridge between an AI chat reply and the Mermaid engine.
//
// The engine (MermaidDiagram.h / MermaidRender.cpp) is pure "text → bitmap". This
// helper adds the CHAT-specific glue used by AiChatPanel_Turn / _Conversations:
//   • ScanMermaid()          — pull ```mermaid fenced blocks out of a reply, returning
//                              both the DISPLAY text (fences removed) and each block's
//                              raw source. Non-mermaid fences (```sql …) are untouched.
//   • RenderMermaidBlocks()  — render each block to a bitmap at a max width, skipping
//                              blank blocks and any not-Ok result.
//
// Kept in its own TU so neither chat TU grows toward the 1000-line charter limit, and
// so the fence-scanning logic has a single source of truth for both the live-turn path
// and the reopen-conversation path.
#pragma once

#include <wx/string.h>
#include <wx/bitmap.h>

#include <vector>

namespace ui {

struct MermaidScan {
    wxString              displayText;   // reply with the ```mermaid blocks removed, trimmed
    std::vector<wxString> blocks;        // raw source of each mermaid block, in order
};

// Scan `full` for ```mermaid … ``` fenced blocks. The block tag match is case-
// insensitive; an unterminated block runs to the end of the text. Everything outside a
// mermaid block (prose and other code fences) is preserved verbatim in `displayText`.
MermaidScan ScanMermaid(const wxString& full);

// Render each non-blank block to a bitmap at `maxWidth` (0 = natural). Blank blocks and
// not-Ok bitmaps are skipped, so the result maps 1:1 to renderable diagrams.
std::vector<wxBitmap> RenderMermaidBlocks(const std::vector<wxString>& blocks, int maxWidth);

} // namespace ui
