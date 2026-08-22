// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncTemporal.h — the NEUTRAL COMPARISON FORM for date/time/timestamp cells.
//
// ---- the defect this exists to fix ----------------------------------------
// SyncCellCompare classified every temporal coercion as CompareOp::TextOnly,
// i.e. "the payload crosses verbatim, compare the text". For booleans that
// assumption was wrong and was fixed; for temporals it is wrong in the same way
// and for the same reason. The two engines render THE SAME INSTANT differently:
//
//     PostgreSQL   2024-01-01 00:00:00+00
//     MySQL        2024-01-01 00:00:00
//
// and ISO-8601-flavoured drivers may hand back `2024-01-01T00:00:00.000Z`. As
// text these are three different strings, so:
//   * in a NON-KEY column every timestamp reported a difference on rows that
//     were identical — a false-positive UPDATE on every temporal column of every
//     cross-engine comparison, which breaks the release-blocking
//     zero-false-positive bar;
//   * in a KEY column the two servers' sort orders and the merge's comparator no
//     longer agreed, which is the silent desync (invented INSERTs + DELETEs from
//     identical rows) the whole ordering-defense layer exists to prevent.
//
// ---- the neutral form ------------------------------------------------------
// A four-field POD: (rank, date, secs, frac). NOT a normalized string — building
// a canonical `YYYY-MM-DD HH:MM:SS.ffffff` wxString would allocate once per cell
// on a path that runs 10M times, and would then have to be compared by code
// point anyway. The POD is filled in registers and compared field by field.
//
//   date  y*10000 + m*100 + d          (0 for a time-only value)
//   secs  SIGNED seconds within the day; for a bare TIME, total seconds, which
//         MySQL allows to run to +/-838:59:59 — hence signed, hence not "second
//         of day".
//   frac  fractional seconds scaled to NANOSECONDS, always 9 digits' worth.
//
// Lexicographic order on those fields IS chronological order, which is what
// makes the form usable for a KEY (see the ordering contract below).
//
// ---- fractional precision: PAD, never TRUNCATE -----------------------------
// MySQL DATETIME(6) renders `.123456`; PG timestamp(3) renders `.123`; either
// may render nothing at all. Scaling to a fixed nanosecond denominator means
// `12:00:00`, `12:00:00.0` and `12:00:00.000000` all become frac == 0 and
// compare EQUAL — that is the false positive gone.
//
// It deliberately does NOT round or truncate to a common precision. Truncating
// `.123456` and `.123` to a shared 3 digits would make them compare equal, which
// is doubly wrong: it hides genuine data loss (the target really is holding a
// different instant, and an operator needs to see that), and truncation is not
// INJECTIVE, so it would collapse two distinct key values into one and desync
// the merge. Zero-padding is injective and monotone; truncation is neither.
//
// ---- timezone suffix: only a UTC suffix is spelling, everything else is data -
// A trailing `Z` / `+00` / `+00:00` / `-00:00` is stripped: it asserts an offset
// of zero, so removing it changes no instant — it is pure spelling, exactly like
// `T` vs space. Any NON-ZERO offset (`+08:00`, `-05`) is REFUSED (rank
// Unparseable) rather than normalized. Converting it would mean doing calendar
// arithmetic to shift the value into UTC, and a column that renders `-05` in
// winter and `-04` in summer would then be canonicalized by a map whose
// relationship to the server's own ORDER BY we cannot verify from here. Refusing
// leaves such a cell comparable only to a byte-identical twin, which is the
// conservative direction: it can cost a false positive, never a false negative,
// and never a desync.
//
// NOTE the division of labour: whether a COLUMN may be canonicalized at all is
// decided once per column, upstream, by BuildBridges — a column whose type on
// either side carries timezone semantics (MySQL TIMESTAMP, PG timestamptz) is
// classified BridgeOp::TemporalTzAmbiguous and never reaches this parser,
// because the session timezone that produced its text is not knowable from the
// text. This file only handles the naive/wall-clock case, where the parse is
// total and honest.
//
// ---- MySQL's zero date -----------------------------------------------------
// `0000-00-00[ 00:00:00]` is not a calendar date and is deliberately NOT parsed
// into (0,0,0): it gets its own rank that sorts FIRST, matching MySQL's own
// ordering (the zero date is the minimum DATE). This is a COMPARISON concern
// only — emission still runs through ConvertCell/IsZeroDate, a different
// function this file does not touch, so a zero date remains Unrepresentable for
// emission exactly as before.
//
// ---- the ordering contract (the dangerous part) ----------------------------
// The merge is a SORTED merge over two streams each sorted by its own server.
// Canonicalizing is sound only if the map is MONOTONE and INJECTIVE on each
// side's native order.
//
//   MONOTONE.  For a naive temporal both engines order chronologically, and
//   chronological order is exactly lexicographic order on
//   (date, secs, frac) — each field is a more significant digit than the next.
//   Zero-padding the fraction preserves it because it is multiplication by a
//   positive constant.
//
//   INJECTIVE ON EACH SIDE.  The map is intentionally many-to-one across
//   ENGINES (`...00:00:00+00` and `...00:00:00` collapse together — that IS the
//   fix). Within ONE stream it is still one-to-one, because a server renders one
//   column with one format for every row: it does not emit `T` on one row and a
//   space on the next, nor 3 fractional digits on one row and 6 on the next. So
//   each stream stays strictly sorted under the comparator the merge uses, which
//   is the property the merge actually requires.
//
// Values that cannot be canonicalized are not guessed at: they are ranked
// consistently and ordered among themselves by code point, so the comparator
// stays TOTAL and TRANSITIVE. A non-transitive comparator inside a sorted merge
// is itself a corruption bug.
#pragma once

#include <wx/string.h>

namespace db::sync {

// The neutral form. Compared field by field, never materialized as text.
struct TemporalKey {
    // Ranks are compared FIRST, so the order is lexicographic on (rank, fields)
    // and therefore total and transitive whatever the fields hold.
    enum Rank : int {
        // MySQL's zero date. Sorts before every real value, as MySQL sorts it.
        Zero        = 0,
        // A real calendar value; `date`/`secs`/`frac` are meaningful.
        Parsed      = 1,
        // Not a temporal we can read (a non-UTC offset, junk, an out-of-range
        // field). Sorts after everything and is ordered among its peers by code
        // point BY THE CALLER — CmpTemporalKey reports 0 for two of these and
        // leaves the tie-break to whoever holds the original text.
        Unparseable = 2
    };

    int       rank = Unparseable;
    long long date = 0;   // y*10000 + m*100 + d; 0 for a bare TIME
    long long secs = 0;   // signed seconds (see header: TIME may exceed a day)
    long long frac = 0;   // fractional seconds in nanoseconds, sign-matched to secs
};

// Parse one cell's text into the neutral form. Allocation-free and total: any
// text at all yields a well-defined TemporalKey. Accepts
//   YYYY-MM-DD
//   YYYY-MM-DD{space|T|t}HH:MM:SS[.f{1,9}][Z|+00|-00|+00:00|-00:00|+0000|-0000]
//   [-]H{1,3}:MM:SS[.f{1,9}]
// and nothing else.
TemporalKey ParseTemporal(const wxString& s);

// Total order on the neutral form. Returns 0 for two Unparseable keys; the
// caller must break that tie by code point on the original text (SyncCellCompare
// does).
int CmpTemporalKey(const TemporalKey& a, const TemporalKey& b);

} // namespace db::sync
