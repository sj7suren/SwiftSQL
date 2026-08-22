// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DataSyncExec.h — the APPLY half of cross-engine data sync (ADR-015 T10).
//
// ---------------------------------------------------------------------------
// WHY THIS IS A SECOND PASS AND NOT A REPLAY OF THE FIRST
// ---------------------------------------------------------------------------
// The compare pass (DataSync.h's DiffRowChanges) answers "what differs, and how
// much?". It scans everything — counts must be exact — but materializes at most
// RowChangeSet::kSampleCap (500) rows. It therefore CANNOT be the thing that
// executes: replaying a 500-row sample against a 500k-row diff would apply
// 0.1% of the work and report success.
//
// So the execute pass re-runs the diff and streams each change straight into
// the target as it is produced. Nothing accumulates: peak memory is one batch
// (500 rows) regardless of table size.
//
// THE HONEST CONSEQUENCE, STATED OUT LOUD: the source and target are read a
// SECOND time, so a row that changed between the two passes is applied as it is
// at execute time, not as the user saw it at compare time. This is not a bug
// that can be designed away without either (a) holding a snapshot transaction
// open across the user's entire review — minutes to hours, which on PostgreSQL
// blocks vacuum and on MySQL grows undo without bound — or (b) materializing
// the whole diff, which is the memory bug this design exists to fix.
//
// So DRIFT — the compare pass and the execute pass legitimately seeing different
// data — is reported by construction rather than by a flag: every counter in
// TableExecReport is what this pass ACTUALLY applied, per table, never a copy of
// what the compare pass predicted. DataExecResult deliberately has no Drifted()
// and no memory of the compare pass: it does not receive the compare pass's
// numbers, so any such predicate here would be a lie about information this type
// does not have. Detecting drift is the caller's job, because only the caller
// holds both sides — src/ui does it by comparing these counters against
// db::sync::SyncPlan::TableUnit::stat.
//
// ---------------------------------------------------------------------------
// PER-(TABLE, CATEGORY) FILTERING IS STRUCTURAL, NOT A FILTER
// ---------------------------------------------------------------------------
// The user checks 插入/更新/删除 per table. That choice is carried here by
// TableDataSpec and converted into DataSyncOptions flags, which the merge
// consults AT THE POINT OF DETECTION (see DataSync.cpp's category gate). A
// category that is off never reaches the RowChangeSink at all, so no
// RowChangeBuilder, no RowChange, no rendered statement and no bound batch row
// is ever constructed for it. There is nothing downstream to accidentally
// execute, and nothing anywhere classifies a statement by its leading verb.
//
// Deletes carry the extra lock: TableDataSpec::deletes_ starts false and the
// ONLY mutator is EnableDeletes(const DeleteAuthorization&), whose argument has
// a private constructor. Honest scope of that guarantee: the token proves the
// delete master switch was consulted on THIS side of the layer boundary. The
// unforgeable one is ui::DeleteGate; this is its db-layer counterpart, and the
// adapter that mints one from a ui::DeleteGate is the single place the two
// meet.
//
// Own translation unit deliberately: SyncEngine.cpp is orchestration and the
// project charter caps every file at 1000 lines.
#pragma once

#include "db/DataSync.h"
#include "db/DbDriver.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace db::sync {

// ---------------------------------------------------------------------------
// Delete authorization
// ---------------------------------------------------------------------------

// Proof that the "allow deleting extra rows in target" master switch was on.
// Cannot be default-constructed; the only producer is AuthorizeDeletes().
class DeleteAuthorization {
public:
    DeleteAuthorization(const DeleteAuthorization&)            = default;
    DeleteAuthorization& operator=(const DeleteAuthorization&) = default;

private:
    DeleteAuthorization() = default;
    friend std::optional<DeleteAuthorization> AuthorizeDeletes(bool masterSwitchOn);
};

// nullopt while the master switch is off — so a spec containing deletes cannot
// be built at all in that state.
std::optional<DeleteAuthorization> AuthorizeDeletes(bool masterSwitchOn);

// ---------------------------------------------------------------------------
// What one table is allowed to do
// ---------------------------------------------------------------------------

// The db-layer mirror of one ui::TableExecSpec entry. Carries its own table
// name so a consumer never maps it back into another collection by position
// (the Round 1 bug ui/SyncSelection.h documents).
class TableDataSpec {
public:
    explicit TableDataSpec(wxString table) : table_(std::move(table)) {}

    const wxString& Table() const { return table_; }

    void EnableInserts() { inserts_ = true; }
    void EnableUpdates() { updates_ = true; }
    // Deletes need the token. There is no other mutator.
    void EnableDeletes(const DeleteAuthorization&) { deletes_ = true; }

    // Rows the user individually unchecked, as EncodeRowKey() strings. Only
    // meaningful for a non-truncated compare (the UI does not offer row-level
    // selection otherwise), and applied by identity, never by position.
    void ExcludeRow(wxString rowKey) { excluded_.insert(std::move(rowKey)); }

    bool Inserts() const { return inserts_; }
    bool Updates() const { return updates_; }
    bool Deletes() const { return deletes_; }
    bool AnyWrite() const { return inserts_ || updates_ || deletes_; }
    const std::set<wxString>& ExcludedRows() const { return excluded_; }

private:
    wxString           table_;
    bool               inserts_ = false;
    bool               updates_ = false;
    bool               deletes_ = false;
    std::set<wxString> excluded_;
};

// One table's unit of work: how to re-derive the diff, and what may be applied.
struct TableExecJob {
    DataDiffSpec  spec;
    TableDataSpec allow{wxString()};
};

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

// What actually happened to ONE table. Reported per table because the run is
// per-table-transactional: on a partial failure the caller must be able to say
// which tables are committed and which are untouched, rather than "it failed".
struct TableExecReport {
    wxString              table;

    // Rows actually applied — and "applied" means COMMITTED, not "accepted by
    // the server". A sweep that rolls back rewinds these to the values it
    // started with, so a counter can never describe rows that no longer exist.
    //
    // That is a real distinction and not a pedantic one: inserts are sent 500 at
    // a time and each successful ExecuteBatch used to be counted immediately,
    // so a table cancelled after two batches reported inserts=1000 while the
    // database held ZERO rows (observed live, mysql_pg_live_exec4_test item 2).
    // A caller cannot re-derive the truth from `committed` alone, because the
    // per-sweep transactions mean committed=false is ALSO the correct state for
    // a table whose DELETE sweep really did commit before its INSERT sweep
    // failed. Both cases must be readable off the same three numbers, which is
    // why the rewind is to the sweep's starting point rather than to zero:
    // committed=false with deletes=2 and inserts=0 says exactly what happened.
    long long             inserts = 0;
    long long             updates = 0;
    long long             deletes = 0;
    long long             excluded = 0;  // skipped by the user's row exclusions
                                         // (rewound with the counters above)
    bool                  attempted = false;
    bool                  committed = false;
    wxString              error;         // empty unless this table failed
    std::vector<wxString> warnings;      // e.g. an unbuildable sequence fix
};

struct DataExecResult {
    std::vector<TableExecReport> tables;

    // Tables that committed / failed / were never reached (a failure stops the
    // run, so later tables are simply not attempted — and saying so is the
    // point of `attempted`).
    std::vector<wxString> Committed() const;
    std::vector<wxString> Failed() const;
    std::vector<wxString> NotAttempted() const;

    bool AllCommitted() const;
};

// Fired per applied batch/statement so a long table does not show a frozen
// gauge. `applied` is cumulative within the table.
using DataExecProgress =
    std::function<void(const wxString& table, const wxString& phase,
                       long long applied)>;

// ---------------------------------------------------------------------------
// How the executor obtains the connection it READS THE TARGET through
// ---------------------------------------------------------------------------
//
// The target must be read on a DIFFERENT connection from the one written
// through — see RunTableSweep in the .cpp for the collision that forces this.
// Production leaves this empty and gets db::CloneConnection, the established
// tunnel-aware chokepoint.
//
// The seam exists for exactly one reason, stated so nobody mistakes it for a
// policy knob: the OFFLINE unit tests drive this executor with stub
// IConnections, and a stub cannot be cloned by a chokepoint whose job is to
// dial a real server. Without the seam the only ways to keep those tests
// running would be to let a failed clone fall back to the shared connection —
// which silently reinstates the defect — or to delete the offline coverage.
//
// Returning nullptr (with `err` set) ABORTS the table, exactly as a failed
// clone does. There is deliberately no way to say "just use the write
// connection".
using TargetReadOpener = std::function<std::unique_ptr<IConnection>(
    const IConnection& tgt, wxString& err)>;

// ---------------------------------------------------------------------------
// The executor
// ---------------------------------------------------------------------------

// Apply `jobs` to the target.
//
// ORDERING. `jobs` must already be in FK topological order (referenced table
// first) — SyncEngine owns that sort and this function does not re-derive it.
// Two sweeps, because the two directions are opposites and doing them in one
// pass is wrong in whichever direction you pick:
//   1. DELETE sweep, in REVERSE topological order. A child row must go before
//      the parent row it references, or the FK rejects the parent's delete.
//   2. INSERT/UPDATE sweep, in FORWARD topological order. A parent row must
//      exist before the child row that references it.
// A table that participates in both is therefore diffed twice — the honest cost
// of never holding the whole diff in memory. Tables with only one category are
// scanned once.
//
// TRANSACTIONS. One per (table, sweep), NOT one for the whole run. A single
// transaction spanning a 10M-row sync exhausts PostgreSQL's WAL and MySQL's
// undo tablespace, and takes the entire run down with the last table. The cost
// is that a failure leaves earlier tables committed — which is why
// DataExecResult reports them individually instead of returning one bool.
//
// Returns false with `err` on the first table that fails or on cancellation;
// `out` is populated either way and is the authoritative account of what landed.
//
// TARGET READS run on their own connection, opened per table by
// `openTargetRead` (default: db::CloneConnection) and closed as soon as that
// table's scan ends. See TargetReadOpener above and RunTableSweep in the .cpp.
bool ExecuteDataSync(IConnection& src, IConnection& tgt,
                     const std::vector<TableExecJob>& jobs,
                     const DataSyncOptions& base, DataExecResult& out,
                     wxString& err, const std::atomic<bool>& stop,
                     const DataExecProgress& progress,
                     const TargetReadOpener& openTargetRead = TargetReadOpener());

} // namespace db::sync
