// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// RoutineCompare.h — pure matching + verdict assignment for the functions &
// stored procedures diff (ADR-013). No I/O: this file never touches an
// IConnection. The catalog readers (MySqlRoutineRead.h / PgRoutineRead.h) hand
// it two vectors and two read statuses; it decides what may be CLAIMED.
//
// Compare and display only. Nothing here renders DDL — see RoutineDiff.h's
// structural guarantee, which this file is required to preserve.
//
// ---------------------------------------------------------------------------
// THE MATCHING KEY, and why it is NOT the canonical type mapper
// ---------------------------------------------------------------------------
// Two routines are "the same routine" when they agree on name + ordered
// argument type list (+ kind; see below). That key is a plain LEXICAL
// normalization: lower-case the type name, drop length/precision, resolve
// engine aliases (int4 -> integer, character varying -> varchar).
//
// ADR-013 explicitly REJECTED keying on db::CompareCanonical(), and the reason
// is not stylistic. CompareCanonical returns a GRADED TypeVerdict
// (Identical/Equivalent/Lossy/Unmappable). `Equivalent` is not an equivalence
// relation: it is not transitive, and the grade can depend on which side you
// pass first. You cannot key a hash map on a relation like that — "is this the
// same routine?" would depend on iteration order, so the same two catalogs
// could produce different diffs on different runs. A lexical key is boring,
// total, and order-independent, which is exactly what a key must be.
//
// The canonical mapper IS used, but only AFTER a name match, to EXPLAIN a
// signature difference in words a human can act on ("varchar(50) vs
// varchar(100)：长度不同"). Explanation is the job a graded verdict is good at.
//
// KIND IN THE KEY (a deliberate addition to the ADR's "schema + name + args").
// MySQL permits a FUNCTION and a PROCEDURE with the SAME name and arguments to
// coexist in one schema. Keying without the kind would pair the source's
// function with the target's procedure and then confidently diff their bodies —
// a wrong answer, not a missing one. So the kind is in the key. To keep the
// useful verdict anyway, a second pass re-joins a leftover SourceOnly and
// TargetOnly that agree on name+args but disagree on kind, and reports the pair
// as SignatureDiffers ("源端为函数，目标端为存储过程") rather than as two
// unrelated one-sided entries.
//
// SCHEMA IN THE KEY (off by default, and this matters cross-engine). A compare
// always runs between ONE source database/schema and ONE target
// database/schema, so the schema qualifier is the CONTAINER, not part of the
// routine's identity. Worse, cross-engine the two containers are named
// differently by construction — MySQL's schema IS the database ("app"), PG's is
// typically "public" — so keying on it would report every routine on both sides
// as one-sided. Set qualifyBySchema only when comparing a genuinely
// multi-schema PG catalog against another.
#pragma once

#include "db/RoutineDiff.h"

#include <wx/string.h>
#include <vector>

namespace db { enum class Dialect; }   // opaque; see DbDriver.h

namespace db::sync {

// The compare-only switch and its knobs.
//
// This is where the "compare routines?" flag lives, and it is deliberately NOT
// a SyncScope field (ADR-013 Q5). SyncScope is the input to plan BUILDING —
// i.e. to what gets EXECUTED — so a compare-only flag there invites an
// `if (scope.routines)` branch inside plan construction, which is one edit away
// from putting routine DDL on the execute path. Plan construction never reads
// this struct.
struct RoutineCompareOptions {
    bool enabled = true;

    // Both sides are the same engine. ONLY then may a body verdict
    // (Identical/BodyDiffers) be issued; cross-engine bodies are ALWAYS
    // NotComparable. See RoutineDiff.h.
    bool sameEngine = false;

    // Fold routine NAME case when building the key. MySQL routine names are
    // case-insensitive; PostgreSQL's are not (unquoted names arrive already
    // folded to lower case, but a quoted "MyFunc" is genuinely distinct from
    // "myfunc"). Folding in a PG↔PG compare could therefore collide two
    // distinct routines — so MakeRoutineCompareOptions turns this ON only when
    // MySQL is involved, where NOT folding is the greater risk.
    bool foldNameCase = true;

    // Include the schema in the key. Off by default — see the header comment.
    bool qualifyBySchema = false;

    Dialect srcDialect{};
    Dialect tgtDialect{};
};

// The options a given dialect pair should use. Prefer this over filling the
// struct by hand: it is the one place the foldNameCase / sameEngine reasoning
// lives, and getting either wrong is a silent wrong-answer bug rather than a
// crash.
RoutineCompareOptions MakeRoutineCompareOptions(Dialect src, Dialect tgt);

// Lexically normalize ONE argument (or return) type for keying:
// trim, lower-case, drop a length/precision suffix, drop AS/array decorations,
// resolve engine aliases. "INT(11) UNSIGNED" -> "integer unsigned",
// "character varying(50)" -> "varchar", "int4" -> "integer".
//
// UNSIGNED is KEPT: in MySQL it is part of the type's meaning (and of the
// signature), and dropping it would key two genuinely different routines the
// same. Length/precision is DROPPED, so varchar(50) vs varchar(100) MATCHES as
// the same routine and then reports SignatureDiffers — which is the useful
// answer ("this routine's parameter got wider"), where keying on the length
// would have reported an unrelated deletion plus an unrelated addition.
wxString NormalizeArgType(const wxString& raw);

// The routine's matching identity under `opt`. Not SQL, never parsed back.
wxString RoutineMatchKey(const RoutineDef& r, const RoutineCompareOptions& opt);

// Match the two catalogs and assign a verdict to every routine on either side.
//
// PERMISSION / SUPPORT DEGRADATION (ADR-013 Q4). If either status is not Ok the
// result carries the statuses and NO pairs: Comparable() is false and the UI
// renders the categories with RoutineReadStatusLabel() and zero children. A
// half-comparison is never produced, because "the target has 40 functions the
// source lacks" computed against a side we could not read is a lie. Nothing
// about the TABLE diff is affected — this function is not an input to it.
//
// Ordering of `pairs`: differences first, then Identical; within each group,
// stable by key. The UI splits by kind via RoutineDiffSet::ByKind().
RoutineDiffSet CompareRoutines(const std::vector<RoutineDef>& source,
                               const std::vector<RoutineDef>& target,
                               RoutineReadStatus sourceStatus,
                               RoutineReadStatus targetStatus,
                               const RoutineCompareOptions& opt);

} // namespace db::sync
