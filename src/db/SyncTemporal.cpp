// SyncTemporal.cpp — see header. A hand-written recursive-descent scan rather
// than wxDateTime::ParseFormat or sscanf: this runs once per temporal cell on a
// 10M-cell table, must not allocate, must not consult a locale (wxDateTime's
// parsers do both), and must REJECT rather than best-effort-accept — a parser
// that silently swallows a trailing `+08:00` would reintroduce exactly the
// silent wrongness this file exists to prevent.
#include "db/SyncTemporal.h"

namespace db::sync {
namespace {

// A cursor over the text. Holds a reference, copies nothing.
struct Cur {
    const wxString& s;
    size_t          i = 0;

    explicit Cur(const wxString& t) : s(t) {}

    bool      End()  const { return i >= s.length(); }
    wxUniChar Peek() const { return s[i]; }
    bool      Eat(wchar_t c) { if (!End() && s[i] == c) { ++i; return true; } return false; }
};

// Scan between `minN` and `maxN` decimal digits. Returns false (and leaves the
// cursor where it stopped) when fewer than `minN` were available. `got` reports
// how many were consumed, which the fractional-second scaling needs.
bool Digits(Cur& c, int minN, int maxN, long long& out, int& got)
{
    out = 0;
    got = 0;
    while (got < maxN && !c.End()) {
        const wxUniChar ch = c.Peek();
        if (ch < L'0' || ch > L'9') break;
        out = out * 10 + static_cast<long long>(ch.GetValue() - L'0');
        ++c.i;
        ++got;
    }
    return got >= minN;
}

// Optional `.fff...`, scaled to nanoseconds so that a value written with 0, 1, 3
// or 6 digits lands on the same number. Absent fraction => 0, which is why
// `12:00:00` and `12:00:00.000000` compare equal.
//
// More than 9 digits is refused rather than truncated: truncation is not
// injective, and no engine we target renders more than 6 anyway, so this can
// only ever fire on text we do not actually understand.
bool ParseFrac(Cur& c, long long& frac)
{
    frac = 0;
    if (!c.Eat(L'.')) return true;

    long long v = 0;
    int       n = 0;
    if (!Digits(c, 1, 9, v, n)) return false;
    if (!c.End() && c.Peek() >= L'0' && c.Peek() <= L'9') return false;

    // 10^(9-n): pads the written digits out to a fixed nanosecond denominator.
    static const long long kScale[10] = {
        0, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1
    };
    frac = v * kScale[n];
    return true;
}

// A trailing timezone offset, which must run to the end of the text.
//   true  => consumed an offset of ZERO (or there was nothing left): the text
//            denotes the same instant with it removed, so dropping it is pure
//            spelling normalization.
//   false => a NON-ZERO offset, or trailing junk. Refused; see the header for
//            why we do not shift such a value into UTC.
bool EatUtcOffsetOrEnd(Cur& c)
{
    if (c.End()) return true;

    if (c.Eat(L'Z') || c.Eat(L'z')) return c.End();

    const wxUniChar sign = c.Peek();
    if (sign != L'+' && sign != L'-') return false;
    ++c.i;

    long long hh = 0, mm = 0;
    int       n  = 0;
    if (!Digits(c, 2, 2, hh, n)) return false;
    if (!c.End()) {
        c.Eat(L':');                       // `+00:00` and `+0000` are both seen
        if (!Digits(c, 2, 2, mm, n)) return false;
    }
    if (!c.End()) return false;
    return hh == 0 && mm == 0;
}

} // namespace

TemporalKey ParseTemporal(const wxString& s)
{
    TemporalKey k;                          // defaults to Unparseable
    if (s.empty()) return k;

    // Checked before parsing, so no branch below has to defend against a month
    // or day of zero. Matches ConvertCell's IsZeroDate test exactly (prefix, so
    // both the bare DATE and the DATETIME spelling land here).
    if (s.StartsWith(L"0000-00-00")) { k.rank = TemporalKey::Zero; return k; }

    Cur  c(s);
    const bool neg = c.Eat(L'-');           // only legal on a bare MySQL TIME

    long long head = 0;
    int       n    = 0;
    if (!Digits(c, 1, 10, head, n)) return k;

    // ---- YYYY-MM-DD[ HH:MM:SS[.f][tz]] ------------------------------------
    if (!neg && n == 4 && !c.End() && c.Peek() == L'-') {
        ++c.i;
        long long mo = 0, d = 0;
        if (!Digits(c, 2, 2, mo, n)) return k;
        if (!c.Eat(L'-'))            return k;
        if (!Digits(c, 2, 2, d, n))  return k;
        // Range-checked, not calendar-checked: rejecting 2024-02-31 would need a
        // leap-year table to buy nothing, since no server emits one. What this
        // does buy is that a malformed field can never masquerade as a real date
        // and sort in among the real ones.
        if (mo < 1 || mo > 12 || d < 1 || d > 31) return k;
        k.date = head * 10000 + mo * 100 + d;

        if (c.End()) { k.rank = TemporalKey::Parsed; return k; }

        // `T` is the ISO separator; a space is what both engines actually print.
        if (!(c.Eat(L' ') || c.Eat(L'T') || c.Eat(L't'))) return k;

        long long h = 0, mi = 0, se = 0;
        if (!Digits(c, 2, 2, h, n))  return k;
        if (!c.Eat(L':'))            return k;
        if (!Digits(c, 2, 2, mi, n)) return k;
        if (!c.Eat(L':'))            return k;
        if (!Digits(c, 2, 2, se, n)) return k;
        if (h > 23 || mi > 59 || se > 60) return k;   // 60 = leap second
        k.secs = h * 3600 + mi * 60 + se;

        if (!ParseFrac(c, k.frac))   return k;
        if (!EatUtcOffsetOrEnd(c))   return k;

        k.rank = TemporalKey::Parsed;
        return k;
    }

    // ---- [-]H[HH]:MM:SS[.f] -- a bare TIME --------------------------------
    // `date` stays 0: every row of a TIME column parses this way, so the field
    // is constant across the stream and drops out of the comparison.
    if (n > 3 || c.End() || c.Peek() != L':') return k;
    ++c.i;

    long long mi = 0, se = 0;
    if (!Digits(c, 2, 2, mi, n)) return k;
    if (!c.Eat(L':'))            return k;
    if (!Digits(c, 2, 2, se, n)) return k;
    // MySQL TIME runs to +/-838:59:59, well past a single day.
    if (head > 838 || mi > 59 || se > 60) return k;
    k.secs = head * 3600 + mi * 60 + se;

    if (!ParseFrac(c, k.frac))   return k;
    if (!EatUtcOffsetOrEnd(c))   return k;

    // Sign is applied to BOTH fields so the lexicographic (secs, frac) order
    // stays chronological on the negative side too: -12:00:00.5 must sort before
    // -12:00:00, and (-43200, -5e8) < (-43200, 0) as required.
    if (neg) { k.secs = -k.secs; k.frac = -k.frac; }

    k.rank = TemporalKey::Parsed;
    return k;
}

int CmpTemporalKey(const TemporalKey& a, const TemporalKey& b)
{
    // Rank first — this is what keeps the order total AND transitive regardless
    // of what the value fields happen to contain.
    if (a.rank != b.rank) return a.rank < b.rank ? -1 : 1;

    // Every zero date is the same zero date; two Unparseable keys are tied here
    // and broken by code point upstream, where the original text still exists.
    if (a.rank != TemporalKey::Parsed) return 0;

    if (a.date != b.date) return a.date < b.date ? -1 : 1;
    if (a.secs != b.secs) return a.secs < b.secs ? -1 : 1;
    if (a.frac != b.frac) return a.frac < b.frac ? -1 : 1;
    return 0;
}

} // namespace db::sync
