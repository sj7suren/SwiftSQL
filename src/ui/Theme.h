// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// Theme.h — SwiftSQL design tokens.
// Single source of truth for colors, lifted from docs/UI/SwiftSql.dc.html.
#pragma once

#include <wx/colour.h>

namespace theme {

// ---- brand ----
inline const wxColour kPrimary(0x2C, 0x7B, 0xE5);        // #2C7BE5 blue
inline const wxColour kPrimaryDark(0x0B, 0x5F, 0xCE);    // #0B5FCE
inline const wxColour kAccent(0x12, 0xB6, 0xD4);         // #12B6D4 cyan
inline const wxColour kGreen(0x1E, 0x9E, 0x63);          // #1E9E63 ok / connected

// ---- text ----
inline const wxColour kTextStrong(0x1B, 0x22, 0x30);     // #1B2230
inline const wxColour kText(0x1F, 0x26, 0x32);           // #1F2632
inline const wxColour kTextBody(0x3F, 0x46, 0x52);       // #3F4652
inline const wxColour kTextSecondary(0x5C, 0x64, 0x72);  // #5C6472
inline const wxColour kTextMuted(0x8A, 0x93, 0xA3);      // #8A93A3
inline const wxColour kTextFaint(0x9A, 0xA1, 0xAD);      // #9AA1AD
inline const wxColour kTextGhost(0xA6, 0xAD, 0xB8);      // #A6ADB8

// ---- surfaces ----
inline const wxColour kWhite(0xFF, 0xFF, 0xFF);
inline const wxColour kSidebarBg(0xF5, 0xF7, 0xFA);      // #F5F7FA
inline const wxColour kToolbarBg(0xF2, 0xF4, 0xF7);      // #F2F4F7
inline const wxColour kMenuBg(0xF7, 0xF8, 0xFA);         // #F7F8FA
// Menu row + tool row share one light-grey chrome; flat buttons hover a shade
// darker (sits between kChromeBg and kBorder, so hover reads without shouting).
inline const wxColour kChromeBg(0xF2, 0xF4, 0xF7);       // #F2F4F7 (== kToolbarBg)
inline const wxColour kChromeBtnHover(0xE4, 0xE8, 0xEE); // #E4E8EE hover
inline const wxColour kEditorBg(0xFC, 0xFD, 0xFE);       // #FCFDFE
inline const wxColour kTabStripBg(0xEE, 0xF0, 0xF4);     // #EEF0F4
inline const wxColour kGridHeaderBg(0xEF, 0xF2, 0xF6);   // #EFF2F6
inline const wxColour kRowHeaderActiveBg(0xE7, 0xEF, 0xFB); // #E7EFFB active row gutter
inline const wxColour kSearchBg(0xED, 0xEF, 0xF3);       // #EDEFF3
inline const wxColour kSelectionBg(0xCC, 0xE0, 0xFA);   // #CCE0FA text selection wash

// ---- borders ----
inline const wxColour kBorder(0xE1, 0xE5, 0xEB);         // #E1E5EB
inline const wxColour kBorderSoft(0xE6, 0xE9, 0xEE);     // #E6E9EE
inline const wxColour kBorderInput(0xDE, 0xE2, 0xE8);    // #DEE2E8
inline const wxColour kBorderGrid(0xEE, 0xF0, 0xF3);     // #EEF0F3

// ---- Empty-state illustration (query editor placeholder; whitened blues) ----
inline const wxColour kEmptyLensFill(0xED, 0xF3, 0xFC);     // #EDF3FC lens interior
inline const wxColour kEmptyBandFill(0xDF, 0xE9, 0xF8);     // #DFE9F8 header band (optional)
inline const wxColour kEmptyStroke(0xA7, 0xC0, 0xE2);       // #A7C0E2 table/grid stroke
inline const wxColour kEmptyStrokeStrong(0x6F, 0x9C, 0xD6); // #6F9CD6 lens ring + handle

// ---- SQL syntax (light editor) ----
inline const wxColour kSynKeyword(0x0B, 0x5F, 0xCE);     // #0B5FCE
inline const wxColour kSynTable(0x0F, 0x76, 0x6E);       // #0F766E
inline const wxColour kSynOperator(0x6B, 0x72, 0x80);    // #6B7280
inline const wxColour kSynString(0xC0, 0x39, 0x2B);      // #C0392B
inline const wxColour kSynFunction(0x1A, 0x87, 0x59);    // #1A8759
inline const wxColour kSynNumber(0xB5, 0x74, 0x1B);      // #B5741B
inline const wxColour kSynComment(0xA0, 0xA6, 0xB0);     // #A0A6B0
inline const wxColour kSynDefault(0x2B, 0x30, 0x3A);     // #2B303A
inline const wxColour kLineNumber(0xB4, 0xBB, 0xC6);     // #B4BBC6

// ---- diff / sync six-state system (cross-db-sync §3.5, colour-blind safe) ----
// Each *Fg on its *Bg tint is ≥5.5:1 (AA); state is never colour-only — it always
// pairs with a distinct glyph + text label in the UI.
inline const wxColour kDiffAddedBg(0xE8, 0xF5, 0xEE);    // #E8F5EE
inline const wxColour kDiffAddedFg(0x14, 0x66, 0x3F);    // #14663F
inline const wxColour kDiffModBg(0xFB, 0xF3, 0xDF);      // #FBF3DF
inline const wxColour kDiffModFg(0x8A, 0x5D, 0x08);      // #8A5D08
inline const wxColour kDiffDelBg(0xFB, 0xEB, 0xEC);      // #FBEBEC
inline const wxColour kDiffDelFg(0xA3, 0x2A, 0x32);      // #A32A32
inline const wxColour kDiffConflictBg(0xF2, 0xEA, 0xFB); // #F2EAFB
inline const wxColour kDiffConflictFg(0x5E, 0x2E, 0x9E); // #5E2E9E
inline const wxColour kDiffUnsupBg(0xF4, 0xF5, 0xF7);    // #F4F5F7

// ---- per-object accent dots (sidebar tree) ----
inline const wxColour kDotBlue(0x2C, 0x7B, 0xE5);
inline const wxColour kDotGreen(0x1E, 0x9E, 0x63);
inline const wxColour kDotAmber(0xC9, 0x8A, 0x16);       // #C98A16
inline const wxColour kDotPurple(0x8B, 0x4F, 0xD6);      // #8B4FD6
inline const wxColour kDotPink(0xE0, 0x55, 0x9B);        // #E0559B
inline const wxColour kDotRed(0xDC, 0x5A, 0x63);         // #DC5A63
inline const wxColour kDotGrey(0x9A, 0xA1, 0xAD);

} // namespace theme
