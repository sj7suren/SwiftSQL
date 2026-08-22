// DataSyncExec.cpp — see header. The second-pass streaming executor.
#include "db/DataSyncExec.h"
#include "db/ConnectionClone.h"
#include "db/SyncSeqFix.h"

#include <algorithm>
#include <memory>

namespace db::sync {

namespace {

// Table-name rendering lives in DbDriver.h (QualifiedTableSql) so the applier
// and the merge cannot spell the target table differently -- the seam a
// wrong-table write would slip through.

// Oracle/DM open a transaction implicitly on the first DML — there is no
// standalone BEGIN (that opens a PL/SQL block), so nothing is emitted; the
// trailing COMMIT/ROLLBACK still bounds it correctly.
wxString BeginSql(Dialect d)
{
    if (d == Dialect::Oracle)    return wxString();
    return d == Dialect::SqlServer ? L"BEGIN TRANSACTION" : L"BEGIN";
}

bool RunOne(IConnection& tgt, const wxString& sql, wxString& err)
{
    QueryResult r;
    return tgt.Execute(sql, r, err);
}

// Rows per ExecuteBatch call. The drivers chunk again internally against each
// engine's 65535-placeholder ceiling, so this is a memory bound, not a
// protocol one: it is the ONLY thing this file ever accumulates.
constexpr size_t kInsertBatchRows = 500;

// ---------------------------------------------------------------------------
// TableApplier — one table, one sweep. Consumes RowChangeBuilders as the merge
// produces them and pushes them at the target; retains at most one insert batch.
// ---------------------------------------------------------------------------
class TableApplier {
public:
    TableApplier(IConnection& tgt, const DataDiffSpec& spec,
                 const RowLayout& layout, const TableDataSpec& allow,
                 TableExecReport& rep, const DataExecProgress& progress)
        : tgt_(tgt), spec_(spec), layout_(layout), allow_(allow), rep_(rep),
          progress_(progress), dialect_(tgt.GetDialect())
    {
        // TargetTable() -- the SAME expression the merge's target cursor reads
        // through. The fallback-to-source-name rule used to be reimplemented
        // here, beside a merge that did not implement it at all; now neither
        // file owns the rule and they cannot disagree about which table this is.
        qualified_ = QualifiedTableSql(spec.tgtDb, spec.TargetTable(), dialect_);

        render_.qualifiedTable = qualified_;
        render_.columns        = layout.columns;
        render_.pkColumns      = layout.pkColumns;
        render_.dialect        = dialect_;

        // Statement HEAD for ExecuteBatch: everything up to and including
        // VALUES, with no tuples. The driver appends its own placeholders.
        insertHead_ = L"INSERT INTO " + qualified_ + L" (";
        for (size_t i = 0; i < layout.columns.size(); ++i) {
            if (i) insertHead_ += L", ";
            insertHead_ += QuoteIdent(layout.columns[i], dialect_);
        }
        insertHead_ += L") VALUES";

        // Which streamed column (if any) feeds the target's identity/
        // auto-increment column. Needed for the sequence-repair postamble: the
        // target's generator does not advance when literal identity values are
        // written, so without this the next production INSERT collides.
        for (size_t i = 0; i < layout.columns.size(); ++i) {
            const NormColumn* c = spec.tgtSchema.FindColumn(layout.columns[i]);
            if (c && c->autoIncrement) { identityIdx_ = static_cast<long long>(i); break; }
        }
    }

    // The RowChangeSink. Returns false to abort the table.
    bool operator()(RowChangeBuilder&& builder, const wxString& rowKey)
    {
        // Row-level exclusion, by identity. Checked BEFORE Build() so an
        // excluded row costs nothing and, more importantly, so a row the user
        // unchecked can never be blamed for a gate refusal it was never going
        // to be subject to.
        if (!allow_.ExcludedRows().empty() &&
            allow_.ExcludedRows().count(rowKey) > 0) {
            ++rep_.excluded;
            return true;
        }

        std::optional<RowChange> row = builder.Build();
        if (!row) {
            // The compare pass already latched this table non-executable and
            // the UI should have greyed it out. Reaching here means something
            // changed on the source between the passes, so the safe move is to
            // stop this table with its transaction still open (and therefore
            // rolled back) rather than apply a partial, silently-incomplete set.
            err_ = L"表 " + spec_.SourceTable().Display() + L" 的行 " + rowKey +
                   L" 含无法安全转换的取值，已中止该表（本表未提交任何改动）";
            for (const Finding& f : builder.Findings())
                if (!f.reason.IsEmpty()) { err_ += L"：" + f.reason; break; }
            return false;
        }

        switch (row->Operation()) {
        case RowChange::Op::Insert: return Insert(*row);
        case RowChange::Op::Update: return Statement(*row, rep_.updates, L"更新");
        case RowChange::Op::Delete: return Statement(*row, rep_.deletes, L"删除");
        }
        return true;
    }

    // Push whatever is still buffered. MUST be called before COMMIT.
    bool Finish()
    {
        if (!FlushInserts()) return false;
        ApplySequenceFixes();
        return true;
    }

    const wxString& Error() const { return err_; }

private:
    bool Insert(const RowChange& row)
    {
        if (row.Values().size() != layout_.columns.size()) {
            err_ = L"表 " + spec_.SourceTable().Display() + L" 的插入行列数与列映射不一致，已中止";
            return false;
        }
        if (identityIdx_ >= 0) {
            const Cell& c = row.Values()[static_cast<size_t>(identityIdx_)];
            long long v = 0;
            if (c.kind != CellKind::Null && c.text.ToLongLong(&v))
                maxIdentity_ = std::max(maxIdentity_, v);
        }
        pending_.push_back(row.Values());
        if (pending_.size() >= kInsertBatchRows) return FlushInserts();
        return true;
    }

    bool FlushInserts()
    {
        if (pending_.empty()) return true;
        wxString e;
        if (!tgt_.ExecuteBatch(insertHead_, pending_, e)) {
            err_ = e.IsEmpty() ? wxString(L"批量插入失败") : e;
            pending_.clear();
            return false;
        }
        rep_.inserts += static_cast<long long>(pending_.size());
        pending_.clear();
        Report(L"插入", rep_.inserts);
        return true;
    }

    // UPDATE / DELETE.
    //
    // These do NOT go through ExecuteBatch, and the reason is a property of
    // that API rather than a shortcut: ExecuteBatch takes a statement head and
    // appends comma-separated value tuples after it, which is exactly an
    // INSERT's shape and no other statement's. An UPDATE's parameters straddle
    // SET and WHERE, and a batched DELETE would need a row-value IN list that
    // has to CLOSE after the tuples. Rather than invent a second batch protocol
    // in six drivers, these are executed one statement per row — rendered,
    // sent, discarded, so memory stays flat even though round trips do not.
    bool Statement(const RowChange& row, long long& counter, const wxChar* phase)
    {
        // An insert buffered behind an update of the same table would land in
        // the wrong order. Flush first: within a table, order is the merge's
        // PK order and must be preserved.
        if (!FlushInserts()) return false;

        const wxString sql = RenderRowDml(row, render_);
        if (sql.IsEmpty()) {
            // RenderRowDml returns empty for an arity mismatch or a missing
            // WHERE. Both are defensive cases that should be unreachable; a
            // WHERE-less UPDATE/DELETE is the most destructive thing this
            // system could send, so an empty render is an abort, never a skip.
            err_ = L"表 " + spec_.SourceTable().Display() + L" 的一行未能渲染为安全语句，已中止";
            return false;
        }
        wxString e;
        if (!RunOne(tgt_, sql, e)) {
            err_ = e.IsEmpty() ? wxString(L"语句执行失败") : e;
            return false;
        }
        ++counter;
        Report(phase, counter);
        return true;
    }

    // Advance the target's identity generator past the values just written.
    // Never fatal: the data is already correct, and a missing fix is a LATENT
    // duplicate-key failure rather than an immediate one — so it is reported as
    // a warning the caller must surface, not swallowed.
    void ApplySequenceFixes()
    {
        if (rep_.inserts == 0) return;
        std::vector<SeqFix> fixes;
        wxString why;
        if (!BuildSequenceFixes(spec_.tgtSchema, dialect_, qualified_,
                                maxIdentity_ > 0 ? maxIdentity_ : -1, fixes, why)) {
            // out empty + why empty  -> no identity column; nothing to do.
            // out empty + why filled -> a fix IS needed and could not be built.
            if (!why.IsEmpty())
                rep_.warnings.push_back(L"表 " + spec_.SourceTable().Display() +
                    L" 的自增/序列未能修复：" + why +
                    L"（目标下次插入可能因主键冲突失败，请手工校正）");
            return;
        }
        for (const SeqFix& f : fixes) {
            wxString e;
            if (!RunOne(tgt_, f.statement, e))
                rep_.warnings.push_back(L"表 " + spec_.SourceTable().Display() + L" 的列 " +
                    f.column + L" 自增/序列修复失败：" + e);
        }
    }

    void Report(const wxString& phase, long long applied)
    {
        if (progress_) progress_(spec_.SourceTable().Key(), phase, applied);
    }

    IConnection&         tgt_;
    const DataDiffSpec&  spec_;
    const RowLayout&     layout_;
    const TableDataSpec& allow_;
    TableExecReport&     rep_;
    DataExecProgress     progress_;
    Dialect              dialect_;

    wxString      qualified_, insertHead_;
    RowRenderSpec render_;

    std::vector<std::vector<Cell>> pending_;   // <= kInsertBatchRows
    long long                      identityIdx_ = -1;
    long long                      maxIdentity_ = 0;
    wxString                       err_;
};

// Run one table for one sweep, inside its own transaction.
//
// `opt` already carries the sweep's category flags, so the merge itself never
// produces a change this sweep is not allowed to apply.
bool RunTableSweep(IConnection& src, IConnection& tgt, const TableExecJob& job,
                   const DataSyncOptions& opt, TableExecReport& rep,
                   wxString& err, const std::atomic<bool>& stop,
                   const DataExecProgress& progress,
                   const TargetReadOpener& openTargetRead)
{
    RowLayout            layout;
    std::vector<Finding> findings;
    if (!BuildRowLayout(job.spec, src.GetDialect(), tgt.GetDialect(), layout,
                        findings)) {
        err = findings.empty()
                  ? (L"表 " + job.spec.SourceTable().Key() + L" 无法建立列映射")
                  : findings.front().reason;
        rep.error = err;
        return false;
    }

    const Dialect  dialect = tgt.GetDialect();
    const wxString begin   = BeginSql(dialect);
    rep.attempted = true;
    // Cleared per sweep: `committed` means "every sweep this table needed has
    // committed". A table whose DELETE sweep committed and whose INSERT sweep
    // then failed reports committed=false WITH a non-zero `deletes` — which is
    // the honest description of a partial apply, and the reason the counters
    // are reported per category rather than as one number.
    rep.committed = false;

    // THE COUNTERS THIS SWEEP STARTS FROM.
    //
    // TableApplier increments rep.inserts/updates/deletes as each batch or
    // statement is ACCEPTED BY THE SERVER — which, inside an open transaction,
    // is not the same thing as applied. If this sweep then rolls back, every
    // one of those rows is gone and a counter still holding them describes rows
    // that do not exist. That is not a cosmetic mismatch: TableExecReport
    // documents these as "rows actually applied", and the caller uses them to
    // tell the user what landed. Observed live (mysql_pg_live_exec4_test item 2)
    // as inserts=1000 on a table the database showed holding ZERO rows, and
    // again on a connection-loss failure (item 3) with the same shape.
    //
    // It stayed invisible until now because the only prior partial-failure
    // coverage failed on the FIRST statement of its sweep, where the counter
    // happened to still be 0. Batched inserts are the case that exposes it: 500
    // rows are counted per successful ExecuteBatch, long before any COMMIT.
    //
    // The restore below is per SWEEP, not per table, and that distinction is the
    // whole reason this is a snapshot rather than a reset to zero: sweep 1
    // (DELETE) may have COMMITTED before sweep 2 (INSERT/UPDATE) failed, and
    // those deletes really are durable. Rewinding to the sweep's own starting
    // point keeps that genuine partial-apply signature — committed=false with a
    // non-zero `deletes` — while dropping the phantom half.
    const long long ins0 = rep.inserts,  upd0 = rep.updates;
    const long long del0 = rep.deletes,  exc0 = rep.excluded;
    auto rewind = [&] {
        rep.inserts  = ins0;
        rep.updates  = upd0;
        rep.deletes  = del0;
        rep.excluded = exc0;
        // rep.warnings is deliberately NOT rewound: a warning is diagnostic
        // text, not a row count, so keeping it can only add information and
        // cannot be misread as work that landed.
    };

    // THE TARGET IS READ ON ITS OWN CONNECTION, NEVER THE ONE WE WRITE THROUGH.
    //
    // The merge reads the target through DataSync.cpp's RowCursor, which drains
    // the result set on a BACKGROUND THREAD while this thread consumes the queue
    // and issues the writes. Handing one IConnection to both meant a write could
    // be sent while that connection still had a query in flight, from a
    // different thread than the one draining it. That is not a rare interleaving:
    // the reader running AHEAD is not the reader being FINISHED, so any diff not
    // confined to the very tail of the key order issues its first write mid-
    // stream. Observed live (mysql_pg_live_scale3.cpp, phase D) as
    //   MySQL:      "Commands out of sync; you can't run this command now"
    //   PostgreSQL: PQexec returning NULL -> an EMPTY error string and a target
    //               connection left permanently dead; on other runs, a segfault.
    // Concurrent use of one client-library connection from two threads is
    // undefined behavior, so the segfault is the honest description and the
    // error text is the lucky case.
    //
    // TRANSACTION VISIBILITY, STATED RATHER THAN INHERITED. This read does NOT
    // join the write transaction, and must not: its job is to establish what the
    // target ALREADY HOLDS so the delta can be computed, and the writes are that
    // delta. A read on a second connection sees committed state only, so it can
    // never observe rows this sweep is concurrently producing. That is not a
    // behavior change either — the merge consumes the target in ascending PK
    // order and every write lands on a key it has already passed, so no write is
    // in a position the reader has yet to reach. Both designs therefore compute
    // the same diff; this one simply removes the possibility.
    //
    // The two sweeps stay correct across that boundary because sweep 1 (DELETE)
    // COMMITS before sweep 2 (INSERT/UPDATE) opens its own read, so sweep 2's
    // baseline includes sweep 1's deletes.
    //
    // Skipped entirely when the target table does not exist: StreamRowChanges
    // then streams the source alone and never touches `tgt`, so cloning would
    // open a connection per table to read nothing — the create-and-fill case.
    //
    // SQLITE, WHERE "A SECOND CONNECTION" MEANS SOMETHING ELSE ENTIRELY.
    //
    // The hazard this clone removes is a CLIENT/SERVER protocol hazard: in-flight
    // result-set state on one wire handle, touched from two threads. SQLite has
    // no wire and no such state, so the original defect genuinely does not exist
    // there — but a second connection is a second FILE HANDLE, contending through
    // the OS lock ladder (SHARED -> RESERVED -> PENDING -> EXCLUSIVE) instead.
    // The clone is kept for SQLite anyway, because it is SAFE there, and that is
    // measured rather than assumed (tests/sqlite_clone_sweep_test.cpp):
    //
    //   * The driver opens SQLITE_OPEN_READWRITE with NO busy timeout and NO WAL
    //     (journal_mode=delete). So any lock conflict fails INSTANTLY with
    //     SQLITE_BUSY — there is no waiting and no deadlock, only a hard error.
    //   * DURING the scan: the reader holds SHARED while this thread writes.
    //     SHARED and RESERVED coexist, and a writer spills its page cache to the
    //     rollback journal WITHOUT escalating — 20000 updates over several MiB
    //     against the 2 MiB default cache all succeed. So the overlap is fine.
    //   * At COMMIT: the writer MUST escalate to EXCLUSIVE, which SHARED blocks.
    //
    // That last point is the whole ballgame, and it is why the target read must
    // be FINISHED before the COMMIT below — not merely "closed eventually".
    // StreamRows finalizes its sqlite3_stmt on every exit path (SqliteStmtGuard,
    // including early stop) and RowCursor joins its producer thread in its own
    // destructor inside StreamRowChanges, so the SHARED lock is already gone by
    // the time control returns here; the tgtRead.reset() below then drops the
    // handle itself. Anything that lets a target read cursor outlive the scan
    // breaks SQLite instantly while leaving MySQL/PostgreSQL working — a silent,
    // engine-specific regression. The negative control in that test file parks
    // exactly such a reader and pins the resulting "database is locked".
    std::unique_ptr<IConnection> tgtRead;
    if (!job.spec.targetMissing) {
        wxString ce;
        // Empty opener == production == the CloneConnection chokepoint. The only
        // caller that supplies one is the offline unit test, whose stub
        // connections nothing could dial a second copy of.
        tgtRead = openTargetRead ? openTargetRead(tgt, ce)
                                 : CloneConnection(tgt, ce);
        // A clone failure ABORTS THIS TABLE. Falling back to the shared
        // connection would reinstate exactly the defect above, and silently.
        // Nothing has been written yet — the transaction is not even open.
        if (!tgtRead) {
            err = L"表 " + job.spec.SourceTable().Display() +
                  L" 无法为读取目标建立独立连接，已中止该表（目标未改变）" +
                  (ce.IsEmpty() ? wxString() : L"：" + ce);
            rep.error = err;
            return false;
        }
        // AND POINT IT AT THE TARGET DATABASE. A clone is dialed from
        // EffectiveProfile(), whose `.database` is the CONNECT-TIME database —
        // not the one this sync targets, and not whatever `tgt` is currently
        // bound to. On PostgreSQL that is the difference between reading the
        // target table and reading a same-named table in another database,
        // because PG's SQL cannot name a database at all (QualifiedTableSql in
        // db/DbDriver.h) — the binding IS the addressing. Reading the wrong
        // baseline is worse than failing to read one: the diff would come back
        // plausible and the writes would be wrong. Same root cause as
        // SyncEngine::BindSessions(); this connection is simply born after it.
        if (!tgtRead->UseDatabase(job.spec.tgtDb, ce)) {
            err = L"表 " + job.spec.SourceTable().Display() +
                  L" 的目标读取连接无法切换到数据库 " + job.spec.tgtDb +
                  L"，已中止该表（目标未改变）：" + ce;
            rep.error = err;
            return false;
        }
    }
    // Read side vs write side, named apart so no later edit can confuse them.
    IConnection& tgtStream = tgtRead ? *tgtRead : tgt;

    if (!begin.IsEmpty()) {
        wxString e;
        if (!RunOne(tgt, begin, e)) { err = e; rep.error = e; return false; }
    }

    TableApplier applier(tgt, job.spec, layout, job.allow, rep, progress);

    RowStat  detected;
    wxString streamErr;
    bool ok = StreamRowChanges(src, tgtStream, job.spec, layout, opt,
        [&applier](RowChangeBuilder&& b, const wxString& key) {
            return applier(std::move(b), key);
        }, detected, streamErr, stop);

    // Drop the read connection the moment the stream is done, BEFORE the commit
    // below rather than at end of scope. RowCursor has already joined its
    // producer thread in its own destructor inside StreamRowChanges, so nothing
    // still references it. Releasing it here keeps a PostgreSQL read snapshot
    // open for the scan only, instead of until the write transaction ends —
    // which is what keeps this off vacuum's back — and bounds concurrent
    // connections at one extra per table rather than one per table cumulatively.
    tgtRead.reset();

    // Finish() flushes the last partial batch and repairs sequences — both must
    // happen INSIDE the transaction, so they run before the commit below and
    // are rolled back with everything else on failure.
    if (ok) ok = applier.Finish();

    if (!ok) {
        wxString e;
        RunOne(tgt, L"ROLLBACK", e);      // best effort; the run is failing anyway
        // Rewind even when the ROLLBACK statement itself failed. The commonest
        // reason it fails is that the connection died (observed live in item 3),
        // and a dead backend is rolled back by the server regardless — so the
        // rows are gone either way and the counters must say so.
        rewind();
        err = !applier.Error().IsEmpty() ? applier.Error() : streamErr;
        if (err.IsEmpty()) err = L"表 " + job.spec.SourceTable().Key() + L" 的数据同步失败";
        rep.error = err;
        return false;
    }

    wxString e;
    // A failed COMMIT leaves nothing applied either — same rewind, same reason.
    //
    // ...but "nothing applied" has to be MADE true, not assumed. A COMMIT that
    // fails does not universally abort its transaction, and on at least one
    // engine it explicitly does NOT: SQLite returns SQLITE_BUSY from COMMIT when
    // another handle still holds a read lock, and documents the transaction as
    // remaining ACTIVE afterwards. Without the rollback below, that leaves the
    // write connection inside an open transaction holding every row this sweep
    // produced — rows `rewind()` has just finished reporting as not applied.
    // Three things follow, all bad: the rows stay pending and visible to this
    // connection (so a later COMMIT from anywhere could still land them), the
    // target file stays locked, and the NEXT sweep's BEGIN fails outright with
    // "cannot start a transaction within a transaction" — turning one table's
    // failure into the whole run's.
    //
    // Best effort and unconditional, exactly like the !ok path above: if it
    // fails because the backend died, the server has rolled the transaction back
    // for us and the counters are right either way. Observed (see
    // tests/sqlite_clone_sweep_test.cpp, negative control) as a sweep that
    // reported failure while the target held ZERO stale rows — i.e. the rewind
    // was lying — and a follow-up sweep that could not begin at all.
    if (!RunOne(tgt, L"COMMIT", e)) {
        wxString re;
        RunOne(tgt, L"ROLLBACK", re);
        err = e; rep.error = e; rewind(); return false;
    }
    rep.committed = true;
    return true;
}

// The per-table report, created once and shared by both sweeps so a table that
// inserts AND deletes reports one line, not two.
TableExecReport& ReportFor(DataExecResult& out, const wxString& table)
{
    for (TableExecReport& r : out.tables)
        if (r.table == table) return r;
    TableExecReport r;
    r.table = table;
    out.tables.push_back(std::move(r));
    return out.tables.back();
}

} // namespace

// ---------------------------------------------------------------------------

std::optional<DeleteAuthorization> AuthorizeDeletes(bool masterSwitchOn)
{
    if (!masterSwitchOn) return std::nullopt;
    return std::optional<DeleteAuthorization>(DeleteAuthorization{});
}

std::vector<wxString> DataExecResult::Committed() const
{
    std::vector<wxString> v;
    for (const auto& r : tables) if (r.committed) v.push_back(r.table);
    return v;
}

std::vector<wxString> DataExecResult::Failed() const
{
    std::vector<wxString> v;
    for (const auto& r : tables) if (r.attempted && !r.error.IsEmpty()) v.push_back(r.table);
    return v;
}

std::vector<wxString> DataExecResult::NotAttempted() const
{
    std::vector<wxString> v;
    for (const auto& r : tables) if (!r.attempted) v.push_back(r.table);
    return v;
}

bool DataExecResult::AllCommitted() const
{
    for (const auto& r : tables)
        if (r.attempted && !r.committed) return false;
    return true;
}

bool ExecuteDataSync(IConnection& src, IConnection& tgt,
                     const std::vector<TableExecJob>& jobs,
                     const DataSyncOptions& base, DataExecResult& out,
                     wxString& err, const std::atomic<bool>& stop,
                     const DataExecProgress& progress,
                     const TargetReadOpener& openTargetRead)
{
    out = DataExecResult{};
    // Seed a report per table up front, in plan order, so NotAttempted() is
    // meaningful the moment a sweep bails out. Reserved so the references the
    // sweeps hold can never be invalidated by a later push_back.
    out.tables.reserve(jobs.size());
    for (const TableExecJob& j : jobs)
        if (j.allow.AnyWrite()) ReportFor(out, j.spec.SourceTable().Key());

    // ---- sweep 1: DELETE, in REVERSE FK topological order ------------------
    // Children before parents, or the FK rejects the parent row's delete.
    for (size_t i = jobs.size(); i-- > 0; ) {
        const TableExecJob& job = jobs[i];
        if (!job.allow.Deletes()) continue;
        if (stop.load()) { err = L"已取消"; return false; }

        DataSyncOptions opt = base;
        opt.insert        = false;
        opt.update        = false;
        opt.deleteMissing = true;      // authorized: EnableDeletes required a token

        TableExecReport& rep = ReportFor(out, job.spec.SourceTable().Key());
        if (!RunTableSweep(src, tgt, job, opt, rep, err, stop, progress,
                           openTargetRead))
            return false;
    }

    // ---- sweep 2: INSERT/UPDATE, in FORWARD FK topological order -----------
    // Parents before children, so a referenced row exists before the row that
    // references it.
    for (const TableExecJob& job : jobs) {
        if (!job.allow.Inserts() && !job.allow.Updates()) continue;
        if (stop.load()) { err = L"已取消"; return false; }

        DataSyncOptions opt = base;
        opt.insert        = job.allow.Inserts();
        opt.update        = job.allow.Updates();
        opt.deleteMissing = false;     // never in this sweep, regardless of the token

        TableExecReport& rep = ReportFor(out, job.spec.SourceTable().Key());
        if (!RunTableSweep(src, tgt, job, opt, rep, err, stop, progress,
                           openTargetRead))
            return false;
    }

    return true;
}

} // namespace db::sync
