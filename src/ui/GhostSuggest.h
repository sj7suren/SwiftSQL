// GhostSuggest.h — the LOCAL half of the editor's grey inline suggestion
// ("ghost text"): given what is typed so far, what is the obvious next piece of
// SQL skeleton?
//
// ===========================================================================
// WHY A LOCAL TIER AT ALL, NEXT TO THE AI ONE
// ===========================================================================
// The AI completer needs enough context to say anything useful, so it only
// fires after a stopped-typing pause and a minimum prefix length — and every
// firing is a paid round trip. But the most-wanted suggestion of all is the
// cheapest: type `select` and the next thing is ` * from `. Nobody should wait
// 750ms and spend a token on that.
//
// So this tier answers the skeleton cases instantly, offline and free, and the
// AI tier overrides it once the statement is substantial enough to deserve a
// real model. Two tiers, one grey rendering.
//
// ===========================================================================
// PURE ON PURPOSE
// ===========================================================================
// No wxSTC, no connection, no schema — just text in, text out — so it is unit
// tested directly (tests/ghost_suggest_test.cpp). The widget half (where the
// grey text is painted, when it is cleared, what Tab does) is EditorPage's, and
// none of that logic leaks in here.
#pragma once

#include <wx/string.h>
#include "db/DbDriver.h"   // db::Dialect

namespace ui::ghost {

// The grey continuation to draw after the caret, or "" for "suggest nothing".
//
//   beforeCaret — buffer text up to the caret. Only its tail matters.
//   afterLine   — the REST OF THE CURRENT LINE after the caret.
//   dialect     — reserved for per-engine skeletons; the current rules are
//                 common to every dialect this program speaks, and the
//                 parameter exists so adding one does not change the signature.
//
// RULES THIS ENFORCES, each of which is a way the feature would otherwise
// become annoying rather than helpful:
//
//   * NOTHING MID-LINE. If there is real text after the caret, a suggestion
//     would be proposing to insert into the middle of an existing statement,
//     which is never what the user means.
//   * NOTHING INSIDE A STRING OR COMMENT. Ghost SQL offered inside 'a literal'
//     or after `--` is noise, and accepting it would corrupt the literal.
//   * ONLY AFTER A COMPLETE KEYWORD. While a word is still being typed the
//     keyword popup owns the interaction; competing with it is what makes two
//     hints fight over the same Tab key.
wxString LocalSkeleton(const wxString& beforeCaret, const wxString& afterLine,
                       db::Dialect dialect);

} // namespace ui::ghost
