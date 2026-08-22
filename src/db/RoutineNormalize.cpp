// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// RoutineNormalize.cpp — the shared character-walk state machine. See the header
// for the design rationale; this file is the machine and nothing else.
#include "db/RoutineNormalize.h"

#include "db/DbDriver.h"   // db::Dialect

namespace db::sync {

namespace {

bool IsSpace(wxUniChar c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

bool IsTagStart(wxUniChar c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool IsTagBody(wxUniChar c)
{
    return IsTagStart(c) || (c >= '0' && c <= '9');
}

// Fold CRLF and lone CR to LF. Applied to the WHOLE body, quoted runs included —
// see the header for why that trade is made deliberately.
wxString FoldNewlines(const wxString& in)
{
    wxString out;
    out.reserve(in.length());
    const size_t n = in.length();
    for (size_t i = 0; i < n; ++i) {
        const wxUniChar c = in[i];
        if (c == '\r') {
            if (i + 1 < n && in[i + 1] == '\n') continue;   // CRLF: drop the CR
            out += '\n';                                    // lone CR
        } else {
            out += c;
        }
    }
    return out;
}

// Is `s[i]` the start of a dollar-quote opener? On success `tagLen` is the FULL
// opener length including both '$'. An invalid tag (e.g. `$1$`, a SQL-function
// parameter reference) is not an opener.
bool DollarOpener(const wxString& s, size_t i, size_t& tagLen)
{
    const size_t n = s.length();
    if (i >= n || s[i] != '$') return false;
    size_t j = i + 1;
    if (j < n && s[j] != '$') {
        if (!IsTagStart(s[j])) return false;
        ++j;
        while (j < n && IsTagBody(s[j])) ++j;
    }
    if (j >= n || s[j] != '$') return false;
    tagLen = j - i + 1;
    return true;
}

// The scanner's states. Named exactly as ADR-013 Q3 specifies.
enum class ScanState {
    Code,
    LineComment,
    BlockComment,
    SingleQuote,
    DoubleQuote,
    BacktickOrIdent,
    DollarQuote
};

NormalizeResult Unresolved(const wxString& original, const wxString& why)
{
    NormalizeResult r;
    r.status = NormalizeStatus::Unresolved;
    r.text   = original;      // ORIGINAL, never a half-normalized body
    r.reason = why;
    return r;
}

} // namespace

NormalizeRules MySqlNormalizeRules()
{
    NormalizeRules r;
    r.dollarQuoting            = false;   // MySQL has no dollar quoting
    r.dashDashNeedsSpace       = true;    // `x--1` is arithmetic, not a comment
    r.hashLineComment          = true;
    r.backtickIdent            = true;
    r.backslashEscapes         = true;
    r.preserveExecutableComment = true;   // /*! ... */ is code
    return r;
}

NormalizeRules PgNormalizeRules()
{
    NormalizeRules r;
    r.dollarQuoting            = true;
    r.dashDashNeedsSpace       = false;   // bare `--` is a comment in PG
    r.hashLineComment          = false;
    r.backtickIdent            = false;   // a backtick is just a character
    r.backslashEscapes         = false;   // standard_conforming_strings = on
    r.preserveExecutableComment = false;
    return r;
}

bool NormalizeRulesKnown(Dialect d)
{
    return d == Dialect::MySQL || d == Dialect::Postgres;
}

NormalizeRules RulesFor(Dialect d)
{
    if (d == Dialect::MySQL)    return MySqlNormalizeRules();
    if (d == Dialect::Postgres) return PgNormalizeRules();
    return NormalizeRules{};   // conservative defaults for unmodelled dialects
}

NormalizeResult NormalizeRoutineBody(const wxString& body, const NormalizeRules& rules)
{
    const wxString s = FoldNewlines(body);
    const size_t   n = s.length();

    wxString out;
    out.reserve(n);

    ScanState st           = ScanState::Code;
    bool      pendingSpace = false;   // deferred: emitted only before real content
    wxString  dollarTag;              // the full `$tag$` opener we must re-find
    size_t    i            = 0;

    // Emit one character of CODE, honouring the deferred-space rule. Leading
    // whitespace is dropped (out is empty), trailing whitespace never lands
    // (pendingSpace is simply never flushed at EOF).
    auto emit = [&](wxUniChar c) {
        if (pendingSpace) {
            if (!out.empty()) out += ' ';
            pendingSpace = false;
        }
        out += c;
    };

    while (i < n) {
        const wxUniChar c = s[i];

        switch (st) {
        case ScanState::Code: {
            if (IsSpace(c)) { pendingSpace = true; ++i; break; }

            // ---- line comments ----
            if (c == '-' && i + 1 < n && s[i + 1] == '-') {
                const bool spaced = (i + 2 >= n) || IsSpace(s[i + 2]);
                if (!rules.dashDashNeedsSpace || spaced) {
                    st = ScanState::LineComment;
                    i += 2;
                    break;
                }
                // MySQL `x--1`: NOT a comment. Fall through and emit the '-'.
            }
            if (c == '#' && rules.hashLineComment) {
                st = ScanState::LineComment;
                ++i;
                break;
            }

            // ---- block comments ----
            if (c == '/' && i + 1 < n && s[i + 1] == '*') {
                const bool executable =
                    rules.preserveExecutableComment && i + 2 < n &&
                    (s[i + 2] == '!' || s[i + 2] == '+');
                if (executable) {
                    // MySQL /*!50003 ... */ and /*+ hint */ are executed. Copy
                    // them through verbatim rather than deleting behaviour.
                    const size_t close = s.find(L"*/", i + 2);
                    if (close == wxString::npos)
                        return Unresolved(body, L"未闭合的可执行注释 /*! … */");
                    const size_t end = close + 2;
                    if (pendingSpace) { if (!out.empty()) out += ' '; pendingSpace = false; }
                    out += s.Mid(i, end - i);
                    i = end;
                    break;
                }
                st = ScanState::BlockComment;
                i += 2;
                break;
            }

            // ---- quote states ----
            if (c == '\'') { emit(c); st = ScanState::SingleQuote;  ++i; break; }
            if (c == '"')  { emit(c); st = ScanState::DoubleQuote;  ++i; break; }
            if (c == '`' && rules.backtickIdent) {
                emit(c); st = ScanState::BacktickOrIdent; ++i; break;
            }
            if (c == '$' && rules.dollarQuoting) {
                size_t tagLen = 0;
                if (DollarOpener(s, i, tagLen)) {
                    dollarTag = s.Mid(i, tagLen);
                    if (pendingSpace) { if (!out.empty()) out += ' '; pendingSpace = false; }
                    out += dollarTag;
                    st = ScanState::DollarQuote;
                    i += tagLen;
                    break;
                }
                // Not an opener (`$1`, a bare `$`): ordinary code.
            }

            emit(c);
            ++i;
            break;
        }

        case ScanState::LineComment: {
            // EOF inside a line comment is perfectly well-formed — a comment on
            // the last line needs no terminator. Only the newline returns us to
            // Code, and it does so as WHITESPACE (rule 4: never fuse tokens).
            if (c == '\n') { st = ScanState::Code; pendingSpace = true; }
            ++i;
            break;
        }

        case ScanState::BlockComment: {
            if (c == '*' && i + 1 < n && s[i + 1] == '/') {
                st = ScanState::Code;
                pendingSpace = true;
                i += 2;
            } else {
                ++i;
            }
            break;
        }

        case ScanState::SingleQuote:
        case ScanState::DoubleQuote:
        case ScanState::BacktickOrIdent: {
            const wxUniChar q = (st == ScanState::SingleQuote)     ? wxUniChar('\'')
                              : (st == ScanState::DoubleQuote)     ? wxUniChar('"')
                                                                   : wxUniChar('`');
            // Backslash escapes apply to STRING literals only, and only where the
            // dialect says so. A quoted identifier never honours them.
            if (rules.backslashEscapes && c == '\\' && st != ScanState::BacktickOrIdent
                && i + 1 < n) {
                out += c;
                out += s[i + 1];
                i += 2;
                break;
            }
            if (c == q) {
                if (i + 1 < n && s[i + 1] == q) {   // '' / "" / `` -> doubled, stays open
                    out += c;
                    out += c;
                    i += 2;
                    break;
                }
                out += c;
                st = ScanState::Code;
                ++i;
                break;
            }
            out += c;   // verbatim: rule 1
            ++i;
            break;
        }

        case ScanState::DollarQuote: {
            if (c == '$' && s.length() - i >= dollarTag.length() &&
                s.Mid(i, dollarTag.length()) == dollarTag) {
                out += dollarTag;
                st = ScanState::Code;
                i += dollarTag.length();
                break;
            }
            out += c;   // verbatim: `--` and `/* */` in here are TEXT
            ++i;
            break;
        }
        }
    }

    switch (st) {
    case ScanState::Code:
    case ScanState::LineComment:
        break;   // both are legitimate end states
    case ScanState::BlockComment:
        return Unresolved(body, L"未闭合的块注释 /* …");
    case ScanState::SingleQuote:
        return Unresolved(body, L"未闭合的字符串字面量 ' …");
    case ScanState::DoubleQuote:
        return Unresolved(body, L"未闭合的双引号 \" …");
    case ScanState::BacktickOrIdent:
        return Unresolved(body, L"未闭合的反引号标识符 ` …");
    case ScanState::DollarQuote:
        return Unresolved(body, L"未闭合的美元引用 " + dollarTag + L" …");
    }

    NormalizeResult r;
    r.status = NormalizeStatus::Ok;
    r.text   = out;
    return r;
}

} // namespace db::sync
