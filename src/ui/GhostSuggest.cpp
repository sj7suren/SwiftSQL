// GhostSuggest.cpp — see header.
#include "ui/GhostSuggest.h"

#include <wx/wxcrt.h>   // wxIsalnum / wxIsspace

namespace ui::ghost {
namespace {

inline bool IsWordCh(wxUniChar c) { return wxIsalnum(c) || c == '_'; }

// Is the caret sitting inside a string literal or a comment on this line?
//
// Scanned over the WHOLE prefix rather than the current line, because a block
// comment or a multi-line string opened earlier is still open here. The scan is
// deliberately simple — it tracks the three things that can swallow a caret
// (single quotes, double quotes, `--`/`#` to end of line, `/* */`) and nothing
// more. Its only job is to answer "should we stay quiet", and staying quiet one
// time too often costs nothing.
bool InLiteralOrComment(const wxString& s)
{
    bool inSingle = false, inDouble = false, inBack = false, inBlock = false;
    const size_t n = s.length();
    for (size_t i = 0; i < n; ++i) {
        const wxUniChar c = s[i];
        if (inBlock) {
            if (c == '*' && i + 1 < n && s[i + 1] == '/') { inBlock = false; ++i; }
            continue;
        }
        if (inSingle) {
            if (c == '\\' && i + 1 < n) { ++i; continue; }   // escaped char
            if (c == '\'') {
                // '' inside a literal is an escaped quote, not the end.
                if (i + 1 < n && s[i + 1] == '\'') { ++i; continue; }
                inSingle = false;
            }
            continue;
        }
        if (inDouble) {
            if (c == '\\' && i + 1 < n) { ++i; continue; }
            if (c == '"') inDouble = false;
            continue;
        }
        if (inBack) { if (c == '`') inBack = false; continue; }

        if (c == '\'') { inSingle = true; continue; }
        if (c == '"')  { inDouble = true; continue; }
        if (c == '`')  { inBack  = true; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { inBlock = true; ++i; continue; }
        // A line comment swallows the caret only if no newline follows it.
        if ((c == '#') || (c == '-' && i + 1 < n && s[i + 1] == '-')) {
            const size_t nl = s.find('\n', i);
            if (nl == wxString::npos) return true;   // comment runs to the caret
            i = nl;
        }
    }
    return inSingle || inDouble || inBack || inBlock;
}

// The last complete word of `s`, upper-cased, plus whether whitespace separates
// it from the caret. Returns "" when the tail is not a word at all.
wxString LastWord(const wxString& s, bool& trailingSpace)
{
    size_t i = s.length();
    trailingSpace = false;
    while (i > 0 && wxIsspace(s[i - 1])) { trailingSpace = true; --i; }
    const size_t end = i;
    while (i > 0 && IsWordCh(s[i - 1])) --i;
    if (i == end) return wxString();
    return s.Mid(i, end - i).Upper();
}

// The word BEFORE the last one — needed to tell `SELECT` (wants ` * from `)
// from `ORDER BY` (wants nothing more).
wxString PrevWord(const wxString& s)
{
    size_t i = s.length();
    while (i > 0 && wxIsspace(s[i - 1])) --i;
    while (i > 0 && IsWordCh(s[i - 1])) --i;              // skip the last word
    while (i > 0 && wxIsspace(s[i - 1])) --i;             // skip the gap
    const size_t end = i;
    while (i > 0 && IsWordCh(s[i - 1])) --i;
    if (i == end) return wxString();
    return s.Mid(i, end - i).Upper();
}

// keyword -> what obviously follows it. Only entries that are UNAMBIGUOUS are
// listed: a skeleton that is right half the time is worse than none, because
// the user still has to read it before rejecting it.
struct Rule { const wchar_t* word; const wchar_t* cont; };

const Rule kRules[] = {
    { L"SELECT", L"* from " },      // the case this feature was asked for
    { L"INSERT", L"into " },
    { L"DELETE", L"from " },
    { L"ORDER",  L"by " },
    { L"GROUP",  L"by " },
    { L"LEFT",   L"join " },
    { L"RIGHT",  L"join " },
    { L"INNER",  L"join " },
    { L"OUTER",  L"join " },
    { L"CROSS",  L"join " },
    { L"CREATE", L"table " },
    { L"DROP",   L"table " },
    { L"ALTER",  L"table " },
    { L"TRUNCATE", L"table " },
    { L"IS",     L"null" },
    { L"UNION",  L"all " },
};

} // namespace

wxString LocalSkeleton(const wxString& beforeCaret, const wxString& afterLine,
                       db::Dialect dialect)
{
    (void)dialect;   // see the header: reserved, rules are dialect-neutral today

    // Rule 1: nothing mid-line.
    for (size_t i = 0; i < afterLine.length(); ++i)
        if (!wxIsspace(afterLine[i])) return wxString();

    if (beforeCaret.IsEmpty()) return wxString();

    // Rule 2: nothing inside a literal or a comment.
    if (InLiteralOrComment(beforeCaret)) return wxString();

    bool trailingSpace = false;
    const wxString last = LastWord(beforeCaret, trailingSpace);
    if (last.IsEmpty()) return wxString();

    // Rule 3: only after a COMPLETE keyword. Without a separating space the word
    // is still being typed and the keyword popup owns the interaction — two
    // hints fighting over one Tab key is exactly what we are avoiding.
    if (!trailingSpace) return wxString();

    // `ORDER BY` / `GROUP BY` already consumed their continuation; do not then
    // offer `BY`'s own (there is none) or re-offer anything for the pair.
    const wxString prev = PrevWord(beforeCaret);
    if (last == L"BY" || last == L"JOIN" || last == L"INTO" || last == L"TABLE")
        return wxString();
    // `INSERT INTO` -> the table name is the user's; `DELETE FROM` likewise.
    if (prev == L"INSERT" || prev == L"DELETE") return wxString();

    for (const Rule& r : kRules)
        if (last == r.word) return r.cont;

    return wxString();
}

} // namespace ui::ghost
