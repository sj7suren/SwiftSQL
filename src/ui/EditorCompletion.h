// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// EditorCompletion.h — the PURE decision layer behind the SQL editor's
// autocompletion popup: which words are offered, and — given a dot-qualified
// prefix like `db.` / `table.` / `db.table.` — which schema source answers it.
//
// WHY IT LIVES HERE. All of this used to be inline in EditorPage.cpp, reading
// characters straight out of a wxStyledTextCtrl and pushing results into
// AutoCompShow, so it could only be exercised by typing into a running app
// attached to a live server. Worse, the qualifier-chain parse existed TWICE —
// once over Scintilla positions (ShowColumnCompletions) and once over a plain
// wxString (ScriptedComplete, the SWIFTSQL_AUTOEDIT verification hook) — which
// means the hook used to VERIFY completion was running a second, independently
// written copy of the logic it was supposed to be checking.
//
// THAT SECOND COPY IS GONE. CompleteQualified below is now the ONE qualified-
// completion decision: EditorPage::ShowColumnCompletions calls it and does
// nothing else but hand the answer to Scintilla, and ScriptedComplete reads back
// what ShowColumnCompletions returned. The hook contains no completion logic at
// all any more, so a defect in the production scan necessarily shows up in it.
//
// DEPENDENCY FITNESS: wxString, std::map/vector/function. No wxWindow, no
// wxStyledTextCtrl, no db:: type — the caller resolves the dialect to keyword
// and function lists and passes them in. This TU links swiftsql::core alone.
#pragma once

#include <wx/string.h>
#include <functional>
#include <vector>

namespace ui::editorcomp {

// Completion categories. The value is the Scintilla registered-image type, so
// these numbers are part of the popup's visual contract (EditorPage registers a
// coloured icon per category) — they are not free to renumber.
enum Category {
    kKeyword  = 1,
    kFunction = 2,
    kDatabase = 3,
    kTable    = 4,
    kColumn   = 5,
};

// ---------------------------------------------------------------------------
// Un-qualified completion: the whole word list.
// ---------------------------------------------------------------------------

// Merge the four sources into one de-duplicated, WORD-SORTED list of
// (word, category). Sorted because Scintilla requires it.
//
// PRECEDENCE IS DELIBERATE AND ASYMMETRIC. Keywords and functions are inserted
// weakly: the first of the two to claim a word keeps it. Databases and tables
// are inserted strongly and OVERWRITE — so a real schema object named `order` or
// `count` shows the database/table icon rather than being disguised as a
// keyword. Live schema out-ranks the static dialect list; that is the rule.
std::vector<std::pair<wxString, int>> MergeWordCandidates(
    const std::vector<wxString>& keywords,
    const std::vector<wxString>& functions,
    const std::vector<wxString>& databases,
    const std::vector<wxString>& tables);

// ---------------------------------------------------------------------------
// Dot-qualified completion.
// ---------------------------------------------------------------------------

// The identifier chain immediately left of the caret's '.', in source order:
// "a."     → {a}
// "x.a.b." → {x, a, b}
struct QualifierChain {
    std::vector<wxString> parts;
    // True when the scan hit a position that yielded no identifier characters —
    // "a..b.", a leading dot, a dot after an operator or a closing paren. The
    // chain is then whatever had been collected BEFORE that point, which may be
    // empty.
    //
    // THE POLICY IS NOW SINGLE AND LIVES IN CompleteQualified: a truncated chain
    // completes to NOTHING, whatever was collected before the break. That is what
    // the live popup has always done (it bailed out of the whole scan on the first
    // empty token); the scripted hook used to resolve the partial chain instead,
    // which is a SECOND divergence between the two implementations, independent of
    // the byte/character one and reachable on pure ASCII — "a..b." offered b's
    // columns through the hook and nothing through the popup. Production's answer
    // survives, both because it is what ships and because it is the conservative
    // one: after a malformed chain we do not know what the user meant, and
    // offering some plausible table's columns is a wrong answer rather than no
    // answer.
    bool truncated = false;
};

// Parse the chain out of the text ENDING AT (and excluding) the triggering dot.
// Scans right-to-left over identifier characters — whatever wxIsalnum accepts,
// plus '_'. wxIsalnum is iswalnum on a whole wxUniChar, so a CJK ideograph IS an
// identifier character here: `用户表.` parses to one part, not zero.
//
// CHARACTER OFFSETS ARE THE SURVIVING SEMANTICS. The scan this replaced in
// EditorPage::ShowColumnCompletions ran over Scintilla positions, which are UTF-8
// BYTE offsets, and classified an identifier by its individual bytes: for U+8868
// (bytes E8 A1 A8) it asked wxIsalnum of 0xA8, got false, and stopped dead —
// so a CJK table name completed to NOTHING, and a mixed name like `表a.`
// completed as if the table were called `a`. Byte offsets are the widget's
// storage detail, not a semantics worth preserving; nothing was gained by
// classifying bytes and a whole class of identifiers was lost. The caller now
// asks Scintilla for the text as a wxString and lets Scintilla do the UTF-8
// decoding it is already good at — see ShowColumnCompletions, which after this
// change performs exactly one byte computation, stepping over the '.' itself.
QualifierChain ParseQualifierChain(const wxString& textBeforeDot);

// The schema sources a qualified completion can consult. Every one is optional
// (an unset callable means "this source is unavailable"), because a fresh editor
// with no connection has none of them.
struct SchemaLookup {
    std::function<bool(const wxString&)>                                   isDatabase;
    std::function<std::vector<wxString>(const wxString&)>                  tablesOf;
    std::function<std::vector<wxString>(const wxString&, const wxString&)> columnsOf;
    std::function<std::vector<wxString>(const wxString&)>                  columns;
    // alias → real table name, resolved against the editor buffer.
    std::function<wxString(const wxString&)>                               resolveTable;
};

struct QualifierCompletion {
    std::vector<wxString> candidates;
    int                   category = kColumn;
};

// Decide what a parsed chain completes to:
//   {db, table} (or longer) → columns of that table in that database
//   {name} where name is a live database → the tables of that database
//   {name} otherwise → columns of the table `name` resolves to (alias-aware)
//   {} → nothing
// Candidates come back in SOURCE ORDER, NOT sorted — callers that feed Scintilla
// must sort, and they do. Returning them unsorted is intentional: it keeps this
// function's answer to "which source, and what did it say" separable from the
// popup's presentation requirement.
QualifierCompletion ResolveQualifier(const QualifierChain& chain,
                                     const SchemaLookup& look);

// THE production qualified-completion decision, end to end: parse the chain out
// of `textBeforeDot`, apply the truncated-chain policy, resolve it against the
// schema, and SORT the result.
//
// This exists so that there is exactly one answer to "the user typed a dot —
// what goes in the popup?". EditorPage::ShowColumnCompletions is now a wrapper
// that fetches `textBeforeDot` from the widget, calls this, and renders; the
// SWIFTSQL_AUTOEDIT hook calls ShowColumnCompletions and reports what came back.
// Neither of them re-derives anything, which is the whole point: the hook can no
// longer agree with itself while disagreeing with what the user sees.
//
// Sorted here, unlike ResolveQualifier, because BOTH former call sites sorted
// immediately afterwards — Scintilla requires it of the popup, and the hook's
// output has to be stable to be diffable. Leaving the sort out was one more thing
// two call sites had to remember to do identically.
QualifierCompletion CompleteQualified(const wxString& textBeforeDot,
                                      const SchemaLookup& look);

// ---------------------------------------------------------------------------
// Alias resolution.
// ---------------------------------------------------------------------------

// Map `ident` to the real table it names, by scanning `bufferText` for a
// "table [AS] alias" binding. `hasColumns` reports whether a name is a real
// table (asking the schema for its columns).
//
// `ident` is returned UNCHANGED when it is already a real table, when no binding
// matches, or when the buffer names nothing — i.e. this function never fails, it
// degrades to the identity. That matters: a wrong answer here would complete
// with some OTHER table's column names, which is more misleading than offering
// none, so every uncertain case resolves to "assume it was a table name".
wxString ResolveTableForIdent(const wxString& bufferText, const wxString& ident,
                              const std::function<bool(const wxString&)>& hasColumns);

// ---------------------------------------------------------------------------
// Popup list rendering.
// ---------------------------------------------------------------------------

// Scintilla's AutoCompShow payload: "word?type" entries joined by '\n', where
// '?' is the type separator EditorPage registers and `type` selects the icon.
wxString BuildAutoCompList(const std::vector<std::pair<wxString, int>>& items);
wxString BuildAutoCompList(const std::vector<wxString>& words, int category);

// Space-joined plain words — the machine-readable form the scripted completion
// verification hook returns.
wxString JoinWords(const std::vector<wxString>& words);

} // namespace ui::editorcomp
