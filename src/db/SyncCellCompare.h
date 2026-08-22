// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCellCompare.h — dialect-aware CELL COMPARISON for the sorted-merge data
// diff (ADR-015 T2/T10). The comparison-side counterpart of SyncValueMap.h.
//
// ---- the defect this exists to fix ----------------------------------------
// DataSync's merge compared source and target cells RAW. A MySQL `tinyint(1)`
// holding 1 and a PostgreSQL `boolean` holding `t` are the same logical value,
// but "1" != "t" as text, so:
//   * in a NON-KEY column every such cell reported a spurious difference — a
//     false-positive UPDATE storm that breaks the release-blocking
//     zero-false-positive bar;
//   * in a KEY column the same row on both sides failed to match as the same
//     key, so the merge emitted DELETE + INSERT instead of one UPDATE (or
//     instead of nothing at all), silently rewriting rows nobody had changed.
//
// ---- why NOT "bridge the source cell into target space and compare" -------
// The obvious fix is to run the source cell through its ColumnBridge and
// compare the result against the raw target cell. It does not work, and the
// reason is worth stating because it is easy to re-propose:
//
//   ConvertCell's output is a value in SQL-LITERAL spelling, not in the target
//   server's READ-BACK spelling. BridgeOp::BoolToPgBool turns 1 into the text
//   "TRUE" because that is what belongs in an INSERT statement. The PostgreSQL
//   target stream hands back "t". "TRUE" != "t", so bridging the source would
//   leave the false positive exactly where it was — while also costing a
//   ConvertedCell (and its `reason` wxString) per cell.
//
// So we canonicalize BOTH sides into a NEUTRAL comparison form instead. That
// choice buys three things a one-sided bridge cannot:
//   1. It is symmetric, so ONE rule per column serves the source cell, the
//      target cell, and the within-side backwards-key check (defense 3) — which
//      compares a side against ITSELF and must therefore never be handed a
//      source->target conversion.
//   2. It allocates nothing: the neutral form of a boolean/integer is a
//      long long computed in a register, never a new wxString.
//   3. It is defined independently of which side is "source", so reversing the
//      sync direction cannot change a verdict.
//
// ---- the ordering contract (this is the dangerous part) -------------------
// The merge is a SORTED merge. Both servers sorted their stream by the RAW
// value; we compare by the canonical value. That is sound if and only if the
// canonicalization is ORDER-PRESERVING (monotone and injective) with respect to
// each side's own native ordering. If it is not, the two streams are no longer
// sorted by the comparator the merge uses, and the merge desyncs — inventing
// INSERTs and DELETEs out of identical data. That is precisely the silent
// corruption DataSyncOrderingTests was written to pin.
//
// Every CompareOp below is therefore accompanied by an explicit proof obligation,
// and KeyOrderSafe() is the gate: a key column whose CompareOp cannot discharge
// it is REFUSED rather than attempted.
//
// WHERE THAT REFUSAL SURFACES — the exact path, because it is NOT the one the
// obvious name suggests. KeyOrderSafe() is consulted in BuildRowLayout()
// (DataSync.cpp), per PK column. A failing column becomes a Finding with
// TypeVerdict::Unmappable; MayAutoAlter() is false for it, so
// RowChangeSet::AddFinding() latches the set non-executable, and SyncEngine
// then reports the table DataVerdict::Blocked carrying that Finding's reason.
//
// It does NOT produce DataVerdict::UnorderableKey. That verdict comes only from
// CheckDataDiffOrdering(), a separate earlier gate that classifies PK columns by
// SCHEMA TYPE via ClassifyKeyOrder() and never calls KeyOrderSafe(). The two
// gates are complementary — CheckDataDiffOrdering catches collation/byte-order
// hazards visible in the schema, this one catches value-bridge hazards only
// visible once the columns are paired — and both refuse the table with a
// specific reason. Anything special-casing UnorderableKey in the UI must not
// assume it covers the bridge-level refusals; match on the verdict being a
// refusal, not on which refusal it is.
//
// The classification switch has no silent default — an unrecognized BridgeOp
// maps to CompareOp::Unsafe, i.e. "usable for equality, never for a key". A
// future coercion that rewrites values in a non-monotone way (a charset
// transcode, a date reformat) therefore fails CLOSED the day it is added.
//
// ---- cost ------------------------------------------------------------------
// All classification happens ONCE PER COLUMN in BuildCompareRules(), from the
// already-computed ColumnBridge. No CanonicalType, no rawType text, no dialect
// is ever consulted here — ColumnBridge already hoisted that reasoning out of
// the inner loop and this file does not undo it. The per-cell cost is one
// `CompareOp` load + branch, and for CompareOp::Raw the branch lands on code
// byte-for-byte identical to the pre-fix comparison.
//
// Same-engine stays free: BuildBridges makes every bridge passthrough when the
// dialects match, so every rule is Raw and CompareRules::allRaw is true — the
// merge then dispatches ONCE PER ROW to the original raw loop and pays not even
// the per-cell branch.
#pragma once

#include "db/SyncTypes.h"     // Cell / CellKind
#include "db/SyncValueMap.h"  // ColumnBridge / BridgeOp

#include <vector>

namespace db::sync {

// How one column's cells must be canonicalized before they are compared.
enum class CompareOp : unsigned char {
    // Compare exactly as the pre-fix merge did: CellKind-sensitive equality,
    // numeric-when-both-numeric ordering. Every same-engine column, and every
    // cross-engine column whose bridge is passthrough.
    Raw,

    // Boolean-ish. Canonical form: a long long. `t/true/y/yes` -> 1,
    // `f/false/n/no` -> 0, and anything that parses as an integer keeps its own
    // integer value.
    //
    // ORDER PROOF. On the PostgreSQL side the domain is {f,t}, mapped to {0,1},
    // and PG orders false < true — monotone. On the MySQL side the domain is the
    // tinyint range, mapped to itself by the integer parse — monotone by
    // construction, including for values outside {0,1} (a tinyint(1) may legally
    // hold 2 or -1; those keep their arithmetic position instead of collapsing
    // onto the booleans). Both maps are injective, so a merge over the two
    // canonical sequences is still a merge over two sorted sequences.
    // (Such an out-of-domain value is still refused for EMISSION by
    // ConvertCell — this only keeps the merge aligned while that happens.)
    Boolish,

    // Integer-ish (BridgeOp::IntRange: the same integer value crossing into a
    // narrower target). Canonical form: the numeric value.
    //
    // ORDER PROOF. Identity on the integers; both engines order integer columns
    // arithmetically. Strictly SAFER than Raw here, because Raw falls back to
    // code-point order the moment the two drivers disagree about CellKind, and
    // code-point order puts "10" before "9".
    Numeric,

    // Naive (wall-clock) date/time/timestamp. Canonical form: the TemporalKey
    // POD of SyncTemporal.h — calendar fields with the fraction scaled to a
    // fixed nanosecond denominator, `T`-vs-space normalized and a ZERO timezone
    // suffix (`Z`, `+00`, `+00:00`) dropped. That is what makes PostgreSQL's
    // `2024-01-01 00:00:00+00` and MySQL's `2024-01-01 00:00:00` — the same
    // instant, spelled two ways — finally compare equal.
    //
    // ORDER PROOF. Lexicographic order on (date, secs, frac) IS chronological
    // order, and both engines order a naive temporal chronologically, so the map
    // is monotone; the fractional scaling is multiplication by a positive
    // constant and preserves it. Injectivity is what needs care: the map is
    // many-to-one ACROSS engines by design (that is the fix), but within ONE
    // stream it stays one-to-one, because a server renders a given column with
    // one format for every row — it does not emit `T` on one row and a space on
    // the next, nor 3 fractional digits on one row and 6 on the next. Each
    // stream therefore remains strictly sorted under this comparator, which is
    // the property the merge actually needs. Crucially the fraction is PADDED,
    // never truncated to a common precision: truncation would not be injective
    // and would collapse two distinct keys into one.
    // A value that cannot be canonicalized (a NON-ZERO offset such as `+08:00`,
    // or junk) is not guessed at — it is ranked after every real value and
    // ordered among its peers by code point. MySQL's zero date gets its own rank
    // that sorts FIRST, matching MySQL's own ordering. See SyncTemporal.h.
    Temporal,

    // Payload-preserving coercions: the text crosses verbatim and only its
    // framing, validation or literal spelling changes (BinaryReframe,
    // TextValidate). Canonical form: the text, with CellKind
    // ignored — the two drivers may label the same payload Text vs Binary.
    //
    // ORDER PROOF. Identity on the text, and the merge already orders text by
    // code point with both servers pinned to byte order by defense 1. Binary
    // payloads are bare uppercase hex on both sides (SyncTypes.h), whose
    // lexicographic order equals the underlying byte order.
    TextOnly,

    // No order proof available, and — for the case that actually produces this —
    // no EQUALITY proof either. Compared by text, which may report a difference
    // between two cells that are in fact the same value, and REFUSED outright as
    // a key.
    //
    // Produced by BridgeOp::TemporalTzAmbiguous: a temporal column whose type on
    // either side carries timezone semantics (MySQL TIMESTAMP, PG timestamptz).
    // Both engines render such a value through a SESSION timezone that the cell
    // text does not record — MySQL prints a TIMESTAMP with no offset suffix at
    // all — so from the text alone the same instant may look different AND
    // different instants may look identical. That is not a gap in the parser; it
    // is missing information, and the honest answer is to refuse rather than
    // assume UTC.
    //
    // Two Findings, doing two different jobs. BuildBridges attaches an
    // INFORMATIONAL one (ValueVerdict::Coerced, SyncValueMap.cpp) so a
    // tz-ambiguous NON-key column is annotated but still compared. When such a
    // column is a PRIMARY KEY, BuildRowLayout's KeyOrderSafe() gate adds a
    // BLOCKING one (TypeVerdict::Unmappable) and the table ends up
    // DataVerdict::Blocked. The refusal is loud either way — but only the second
    // one stops the table.
    //
    // Also where an unclassified future BridgeOp lands, so the gate fails closed.
    Unsafe
};

// True when a key column carrying this op keeps the sorted merge sound. The
// single predicate BuildRowLayout() gates each PK column on (NOT
// CheckDataDiffOrdering, which is the separate schema-type gate — see the
// refusal-path note at the top of this file). The structural counterpart of
// MayEmit(ValueVerdict) / MayAutoAlter(TypeVerdict).
constexpr bool KeyOrderSafe(CompareOp op)
{
    return op == CompareOp::Raw || op == CompareOp::Boolish ||
           op == CompareOp::Numeric || op == CompareOp::TextOnly ||
           op == CompareOp::Temporal;
}

// The per-table comparison plan, parallel to RowLayout::valueBridges.
struct CompareRules {
    std::vector<CompareOp> ops;
    // True when every op is Raw — always the case for same-engine sync. Lets the
    // merge skip the per-cell dispatch entirely rather than branch on it.
    bool allRaw = true;

    CompareOp At(size_t i) const
    {
        return i < ops.size() ? ops[i] : CompareOp::Raw;
    }
};

// Classify ONE column pair, from the bridge BuildBridges already computed. Pure
// mapping — no type text is parsed, nothing is re-derived.
CompareOp CompareOpFor(const ColumnBridge& bridge);

// Classify a whole table's columns, once. `out` is cleared first.
void BuildCompareRules(const std::vector<ColumnBridge>& bridges,
                       CompareRules& out);

// Equality under `op`. For Raw this is byte-for-byte the historical CellEqual
// (CellKind-sensitive), so no same-engine verdict can move.
bool CellsEqual(const Cell& a, const Cell& b, CompareOp op);

// Total order under `op`. For Raw this is byte-for-byte the historical CmpCell.
// Every op yields a TOTAL, TRANSITIVE order (values that cannot be canonicalized
// are ranked consistently after those that can, then ordered by code point) —
// a non-transitive comparator in a sorted merge is itself a corruption bug.
int CompareCells(const Cell& a, const Cell& b, CompareOp op);

// The historical raw comparators, moved here so the merge and this file cannot
// drift apart. CmpCell(a,b) == CompareCells(a,b,CompareOp::Raw).
int CmpCodePoints(const wxString& a, const wxString& b);
int CmpCell(const Cell& a, const Cell& b);

} // namespace db::sync
