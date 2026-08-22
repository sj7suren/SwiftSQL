// SyncSpecScript.h — a scripted driver for the data-sync compare screen's
// MODEL, plus the outcome vocabulary and the deterministic rendering of what a
// given dialog state would execute.
//
// ---------------------------------------------------------------------------
// WHAT THIS ESTABLISHES, AND WHAT IT DOES NOT
// ---------------------------------------------------------------------------
// It drives ui::SyncCompareSession — the exact object ui::SyncComparePage owns
// and builds its two execution artefacts from — through a sequence of the
// gestures a user performs (arm the delete master switch, tick a category on a
// table, exclude a row), and renders the resulting db::sync::DataExecPlan and
// filtered db::sync::SyncPlan as stable text.
//
// So it answers: 「given this dialog state, what will be executed?」
//
// It does NOT answer 「does the dialog render that state correctly?」. Nothing
// here creates a window, so nothing here can observe a checkbox, a grid cell,
// a greyed-out control or a summary label. A green run means the BACKEND
// CONTRACT REACHABLE FROM THE GUI SEAT holds; it is not, and must never be
// reported as, evidence that the screen draws. That boundary is the whole
// reason this file says so twice.
//
// ---------------------------------------------------------------------------
// OUTCOME VOCABULARY — why it looks like ui::gridsql::ScriptedEditToken's
// ---------------------------------------------------------------------------
// The SWIFTSQL_AUTOEDIT hook used to print `ERR:` for an empty diff, which
// merged 「the value was already correct」, 「the write was refused」 and
// 「nothing was edited」 into one error-shaped line. Every live test built on
// that output was correspondingly weaker than it looked. The fix was one token
// per distinguishable outcome, with benign outcomes NOT error-shaped and a
// refusal distinguishable from "nothing to do".
//
// The same two rules are applied here, to a wider surface:
//
//   * BENIGN IS NOT ERROR-SHAPED. A structure-only sync legitimately produces
//     an EMPTY DataExecPlan. That is `OK:STRUCTURE_ONLY`, not an error and not
//     a bare `EMPTY` — a reader who cannot tell it from 「the user selected
//     nothing」 learns nothing from a green run.
//   * A REFUSAL IS NOT A NO-OP. Ticking 删除 while the master switch is off is
//     accepted by the check-state model and then stripped by the gate. That is
//     `APPLIED:INACTIVE:DELETES_DISARMED` — it says both halves out loud,
//     because 「the click did nothing」 and 「the click was recorded and the
//     gate will veto it」 are different facts about the delete path.
//   * A GESTURE THAT NAMED SOMETHING NONEXISTENT IS NEVER SILENT. A row key
//     that matches no row in the compare is `NO_SUCH_ROW`, and the exclusion
//     is NOT applied. ui::SyncSelection::SetRowExcluded accepts any key by
//     design (it holds no row inventory), so a typo'd or re-derived key would
//     otherwise be stored, exclude nothing, and let the run report success
//     while syncing a row the user unchecked. That failure mode is the one the
//     data layer's RowChange::RowKey() carry-through exists to prevent, and a
//     scripted run must not be able to reintroduce it above the seam.
//   * A MALFORMED SCRIPT IS A LOUD FAILURE, NEVER 「nothing selected」. A
//     mistyped directive yields `BAD_SCRIPT:<n>` for the whole run, so a
//     verification run that never actually exercised anything cannot be
//     mistaken for one that did.
//
// ---------------------------------------------------------------------------
// SCRIPT SYNTAX
// ---------------------------------------------------------------------------
// Directives are separated by ';' or newlines; fields within one by spaces.
// Table names are stable ids (db::sync::QualifiedName::Key()), never indices —
// there is deliberately no positional directive, for the same reason
// ui::StableId deletes its integral constructors.
//
//   deletes on|off                 the delete master switch
//   check   <table> <category>     tick one (table, category)
//   uncheck <table> <category>
//   table   <table> on|off         whole-table sweep (never touches deletes)
//   all     <category> on|off      sweep every table in the compare
//   exclude <table> <rowkey>       row-level exclusion, by verbatim row key
//   include <table> <rowkey>
//   exclude-op <table> insert|update|delete    exclude every row of that op,
//                                              each by the key its node carries
//
//   <category> ::= structure | inserts | updates | deletes
#pragma once

#include <vector>
#include <wx/string.h>

namespace ui {

class SyncCompareSession;

namespace syncspec {

// ---------------------------------------------------------------------------
// One gesture's outcome
// ---------------------------------------------------------------------------

enum class GestureStatus {
    Applied,                  // the model accepted it and it is active
    InactiveDeletesDisarmed,  // accepted, but the master switch will strip it
    InactiveNotExecutable,    // accepted, but the diff vetoes this category
    NoSuchTable,              // named a table absent from the compare
    NoSuchCategory,           // unknown category word
    NoSuchRow,                // named a row key no row in the compare carries
    NoRowSelection,           // truncated table — row selection is not offered
    BadDirective,             // unparseable
};

struct GestureOutcome {
    GestureStatus status = GestureStatus::Applied;
    wxString      source;   // the directive text, verbatim
    wxString      detail;   // the offending name / a count / the category
};

// APPLIED | APPLIED:INACTIVE:DELETES_DISARMED | APPLIED:INACTIVE:NOT_EXECUTABLE
// NO_SUCH_TABLE:<t> | NO_SUCH_CATEGORY:<c> | NO_SUCH_ROW:<k>
// NO_ROW_SELECTION:<t> | BAD_DIRECTIVE
wxString GestureToken(const GestureOutcome& g);

// ---------------------------------------------------------------------------
// The run's outcome
// ---------------------------------------------------------------------------

enum class ScriptStatus {
    Ok,                   // both halves will execute
    OkStructureOnly,      // DDL only — an EMPTY DataExecPlan, legitimately
    OkDataOnly,           // data only — no DDL selected
    EmptyNothingSelected, // the spec is empty: nothing at all will run
    EmptyNoPlan,          // no compare result was adopted
    BadScript,            // at least one directive was unparseable
};

struct ScriptResult {
    ScriptStatus                status = ScriptStatus::EmptyNoPlan;
    wxString                    detail;
    std::vector<GestureOutcome> gestures;
    wxString                    render;   // see RenderPlans()
};

// OK | OK:STRUCTURE_ONLY | OK:DATA_ONLY | EMPTY:NOTHING_SELECTED
// EMPTY:NO_PLAN | BAD_SCRIPT:<n>
wxString ScriptToken(const ScriptResult& r);

// ---------------------------------------------------------------------------
// Rendering — the machine-readable account of what would execute
// ---------------------------------------------------------------------------
//
// Field-separated by '|' rather than spaces because preamble statements
// contain both spaces and '='. Line kinds:
//
//   PRE|<stmt>                      plan preamble, in order
//   POST|<stmt>                     plan postamble, in order
//   DDL|<table>|<statement count>   one per table whose DDL survived the spec
//   DATA|<table>|I=<0|1>|U=<0|1>|D=<0|1>|X=<key,key>
//                                   one per db::sync::TableDataSpec, in the
//                                   spec's key order (identity, not position)
//
// Deterministic: ui::ExecutionSpec sorts by TableKey, so the DATA block is
// stable regardless of how the compare happened to order its units.
wxString RenderPlans(const SyncCompareSession& session);

// Apply `script` to `session` and report. The session is mutated — this IS the
// gesture path, not a simulation of it.
ScriptResult RunSpecScript(SyncCompareSession& session, const wxString& script);

// The full report body: verdict line, one line per gesture, then the render.
// This is what the SWIFTSQL_SYNCSPEC hook writes to disk.
wxString FormatScriptReport(const ScriptResult& r);

} // namespace syncspec
} // namespace ui
