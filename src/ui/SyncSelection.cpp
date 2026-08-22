// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncSelection.cpp — see header. Pure logic, no I/O, no wx widgets.
#include "ui/SyncSelection.h"

#include <algorithm>

namespace ui {

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------

const std::vector<ChangeCategory>& AllCategories()
{
    static const std::vector<ChangeCategory> kAll = {
        ChangeCategory::Structure,
        ChangeCategory::Inserts,
        ChangeCategory::Updates,
        ChangeCategory::Deletes,
    };
    return kAll;
}

const wxString& CategoryName(ChangeCategory cat)
{
    static const wxString kStructure = L"structure";
    static const wxString kInserts   = L"inserts";
    static const wxString kUpdates   = L"updates";
    static const wxString kDeletes   = L"deletes";
    static const wxString kUnknown   = L"?";
    switch (cat) {
    case ChangeCategory::Structure: return kStructure;
    case ChangeCategory::Inserts:   return kInserts;
    case ChangeCategory::Updates:   return kUpdates;
    case ChangeCategory::Deletes:   return kDeletes;
    }
    return kUnknown;
}

// ---------------------------------------------------------------------------
// TableDiff
// ---------------------------------------------------------------------------

bool TableDiff::Has(ChangeCategory cat) const
{
    switch (cat) {
    case ChangeCategory::Structure: return hasStructure;
    case ChangeCategory::Inserts:   return inserts > 0;
    case ChangeCategory::Updates:   return updates > 0;
    case ChangeCategory::Deletes:   return deletes > 0;
    }
    return false;
}

bool TableDiff::Executable(ChangeCategory cat) const
{
    if (!Has(cat)) return false;
    // The structure half and the data half are produced by different
    // components (SchemaDelta vs RowChangeSet) and are blocked independently.
    return cat == ChangeCategory::Structure ? structureExecutable : dataExecutable;
}

// ---------------------------------------------------------------------------
// TableExecSpec
// ---------------------------------------------------------------------------

bool TableExecSpec::Includes(ChangeCategory cat) const
{
    switch (cat) {
    case ChangeCategory::Structure: return structure_;
    case ChangeCategory::Inserts:   return inserts_;
    case ChangeCategory::Updates:   return updates_;
    case ChangeCategory::Deletes:   return deletes_;
    }
    return false;
}

bool TableExecSpec::Empty() const
{
    return !structure_ && !inserts_ && !updates_ && !deletes_;
}

// Each enabler re-validates against the authoritative diff. This is the second
// of the two layers described in the header: SyncSelection::Build() already
// filters, but these make a future filtering bug non-exploitable rather than
// merely unlikely.
void TableExecSpec::EnableStructure(const TableDiff& diff)
{
    if (!diff.Executable(ChangeCategory::Structure)) return;
    structure_ = true;
}

void TableExecSpec::EnableInserts(const TableDiff& diff)
{
    if (!diff.Executable(ChangeCategory::Inserts)) return;
    inserts_ = true;
}

void TableExecSpec::EnableUpdates(const TableDiff& diff)
{
    if (!diff.Executable(ChangeCategory::Updates)) return;
    updates_ = true;
}

// The `gate` parameter is unused at runtime BY DESIGN: its entire job is to be
// impossible to supply unless SyncSelection::AcquireDeleteGate() handed one
// out, which it only does while the delete master switch is on. The compiler,
// not a runtime `if`, is what enforces the master switch here.
void TableExecSpec::EnableDeletes(const TableDiff& diff, const DeleteGate& gate)
{
    (void)gate;
    if (!diff.Executable(ChangeCategory::Deletes)) return;
    deletes_ = true;
}

void TableExecSpec::ExcludeRows(const TableDiff& diff, const std::set<RowKey>& rows)
{
    // A truncated diff never carries row-level decisions — see header.
    if (diff.truncated) return;
    excludedRows_ = rows;
}

// ---------------------------------------------------------------------------
// ExecutionSpec
// ---------------------------------------------------------------------------

const TableExecSpec* ExecutionSpec::Find(const TableKey& table) const
{
    for (const auto& t : tables_)
        if (t.Table() == table) return &t;
    return nullptr;
}

bool ExecutionSpec::ContainsDeletes() const
{
    for (const auto& t : tables_)
        if (t.Deletes()) return true;
    return false;
}

// ---------------------------------------------------------------------------
// SyncSelection::TableState
// ---------------------------------------------------------------------------

bool SyncSelection::TableState::Checked(ChangeCategory cat) const
{
    switch (cat) {
    case ChangeCategory::Structure: return structure;
    case ChangeCategory::Inserts:   return inserts;
    case ChangeCategory::Updates:   return updates;
    case ChangeCategory::Deletes:   return deletes;
    }
    return false;
}

void SyncSelection::TableState::SetChecked(ChangeCategory cat, bool checked)
{
    switch (cat) {
    case ChangeCategory::Structure: structure = checked; break;
    case ChangeCategory::Inserts:   inserts   = checked; break;
    case ChangeCategory::Updates:   updates   = checked; break;
    case ChangeCategory::Deletes:   deletes   = checked; break;
    }
}

// ---------------------------------------------------------------------------
// SyncSelection — adopting a compare result
// ---------------------------------------------------------------------------

SyncSelection::TableState SyncSelection::SeedFrom(const TableDiff& diff)
{
    TableState st;
    st.diff = diff;
    // Non-destructive categories start checked when there is something
    // executable to do; a blocked category is never pre-checked, so the UI
    // never shows a tick next to something that cannot run.
    st.structure = diff.Executable(ChangeCategory::Structure);
    st.inserts   = diff.Executable(ChangeCategory::Inserts);
    st.updates   = diff.Executable(ChangeCategory::Updates);
    // Hard product rule: deletes default OFF, no exceptions, regardless of
    // executability or the master switch's current position.
    st.deletes = false;
    return st;
}

void SyncSelection::Reset(const CompareResult& result)
{
    tables_.clear();
    for (const auto& d : result.tables) {
        if (d.key.IsEmpty()) continue;   // an unidentifiable table is not selectable
        tables_[d.key] = SeedFrom(d);
    }
}

void SyncSelection::Rebind(const CompareResult& result)
{
    std::map<TableKey, TableState> next;
    for (const auto& d : result.tables) {
        if (d.key.IsEmpty()) continue;
        auto prev = tables_.find(d.key);
        if (prev == tables_.end()) {
            next[d.key] = SeedFrom(d);   // newly appeared table
            continue;
        }
        // Carry the user's decisions over BY KEY — never by position, which is
        // precisely what makes a reordered compare result a no-op here.
        TableState st = prev->second;
        st.diff       = d;
        // A category that became non-executable, or vanished from the diff,
        // must not stay checked; re-seed only those bits.
        for (ChangeCategory cat : AllCategories())
            if (st.Checked(cat) && !d.Executable(cat)) st.SetChecked(cat, false);
        // Row-level decisions are meaningless once a diff turns truncated.
        if (d.truncated) st.excludedRows.clear();
        next[d.key] = std::move(st);
    }
    tables_.swap(next);
}

std::vector<TableKey> SyncSelection::Keys() const
{
    std::vector<TableKey> keys;
    keys.reserve(tables_.size());
    for (const auto& kv : tables_) keys.push_back(kv.first);
    return keys;   // std::map iteration order == TableKey order, already sorted
}

const TableDiff* SyncSelection::Find(const TableKey& table) const
{
    const TableState* st = Lookup(table);
    return st ? &st->diff : nullptr;
}

SyncSelection::TableState* SyncSelection::Lookup(const TableKey& table)
{
    auto it = tables_.find(table);
    return it == tables_.end() ? nullptr : &it->second;
}

const SyncSelection::TableState* SyncSelection::Lookup(const TableKey& table) const
{
    auto it = tables_.find(table);
    return it == tables_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// SyncSelection — check state
// ---------------------------------------------------------------------------

bool SyncSelection::IsChecked(const TableKey& table, ChangeCategory cat) const
{
    const TableState* st = Lookup(table);
    return st && st->Checked(cat);
}

bool SyncSelection::IsActive(const TableKey& table, ChangeCategory cat) const
{
    const TableState* st = Lookup(table);
    if (!st) return false;
    if (!st->Checked(cat)) return false;
    if (!st->diff.Executable(cat)) return false;
    // Gate 2 of the delete double-gate (gate 1 is the per-table check bit
    // tested above). Build() does not re-implement this test — it goes through
    // AcquireDeleteGate(), so the two can never drift apart.
    if (cat == ChangeCategory::Deletes && !allowDeletes_) return false;
    return true;
}

bool SyncSelection::SetChecked(const TableKey& table, ChangeCategory cat, bool checked)
{
    TableState* st = Lookup(table);
    if (!st) return false;
    st->SetChecked(cat, checked);
    return true;
}

void SyncSelection::SetCheckedForAll(const std::vector<TableKey>& visible,
                                     ChangeCategory cat, bool checked)
{
    for (const auto& key : visible) SetChecked(key, cat, checked);
}

void SyncSelection::InvertChecked(const std::vector<TableKey>& visible, ChangeCategory cat)
{
    for (const auto& key : visible) {
        TableState* st = Lookup(key);
        if (!st) continue;
        st->SetChecked(cat, !st->Checked(cat));
    }
}

void SyncSelection::SetTableChecked(const TableKey& table, bool checked)
{
    TableState* st = Lookup(table);
    if (!st) return;
    st->structure = checked;
    st->inserts   = checked;
    st->updates   = checked;
    // Deletes intentionally untouched — see header. A bulk sweep must never
    // arm a destructive category on the user's behalf.
}

void SyncSelection::SetAllTablesChecked(const std::vector<TableKey>& visible, bool checked)
{
    for (const auto& key : visible) SetTableChecked(key, checked);
}

// ---------------------------------------------------------------------------
// SyncSelection — row-level selection
// ---------------------------------------------------------------------------

bool SyncSelection::RowSelectionAvailable(const TableKey& table) const
{
    const TableState* st = Lookup(table);
    return st && !st->diff.truncated;
}

bool SyncSelection::SetRowExcluded(const TableKey& table, const RowKey& row, bool excluded)
{
    TableState* st = Lookup(table);
    if (!st) return false;
    // Refused for a capped sample: the user cannot meaningfully hand-pick rows
    // they were never shown.
    if (st->diff.truncated) return false;
    if (row.IsEmpty()) return false;
    if (excluded) st->excludedRows.insert(row);
    else          st->excludedRows.erase(row);
    return true;
}

bool SyncSelection::IsRowExcluded(const TableKey& table, const RowKey& row) const
{
    const TableState* st = Lookup(table);
    return st && st->excludedRows.count(row) != 0;
}

const std::set<RowKey>& SyncSelection::ExcludedRows(const TableKey& table) const
{
    static const std::set<RowKey> kNone;
    const TableState*             st = Lookup(table);
    return st ? st->excludedRows : kNone;
}

void SyncSelection::ClearRowExclusions(const TableKey& table)
{
    if (TableState* st = Lookup(table)) st->excludedRows.clear();
}

// ---------------------------------------------------------------------------
// SyncSelection — output
// ---------------------------------------------------------------------------

std::optional<DeleteGate> SyncSelection::AcquireDeleteGate() const
{
    if (!allowDeletes_) return std::nullopt;
    return DeleteGate{};
}

ExecutionSpec SyncSelection::Build() const
{
    ExecutionSpec spec;

    // Acquired ONCE, before the loop: if the master switch is off this is
    // nullopt and there is simply no token in existence for EnableDeletes() to
    // be called with, for any table, in this Build(). That is the whole
    // enforcement — there is no `if (allowDeletes_)` further down to forget.
    const std::optional<DeleteGate> gate = AcquireDeleteGate();

    for (const auto& kv : tables_) {   // sorted by TableKey => deterministic
        const TableState& st   = kv.second;
        const TableDiff&  diff = st.diff;

        TableExecSpec entry(kv.first);
        if (st.Checked(ChangeCategory::Structure)) entry.EnableStructure(diff);
        if (st.Checked(ChangeCategory::Inserts))   entry.EnableInserts(diff);
        if (st.Checked(ChangeCategory::Updates))   entry.EnableUpdates(diff);
        if (st.Checked(ChangeCategory::Deletes) && gate)
            entry.EnableDeletes(diff, *gate);

        if (entry.Empty()) continue;   // nothing selected => not in the spec at all
        entry.ExcludeRows(diff, st.excludedRows);
        spec.tables_.push_back(std::move(entry));
    }

    return spec;
}

SelectionSummary SyncSelection::Summarize() const
{
    // Deliberately derived from Build() rather than recomputed from check
    // state: any counter the user reads is then, by construction, a count of
    // what will actually run (deletes included — i.e. zero when gated off).
    const ExecutionSpec spec = Build();

    SelectionSummary sum;
    sum.tables = spec.TableCount();
    for (const auto& entry : spec.Tables()) {
        const TableDiff* diff = Find(entry.Table());
        if (!diff) continue;   // unreachable: entries come from tables_
        if (entry.Structure()) ++sum.structureTables;
        if (entry.Inserts()) sum.inserts += diff->inserts;
        if (entry.Updates()) sum.updates += diff->updates;
        if (entry.Deletes()) sum.deletes += diff->deletes;
        sum.excludedRows += entry.ExcludedRows().size();
    }
    return sum;
}

} // namespace ui
