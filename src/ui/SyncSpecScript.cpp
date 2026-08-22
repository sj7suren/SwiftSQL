// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncSpecScript.cpp — see header.
#include "ui/SyncSpecScript.h"

#include "db/SyncEngine.h"
#include "ui/SyncCompareSession.h"

#include <wx/tokenzr.h>

namespace ui::syncspec {
namespace {

// ---------------------------------------------------------------------------
// Tree lookup — by stable id, never by position
// ---------------------------------------------------------------------------

const DiffNode* FindTableNode(const DiffTree& tree, const wxString& id)
{
    if (id.IsEmpty()) return nullptr;
    for (const auto& root : tree.roots) {
        if (root->kind != DiffNodeKind::Category) continue;
        for (const auto& t : root->children)
            if (t->kind == DiffNodeKind::Table && t->id == id) return t.get();
    }
    return nullptr;
}

// Does any concrete row of this table carry EXACTLY this key? Summary children
// (「修改 5 行」) have an empty rowKey and are therefore never matched — a
// counter must not be excludable.
bool TableHasRowKey(const DiffNode* table, const wxString& rowKey)
{
    if (!table || rowKey.IsEmpty()) return false;
    for (const auto& c : table->children)
        if (c->kind == DiffNodeKind::Row && c->rowKey == rowKey) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Word parsing
// ---------------------------------------------------------------------------

bool ParseCategory(const wxString& w, ChangeCategory& out)
{
    if (w == L"structure") { out = ChangeCategory::Structure; return true; }
    if (w == L"inserts")   { out = ChangeCategory::Inserts;   return true; }
    if (w == L"updates")   { out = ChangeCategory::Updates;   return true; }
    if (w == L"deletes")   { out = ChangeCategory::Deletes;   return true; }
    return false;
}

bool ParseOnOff(const wxString& w, bool& out)
{
    if (w == L"on")  { out = true;  return true; }
    if (w == L"off") { out = false; return true; }
    return false;
}

bool ParseOp(const wxString& w, DiffOp& out)
{
    if (w == L"insert") { out = DiffOp::Add;    return true; }
    if (w == L"update") { out = DiffOp::Modify; return true; }
    if (w == L"delete") { out = DiffOp::Drop;   return true; }
    return false;
}

GestureOutcome Make(GestureStatus s, const wxString& src, const wxString& detail = wxString())
{
    GestureOutcome g;
    g.status = s;
    g.source = src;
    g.detail = detail;
    return g;
}

// A `check` that the model accepted: say whether the gate will actually let it
// run, and — when it will not — WHICH gate. "Recorded but vetoed" and "recorded
// and live" are different facts about the delete path and must not share a
// token.
GestureStatus CheckedOutcome(const SyncCompareSession& s, const TableKey& t, ChangeCategory c)
{
    if (s.Selection().IsActive(t, c)) return GestureStatus::Applied;
    if (c == ChangeCategory::Deletes && !s.AllowDeletes())
        return GestureStatus::InactiveDeletesDisarmed;
    return GestureStatus::InactiveNotExecutable;
}

// ---------------------------------------------------------------------------
// One directive
// ---------------------------------------------------------------------------

GestureOutcome ApplyDirective(SyncCompareSession& s, const wxString& line,
                              const std::vector<wxString>& w)
{
    const wxString& verb = w[0];

    if (verb == L"deletes" && w.size() == 2) {
        bool on = false;
        if (!ParseOnOff(w[1], on)) return Make(GestureStatus::BadDirective, line);
        s.SetAllowDeletes(on);
        return Make(GestureStatus::Applied, line, on ? L"on" : L"off");
    }

    if ((verb == L"check" || verb == L"uncheck") && w.size() == 3) {
        ChangeCategory cat{};
        if (!ParseCategory(w[2], cat))
            return Make(GestureStatus::NoSuchCategory, line, w[2]);
        const TableKey key(w[1]);
        const bool     on = (verb == L"check");
        if (!s.Selection().SetChecked(key, cat, on))
            return Make(GestureStatus::NoSuchTable, line, w[1]);
        if (!on) return Make(GestureStatus::Applied, line);
        return Make(CheckedOutcome(s, key, cat), line, w[2]);
    }

    if (verb == L"table" && w.size() == 3) {
        bool on = false;
        if (!ParseOnOff(w[2], on)) return Make(GestureStatus::BadDirective, line);
        const TableKey key(w[1]);
        if (!s.Selection().Find(key)) return Make(GestureStatus::NoSuchTable, line, w[1]);
        s.Selection().SetTableChecked(key, on);
        return Make(GestureStatus::Applied, line);
    }

    if (verb == L"all" && w.size() == 3) {
        ChangeCategory cat{};
        if (!ParseCategory(w[1], cat))
            return Make(GestureStatus::NoSuchCategory, line, w[1]);
        bool on = false;
        if (!ParseOnOff(w[2], on)) return Make(GestureStatus::BadDirective, line);
        s.Selection().SetCheckedForAll(s.Selection().Keys(), cat, on);
        return Make(GestureStatus::Applied, line);
    }

    if ((verb == L"exclude" || verb == L"include") && w.size() == 3) {
        const TableKey key(w[1]);
        if (!s.Selection().Find(key)) return Make(GestureStatus::NoSuchTable, line, w[1]);
        if (!s.Selection().RowSelectionAvailable(key))
            return Make(GestureStatus::NoRowSelection, line, w[1]);
        // The inventory check the selection model deliberately cannot do: it
        // stores keys and holds no rows, so a key that matches nothing would be
        // accepted, exclude nothing, and pass silently. See the header.
        if (!TableHasRowKey(FindTableNode(s.Tree(), w[1]), w[2]))
            return Make(GestureStatus::NoSuchRow, line, w[2]);
        s.Selection().SetRowExcluded(key, RowKey(w[2]), verb == L"exclude");
        return Make(GestureStatus::Applied, line, w[2]);
    }

    if (verb == L"exclude-op" && w.size() == 3) {
        DiffOp op{};
        if (!ParseOp(w[2], op)) return Make(GestureStatus::BadDirective, line);
        const TableKey key(w[1]);
        if (!s.Selection().Find(key)) return Make(GestureStatus::NoSuchTable, line, w[1]);
        if (!s.Selection().RowSelectionAvailable(key))
            return Make(GestureStatus::NoRowSelection, line, w[1]);
        const DiffNode* node = FindTableNode(s.Tree(), w[1]);
        int             n    = 0;
        if (node) {
            for (const auto& c : node->children) {
                if (c->kind != DiffNodeKind::Row || c->rowKey.IsEmpty()) continue;
                if (c->op != op) continue;
                // The key the NODE carries, copied — never re-derived. A
                // re-encoded cross-engine key is a different string and would
                // silently miss.
                s.Selection().SetRowExcluded(key, RowKey(c->rowKey), true);
                ++n;
            }
        }
        if (n == 0) return Make(GestureStatus::NoSuchRow, line, w[2]);
        return Make(GestureStatus::Applied, line, wxString::Format(L"%d", n));
    }

    return Make(GestureStatus::BadDirective, line);
}

} // namespace

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

wxString GestureToken(const GestureOutcome& g)
{
    switch (g.status) {
    case GestureStatus::Applied:
        return g.detail.IsEmpty() ? wxString(L"APPLIED") : L"APPLIED:" + g.detail;
    case GestureStatus::InactiveDeletesDisarmed:
        return L"APPLIED:INACTIVE:DELETES_DISARMED";
    case GestureStatus::InactiveNotExecutable:
        return L"APPLIED:INACTIVE:NOT_EXECUTABLE";
    case GestureStatus::NoSuchTable:    return L"NO_SUCH_TABLE:" + g.detail;
    case GestureStatus::NoSuchCategory: return L"NO_SUCH_CATEGORY:" + g.detail;
    case GestureStatus::NoSuchRow:      return L"NO_SUCH_ROW:" + g.detail;
    case GestureStatus::NoRowSelection: return L"NO_ROW_SELECTION:" + g.detail;
    case GestureStatus::BadDirective:   return L"BAD_DIRECTIVE";
    }
    return L"BAD_DIRECTIVE";
}

wxString ScriptToken(const ScriptResult& r)
{
    switch (r.status) {
    case ScriptStatus::Ok:                   return L"OK";
    case ScriptStatus::OkStructureOnly:      return L"OK:STRUCTURE_ONLY";
    case ScriptStatus::OkDataOnly:           return L"OK:DATA_ONLY";
    case ScriptStatus::EmptyNothingSelected: return L"EMPTY:NOTHING_SELECTED";
    case ScriptStatus::EmptyNoPlan:          return L"EMPTY:NO_PLAN";
    case ScriptStatus::BadScript:            return L"BAD_SCRIPT:" + r.detail;
    }
    return L"BAD_SCRIPT:?";
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

wxString RenderPlans(const SyncCompareSession& session)
{
    wxString out;
    if (!session.HasPlan()) return out;

    const db::sync::SyncPlan     ddl  = session.BuildExecutionPlan();
    const db::sync::DataExecPlan data = session.BuildDataExecPlan();

    for (const wxString& s : ddl.preamble)  out += L"PRE|"  + s + L"\n";
    for (const wxString& s : ddl.postamble) out += L"POST|" + s + L"\n";

    for (const db::sync::SyncPlan::TableUnit& u : ddl.units) {
        if (u.ddl.empty()) continue;
        out += wxString::Format(L"DDL|%s|%d\n", u.table.Key(),
                                static_cast<int>(u.ddl.size()));
    }

    for (const db::sync::TableDataSpec& t : data.tables) {
        wxString x;
        for (const wxString& k : t.ExcludedRows()) {
            if (!x.IsEmpty()) x += L",";
            x += k;
        }
        out += wxString::Format(L"DATA|%s|I=%d|U=%d|D=%d|X=%s\n", t.Table(),
                                t.Inserts() ? 1 : 0, t.Updates() ? 1 : 0,
                                t.Deletes() ? 1 : 0, x);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

ScriptResult RunSpecScript(SyncCompareSession& session, const wxString& script)
{
    ScriptResult r;

    wxStringTokenizer lines(script, L";\r\n", wxTOKEN_STRTOK);
    int               index = 0;
    int               firstBad = -1;
    while (lines.HasMoreTokens()) {
        const wxString line = lines.GetNextToken().Trim(true).Trim(false);
        if (line.IsEmpty() || line.StartsWith(L"#")) continue;
        ++index;

        std::vector<wxString> words;
        wxStringTokenizer     wt(line, L" \t", wxTOKEN_STRTOK);
        while (wt.HasMoreTokens()) words.push_back(wt.GetNextToken());

        GestureOutcome g = words.empty() ? Make(GestureStatus::BadDirective, line)
                                         : ApplyDirective(session, line, words);
        if (g.status == GestureStatus::BadDirective && firstBad < 0) firstBad = index;
        r.gestures.push_back(std::move(g));
    }

    r.render = RenderPlans(session);

    // A malformed script outranks every other verdict. A run whose directives
    // never landed must not be able to report "nothing was selected", which is
    // also what a correctly-run empty selection reports.
    if (firstBad >= 0) {
        r.status = ScriptStatus::BadScript;
        r.detail = wxString::Format(L"%d", firstBad);
        return r;
    }
    if (!session.HasPlan())          { r.status = ScriptStatus::EmptyNoPlan; return r; }
    if (!session.HasAnythingSelected()) {
        r.status = ScriptStatus::EmptyNothingSelected;
        return r;
    }

    const db::sync::SyncPlan     ddl  = session.BuildExecutionPlan();
    const db::sync::DataExecPlan data = session.BuildDataExecPlan();
    bool                         anyDdl = false;
    for (const db::sync::SyncPlan::TableUnit& u : ddl.units)
        if (!u.ddl.empty()) { anyDdl = true; break; }

    // Structure-only is a legitimate, extremely common outcome and an EMPTY
    // DataExecPlan is its correct shape — it gets its own OK token rather than
    // sharing one with "nothing selected".
    if (anyDdl && data.Empty())  r.status = ScriptStatus::OkStructureOnly;
    else if (!anyDdl)            r.status = ScriptStatus::OkDataOnly;
    else                         r.status = ScriptStatus::Ok;
    return r;
}

wxString FormatScriptReport(const ScriptResult& r)
{
    wxString out = ScriptToken(r) + L"\n";
    int      i   = 0;
    for (const GestureOutcome& g : r.gestures)
        out += wxString::Format(L"G%d|%s|%s\n", ++i, GestureToken(g), g.source);
    out += r.render;
    return out;
}

} // namespace ui::syncspec
