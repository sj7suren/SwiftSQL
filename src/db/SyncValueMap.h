// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncValueMap.h — cross-engine VALUE conversion for the table-data sync suite
// (ADR-015 T1). The value-level analogue of SyncTypeMap.h: where SyncTypeMap
// answers "may this column TYPE be auto-ALTERed?" (TypeVerdict/MayAutoAlter),
// this answers "may this cell VALUE be emitted into the target?"
// (ValueVerdict/MayEmit).
//
// Deliberately NOT part of DialectProfile: ADR-014 declares DialectProfile a
// separate bounded context (Interpret/Render are its only two morphisms), and
// the architecture explicitly ruled that value conversion must not become a
// third one. We only CONSUME DialectProfile::Interpret here to obtain both
// sides' CanonicalType; the dependency direction is one-way and this header is
// invisible to DialectProfile.
//
// Performance contract — the reason ColumnBridge exists at all:
//   All type reasoning happens ONCE PER COLUMN PAIR in BuildBridges(). A table
//   with 10M cells must not perform 10M type dispatches. ConvertCell() on the
//   hot path is a single `passthrough` bool test in the common case (same-engine
//   sync, or any column pair that needs no conversion), and never re-derives
//   anything from CanonicalType/rawType.
//
// Pure value types + free functions: no connection, no vendor header, no wx UI
// beyond wxString.
#pragma once

#include "db/SchemaDelta.h"   // db::sync::Finding
#include "db/SchemaModel.h"   // TableSchema / NormColumn / ColKind
#include "db/SyncTypeMap.h"   // CanonicalType / TypeVerdict / MayAutoAlter
#include "db/SyncTypes.h"     // Cell / CellKind

#include <cstddef>
#include <vector>

namespace db {
// Opaque enum declaration (same trick SyncTypes.h uses): db::Dialect is defined
// in DbDriver.h, but a scoped enum without an enum-base is a COMPLETE type once
// declared, so ColumnBridge may hold one by value without dragging the whole
// driver header into every consumer (RowChange.h, and through it the UI track).
enum class Dialect;
}

namespace db::sync {

// The verdict a single cell's conversion earns. Mirrors TypeVerdict's shape and
// intent exactly:
//   Exact          — the value crosses unchanged and means the same thing.
//   Coerced        — the value is rewritten (bool literal, bytea framing) but no
//                    information is lost; safe to emit automatically.
//   Unrepresentable— the target cannot hold this value truthfully. NEVER emitted,
//                    NEVER silently substituted (in particular never coerced to
//                    NULL — silently turning a date into NULL is precisely the
//                    class of silent wrongness this design forbids). It fails the
//                    WHOLE ROW and surfaces as a Finding for a human.
enum class ValueVerdict { Exact, Coerced, Unrepresentable };

// The ONE predicate that decides whether a converted value may be emitted.
// The exact counterpart of MayAutoAlter(TypeVerdict) — RowChange.h gates on this
// structurally (see RowChangeBuilder), so no caller can route around it.
constexpr bool MayEmit(ValueVerdict v)
{
    return v == ValueVerdict::Exact || v == ValueVerdict::Coerced;
}

// Value verdicts are reported through the SAME db::sync::Finding the schema
// track already uses (so the diff-review tree renders both kinds of problem
// uniformly, and there is one audit-trail type rather than two). This is the
// order-preserving mapping between the two ladders.
constexpr TypeVerdict ToTypeVerdict(ValueVerdict v)
{
    return v == ValueVerdict::Exact       ? TypeVerdict::Identical
         : v == ValueVerdict::Coerced     ? TypeVerdict::Equivalent
                                          : TypeVerdict::Unmappable;
}

// The two safety ladders must never drift apart: whatever may be emitted as a
// value must map onto a verdict that may auto-alter, and vice versa. If someone
// later adds a verdict to either enum, this fails at COMPILE time.
static_assert(MayEmit(ValueVerdict::Exact)   == MayAutoAlter(ToTypeVerdict(ValueVerdict::Exact)),   "verdict ladders drifted");
static_assert(MayEmit(ValueVerdict::Coerced) == MayAutoAlter(ToTypeVerdict(ValueVerdict::Coerced)), "verdict ladders drifted");
static_assert(MayEmit(ValueVerdict::Unrepresentable) == MayAutoAlter(ToTypeVerdict(ValueVerdict::Unrepresentable)), "verdict ladders drifted");

// The precomputed per-cell operation. One enum test on the hot path replaces all
// CanonicalType reasoning. Ops are mutually exclusive: BuildBridges picks exactly
// one per column pair.
enum class BridgeOp : unsigned char {
    Identity,        // copy through; no inspection at all
    IntRange,        // integer: verify the ACTUAL value fits the target's range
    BoolToPgBool,    // MySQL tinyint(1) 0/1 -> PG boolean FALSE/TRUE
    PgBoolToInt,     // PG boolean t/f -> MySQL tinyint(1) 0/1
    BinaryReframe,   // binary/BLOB: hex payload reframed by the target dialect
    // Naive (wall-clock) temporal on BOTH sides: MySQL DATE/DATETIME/TIME <->
    // PG date/timestamp/time WITHOUT time zone. Rejects MySQL zero-dates the
    // target cannot store; for COMPARISON the text is canonicalizable, because a
    // wall-clock reading means the same thing on both servers with no session
    // state involved (CompareOp::Temporal).
    TemporalGuard,
    // Temporal where EITHER side's type carries timezone semantics (MySQL
    // TIMESTAMP, PG timestamptz). Emission behaves exactly like TemporalGuard —
    // the value is carried across as-is, which is what it has always done — but
    // the two texts CANNOT be compared for equality, because what each engine
    // printed depends on a session timezone that is nowhere in the cell text:
    // MySQL renders a TIMESTAMP in the session zone with NO offset suffix, so
    // `2024-01-01 00:00:00` from MySQL and `2024-01-01 00:00:00+00` from PG are
    // the same instant only if MySQL's session happened to be UTC — and the
    // identical MySQL text under a different session is a different instant. We
    // cannot tell which, so we refuse to decide (CompareOp::Unsafe: never a key,
    // and a Finding warns that a non-key column may report a false difference).
    TemporalTzAmbiguous,
    TextValidate     // reject text that failed Unicode decoding
};

// Per-column-pair conversion plan, computed ONCE PER TABLE by BuildBridges().
// Never construct one by hand on a hot path — every field here is the cached
// answer to a question that would otherwise be asked per cell.
struct ColumnBridge {
    size_t        srcIdx = 0;        // index into the streamed source row
    CanonicalType from;              // source column, via DialectProfile::Interpret
    CanonicalType to;                // target column, via DialectProfile::Interpret
    Dialect       toDialect{};       // literal framing / bool spelling target

    // Fast path flag. True => ConvertCell returns the input untouched with
    // verdict Exact after ONE bool test. Always true for same-engine sync, and
    // for any cross-engine pair that needs no per-value inspection. This is what
    // makes "same-engine data sync pays nothing for cross-engine machinery" a
    // structural property rather than an aspiration.
    bool          passthrough = true;
    BridgeOp      op = BridgeOp::Identity;

    // BridgeOp::IntRange only — the target integer type's inclusive bounds,
    // resolved from the target column's rawType at build time.
    long long     minValue = 0;
    long long     maxValue = 0;
    // True when the target is an unsigned type whose upper bound exceeds
    // LLONG_MAX (MySQL BIGINT UNSIGNED); maxValue is then meaningless and the
    // unsigned 64-bit path is used instead.
    bool          maxIsUnsigned64 = false;

    // Diagnostics only — never re-emitted into SQL.
    wxString      column;
};

// Build the per-column conversion plan for one table pair.
//
// Deviation from the ADR sketch, and why: the sketch passed only `tgtDialect`,
// but a source column cannot be interpreted without knowing which engine
// produced its rawType ("bigint unsigned" is MySQL text; "bigint" is PG text) —
// DialectProfile::Interpret is per-dialect. srcDialect is therefore required,
// not optional. When srcDialect == tgtDialect every bridge degenerates to
// passthrough Identity and NO Interpret call is even made.
//
// Columns are matched by name (case-sensitive, as the schema diff does). A
// source column absent from the target is skipped with a Finding; a target
// column absent from the source is skipped with a Finding (it will take its
// DEFAULT/NULL). A column pair whose types cannot carry values at all yields a
// blocking Finding (verdict Unmappable) and `false` as the return value.
//
// Returns false when at least one blocking Finding was produced — the caller
// must not sync that table's data. `out`/`findings` are always left in a
// consistent state; `out` is cleared first.
bool BuildBridges(const TableSchema& src, Dialect srcDialect,
                  const TableSchema& tgt, Dialect tgtDialect,
                  std::vector<ColumnBridge>& out, std::vector<Finding>& findings);

// Result of one cell conversion. `reason` is empty for Exact (no allocation on
// the fast path) and always populated for Coerced/Unrepresentable.
struct ConvertedCell {
    Cell         cell;
    ValueVerdict verdict = ValueVerdict::Exact;
    wxString     reason;
};

// Convert ONE cell through its precomputed bridge. Hot path: a single
// `bridge.passthrough` test in the common case.
//
// Invariants this function guarantees, relied on by RowChangeBuilder:
//   * NULL in => NULL out, always, for every op. NULL and the empty string are
//     distinct in Cell and are never converted into one another in either
//     direction.
//   * verdict == Unrepresentable => `cell` is meaningless and MUST NOT be used.
//     There is no "best effort" fallback value; callers gate on MayEmit().
ConvertedCell ConvertCell(const Cell& in, const ColumnBridge& bridge);

} // namespace db::sync
