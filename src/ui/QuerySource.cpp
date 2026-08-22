// QuerySource.cpp — see QuerySource.h for why this exists and what it refuses.
#include "ui/QuerySource.h"

#include "db/RoutineNormalize.h"   // the shared dialect quote-state machine

#include <vector>

namespace ui::qsrc {
namespace {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------
// The input is already comment-free (NormalizeRoutineBody ran first), so the
// scanner needs quote states only.
enum class TokKind {
    Word,    // an UNQUOTED identifier or keyword, verbatim (case NOT folded)
    Ident,   // a QUOTED identifier, already unwrapped and un-doubled
    Str,     // a string literal — never an identifier, so its text is dropped
    Punct    // one character of anything else
};

struct Tok {
    TokKind  kind = TokKind::Punct;
    wxString text;
    int      depth = 0;   // parenthesis nesting at this token
};

// Unquoted identifier characters. `$` and `#` are legal in MySQL names; every
// code point >= 0x80 is accepted so an unquoted CJK table name (legal on both
// MySQL and PostgreSQL) is one token rather than a run of Punct.
bool IsIdentChar(wxChar c)
{
    if (c >= 128) return true;
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') || c == L'_' || c == L'$' || c == L'#';
}

bool IsSpace(wxChar c)
{
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r';
}

// PostgreSQL `$$` / `$tag$`. A tag is empty or [A-Za-z_][A-Za-z_0-9]*, which is
// what stops a SQL-function parameter reference like `$1` from opening a literal
// — the same rule RoutineNormalize applies. Without this the CONTENTS of a
// dollar-quoted string tokenize as ordinary words, and `SELECT $$ from x $$ FROM t`
// binds to `x`: a wrong table derived from inside a string literal.
bool DollarOpener(const wxString& s, size_t i, size_t& len)
{
    if (i >= s.length() || s[i] != L'$') return false;
    size_t j = i + 1;
    if (j < s.length() && s[j] != L'$') {
        const wxChar c0 = s[j];
        if (!((c0 >= L'a' && c0 <= L'z') || (c0 >= L'A' && c0 <= L'Z') || c0 == L'_'))
            return false;
        ++j;
        while (j < s.length() &&
               ((s[j] >= L'a' && s[j] <= L'z') || (s[j] >= L'A' && s[j] <= L'Z') ||
                (s[j] >= L'0' && s[j] <= L'9') || s[j] == L'_')) ++j;
    }
    if (j >= s.length() || s[j] != L'$') return false;
    len = j - i + 1;
    return true;
}

bool Is(const Tok& t, TokKind k, const wchar_t* text)
{
    return t.kind == k && t.text.CmpNoCase(text) == 0;
}

enum class ScanOutcome { Ok, Unterminated, UnknownQuoting };

// Read one quoted identifier delimited by `q`, collapsing a DOUBLED delimiter to
// a single literal character. This is the whole point of the module: `` `we``ird` ``
// yields ``we`ird``, where the old code yielded `weird`.
bool ReadQuoted(const wxString& s, size_t& i, wxChar q, wxString& out)
{
    ++i;                       // opening delimiter
    for (; i < s.length(); ++i) {
        if (s[i] != q) { out += s[i]; continue; }
        if (i + 1 < s.length() && s[i + 1] == q) { out += q; ++i; continue; }
        ++i;                   // closing delimiter
        return true;
    }
    return false;              // ran off the end still inside the identifier
}

ScanOutcome Tokenize(const wxString& s, const db::sync::NormalizeRules& rules,
                     std::vector<Tok>& out)
{
    int depth = 0;
    for (size_t i = 0; i < s.length();) {
        const wxChar c = s[i];
        if (IsSpace(c)) { ++i; continue; }

        // String literal. Its CONTENT is never an identifier, so a ` from ` or a
        // table name inside one must not be seen by the grammar below.
        if (c == L'\'') {
            ++i;
            bool closed = false;
            while (i < s.length()) {
                if (rules.backslashEscapes && s[i] == L'\\' && i + 1 < s.length()) {
                    i += 2; continue;
                }
                if (s[i] == L'\'') {
                    if (i + 1 < s.length() && s[i + 1] == L'\'') { i += 2; continue; }
                    ++i; closed = true; break;
                }
                ++i;
            }
            if (!closed) return ScanOutcome::Unterminated;
            out.push_back({TokKind::Str, wxString(), depth});
            continue;
        }

        // A double-quoted identifier. Treated as an identifier in EVERY dialect:
        // in the one position this module cares about — right after FROM — a
        // string literal is not valid SQL anyway, so reading `"t"` as a name is
        // the only interpretation that can be meant, and it keeps MySQL under
        // ANSI_QUOTES working. The doubling rule is `""` either way.
        if (c == L'"' || (c == L'`' && rules.backtickIdent)) {
            wxString v;
            if (!ReadQuoted(s, i, c, v)) return ScanOutcome::Unterminated;
            out.push_back({TokKind::Ident, v, depth});
            continue;
        }

        if (c == L'$' && rules.dollarQuoting) {
            size_t tagLen = 0;
            if (DollarOpener(s, i, tagLen)) {
                const wxString tag = s.Mid(i, tagLen);
                const size_t   at  = s.find(tag, i + tagLen);
                if (at == wxString::npos) return ScanOutcome::Unterminated;
                i = at + tag.length();
                out.push_back({TokKind::Str, wxString(), depth});
                continue;
            }
            // Not an opener (`$1`, a bare `$`): falls through to the word scan.
        }

        // SQL Server `[name]` (and its `]]` doubling) is a quoting form this
        // scanner does not model. Refusing beats reading `[t]` as a NAME that
        // happens to contain brackets, which is what the old stripper did.
        if (c == L'[') return ScanOutcome::UnknownQuoting;

        if (IsIdentChar(c)) {
            const size_t start = i;
            while (i < s.length() && IsIdentChar(s[i])) ++i;
            out.push_back({TokKind::Word, s.Mid(start, i - start), depth});
            continue;
        }

        if (c == L'(') { out.push_back({TokKind::Punct, L"(", depth}); ++depth; ++i; continue; }
        if (c == L')') { if (depth > 0) --depth;
                         out.push_back({TokKind::Punct, L")", depth}); ++i; continue; }

        out.push_back({TokKind::Punct, wxString(c), depth});
        ++i;
    }
    return ScanOutcome::Ok;
}

// ---------------------------------------------------------------------------
// Grammar
// ---------------------------------------------------------------------------
// Anything beyond "SELECT <list> FROM <one table> [alias] [WHERE/ORDER/LIMIT…]"
// is NotSimple. The set is deliberately over-broad: a false NotSimple costs the
// user an editable grid, a false Ok costs them the wrong table.
bool IsDisqualifyingWord(const Tok& t)
{
    static const wchar_t* kWords[] = {
        L"join", L"union", L"intersect", L"except", L"group", L"distinct",
        L"having", L"into", L"values", L"lateral", L"unnest"
    };
    for (const wchar_t* w : kWords) if (Is(t, TokKind::Word, w)) return true;
    return false;
}

bool IsAggregateCall(const std::vector<Tok>& t, size_t i)
{
    static const wchar_t* kFns[] = {
        L"count", L"sum", L"avg", L"min", L"max",
        L"group_concat", L"string_agg", L"array_agg"
    };
    if (i + 1 >= t.size() || !Is(t[i + 1], TokKind::Punct, L"(")) return false;
    for (const wchar_t* f : kFns) if (Is(t[i], TokKind::Word, f)) return true;
    return false;
}

SelectSource NotSimple()
{
    SelectSource s;
    s.status = SourceStatus::NotSimple;
    return s;
}

} // namespace

SelectSource ParseSelectSource(const wxString& sql, db::Dialect d)
{
    const db::sync::NormalizeRules rules = db::sync::RulesFor(d);

    // Reused wholesale: comment removal that knows `#`, `--<space>`, `$$…$$` and
    // `/*! … */`, and that never touches the inside of a quoted run. When it says
    // Unresolved the text ended mid-quote — there is nothing honest to parse.
    const db::sync::NormalizeResult norm = db::sync::NormalizeRoutineBody(sql, rules);
    if (!norm.Ok()) {
        SelectSource s;
        s.status = SourceStatus::Unparsable;
        return s;
    }

    std::vector<Tok> t;
    switch (Tokenize(norm.text, rules, t)) {
    case ScanOutcome::Unterminated: {
        SelectSource s; s.status = SourceStatus::Unparsable; return s;
    }
    case ScanOutcome::UnknownQuoting: {
        SelectSource s; s.status = SourceStatus::UnknownQuoting; return s;
    }
    case ScanOutcome::Ok:
        break;
    }

    if (t.empty() || !Is(t[0], TokKind::Word, L"select")) return NotSimple();

    size_t from = 0;
    bool   haveFrom = false;
    for (size_t i = 1; i < t.size(); ++i) {
        // A second SELECT is a subquery or a set operation either way: the
        // statement then has more than one candidate base table and this module
        // will not choose between them.
        if (Is(t[i], TokKind::Word, L"select")) return NotSimple();
        if (IsDisqualifyingWord(t[i])) return NotSimple();
        if (IsAggregateCall(t, i)) return NotSimple();
        if (!haveFrom && t[i].depth == 0 && Is(t[i], TokKind::Word, L"from")) {
            from = i;
            haveFrom = true;
        }
    }
    if (!haveFrom) return NotSimple();

    // The FROM target: `name`, `qual.name`, and nothing longer.
    std::vector<wxString> parts;
    size_t i = from + 1;
    for (;;) {
        if (i >= t.size()) return NotSimple();
        if (t[i].kind != TokKind::Word && t[i].kind != TokKind::Ident)
            return NotSimple();                       // `(` (derived table), `,`, …
        parts.push_back(t[i].text);
        ++i;
        if (i < t.size() && Is(t[i], TokKind::Punct, L".")) { ++i; continue; }
        break;
    }
    // db.schema.table cannot be expressed by the bare name EditableSpec carries.
    if (parts.size() > 2) return NotSimple();
    // A comma means a second table in the FROM list (an old-style join).
    if (i < t.size() && Is(t[i], TokKind::Punct, L",")) return NotSimple();
    // `FROM t (…)` is a function call / derived table, not a plain table.
    if (i < t.size() && Is(t[i], TokKind::Punct, L"(")) return NotSimple();

    SelectSource s;
    s.status = SourceStatus::Ok;
    s.table  = parts.back();
    if (parts.size() == 2) s.qualifier = parts.front();
    if (s.table.IsEmpty()) return NotSimple();        // `FROM ""`
    return s;
}

BindRefusal ClassifyBind(const SelectSource& src, db::Dialect d,
                         const wxString& currentDb)
{
    switch (src.status) {
    case SourceStatus::NotSimple:      return BindRefusal::NotSimpleSelect;
    case SourceStatus::Unparsable:     return BindRefusal::Unparsable;
    case SourceStatus::UnknownQuoting: return BindRefusal::UnknownQuoting;
    case SourceStatus::Ok:             break;
    }

    if (src.qualifier.IsEmpty()) return BindRefusal::None;

    // See the qualifier rule in QuerySource.h. MySQL is the only dialect where an
    // explicit qualifier can be PROVEN to name the table we would then write to.
    if (d == db::Dialect::MySQL && !currentDb.IsEmpty() && src.qualifier == currentDb)
        return BindRefusal::None;

    return BindRefusal::ForeignQualifier;
}

} // namespace ui::qsrc
