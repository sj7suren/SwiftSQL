// SyncSelection.h — pure, non-wx model of the user's check-state on the
// data-sync compare screen (T6 of ADR-015).
//
// "Non-wx" follows SchemaModel.h / SyncDiffModel.h convention: wxString is
// used (the project's baseline string type, not a UI dependency), but nothing
// here touches wxWindow/wxGrid/wxColour/etc. This file is unit-testable
// headless — see tests/SyncSelectionTests.cpp.
//
// ---------------------------------------------------------------------------
// WHY THIS COMPONENT EXISTS: the bug class it makes impossible
// ---------------------------------------------------------------------------
// Round 1's wizard rendered diffs into a flat wxListCtrl and mapped the user's
// checked rows back to the plan BY ROW POSITION (`plan_.units[i]`, i = list
// row index). When the flat list was later replaced by a categorized tree —
// which interleaves category header rows among data rows — that positional
// assumption would have silently executed the WRONG TABLE'S DDL. It was caught
// and fixed by keying on stable table names.
//
// This component is designed so that mistake cannot be reintroduced, and the
// enforcement is in the TYPE SYSTEM, not in a comment:
//
//   1. Every public entry point that identifies a table or a row takes a
//      TableKey / RowKey. Those types are `explicit`-only from wxString and
//      have EVERY integral constructor `= delete`d (see StableId below), so
//      `sel.SetChecked(3, ...)`, `TableKey(i)`, `TableKey k = row;` are all
//      COMPILE ERRORS, not runtime surprises.
//   2. There is NO positional accessor anywhere in this API. No
//      `operator[](size_t)`, no `TableAt(n)`, no exposure of the source
//      collection's order. Enumeration (`Keys()`) hands back keys, so even a
//      `for (i = 0; ...)` loop can only ever produce a key.
//   3. Internal storage is std::map<TableKey, ...>. There is no index to
//      accidentally reach for, because none exists.
//   4. ExecutionSpec entries each CARRY their own TableKey. Consumers never
//      need to map a spec back into another collection, which is the exact
//      operation that went wrong in Round 1.
//
// ---------------------------------------------------------------------------
// DATA-LAYER SEAM (integration note)
// ---------------------------------------------------------------------------
// src/db/RowChange.h (db::RowChangeSet / db::RowStat / db::Finding, with the
// `truncated` and `executable` flags) was being authored in parallel and did
// NOT exist when this file was written. Rather than create another engineer's
// file, this header declares the minimal view it needs — ui::TableDiff /
// ui::CompareResult — as plain value types. Swapping in the real header is
// mechanical: populate TableDiff from db::RowChangeSet at the ONE adapter
// point documented above TableDiff. Nothing else in this file, and no test,
// depends on how TableDiff got filled.
#pragma once

#include <wx/string.h>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace ui {

// ---------------------------------------------------------------------------
// Stable identity
// ---------------------------------------------------------------------------

// A stable, opaque identity string. Tagged so a TableKey can never be passed
// where a RowKey is expected (or vice versa) even though both wrap wxString.
//
// The deleted integral constructors are the load-bearing part of this class:
// they are what turns "someone used a row index as an identity" from a silent
// data-corruption bug into a compiler diagnostic. Do not remove them, and do
// not add a non-explicit constructor.
template <class Tag>
class StableId {
public:
    StableId() = default;
    explicit StableId(wxString value) : value_(std::move(value)) {}

    // String-literal overloads. These are NOT redundant: a `const wchar_t*`
    // reaches the deleted bool constructor below by a standard pointer->bool
    // conversion, which outranks the user-defined conversion to wxString, so
    // without these `TableKey(L"orders")` would resolve to a deleted function.
    explicit StableId(const wchar_t* value) : value_(value) {}
    explicit StableId(const char* value) : value_(value) {}

    // A POSITION IS NOT AN IDENTITY. See the file header. Any attempt to build
    // an id out of a row/column/tree index fails to compile here.
    StableId(bool)               = delete;
    StableId(char)               = delete;
    StableId(int)                = delete;
    StableId(unsigned)           = delete;
    StableId(long)               = delete;
    StableId(unsigned long)      = delete;
    StableId(long long)          = delete;
    StableId(unsigned long long) = delete;

    const wxString& Value() const { return value_; }
    bool            IsEmpty() const { return value_.IsEmpty(); }

    bool operator==(const StableId& o) const { return value_ == o.value_; }
    bool operator!=(const StableId& o) const { return !(*this == o); }
    // Ordering is over the identity string, NOT over any source-collection
    // position — that is what makes std::map keyed on this reorder-invariant.
    bool operator<(const StableId& o) const { return value_.Cmp(o.value_) < 0; }

private:
    wxString value_;
};

struct TableIdTag {};
struct RowIdTag {};

// Identifies one table in the compare result. By convention this is the same
// string SyncDiffModel.h uses for DiffNode::id (== db::sync::SyncPlan::
// TableUnit::table), so a selection made in the grid and a node clicked in the
// drill-in tree refer to the same table without any translation layer.
using TableKey = StableId<TableIdTag>;

// Identifies one differing row within a table. Expected to be the encoded
// primary key (the only row identity that survives re-sorting/re-paging).
using RowKey = StableId<RowIdTag>;

// ---------------------------------------------------------------------------
// Change categories
// ---------------------------------------------------------------------------

// The granularity the user actually checks: (table, category). Per ADR-015 the
// compare screen offers table-level and category-level checkboxes, not
// per-statement ones.
enum class ChangeCategory { Structure, Inserts, Updates, Deletes };

// All four, in display order. Provided so callers iterate categories instead
// of hard-coding a count that would drift if a category is ever added.
const std::vector<ChangeCategory>& AllCategories();

// Stable, non-localized identifier for logs/tests. Display strings belong in
// the UI layer, not here.
const wxString& CategoryName(ChangeCategory cat);

// ---------------------------------------------------------------------------
// Data-layer seam — see file header
// ---------------------------------------------------------------------------

// One table's compare outcome, as this selection model needs to see it.
//
// Populated from db::sync::RowChangeSet by ui::MakeTableDiff() — see
// src/ui/SyncSelectionAdapter.h, which is the ONE place that knows about the
// data layer. This file deliberately does not include db/RowChange.h: keeping
// the model dependency-free is what lets tests/SyncSelectionTests.cpp link
// only swiftsql::core, which in turn is a standing fitness function against
// dependency creep into this component.
// Why this table's DATA was or wasn't diffed. A pure mirror of
// db::sync::DataVerdict, redeclared here so this header keeps depending on
// nothing but wxString (see the file header's fitness-function note); the
// translation is done once, in SyncSelectionAdapter.h / SyncCompareRow.h.
//
// This is a VERDICT, not a reason string. The compare grid's 状态 column has
// these cases in its vocabulary, and the alternative to carrying them
// structurally is for the UI to recover them by grepping a human-readable
// warning sentence — exactly the string-sniffing this component forbids.
enum class DataStatus {
    NotRequested,    // data sync not in scope, or the table is target-only
    Ok,              // diffed successfully
    NoPrimaryKey,    // 无主键（已排除）
    UnorderableKey,  // PK order cannot be guaranteed on both engines
    TargetMissing,   // target lacks the table and structure sync won't create it
    Blocked,         // a column/value the gate refuses to convert
};

struct TableDiff {
    TableKey key;

    // Structure half.
    bool hasStructure        = false;   // this table has column/index/FK changes
    bool structureExecutable = true;    // false => blocked, can never be executed

    // Data half (row counts as detected by the data diff).
    long long inserts = 0;
    long long updates = 0;
    long long deletes = 0;

    // True when the data diff hit its cap and holds only a sample. Row-level
    // selection is NOT offered in that case: letting the user believe they
    // hand-picked from 500k rows when they saw 1k would be a lie in the UI.
    bool truncated = false;

    // db::RowChangeSet::executable — false when the set carries blocking
    // findings (e.g. a value that cannot be safely converted cross-engine).
    // A false here excludes every data category of this table from the
    // execution spec, unconditionally.
    bool dataExecutable = true;

    // Why the data half looks the way it does. Purely informational for the
    // selection model — a refused verdict always arrives with zero counters, so
    // Has()/Executable() already exclude it. It exists so the grid can say WHICH
    // refusal happened instead of showing a bare 就绪 for a table that was never
    // diffed at all.
    DataStatus dataStatus = DataStatus::NotRequested;

    // Human-readable audit trail, for the grid's "why is this greyed out?"
    // affordance. DISPLAY ONLY — never parsed to make a decision. Whether a
    // category may run is answered by dataExecutable/structureExecutable,
    // which the data layer derives; re-deriving it from text here would be
    // exactly the kind of string-sniffing SyncDiffModel.h warns against.
    std::vector<wxString> findings;

    // True when this table has any change at all in the given category.
    bool Has(ChangeCategory cat) const;
    // True when the category is both present and permitted to execute. Note
    // this does NOT consider the delete master switch — that lives in
    // SyncSelection, because it is global state, not a property of the diff.
    bool Executable(ChangeCategory cat) const;
};

// The full compare outcome handed to the selection model. Deliberately a flat
// vector with NO documented ordering contract: SyncSelection keys off
// TableDiff::key and is invariant to how this vector happens to be ordered.
struct CompareResult {
    std::vector<TableDiff> tables;
};

// ---------------------------------------------------------------------------
// Delete safety: the capability token
// ---------------------------------------------------------------------------

// Proof that the global "allow deleting extra rows in target" master switch
// was ON at the moment the execution spec was built.
//
// It cannot be forged: the default constructor is private and SyncSelection is
// the only friend. The only producer is SyncSelection::AcquireDeleteGate(),
// which returns std::nullopt while the master switch is off. Since
// TableExecSpec::EnableDeletes() REQUIRES one of these by signature, an
// execution spec containing deletes cannot be constructed — by any code path,
// including buggy future code — unless the master switch was on.
class DeleteGate {
public:
    DeleteGate(const DeleteGate&)            = default;
    DeleteGate& operator=(const DeleteGate&) = default;

private:
    DeleteGate() = default;
    friend class SyncSelection;
};

// ---------------------------------------------------------------------------
// Execution spec — what will actually run
// ---------------------------------------------------------------------------

// One table's slice of the execution spec. Constructible ONLY by SyncSelection
// (private ctor + friend), and every category enabler re-validates against the
// authoritative TableDiff it is handed. That double layer is intentional: even
// if a future edit to SyncSelection::Build() forgets a check, the enabler
// physically cannot be called without the TableDiff that vetoes it.
class TableExecSpec {
public:
    // Carries its own identity, so a consumer never maps this back into
    // another collection by position — the Round 1 bug has no foothold here.
    const TableKey& Table() const { return table_; }

    bool Structure() const { return structure_; }
    bool Inserts() const { return inserts_; }
    bool Updates() const { return updates_; }
    bool Deletes() const { return deletes_; }
    bool Includes(ChangeCategory cat) const;

    // Rows the user explicitly unchecked. Always empty for a truncated table
    // (row-level selection is not offered there). Semantics: everything in an
    // included category runs EXCEPT these rows.
    const std::set<RowKey>& ExcludedRows() const { return excludedRows_; }

    // True when no category is included — such an entry is never emitted into
    // an ExecutionSpec, so this only ever reads false in practice.
    bool Empty() const;

private:
    explicit TableExecSpec(TableKey table) : table_(std::move(table)) {}

    // Each enabler is a no-op unless `diff` says the category is present AND
    // executable. `diff` must be the entry for this table.
    void EnableStructure(const TableDiff& diff);
    void EnableInserts(const TableDiff& diff);
    void EnableUpdates(const TableDiff& diff);
    // The delete path additionally demands an unforgeable DeleteGate.
    void EnableDeletes(const TableDiff& diff, const DeleteGate& gate);
    // No-op when diff.truncated — row-level selection is not offered there.
    void ExcludeRows(const TableDiff& diff, const std::set<RowKey>& rows);

    friend class SyncSelection;

    TableKey         table_;
    bool             structure_ = false;
    bool             inserts_   = false;
    bool             updates_   = false;
    bool             deletes_   = false;
    std::set<RowKey> excludedRows_;
};

// The filtered result: exactly what should execute. Produced only by
// SyncSelection::Build().
//
// ORDERING: entries are sorted by TableKey — deterministic and therefore
// independent of the order CompareResult::tables happened to be in. This is
// NOT an execution order; FK topological ordering remains SyncEngine's job.
class ExecutionSpec {
public:
    const std::vector<TableExecSpec>& Tables() const { return tables_; }
    bool                              Empty() const { return tables_.empty(); }
    std::size_t                       TableCount() const { return tables_.size(); }

    // Lookup is by identity, never by position.
    const TableExecSpec* Find(const TableKey& table) const;

    // True if any table includes deletes. Guaranteed false whenever the master
    // switch was off at Build() time — see DeleteGate.
    bool ContainsDeletes() const;

private:
    ExecutionSpec() = default;
    friend class SyncSelection;

    std::vector<TableExecSpec> tables_;
};

// Footer counters for the compare screen. Derived FROM an ExecutionSpec so the
// number the user reads can never disagree with what will run — in particular
// the delete count is 0 whenever the master switch is off.
struct SelectionSummary {
    std::size_t tables           = 0;   // tables with anything selected
    std::size_t structureTables  = 0;
    long long   inserts          = 0;
    long long   updates          = 0;
    long long   deletes          = 0;
    std::size_t excludedRows     = 0;   // rows individually unchecked by the user
};

// ---------------------------------------------------------------------------
// The selection model
// ---------------------------------------------------------------------------

class SyncSelection {
public:
    SyncSelection() = default;

    // ---- adopting a compare result ----

    // Discard all state and seed defaults from `result`:
    //   structure / inserts / updates -> checked when present and executable
    //   deletes                       -> ALWAYS off (product safety default)
    void Reset(const CompareResult& result);

    // Adopt a NEW compare result while preserving existing check state BY KEY:
    // tables that survived keep their checks, vanished tables are pruned, new
    // tables get seeded defaults. Order of `result.tables` is irrelevant — this
    // is the re-compare/refresh path, and the reorder regression test.
    void Rebind(const CompareResult& result);

    // Keys of every table in the adopted result, sorted. Returns KEYS, never
    // indices — see file header point 2.
    std::vector<TableKey> Keys() const;

    // The adopted diff for a table, or nullptr when unknown.
    const TableDiff* Find(const TableKey& table) const;

    std::size_t TableCount() const { return tables_.size(); }

    // ---- the delete master switch ----

    // Global "allow deleting extra rows in target". Off by default.
    bool AllowDeletes() const { return allowDeletes_; }
    void SetAllowDeletes(bool on) { allowDeletes_ = on; }

    // ---- check state ----

    // The raw check bit the user toggled. For Deletes this is only half the
    // story — use IsActive() for anything that decides behaviour.
    bool IsChecked(const TableKey& table, ChangeCategory cat) const;

    // Whether this (table, category) will actually be executed: the check bit
    // AND the category is present AND executable AND — for Deletes — the
    // master switch is on. This is the predicate the UI should render from.
    bool IsActive(const TableKey& table, ChangeCategory cat) const;

    // Returns false when `table` is not in the adopted result (no silent
    // creation of phantom selections).
    bool SetChecked(const TableKey& table, ChangeCategory cat, bool checked);

    // Select-all / invert over the caller-supplied VISIBLE set (filtering and
    // sorting are the grid's business; both are expressed as keys, so a
    // filtered/sorted view cannot desynchronize from the model).
    void SetCheckedForAll(const std::vector<TableKey>& visible, ChangeCategory cat, bool checked);
    void InvertChecked(const std::vector<TableKey>& visible, ChangeCategory cat);

    // Whole-table convenience: sets structure/inserts/updates only.
    // DELIBERATELY DOES NOT TOUCH Deletes — a "select all" sweep must never
    // silently arm destructive deletes. To check deletes the user must act on
    // the Deletes category explicitly.
    void SetTableChecked(const TableKey& table, bool checked);
    void SetAllTablesChecked(const std::vector<TableKey>& visible, bool checked);

    // ---- row-level selection ----

    // Row-level selection is offered only when the table's diff was fully
    // materialized (!truncated).
    bool RowSelectionAvailable(const TableKey& table) const;

    // Exclude/re-include a single row. Returns false — and changes nothing —
    // when the table is unknown or its diff is truncated.
    bool SetRowExcluded(const TableKey& table, const RowKey& row, bool excluded);

    bool IsRowExcluded(const TableKey& table, const RowKey& row) const;
    const std::set<RowKey>& ExcludedRows(const TableKey& table) const;
    void                    ClearRowExclusions(const TableKey& table);

    // ---- output ----

    // The filtered execution spec. Unchecked, absent and non-executable
    // categories are structurally excluded; deletes are impossible unless the
    // master switch is on.
    ExecutionSpec Build() const;

    // Footer counters, computed from Build() so they can never disagree.
    SelectionSummary Summarize() const;

private:
    struct TableState {
        TableDiff        diff;
        bool             structure = false;
        bool             inserts   = false;
        bool             updates   = false;
        bool             deletes   = false;   // seeded false, always
        std::set<RowKey> excludedRows;

        bool  Checked(ChangeCategory cat) const;
        void  SetChecked(ChangeCategory cat, bool checked);
    };

    // The ONLY producer of a DeleteGate. Returns nullopt while the master
    // switch is off, which is what makes delete-bearing specs unconstructible.
    std::optional<DeleteGate> AcquireDeleteGate() const;

    static TableState SeedFrom(const TableDiff& diff);

    TableState*       Lookup(const TableKey& table);
    const TableState* Lookup(const TableKey& table) const;

    // Keyed by identity. There is no index here to misuse.
    std::map<TableKey, TableState> tables_;
    bool                           allowDeletes_ = false;
};

} // namespace ui
