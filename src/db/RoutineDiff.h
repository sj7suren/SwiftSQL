// RoutineDiff.h — the carrier for the functions & stored procedures DIFF
// (ADR-013). Compare and display ONLY.
//
// ===========================================================================
// THE STRUCTURAL GUARANTEE — the entire point of this header
// ===========================================================================
// The CEO ruling standing over this feature is: never generate or apply routine
// DDL, cross-engine OR same-engine. T-SQL, PL/pgSQL and MySQL's routine dialect
// are Turing-complete languages, not type tables; auto-translating procedural
// code between engines is a data-integrity liability, and a human rewrites it.
//
// This header enforces that the same way RowChange.h enforces its own rule —
// structurally, not by convention — but INVERTED. In RowChange.h execution is
// the POINT, so the gate is a verdict: you cannot obtain a RowChange for a row
// that must not be emitted. Here execution is FORBIDDEN, so the gate is an
// ABSENCE: there is simply nowhere for a statement to live.
//
// Concretely, and this is a maintenance contract, not a description:
//
//   1. NO type in this file has a field holding DDL or a statement. Not `ddl`,
//      not `sql`, not `statement`, not an empty one, not a commented-out one.
//      Adding one is the whole violation; there is no partial version of it.
//   2. `body` is NOT such a field. It holds the routine's INNER SOURCE exactly
//      as the catalog returns it (information_schema.ROUTINES.ROUTINE_DEFINITION
//      / pg_proc.prosrc) — a `BEGIN … END` block or a bare expression. It is not
//      a CREATE statement and never becomes one: handing it to Execute() is a
//      syntax error, not an accidental deployment. No function anywhere in
//      RoutineDiff / RoutineNormalize / RoutineCompare / *RoutineRead renders a
//      CREATE, DROP or ALTER for a routine. Grep for it; the absence is the
//      feature.
//   3. RoutineDiffSet is NOT part of SyncPlan, deliberately (ADR-013 Q1).
//      SyncPlan is what SyncEngine::Execute consumes; anything living there is
//      one careless `for (auto& u : plan.units)` away from being executed. This
//      set is a SEPARATE value that the execute path never sees and cannot
//      reach.
//   4. `routines` is NOT a SyncScope flag (ADR-013 Q5). SyncScope is the input
//      to plan BUILDING, i.e. to what gets executed; a compare-only flag there
//      invites `if (scope.routines)` inside plan construction. The switch lives
//      on RoutineCompareOptions instead, which plan construction never reads.
//
// A reviewer's one-line check for a future change: if a diff to this file adds
// a string field to RoutineDef or a method to RoutineDiffSet that returns
// something you could pass to IConnection::Execute, the change is wrong.
//
// ===========================================================================
// NAME NOTE — why this is RoutineDef and not RoutineInfo
// ===========================================================================
// ADR-013 specified the name `RoutineInfo`. It cannot be used, and the reason is
// worth recording so nobody "fixes" it back.
//
// db::RoutineInfo ALREADY exists in DbDriver.h: a two-field {name,type} value
// used by the dump-export path. A db::sync::RoutineInfo would compile inside
// this library (the inner namespace wins) but BREAK every consumer that says
// `using namespace db; using namespace db::sync;` — both names then arrive at
// the same scope and every mention is ambiguous (MSVC C2872). That is not
// hypothetical: it is exactly what the test TU did on its first build, and the
// UI that wires up the tree categories would hit it next.
//
// Reusing db::RoutineInfo instead was rejected: it would give the dump path and
// the diff path one shared type of which each uses a different half, and adding
// this type's fields to it would put a `body` string on the dump path's value
// object for no reason. Renaming the EXISTING type was also rejected — it would
// mean editing MySqlDriver.cpp (978/1000) and PgDriver.cpp (940/1000), both at
// the charter's line ceiling, for a cosmetic gain.
//
// So: RoutineDef, "a routine's definition as read from a catalog". Distinct
// name, no ambiguity for any consumer, no driver edits.
#pragma once

#include <wx/string.h>

#include <vector>

namespace db { enum class Dialect; }   // opaque; see DbDriver.h

namespace db::sync {

// Functions and procedures are diffed together but presented as two categories
// (the user asked for "不同的函数，存储过程"), so the kind rides on every record.
enum class RoutineKind { Function, Procedure };

// Whether one side's routine catalog could be read at all (ADR-013 Q4).
//
// This is a PER-CATEGORY state, never a compare failure. A read-only account
// that can see tables but not routine bodies must still get its table diff in
// full; failing the whole comparison because routines are unreadable would
// regress shipped behaviour for exactly the accounts most likely to be pointed
// at a production server.
enum class RoutineReadStatus {
    Ok,                // the catalog was read; the routine list is authoritative
    PermissionDenied,  // the account cannot see routines (or their bodies)
    // Everything else that stopped us reading: no reader for this dialect, or
    // the catalog query failed. The two are merged because they are the same
    // thing to the user — the list is not available — and the specifics belong
    // in RoutineDiffSet::sourceStatusDetail, which carries the server's own
    // error text. What matters is that neither is ever confused with "empty".
    Unsupported
};

// Non-Ok means: render the category with this label and ZERO children. It never
// means "there are no routines" and it never means "they are identical".
inline wxString RoutineReadStatusLabel(RoutineReadStatus s)
{
    switch (s) {
    case RoutineReadStatus::PermissionDenied: return L"不可见（权限不足）";
    case RoutineReadStatus::Unsupported:      return L"不可见（无法读取）";
    case RoutineReadStatus::Ok:               break;
    }
    return wxString();
}

// One routine as read from a catalog. Every field is either an identity
// component or something a human needs in order to understand a verdict.
//
// See guarantee 2 above regarding `body`.
struct RoutineDef {
    wxString    schema;                 // MySQL: the database; PG: the nspname
    wxString    name;
    RoutineKind kind = RoutineKind::Function;

    // Argument types in DECLARED ORDER, engine-native spelling, e.g.
    // {"int(11)", "varchar(50)"} or {"integer", "text"}. Raw — the matching key
    // is derived from these, never stored in their place.
    std::vector<wxString> argTypes;

    // Argument modes in the same order, upper-case: "IN" / "OUT" / "INOUT", or
    // empty where the catalog does not say. NOT part of the matching key (see
    // RoutineCompare.h); a mode-only difference surfaces as SignatureDiffers
    // AFTER the name+types match, which is the honest ordering.
    std::vector<wxString> argModes;

    wxString returnType;                // empty for procedures
    wxString language;                  // "SQL" / "plpgsql" — as the catalog spells it
    bool     securityDefiner = false;   // MySQL SQL SECURITY DEFINER / PG prosecdef
    wxString volatility;                // PG provolatile: "immutable"/"stable"/"volatile"

    // The routine's INNER SOURCE, verbatim from the catalog. Display + compare
    // input only. Not DDL. Never executed. Never rendered into DDL.
    wxString body;

    // Was `body` actually READABLE for this routine?
    //
    // This exists because of a MySQL behaviour that is a false-negative trap:
    // information_schema.ROUTINES lists a routine to any account, but returns
    // ROUTINE_DEFINITION as NULL unless the account has privileges on it. So a
    // partially-privileged user gets a full routine LIST with some bodies
    // blank — and if blank were treated as a body, two routines whose bodies we
    // cannot see would compare EQUAL and be reported "相同". That is the worst
    // possible output: a confident claim of sameness about something we never
    // saw. RoutineReadStatus cannot express it either, since it is per-side,
    // not per-routine.
    //
    // A false here forces RoutineVerdict::NotComparable for that pair. Readers
    // must set it deliberately; the default is true so a reader that has a real
    // body does not have to remember to say so.
    bool bodyReadable = true;

    // Convenience for display: name(argtypes). Not the matching key — that is
    // RoutineMatchKey() in RoutineCompare.h, which normalizes. Never SQL.
    wxString Signature() const
    {
        wxString s = name + L"(";
        for (size_t i = 0; i < argTypes.size(); ++i) {
            if (i) s += L", ";
            if (i < argModes.size() && !argModes[i].IsEmpty() && argModes[i] != L"IN")
                s += argModes[i] + L" ";
            s += argTypes[i];
        }
        return s + L")";
    }
};

// What we are willing to CLAIM about a pair. The set is closed; there is no
// "probably differs".
enum class RoutineVerdict {
    Identical,          // same signature, and normalized bodies compare equal
    SourceOnly,         // present on the source, absent on the target
    TargetOnly,         // present on the target, absent on the source
    SignatureDiffers,   // matched by name, but the declaration differs
    BodyDiffers,        // SAME-ENGINE ONLY — see below
    NotComparable       // we decline to claim anything; the reason says why
};

// BodyDiffers is same-engine only, and NotComparable is the ALWAYS answer for a
// cross-engine body pair. That is not caution for its own sake: a PL/pgSQL body
// and a MySQL body are written in different Turing-complete languages, so
// "these two bodies differ" is not a finding — it is a tautology — and "these
// two bodies are the same" is a claim we have no basis to make. Reporting
// NotComparable and showing both bodies side by side is the only honest output.
//
// NotComparable is also where every uncertainty degrades TO: an unresolvable
// body (RoutineNormalize's Unresolved), a language mismatch, an ambiguous
// matching key. Never to BodyDiffers. "Can't tell" is a true statement;
// "differs" would be a false positive, and this feature's release bar is zero
// false positives.
inline bool RoutineVerdictIsDifference(RoutineVerdict v)
{
    return v != RoutineVerdict::Identical;
}

// A short zh label for the verdict — the text the tree node shows.
inline wxString RoutineVerdictLabel(RoutineVerdict v)
{
    switch (v) {
    case RoutineVerdict::Identical:        return L"相同";
    case RoutineVerdict::SourceOnly:       return L"仅源端存在";
    case RoutineVerdict::TargetOnly:       return L"仅目标端存在";
    case RoutineVerdict::SignatureDiffers: return L"签名不同";
    case RoutineVerdict::BodyDiffers:      return L"函数体不同";
    case RoutineVerdict::NotComparable:    return L"无法比较";
    }
    return wxString();
}

// One matched (or unmatched) routine and what we concluded about it.
//
// Both sides are carried whole so the UI can show them side by side. That is
// the ENTIRE deliverable of this feature: a human reads the two bodies and
// decides what to do. There is no "apply" affordance for this type to feed.
struct RoutinePair {
    RoutineVerdict verdict = RoutineVerdict::NotComparable;
    RoutineKind    kind    = RoutineKind::Function;

    // The normalized matching key both sides resolved to (see RoutineCompare.h).
    // Diagnostic/dedup use; not SQL, never parsed back.
    wxString key;

    bool hasSource = false;
    bool hasTarget = false;
    RoutineDef source;   // meaningful only when hasSource
    RoutineDef target;   // meaningful only when hasTarget

    // Why this verdict — always filled for SignatureDiffers / NotComparable, and
    // a short note otherwise. This is what the user reads instead of the DDL we
    // refuse to generate, so it has to actually explain (e.g. "参数 2 类型
    // varchar(50) vs varchar(100)：长度不同" or "跨引擎：plpgsql 与 SQL 属不同过
    // 程语言，不作同异判断").
    wxString reason;

    // Display name, preferring whichever side exists.
    const RoutineDef& Either() const { return hasSource ? source : target; }
};

struct RoutineDiffStat {
    int identical        = 0;
    int sourceOnly       = 0;
    int targetOnly       = 0;
    int signatureDiffers = 0;
    int bodyDiffers      = 0;
    int notComparable    = 0;

    int Differences() const
    {
        return sourceOnly + targetOnly + signatureDiffers + bodyDiffers + notComparable;
    }
    int Total() const { return identical + Differences(); }
};

// The whole result of a routine comparison. A VALUE, produced by
// CompareRoutines() and consumed by the UI. Not reachable from SyncPlan, not
// reachable from SyncEngine::Execute (guarantee 3).
struct RoutineDiffSet {
    // Per-side read outcome. Non-Ok collapses that side's categories to the
    // "不可见" label with zero children and leaves the TABLE diff completely
    // untouched — this type is not an input to the table diff at all, which is
    // what makes that guarantee structural rather than a promise.
    RoutineReadStatus sourceStatus = RoutineReadStatus::Unsupported;
    RoutineReadStatus targetStatus = RoutineReadStatus::Unsupported;
    wxString          sourceStatusDetail;   // server error text, when there was one
    wxString          targetStatusDetail;

    // False when the two sides are different engines. Drives the "bodies are
    // ALWAYS NotComparable" rule, and is recorded here so the UI can explain
    // why a cross-engine run shows no body verdicts.
    bool sameEngine = false;

    std::vector<RoutinePair> pairs;   // functions and procedures, kind on each
    RoutineDiffStat          stat;

    // True when BOTH sides were read successfully. When false, `pairs` is empty
    // by construction: we never emit a half-comparison, because "the target has
    // 40 functions the source lacks" read off a side we could not see is a lie.
    bool Comparable() const
    {
        return sourceStatus == RoutineReadStatus::Ok &&
               targetStatus == RoutineReadStatus::Ok;
    }

    bool Empty() const { return pairs.empty(); }

    // Pointers into `pairs` for one category, in the order produced by
    // CompareRoutines (differences first, then identical; stable by key). The
    // UI builds its two tree categories from these two calls. Non-owning: the
    // set must outlive the returned vector.
    std::vector<const RoutinePair*> ByKind(RoutineKind k) const
    {
        std::vector<const RoutinePair*> out;
        for (const auto& p : pairs)
            if (p.kind == k) out.push_back(&p);
        return out;
    }

    // Differences only, for the default "just show me what's different" view.
    std::vector<const RoutinePair*> DifferencesByKind(RoutineKind k) const
    {
        std::vector<const RoutinePair*> out;
        for (const auto& p : pairs)
            if (p.kind == k && RoutineVerdictIsDifference(p.verdict)) out.push_back(&p);
        return out;
    }
};

} // namespace db::sync
