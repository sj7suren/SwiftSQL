// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// EditorCompletion.cpp — implementation of the pure autocompletion decision
// layer. Every function here was transcribed out of EditorPage.cpp when that TU
// was split under the charter's 1000-line ceiling, preserving behaviour
// statement for statement; the differences are only that the schema now arrives
// as parameters instead of being read off member callbacks and a wxSTC.
#include "ui/EditorCompletion.h"

#include <wx/wxcrt.h>   // wxIsalnum

#include <algorithm>
#include <map>

namespace ui::editorcomp {

std::vector<std::pair<wxString, int>> MergeWordCandidates(
    const std::vector<wxString>& keywords,
    const std::vector<wxString>& functions,
    const std::vector<wxString>& databases,
    const std::vector<wxString>& tables)
{
    // std::map keeps them sorted by word (Scintilla needs sorted). emplace for
    // the dialect lists (first writer wins), operator[] for the live schema
    // (last writer wins) — see the precedence note in the header.
    std::map<wxString, int> uniq;
    for (const wxString& k : keywords)  uniq.emplace(k, kKeyword);
    for (const wxString& f : functions) uniq.emplace(f, kFunction);
    for (const wxString& x : databases) uniq[x] = kDatabase;
    for (const wxString& x : tables)    uniq[x] = kTable;
    return { uniq.begin(), uniq.end() };
}

QualifierChain ParseQualifierChain(const wxString& textBeforeDot)
{
    QualifierChain out;
    const wxString& head = textBeforeDot;
    size_t k = head.length();
    for (;;) {
        const size_t end = k;
        while (k > 0 && (wxIsalnum(head[k - 1]) || head[k - 1] == '_')) --k;
        wxString tok = head.Mid(k, end - k);
        // Historical no-ops: the scan above consumes identifier characters only,
        // so a quote can never appear inside `tok`. Kept because they were in the
        // transcribed original and removing them is a (safe, but separate) change.
        tok.Replace(L"`", L""); tok.Replace(L"\"", L"");
        if (tok.IsEmpty()) { out.truncated = true; break; }
        out.parts.insert(out.parts.begin(), tok);   // prepend (we scan right→left)
        if (k > 0 && head[k - 1] == '.') { --k; continue; }
        break;
    }
    return out;
}

QualifierCompletion ResolveQualifier(const QualifierChain& chain,
                                     const SchemaLookup& look)
{
    QualifierCompletion out;
    const std::vector<wxString>& parts = chain.parts;
    if (parts.size() >= 2) {
        // "db.table." → columns of that table in that database
        if (look.columnsOf)
            out.candidates = look.columnsOf(parts[parts.size() - 2], parts.back());
        return out;
    }
    if (parts.empty()) return out;

    const wxString& ident = parts.back();
    if (look.isDatabase && look.isDatabase(ident)) {
        // "db." → tables of that database
        if (look.tablesOf) {
            out.candidates = look.tablesOf(ident);
            out.category   = kTable;
        }
    } else if (look.columns) {
        // "table." (alias-aware) → columns via the active DB
        out.candidates = look.columns(look.resolveTable ? look.resolveTable(ident) : ident);
    }
    return out;
}

QualifierCompletion CompleteQualified(const wxString& textBeforeDot,
                                      const SchemaLookup& look)
{
    const QualifierChain chain = ParseQualifierChain(textBeforeDot);
    // A malformed chain completes to nothing — the live popup's long-standing
    // behaviour, adopted here so the scripted hook stops disagreeing with it.
    // See the note on QualifierChain::truncated.
    if (chain.truncated) return {};

    QualifierCompletion out = ResolveQualifier(chain, look);
    std::sort(out.candidates.begin(), out.candidates.end());
    return out;
}

wxString ResolveTableForIdent(const wxString& bufferText, const wxString& ident,
                              const std::function<bool(const wxString&)>& hasColumns)
{
    if (!hasColumns) return ident;
    if (hasColumns(ident)) return ident;   // ident is a real table

    // Split the buffer into bare identifier words, dropping everything else.
    std::vector<wxString> words;
    wxString cur;
    for (wxUniChar c : bufferText) {
        if (wxIsalnum(c) || c == '_') cur += c;
        else if (!cur.IsEmpty()) { words.push_back(cur); cur.clear(); }
    }
    if (!cur.IsEmpty()) words.push_back(cur);

    // Scan for a "table [AS] alias" binding whose alias matches `ident`.
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        size_t aliasAt = i + 1;
        if (words[i + 1].Lower() == L"as" && i + 2 < words.size()) aliasAt = i + 2;
        if (words[aliasAt].IsSameAs(ident, false) && hasColumns(words[i]))
            return words[i];
    }
    return ident;
}

wxString BuildAutoCompList(const std::vector<std::pair<wxString, int>>& items)
{
    wxString list;
    for (const auto& c : items) {
        if (!list.IsEmpty()) list += L'\n';
        list += c.first + L'?' + wxString::Format(L"%d", c.second);   // "word?type" → icon
    }
    return list;
}

wxString BuildAutoCompList(const std::vector<wxString>& words, int category)
{
    wxString list;
    for (const wxString& c : words) {
        if (!list.IsEmpty()) list += L'\n';
        list += c + L'?' + wxString::Format(L"%d", category);
    }
    return list;
}

wxString JoinWords(const std::vector<wxString>& words)
{
    wxString out;
    for (const wxString& c : words) { if (!out.IsEmpty()) out += L' '; out += c; }
    return out;
}

} // namespace ui::editorcomp
