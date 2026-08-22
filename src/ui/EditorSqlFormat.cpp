// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// EditorSqlFormat.cpp — implementation of the pure SQL beautifier.
//
// Transcribed VERBATIM out of EditorPage::FormatSql when EditorPage.cpp was
// split under the charter's 1000-line ceiling: same token loop, same emit/nl
// helpers, same clause sets, same order of tests. The only change is where the
// keyword set comes from — it used to be built here from a db::Dialect callback,
// and is now a parameter, which is what makes the function testable.
#include "ui/EditorSqlFormat.h"

#include <wx/wxcrt.h>   // wxIsalnum

namespace ui::editorfmt {

// Clauses that start a new line at paren depth 0. JOIN and its qualifiers are
// here so a multi-table SELECT stacks readably; ON follows for the same reason.
static const std::set<wxString> kBreak = {
    L"FROM", L"WHERE", L"GROUP", L"ORDER", L"HAVING", L"LIMIT", L"UNION",
    L"JOIN", L"LEFT", L"RIGHT", L"INNER", L"OUTER", L"FULL", L"CROSS",
    L"ON", L"SET", L"VALUES", L"RETURNING" };

// Boolean connectives: new line, but INDENTED under the clause they belong to.
static const std::set<wxString> kIndentBreak = { L"AND", L"OR" };

wxString FormatSql(const wxString& src, const std::set<wxString>& kw)
{
    wxString out;
    int depth = 0;
    bool lineStart = true, afterDot = false;
    bool pendingCreate = false, inCreateTable = false;   // CREATE TABLE column-list layout
    int  createParenDepth = -1;                           // depth of the column-list paren
    auto nl = [&](const wxString& indent = wxString()) {
        while (!out.IsEmpty() && out.Last() == ' ') out.RemoveLast();
        out += L"\n" + indent; lineStart = true;
    };
    auto emit = [&](const wxString& t) {
        if (!lineStart && !out.IsEmpty() && out.Last() != ' ' && out.Last() != '(' &&
            out.Last() != '.' && t != L"," && t != L")" && t != L"(" && t != L";")
            out += L' ';
        out += t; lineStart = false;
    };

    const size_t n = src.length();
    for (size_t i = 0; i < n; ) {
        const wxChar c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (c == '\'' || c == '"' || c == '`') {            // string / quoted ident
            const wxChar q = c; wxString tok(q); ++i;
            while (i < n) { tok += src[i]; if (src[i] == q) { ++i; break; } ++i; }
            emit(tok); continue;
        }
        if (c == '-' && i + 1 < n && src[i + 1] == '-') {   // line comment
            wxString tok; while (i < n && src[i] != '\n') tok += src[i++];
            emit(tok); nl(); continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {   // block comment
            wxString tok(L"/*"); i += 2;
            while (i < n) { tok += src[i]; if (src[i] == '/' && src[i-1] == '*') { ++i; break; } ++i; }
            emit(tok); continue;
        }
        if (wxIsalnum(c) || c == '_') {                     // word
            wxString w; while (i < n && (wxIsalnum(src[i]) || src[i] == '_')) w += src[i++];
            const wxString up = w.Upper();
            // a word right after '.' is a column ref — leave it as typed
            if (afterDot) { emit(w); afterDot = false; continue; }
            // CREATE TABLE (but not CREATE VIEW/INDEX/PROCEDURE/…) → lay its columns out.
            if (up == L"CREATE") pendingCreate = true;
            else if (pendingCreate) {
                if (up == L"TABLE") { inCreateTable = true; pendingCreate = false; }
                else if (up != L"TEMPORARY" && up != L"TEMP" && up != L"UNLOGGED" &&
                         up != L"GLOBAL" && up != L"LOCAL")
                    pendingCreate = false;
            }
            if (depth == 0 && kBreak.count(up) && !out.IsEmpty()) nl();
            else if (depth == 0 && kIndentBreak.count(up)) nl(L"  ");
            emit(kw.count(up) ? up : w);
            continue;
        }
        if (c == '.') {                                     // member access — tight
            while (!out.IsEmpty() && out.Last() == ' ') out.RemoveLast();
            out += L'.'; lineStart = false; afterDot = true; ++i; continue;
        }
        if (c == '(') {
            if (inCreateTable && createParenDepth == -1) {       // the column-list paren
                if (!lineStart && !out.IsEmpty() && out.Last() != ' ') out += L' ';
                out += L"("; lineStart = false;
                createParenDepth = depth; ++depth; nl(L"  ");     // each column on its own line
            } else {
                ++depth; emit(L"(");
            }
            ++i; continue;
        }
        if (c == ')') {
            if (inCreateTable && createParenDepth != -1 && depth == createParenDepth + 1) {
                --depth; nl(); emit(L")");                        // close column list on its own line
                createParenDepth = -1; inCreateTable = false;
            } else {
                if (depth) --depth; emit(L")");
            }
            ++i; continue;
        }
        if (c == ',') {
            emit(L","); ++i;
            if (depth == 0) nl(L"  ");
            else if (inCreateTable && createParenDepth != -1 && depth == createParenDepth + 1)
                nl(L"  ");                                        // column separator inside CREATE TABLE
            continue;
        }
        if (c == ';') { emit(L";"); ++i; nl(); continue; }
        wxString op(c); ++i;                                // operators (<=, >=, <>…)
        if (i < n && wxString(L"=<>|").Find(src[i]) != wxNOT_FOUND &&
            wxString(L"<>=!|").Find(c) != wxNOT_FOUND) op += src[i++];
        emit(op);
    }
    out.Trim();
    return out;
}

} // namespace ui::editorfmt
