// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SqlScript.cpp — dialect-aware SQL statement splitter. See SqlScript.h.
#include "db/SqlScript.h"

#include <wx/wxcrt.h>   // wxIsalnum (StatementTouchesTables' word scanner)

namespace db {
namespace {

bool IsIdentChar(wxUniChar c)
{
    const wchar_t v = static_cast<wchar_t>(c.GetValue());
    return (v >= L'a' && v <= L'z') || (v >= L'A' && v <= L'Z') ||
           (v >= L'0' && v <= L'9') || v == L'_';
}

bool IsSpace(wxUniChar c)
{
    const wchar_t v = static_cast<wchar_t>(c.GetValue());
    return v == L' ' || v == L'\t' || v == L'\r' || v == L'\n' ||
           v == L'\f' || v == L'\v';
}

wchar_t LowerAscii(wxUniChar c)
{
    wchar_t v = static_cast<wchar_t>(c.GetValue());
    if (v >= L'A' && v <= L'Z') v += 32;
    return v;
}

} // namespace

std::vector<wxString> SplitSqlScript(const wxString& text, Dialect dialect)
{
    const bool mysql = (dialect == Dialect::MySQL);
    const size_t n = text.length();

    std::vector<wxString> out;
    wxString cur;
    wxString delimiter = L";";
    bool hasContent = false;   // does `cur` hold real (non-comment) executable text?

    auto flush = [&]() {
        if (hasContent) {
            wxString t = cur;
            t.Trim(true).Trim(false);
            if (!t.IsEmpty()) out.push_back(t);
        }
        cur.clear();
        hasContent = false;
    };
    auto matchAt = [&](size_t pos, const wxString& s) -> bool {
        if (s.empty() || pos + s.length() > n) return false;
        for (size_t k = 0; k < s.length(); ++k)
            if (text[pos + k] != s[k]) return false;
        return true;
    };
    auto matchKeywordCI = [&](size_t pos, const wxString& kw) -> bool {
        if (pos + kw.length() > n) return false;
        for (size_t k = 0; k < kw.length(); ++k)
            if (LowerAscii(text[pos + k]) != LowerAscii(kw[k])) return false;
        return true;
    };

    size_t i = 0;
    while (i < n) {
        const wxUniChar ch = text[i];

        // ---- MySQL DELIMITER directive — only at statement start ----
        // Reassigns the terminator (e.g. `DELIMITER //`) and is a client-side
        // command, so it is consumed here and never sent to the server.
        if (mysql && !hasContent && matchKeywordCI(i, L"DELIMITER") &&
            (i + 9 >= n || IsSpace(text[i + 9]))) {
            size_t j = i + 9;
            while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
            wxString delim;
            while (j < n && !IsSpace(text[j])) { delim += text[j]; ++j; }
            if (!delim.IsEmpty()) delimiter = delim;
            while (j < n && text[j] != '\n') ++j;   // skip rest of line
            if (j < n) ++j;                          // and the newline
            i = j;
            cur.clear();
            continue;
        }

        // ---- statement terminator (the current delimiter) ----
        if (matchAt(i, delimiter)) {
            flush();
            i += delimiter.length();
            continue;
        }

        // ---- line comment (-- … or # … for MySQL) — preserved, not split ----
        if (matchAt(i, L"--") || (mysql && ch == '#')) {
            while (i < n && text[i] != '\n') { cur += text[i]; ++i; }
            continue;   // leave the newline for normal handling
        }

        // ---- block comment /* … */ — preserved; /*! … */ is executable on MySQL ----
        if (ch == '/' && i + 1 < n && text[i + 1] == '*') {
            if (mysql && i + 2 < n && text[i + 2] == '!') hasContent = true;
            cur += L"/*"; i += 2;
            while (i < n && !(text[i] == '*' && i + 1 < n && text[i + 1] == '/')) {
                cur += text[i]; ++i;
            }
            if (i < n) { cur += L"*/"; i += 2; }
            continue;
        }

        // ---- PostgreSQL dollar-quoted body $tag$ … $tag$ ----
        if (!mysql && ch == '$') {
            size_t j = i + 1;
            while (j < n && IsIdentChar(text[j])) ++j;
            if (j < n && text[j] == '$') {                 // a valid opening tag
                const wxString tag = text.Mid(i, j - i + 1);
                cur += tag; hasContent = true; i = j + 1;
                while (i < n && !matchAt(i, tag)) { cur += text[i]; ++i; }
                if (i < n) { cur += tag; i += tag.length(); }
                continue;
            }
            // otherwise ($1 params, plain $) fall through to normal handling
        }

        // ---- single-quoted string literal ----
        if (ch == '\'') {
            cur += ch; hasContent = true; ++i;
            while (i < n) {
                const wxUniChar c = text[i];
                if (mysql && c == '\\' && i + 1 < n) { cur += c; cur += text[i + 1]; i += 2; continue; }
                if (c == '\'') {
                    if (i + 1 < n && text[i + 1] == '\'') { cur += L"''"; i += 2; continue; } // escaped
                    cur += c; ++i; break;
                }
                cur += c; ++i;
            }
            continue;
        }

        // ---- double-quoted string / identifier ----
        if (ch == '"') {
            cur += ch; hasContent = true; ++i;
            while (i < n) {
                const wxUniChar c = text[i];
                if (mysql && c == '\\' && i + 1 < n) { cur += c; cur += text[i + 1]; i += 2; continue; }
                if (c == '"') {
                    if (i + 1 < n && text[i + 1] == '"') { cur += L"\"\""; i += 2; continue; }
                    cur += c; ++i; break;
                }
                cur += c; ++i;
            }
            continue;
        }

        // ---- backtick identifier (MySQL) ----
        if (mysql && ch == '`') {
            cur += ch; hasContent = true; ++i;
            while (i < n) {
                const wxUniChar c = text[i];
                if (c == '`') {
                    if (i + 1 < n && text[i + 1] == '`') { cur += L"``"; i += 2; continue; }
                    cur += c; ++i; break;
                }
                cur += c; ++i;
            }
            continue;
        }

        // ---- ordinary character ----
        cur += ch;
        if (!IsSpace(ch)) hasContent = true;
        ++i;
    }
    flush();   // trailing statement with no terminator
    return out;
}

// ---------------------------------------------------------------------------
// StatementTouchesTables — see the header for what this is for and why it is
// keyword-shaped rather than a parser.
// ---------------------------------------------------------------------------
namespace {

// The leading words of a statement, upper-cased, with leading comments and
// whitespace skipped. Capped: the verb and its qualifiers are always within the
// first handful of words, and scanning a 10 MB INSERT to answer "is this a
// CREATE TABLE" would make this predicate cost more than the query it saves.
std::vector<wxString> LeadingWords(const wxString& statement, size_t want)
{
    std::vector<wxString> out;
    wxString cur;
    const size_t n = statement.length();
    size_t i = 0;
    auto push = [&] {
        if (!cur.IsEmpty()) { out.push_back(cur.Upper()); cur.clear(); }
    };
    while (i < n && out.size() < want) {
        const wxUniChar c = statement[i];
        // Skip the comment forms that can precede the verb. A script generated by
        // this program's own exporter starts every statement with a `-- 表 X`
        // banner, so a version that did not skip comments would answer "no" for
        // every table in a dump it produced itself.
        if (c == '-' && i + 1 < n && statement[i + 1] == '-') {
            push();
            while (i < n && statement[i] != '\n') ++i;
            continue;
        }
        if (c == '#') {                       // MySQL line comment
            push();
            while (i < n && statement[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && statement[i + 1] == '*') {
            push();
            i += 2;
            while (i + 1 < n && !(statement[i] == '*' && statement[i + 1] == '/')) ++i;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (wxIsalnum(c) || c == '_') { cur += c; ++i; continue; }
        push();
        ++i;
    }
    push();
    return out;
}

// Qualifiers that may sit between the verb and the object-type keyword.
bool IsNoiseWord(const wxString& up)
{
    return up == L"OR" || up == L"REPLACE" || up == L"TEMPORARY" || up == L"TEMP" ||
           up == L"IF" || up == L"NOT" || up == L"EXISTS" || up == L"GLOBAL" ||
           up == L"LOCAL" || up == L"UNLOGGED" || up == L"EXTERNAL";
}

} // namespace

bool StatementTouchesTables(const wxString& statement)
{
    // 8 words is comfortably past the longest real prefix
    // ("CREATE OR REPLACE GLOBAL TEMPORARY TABLE IF NOT" is already 8).
    const std::vector<wxString> w = LeadingWords(statement, 9);
    if (w.empty()) return false;
    const wxString& verb = w[0];

    // TRUNCATE's object keyword is optional (`TRUNCATE t` == `TRUNCATE TABLE t`),
    // and it can only ever mean a table, so the verb alone settles it.
    if (verb == L"TRUNCATE") return true;
    // RENAME TABLE is MySQL's; a bare `RENAME` in other dialects is not a table op.
    if (verb == L"RENAME")
        return w.size() > 1 && w[1] == L"TABLE";
    if (verb != L"CREATE" && verb != L"DROP" && verb != L"ALTER") return false;

    // Walk past the qualifiers to the object-type keyword. Finding TABLE means
    // yes; finding anything else (VIEW / INDEX / FUNCTION / DATABASE / …) means
    // no — those have their own folders and their own refresh paths.
    for (size_t i = 1; i < w.size(); ++i) {
        if (IsNoiseWord(w[i])) continue;
        return w[i] == L"TABLE";
    }
    return false;   // ran out of words — a truncated / malformed statement
}

bool ScriptTouchesTables(const std::vector<wxString>& statements)
{
    for (const wxString& s : statements)
        if (StatementTouchesTables(s)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Execution plans — see the header for the "nothing here executes" contract.
// ---------------------------------------------------------------------------
bool IsExplainable(const wxString& statement)
{
    const std::vector<wxString> w = LeadingWords(statement, 2);
    if (w.empty()) return false;
    const wxString& v = w[0];
    return v == L"SELECT" || v == L"WITH" || v == L"INSERT" ||
           v == L"UPDATE" || v == L"DELETE" || v == L"REPLACE";
}

std::vector<wxString> BuildExplainSql(const wxString& statement, Dialect dialect)
{
    std::vector<wxString> out;
    if (!IsExplainable(statement)) return out;

    // Trim so the engine never sees a leading newline before the verb, and drop
    // a trailing ';' — several engines reject `EXPLAIN SELECT 1;;`.
    wxString s = statement;
    s.Trim(true).Trim(false);
    while (!s.IsEmpty() && s.Last() == ';') { s.RemoveLast(); s.Trim(true); }
    if (s.IsEmpty()) return out;

    switch (dialect) {
    case Dialect::Postgres:
        // VERBOSE + COSTS, never ANALYZE: the planner is asked what it WOULD do.
        out.push_back(L"EXPLAIN (VERBOSE, COSTS) " + s);
        break;
    case Dialect::Sqlite:
        out.push_back(L"EXPLAIN QUERY PLAN " + s);
        break;
    case Dialect::Oracle:
        // Two steps by design: the first stores the plan in PLAN_TABLE and
        // returns nothing, the second reads it back as rows.
        out.push_back(L"EXPLAIN PLAN FOR " + s);
        out.push_back(L"SELECT PLAN_TABLE_OUTPUT FROM TABLE(DBMS_XPLAN.DISPLAY())");
        break;
    case Dialect::SqlServer:
        break;              // see the header: deliberately unsupported
    case Dialect::MySQL:
        out.push_back(L"EXPLAIN " + s);
        break;
    }
    return out;
}

} // namespace db
