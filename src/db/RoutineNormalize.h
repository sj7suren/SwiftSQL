// RoutineNormalize.h — dialect-aware whitespace/comment normalization of stored
// routine BODIES, for the compare-only functions & procedures diff (ADR-013 Q3).
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT "strip comments and squeeze spaces"
// ---------------------------------------------------------------------------
// The naive version ships false positives, which this feature's release bar
// forbids outright. Three concrete ways a single dialect-blind stripper is
// wrong:
//
//   * PostgreSQL: inside `$$ ... $$` (or `$tag$ ... $tag$`) a `--` or a `/* */`
//     is ORDINARY TEXT, not a comment. Stripping it rewrites the body and two
//     identical routines stop matching.
//   * MySQL: `--` only starts a comment when FOLLOWED BY WHITESPACE. `x--1` is
//     `x - (-1)`, arithmetic. A stripper that fires on bare `--` deletes the
//     rest of a perfectly good expression. MySQL also has `#` line comments,
//     which PostgreSQL does not.
//   * Identifier quoting differs: MySQL uses backticks (and, without
//     ANSI_QUOTES, `"..."` is a STRING literal), PostgreSQL uses double quotes.
//     Whatever the flavour, the contents are never ours to touch.
//
// So: ONE shared character-walk state machine, parameterized by a small
// per-dialect NormalizeRules. The states are Code, LineComment, BlockComment,
// SingleQuote, DoubleQuote, BacktickOrIdent, DollarQuote. The invariants:
//
//   1. NOTHING is ever stripped, collapsed or rewritten while the machine is in
//      any quote state. Quoted runs are copied byte for byte.
//   2. Whitespace collapse happens in the Code state ONLY.
//   3. NO CASE FOLDING ANYWHERE. Lower-casing the body would corrupt string
//      literals ('Alice' -> 'alice') and quoted identifiers, i.e. it would
//      report two genuinely different routines as identical — a false NEGATIVE
//      that is worse than the false positive it was meant to avoid. Keyword
//      case differences therefore DO surface as BodyDiffers. That is the
//      deliberate choice: "differs by keyword case" is a true statement, and a
//      human reading the two bodies side by side sees why immediately.
//   4. A comment collapses to a single SPACE, never to nothing: `a/*c*/b` must
//      not become `ab`, which would fuse two tokens into one.
//
// ---------------------------------------------------------------------------
// DELIMITER — deliberately NOT handled
// ---------------------------------------------------------------------------
// `DELIMITER $$` is a CLIENT construct (mysql CLI, workbench). It is not SQL,
// the server never sees it, and it therefore CANNOT appear in a body read from
// information_schema.ROUTINES.ROUTINE_DEFINITION or from pg_proc.prosrc — which
// is the only way bodies enter this system. Adding a DELIMITER rule would be
// dead code that implies this normalizer is safe on hand-pasted script text.
// It is not, and it is never fed such text.
//
// ---------------------------------------------------------------------------
// UNRESOLVABLE INPUT DEGRADES TO "CAN'T TELL", NEVER TO "DIFFERS"
// ---------------------------------------------------------------------------
// If the walk hits end-of-input while still inside a quote or block comment,
// the body is not something this scanner understands (truncated catalog read, a
// dialect quirk we do not model, a `$$` we mis-framed). The function then
// returns NormalizeStatus::Unresolved and hands back the ORIGINAL text
// untouched. The compare layer turns that into RoutineVerdict::NotComparable.
// Guessing would mean reporting "differs" for two routines we simply failed to
// parse — the exact false positive this whole design exists to prevent.
#pragma once

#include <wx/string.h>

namespace db { enum class Dialect; }   // opaque; see DbDriver.h

namespace db::sync {

// The per-dialect knobs the shared scanner reads. Everything defaults to the
// most conservative setting (no dollar quoting, `--` fires bare, no backticks),
// so an unconfigured rules object cannot silently enable a dialect feature.
struct NormalizeRules {
    // PostgreSQL `$$ ... $$` / `$tag$ ... $tag$`. A tag must be empty or match
    // [A-Za-z_][A-Za-z_0-9]*, which is what keeps a SQL-function parameter
    // reference like `$1` from being mistaken for an opener.
    bool dollarQuoting = false;

    // MySQL: `--` starts a comment only when followed by whitespace or EOL.
    bool dashDashNeedsSpace = false;

    // MySQL `#` to end of line.
    bool hashLineComment = false;

    // MySQL backtick-quoted identifiers (`` `` `` doubles inside).
    bool backtickIdent = false;

    // MySQL: backslash escapes are active inside string literals, so `\'` does
    // NOT close the literal. PostgreSQL with the modern default
    // (standard_conforming_strings = on) does not do this, and enabling it there
    // would mis-frame a body containing a literal backslash.
    bool backslashEscapes = false;

    // MySQL executable comments: `/*! ... */` and optimizer hints `/*+ ... */`
    // are CODE, not commentary — the server runs what is inside. They are
    // therefore preserved verbatim instead of being stripped; deleting them
    // would report two routines with genuinely different behaviour as identical.
    bool preserveExecutableComment = false;
};

NormalizeRules MySqlNormalizeRules();
NormalizeRules PgNormalizeRules();

// Rules for `d`. MySQL and Postgres are modelled; every other dialect gets the
// conservative default set. Callers that need certainty about whether a dialect
// is modelled should ask NormalizeRulesKnown() rather than inferring it from a
// returned rules object.
NormalizeRules RulesFor(Dialect d);
bool NormalizeRulesKnown(Dialect d);

enum class NormalizeStatus {
    Ok,          // the walk finished in the Code state; `text` is normalized
    Unresolved   // ended inside a quote/comment; `text` is the ORIGINAL input
};

struct NormalizeResult {
    NormalizeStatus status = NormalizeStatus::Ok;
    wxString        text;      // normalized body, or the original when Unresolved
    wxString        reason;    // non-empty only when Unresolved; human-readable (zh)

    bool Ok() const { return status == NormalizeStatus::Ok; }
};

// Normalize one routine body. Pure: no I/O, no globals, no locale dependence.
//
// Line endings are folded CRLF/CR -> LF as a PRE-PASS, i.e. INCLUDING inside
// string literals. That is a knowing trade: it means two routines whose only
// difference is the line ending inside a multi-line string literal are reported
// Identical (a false negative), and it buys immunity to the far more common case
// where the same routine was loaded through clients that disagreed about line
// endings (a false positive, which is the failure mode we are not allowed to
// have).
NormalizeResult NormalizeRoutineBody(const wxString& body, const NormalizeRules& rules);

} // namespace db::sync
