// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCellCompare.cpp — see header. Two layers, the same split SyncValueMap.cpp
// uses: BuildCompareRules does all the classification once per column;
// CellsEqual/CompareCells switch on the precomputed CompareOp and derive nothing.
#include "db/SyncCellCompare.h"

#include "db/SyncTemporal.h"

namespace db::sync {
namespace {

// The canonical numeric form of a cell, for Boolish/Numeric columns.
//   rank 0 => `v` is meaningful and orders arithmetically.
//   rank 1 => the cell has no numeric canonical form; it is ranked AFTER every
//             canonicalizable value and ordered among its peers by code point.
// Splitting on rank first is what keeps the order total AND transitive: it is a
// lexicographic order on (rank, key), and both components are total orders.
struct Canon {
    int       rank = 0;
    long long v    = 0;
    double    d    = 0;
    bool      isDouble = false;
};

// `allowWords` distinguishes Boolish (accepts t/f/true/false/y/n/yes/no) from
// Numeric (does not — an integer column containing the text "true" is not
// something we should quietly invent a value for).
Canon Canonicalize(const Cell& c, bool allowWords)
{
    Canon out;
    long long ll = 0;
    if (c.text.ToLongLong(&ll)) { out.v = ll; return out; }

    if (allowWords) {
        const wxString s = c.text.Lower();
        if (s == L"t" || s == L"true"  || s == L"y" || s == L"yes") { out.v = 1; return out; }
        if (s == L"f" || s == L"false" || s == L"n" || s == L"no")  { out.v = 0; return out; }
    }

    // Mirrors CmpCell's own fallback so a Numeric column holding a decimal text
    // still orders arithmetically rather than lexicographically.
    double d = 0;
    if (c.text.ToDouble(&d)) { out.isDouble = true; out.d = d; return out; }

    out.rank = 1;
    return out;
}

int CmpCanon(const Canon& a, const Canon& b, const Cell& ca, const Cell& cb)
{
    if (a.rank != b.rank) return a.rank < b.rank ? -1 : 1;
    if (a.rank == 1)      return CmpCodePoints(ca.text, cb.text);
    // Both canonicalized. Integers compare as integers; a double on either side
    // promotes the comparison to double, which is exactly what CmpCell does.
    if (!a.isDouble && !b.isDouble)
        return a.v < b.v ? -1 : (a.v > b.v ? 1 : 0);
    const double da = a.isDouble ? a.d : static_cast<double>(a.v);
    const double db = b.isDouble ? b.d : static_cast<double>(b.v);
    return da < db ? -1 : (da > db ? 1 : 0);
}

// Temporal ordering, with the tie between two un-canonicalizable values broken
// by code point — CmpTemporalKey cannot do it because it no longer has the text.
// Keeping the fallback here (rather than inside SyncTemporal) is what makes the
// rank scheme a lexicographic order on (rank, key-or-text): total and
// transitive, which a sorted merge's comparator is required to be.
int CmpTemporal(const Cell& a, const Cell& b)
{
    const TemporalKey ka = ParseTemporal(a.text);
    const TemporalKey kb = ParseTemporal(b.text);
    if (ka.rank == TemporalKey::Unparseable && kb.rank == TemporalKey::Unparseable)
        return CmpCodePoints(a.text, b.text);
    return CmpTemporalKey(ka, kb);
}

// NULL handling, shared by every op and identical to the historical CmpCell:
// NULL sorts before every non-NULL, two NULLs are equal. `handled` tells the
// caller whether the result is authoritative.
int CmpNull(const Cell& a, const Cell& b, bool& handled)
{
    const bool an = a.kind == CellKind::Null, bn = b.kind == CellKind::Null;
    handled = an || bn;
    if (!handled) return 0;
    return (an && bn) ? 0 : (an ? -1 : 1);
}

} // namespace

// ---------------------------------------------------------------------------
// The historical raw comparators, moved verbatim from DataSync.cpp.
// ---------------------------------------------------------------------------

// Compare two strings by Unicode CODE POINT.
//
// Not wxString::Cmp. On MSW a wxString is UTF-16, and Cmp compares code *units*,
// which is a different order from code points: a supplementary character encodes
// as a lead surrogate in U+D800..U+DBFF and therefore sorts BEFORE U+E000..U+FFFF
// under code-unit order, but AFTER them under code-point order — which is also
// UTF-8 byte order, which is what the servers give us once binaryOrder forces
// utf8mb4_bin / "C" (see StreamOptions). Iterating a wxString yields decoded
// wxUniChars, i.e. whole code points, so this agrees with the servers across the
// full range rather than only the BMP.
int CmpCodePoints(const wxString& a, const wxString& b)
{
    wxString::const_iterator ia = a.begin(), ib = b.begin();
    for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
        const wxUint32 ca = (*ia).GetValue(), cb = (*ib).GetValue();
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (ia != a.end()) return 1;    // a has b as a strict prefix
    if (ib != b.end()) return -1;
    return 0;
}

// Total order used by the merge for an un-coerced column. MUST match the
// server's ORDER BY: numeric PKs compare numerically (the validated path); text
// compares by code point, which is what StreamOptions::binaryOrder makes both
// servers use.
int CmpCell(const Cell& a, const Cell& b)
{
    bool handled = false;
    const int n = CmpNull(a, b, handled);
    if (handled) return n;
    if (a.kind == CellKind::Numeric && b.kind == CellKind::Numeric) {
        long long la = 0, lb = 0;
        if (a.text.ToLongLong(&la) && b.text.ToLongLong(&lb))
            return la < lb ? -1 : (la > lb ? 1 : 0);
        double da = 0, db2 = 0;
        if (a.text.ToDouble(&da) && b.text.ToDouble(&db2))
            return da < db2 ? -1 : (da > db2 ? 1 : 0);
    }
    return CmpCodePoints(a.text, b.text);
}

// ---------------------------------------------------------------------------
// Classification — once per column, never per cell.
// ---------------------------------------------------------------------------
CompareOp CompareOpFor(const ColumnBridge& bridge)
{
    // The fast exit, and the one that makes same-engine free: BuildBridges marks
    // every same-engine column passthrough, and a passthrough column by
    // definition needs no conversion, hence no canonicalization either.
    if (bridge.passthrough) return CompareOp::Raw;

    switch (bridge.op) {
    // Both directions of the boolean coercion canonicalize to the SAME integer
    // space, which is why one op covers both: the merge must not care which side
    // happens to be the PostgreSQL one.
    case BridgeOp::BoolToPgBool:
    case BridgeOp::PgBoolToInt:
        return CompareOp::Boolish;

    case BridgeOp::IntRange:
        return CompareOp::Numeric;

    case BridgeOp::BinaryReframe:
    case BridgeOp::TextValidate:
        return CompareOp::TextOnly;

    // Naive wall-clock on both sides: canonicalizable, and provably
    // order-preserving per side, so it may also key.
    case BridgeOp::TemporalGuard:
        return CompareOp::Temporal;

    // Timezone semantics on at least one side. What each engine printed depends
    // on a session timezone the cell text does not carry, so equality is not
    // decidable from the text at all — not merely un-orderable. Refused as a key
    // and compared by text otherwise; BuildBridges already attached the Finding
    // that makes this visible rather than silent.
    case BridgeOp::TemporalTzAmbiguous:
        return CompareOp::Unsafe;

    // A non-passthrough Identity is not something BuildBridges produces; treat it
    // as raw rather than inventing a canonical form for it.
    case BridgeOp::Identity:
        return CompareOp::Raw;
    }

    // NO silent default. A BridgeOp added later and not classified above lands
    // here, is usable for equality, and is refused as a key by KeyOrderSafe —
    // fail closed, per the header's ordering contract.
    return CompareOp::Unsafe;
}

void BuildCompareRules(const std::vector<ColumnBridge>& bridges,
                       CompareRules& out)
{
    out.ops.clear();
    out.ops.reserve(bridges.size());
    out.allRaw = true;
    for (const ColumnBridge& b : bridges) {
        const CompareOp op = CompareOpFor(b);
        if (op != CompareOp::Raw) out.allRaw = false;
        out.ops.push_back(op);
    }
}

// ---------------------------------------------------------------------------
// The hot path.
// ---------------------------------------------------------------------------
bool CellsEqual(const Cell& a, const Cell& b, CompareOp op)
{
    // Historical behavior, preserved bit-for-bit: equality is CellKind-sensitive
    // for an un-coerced column. Relaxing it here would silently change every
    // same-engine verdict in the product.
    if (op == CompareOp::Raw) return a.kind == b.kind && a.text == b.text;

    const bool an = a.kind == CellKind::Null, bn = b.kind == CellKind::Null;
    if (an || bn) return an && bn;

    switch (op) {
    case CompareOp::Boolish:
    case CompareOp::Numeric:
        return CmpCanon(Canonicalize(a, op == CompareOp::Boolish),
                        Canonicalize(b, op == CompareOp::Boolish), a, b) == 0;
    case CompareOp::Temporal:
        return CmpTemporal(a, b) == 0;
    // CellKind deliberately ignored: the same payload may arrive labelled Text
    // by one driver and Binary by the other, and a kind-only difference is not a
    // difference in the DATA.
    case CompareOp::TextOnly:
    case CompareOp::Unsafe:
    default:
        return a.text == b.text;
    }
}

int CompareCells(const Cell& a, const Cell& b, CompareOp op)
{
    if (op == CompareOp::Raw) return CmpCell(a, b);

    bool handled = false;
    const int n = CmpNull(a, b, handled);
    if (handled) return n;

    switch (op) {
    case CompareOp::Boolish:
    case CompareOp::Numeric:
        return CmpCanon(Canonicalize(a, op == CompareOp::Boolish),
                        Canonicalize(b, op == CompareOp::Boolish), a, b);
    case CompareOp::Temporal:
        return CmpTemporal(a, b);
    case CompareOp::TextOnly:
    case CompareOp::Unsafe:
    default:
        return CmpCodePoints(a.text, b.text);
    }
}

} // namespace db::sync
