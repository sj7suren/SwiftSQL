// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// EditorSqlFormat.h — the PURE SQL beautifier behind the editor's 美化 action.
//
// This is the whole of what the 美化/格式化 button does, as one free function
// over plain values: a hand-written tokenizer that upper-cases keywords, breaks
// major clauses onto their own lines, indents AND/OR and top-level commas, and
// lays a CREATE TABLE column list out one column per line — while passing
// strings, quoted identifiers and comments through untouched.
//
// WHY IT LIVES HERE. It used to be a ~100-line loop inline in EditorPage.cpp,
// reading its keyword set off a CompletionSource callback and writing its result
// straight into a wxStyledTextCtrl. It was therefore reachable ONLY by opening
// the app, connecting to a server (the dialect callback supplies the keywords)
// and clicking the button — so the one piece of this program that REWRITES THE
// USER'S TYPED TEXT had no test at all. Now it is a plain function over a string
// and a keyword set. See tests/EditorSqlFormatTests.cpp.
//
// DEPENDENCY FITNESS: wxString and std::set, nothing else. No wxWindow, no
// wxStyledTextCtrl, and deliberately NO db::Dialect — the caller resolves the
// dialect to a keyword set and passes it in, so this TU links against
// swiftsql::core alone. If anyone pulls a widget or a db type in here, the test
// target stops linking, which is the point.
#pragma once

#include <wx/string.h>
#include <set>

namespace ui::editorfmt {

// Pretty-print `src`. `upperKeywords` is the dialect's keyword list, ALREADY
// UPPER-CASED (the function compares against it with the token's own .Upper(),
// so a lower-cased set would silently match nothing and no word would be
// re-cased — the layout would still happen).
//
// An empty `upperKeywords` is legitimate and means "re-case nothing": layout
// still applies, because the clause-breaking sets below are intrinsic to SQL and
// independent of dialect. That is exactly what a fresh editor with no connection
// yet does, and it must not be mistaken for a failure.
//
// Total: never throws, never returns an error. Malformed input (an unterminated
// string or block comment, an unbalanced paren) is consumed to end-of-input and
// emitted as-is rather than rejected — the beautifier's contract is that it
// always gives you something back, since the alternative is silently eating the
// buffer the user was typing into.
wxString FormatSql(const wxString& src, const std::set<wxString>& upperKeywords);

} // namespace ui::editorfmt
