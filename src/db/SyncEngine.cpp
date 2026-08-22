// SyncEngine.cpp — see header. Orchestration only.
#include "db/SyncEngine.h"
#include "db/SchemaDiff.h"

#include <algorithm>
#include <map>
#include <set>

namespace db::sync {

namespace {

// Finding -> a human-readable line for SyncPlan::warnings (UI-facing free
// text, unchanged shape — the UI this round is a different track's file, so
// SyncPlan's public field types stay exactly as they were; only TableUnit
// grows the additive `changes` field). Finding.column == "" marks a
// table-level note (options mismatch, cross-engine createTable skip) rather
// than a specific column.
wxString FormatFinding(const Finding& f)
{
    const wchar_t* verdictTxt = L"不可映射";
    switch (f.verdict) {
    case TypeVerdict::Identical:  verdictTxt = L"一致";   break;
    case TypeVerdict::Equivalent: verdictTxt = L"等价";   break;
    case TypeVerdict::Lossy:      verdictTxt = L"有损";   break;
    case TypeVerdict::Unmappable: verdictTxt = L"不可映射"; break;
    }
    const wxString where = f.column.IsEmpty() ? f.table : (f.table + L"." + f.column);
    return L"[" + wxString(verdictTxt) + L"] " + where + L"：" + f.reason;
}

// Transaction start for the dialect (mirrors RunScriptDialog::BeginSql). Returned
// without a terminator so a single Execute autocommits.
wxString BeginSql(Dialect d)
{
    // Oracle/DM start a transaction implicitly on the first DML — there is no
    // standalone BEGIN (that opens a PL/SQL block), so emit nothing; the trailing
    // COMMIT/ROLLBACK still bound the transaction correctly.
    if (d == Dialect::Oracle)    return wxString();
    return d == Dialect::SqlServer ? L"BEGIN TRANSACTION" : L"BEGIN";
}

// FK topological order: a referenced table sorts before the table referencing it
// (so CREATE/INSERT parents come first). Kahn's algorithm over the FK edges that
// stay inside the unit set; on a cycle it falls back to the incoming order (with
// FOREIGN_KEY_CHECKS=0 the target still applies cleanly on MySQL).
std::vector<size_t> TopoOrder(
    const std::vector<wxString>& tables,
    const std::map<wxString, std::set<wxString>>& refs)   // table → tables it refs
{
    const size_t n = tables.size();
    std::map<wxString, size_t> pos;
    for (size_t i = 0; i < n; ++i) pos[tables[i]] = i;

    std::vector<int> indeg(n, 0);                 // # of in-set deps still ahead
    std::vector<std::vector<size_t>> dependents(n);
    for (size_t i = 0; i < n; ++i) {
        auto it = refs.find(tables[i]);
        if (it == refs.end()) continue;
        for (const wxString& rt : it->second) {
            auto p = pos.find(rt);
            if (p == pos.end() || p->second == i) continue;   // external / self
            ++indeg[i];
            dependents[p->second].push_back(i);   // rt done → unlock i
        }
    }

    std::vector<size_t> order;
    order.reserve(n);
    std::vector<bool> emitted(n, false);
    // Stable: walk in original order picking any ready (indeg 0) node.
    bool progressed = true;
    while (order.size() < n && progressed) {
        progressed = false;
        for (size_t i = 0; i < n; ++i) {
            if (emitted[i] || indeg[i] != 0) continue;
            emitted[i] = true;
            order.push_back(i);
            progressed = true;
            for (size_t d : dependents[i]) --indeg[d];
        }
    }
    // Cycle remainder → append in original order.
    for (size_t i = 0; i < n; ++i)
        if (!emitted[i]) order.push_back(i);
    return order;
}

// Replays a plan's postamble on the target connection — a SESSION RESTORE, not
// a "finish" step. The MySQL pair is `SET FOREIGN_KEY_CHECKS=0` in the preamble
// and `=1` here, and a session left with FK enforcement off is exactly as broken
// whether the run succeeded, failed halfway, or was cancelled. So this is a
// scope guard: the destructor runs it on every exit path including an exception.
//
// It is deliberately NOT transactional state. `SET FOREIGN_KEY_CHECKS` is
// session state; a ROLLBACK does not restore it, which is why the restore must
// happen after — and outside — the transaction, on the SAME connection that ran
// the preamble.
//
// Run() is idempotent so the normal path can invoke it at a deterministic point
// (after the data phase, transaction already closed) and still inspect whether
// it worked, while the destructor remains the backstop for early returns.
class PostambleGuard {
public:
    PostambleGuard(IConnection& tgt, const std::vector<wxString>& stmts)
        : tgt_(tgt), stmts_(stmts) {}
    PostambleGuard(const PostambleGuard&)            = delete;
    PostambleGuard& operator=(const PostambleGuard&) = delete;

    ~PostambleGuard() { Run(); }

    void Run()
    {
        if (ran_) return;
        ran_ = true;
        for (const wxString& s : stmts_) {
            QueryResult r; wxString e;
            // Every statement is attempted even if an earlier one failed: they
            // are independent restores, and skipping the rest because #1 failed
            // would leave MORE session state dirty, not less. First error wins
            // for reporting.
            if (!tgt_.Execute(s, r, e) && err_.IsEmpty())
                err_ = e.IsEmpty() ? (L"语句失败: " + s) : e;
        }
    }

    const wxString& Error() const { return err_; }

private:
    IConnection&                 tgt_;
    const std::vector<wxString>& stmts_;
    bool                         ran_ = false;
    wxString                     err_;
};

} // namespace

SyncEngine::SyncEngine(IConnection& src, IConnection& tgt,
                       wxString srcDb, wxString tgtDb)
    : src_(src), tgt_(tgt), srcDb_(std::move(srcDb)), tgtDb_(std::move(tgtDb))
{}

// ---- WHICH DATABASE THE STATEMENTS LAND IN ---------------------------------
//
// THE P0 THIS EXISTS FOR: a MySQL->PostgreSQL structure sync into a genuinely
// EMPTY target database failed on its very first statement with
// `relation "OpenIddictApplications" already exists`. The compare was right and
// the DDL was right; they were simply aimed at two different databases.
//
// PostgreSQL cannot name a database in an identifier — QualifiedTableSql()
// (db/DbDriver.h) renders `"schema"."tbl"` for PG and DROPS `db`, because the
// only way to address a database there is the SESSION's binding. So on a PG
// target, WHICH DATABASE IS WRITTEN is a property of the connection, not of the
// SQL. MySQL DDL has the same shape for a different reason: RenderCreateTable
// emits a bare `CREATE TABLE `tbl`` (MySqlSql.cpp) with no db qualifier, so it
// too lands in whatever the session's current database is. (There it fails
// SILENTLY rather than loudly — that CREATE carries IF NOT EXISTS.)
//
// BuildPlan is immune by construction: every call it makes passes the database
// (ListTables(tgtDb_), GetTableSchema(tgtDb_, ...)), and PgDriver::ensureDb
// reconnects on each. Execute is NOT — it only ever calls tgt_.Execute(sql),
// which binds nothing.
//
// That gap was invisible while both phases shared one connection (the compare
// left it bound as a side effect). It became reachable the moment the UI moved
// each phase onto its own db::CloneConnection: a clone is dialed from
// EffectiveProfile(), whose `.database` is the CONNECT-TIME database and is
// never updated by ensureDb. So the execute clone opens on the profile's
// database — not the one the user picked in the wizard — and the plan's CREATEs
// are replayed into the wrong, already-populated database.
//
// Binding here rather than in each caller puts the invariant where the SQL is
// issued. ui::MainFrame_Automation already did this by hand before constructing
// the engine (and may keep doing so — UseDatabase is idempotent: a no-op when
// already bound, and a no-op for an empty name).
//
// ORDER MATTERS, and only in one case: when src_ and tgt_ are the SAME
// connection (automation, one server, two databases). Target LAST, because the
// source reads are db-qualified by the engine (MySQL) while the target WRITES
// are not — so the session must be left pointing at the target.
bool SyncEngine::BindSessions(wxString& err)
{
    if (!src_.UseDatabase(srcDb_, err)) {
        err = L"无法将源连接切换到数据库 " + srcDb_ + L"：" + err;
        return false;
    }
    if (!tgt_.UseDatabase(tgtDb_, err)) {
        err = L"无法将目标连接切换到数据库 " + tgtDb_ + L"：" + err;
        return false;
    }
    return true;
}

// ---- A4: BuildPlan's structure half, usable standalone ----------------------

SchemaDelta SyncEngine::Compare(const TableSchema& srcSchema, const TableSchema& tgtSchema) const
{
    SchemaDiffResult d = DiffSchema(srcSchema, tgtSchema, src_.GetDialect(), tgt_.GetDialect());

    SchemaDelta delta;
    delta.table        = d.changes.table;
    delta.createTable  = d.changes.createTable;
    delta.createSchema = d.changes.createSchema;
    delta.dropTable    = d.changes.dropTable;
    delta.indexes      = d.changes.indexes;
    delta.foreignKeys  = d.changes.foreignKeys;
    delta.findings     = std::move(d.warnings);
    // Re-thread every surviving ColumnChange through SchemaDelta's verdict gate
    // (AddColumnChange) rather than assigning `columns_` directly. DiffSchema
    // already only constructs a ColumnChange after MayAutoAlter(verdict) passed
    // (or for Add/Drop/same-dialect-Modify, which carry no cross-engine
    // ambiguity at all — Equivalent is the correct label for those too), so
    // every push here is guaranteed to succeed; the point is that SchemaDelta's
    // "a Lossy/Unmappable column cannot become a ColumnChange" guarantee is
    // enforced a SECOND, independent time at this layer, not merely assumed
    // from DiffSchema's own internal discipline.
    for (auto& cc : d.changes.columns)
        delta.AddColumnChange(TypeVerdict::Equivalent, std::move(cc));
    return delta;
}

bool SyncEngine::Plan(const SchemaDelta& delta, std::vector<wxString>& ddl, wxString& err)
{
    ddl.clear();
    if (delta.Empty()) return true;
    return tgt_.RenderSchemaChange(delta.ToChangeSet(), ddl, err);
}

void SyncEngine::PlanTableData(const TableSchema& srcSchema,
                               const TableSchema& tgtSchema, bool tgtHas,
                               bool tableWillExist, SyncPlan::TableUnit& unit,
                               std::vector<wxString>& warnings, wxString& err,
                               const std::atomic<bool>& stop)
{
    const QualifiedName& table = unit.table;
    // Display() (not Key()) for human-facing text: 'archive.orders' reads as a
    // table name, while Key() is an escaped identity string meant for maps.
    const wxString tableText = table.Display();

    // The replayable spec. Retained on the unit so Execute() can re-derive the
    // diff without re-introspecting either side (see DataSyncExec.h's two-pass
    // note). Two TableSchema copies per table — small, and they are exactly
    // what pass 2 would otherwise have to fetch again.
    unit.dataSpec.srcDb         = srcDb_;
    unit.dataSpec.tgtDb         = tgtDb_;
    unit.dataSpec.srcSchema     = srcSchema;
    unit.dataSpec.tgtSchema     = tgtHas ? tgtSchema : srcSchema;
    unit.dataSpec.targetMissing = !tgtHas;

    auto refuse = [&](DataVerdict v, const wxString& why) {
        unit.dataVerdict       = v;
        unit.dataVerdictReason = why;
        warnings.push_back(why);
    };

    if (!tgtHas && !tableWillExist) {
        refuse(DataVerdict::TargetMissing,
               L"表 " + tableText + L" 目标不存在且未同步结构，数据无法写入，已跳过");
        return;
    }
    if (tgtHas && srcSchema.primaryKey.empty()) {
        // The verdict the UI's 状态 column needs. Previously this existed only
        // as this same sentence inside warnings, which no consumer could
        // legitimately parse back into a decision.
        refuse(DataVerdict::NoPrimaryKey,
               L"表 " + tableText + L" 无主键，已排除数据同步");
        return;
    }
    if (tgtHas) {
        // Ordering gate, run HERE rather than deep inside the diff, so a single
        // un-orderable table becomes a per-table verdict instead of aborting
        // the entire compare (which is what it used to do).
        std::vector<Finding>  findings;
        std::vector<wxString> binCols;
        if (!CheckDataDiffOrdering(srcSchema, src_.GetDialect(), tgt_.GetDialect(),
                                   findings, binCols)) {
            refuse(DataVerdict::UnorderableKey,
                   findings.empty()
                       ? (L"表 " + tableText + L" 的主键排序无法在两端保证一致，已排除数据比对")
                       : FormatFinding(findings.front()));
            return;
        }
    }

    // Every category ON: the compare must report the TRUE counts. Suppressing
    // deletes here — the old default — showed a delete count of zero for tables
    // that had them, so the user could not even discover there was something to
    // approve. What the user approves is applied in pass 2 (DataSyncExec).
    DataSyncOptions dopt;
    dopt.insert = dopt.update = dopt.deleteMissing = true;

    if (!DiffRowChanges(src_, tgt_, unit.dataSpec, dopt, unit.layout, unit.rows,
                        err, stop))
        return;                       // genuine I/O failure — err is set

    unit.stat.inserts = unit.rows.Stat().inserts;
    unit.stat.updates = unit.rows.Stat().updates;
    unit.stat.deletes = unit.rows.Stat().deletes;

    if (!unit.rows.Executable()) {
        unit.dataVerdict = DataVerdict::Blocked;
        for (const Finding& f : unit.rows.Findings())
            if (!MayAutoAlter(f.verdict)) { unit.dataVerdictReason = FormatFinding(f); break; }
        if (unit.dataVerdictReason.IsEmpty())
            unit.dataVerdictReason = L"表 " + tableText + L" 含无法安全转换的取值，已排除数据同步";
        warnings.push_back(unit.dataVerdictReason);
        return;
    }

    unit.dataVerdict = DataVerdict::Ok;

    // Preview text, rendered over the CAPPED sample only. Bounded by
    // construction: RowChangeSet never holds more than kSampleCap rows, so this
    // loop cannot be the memory bug it replaced.
    if (!unit.layout.Empty()) {
        RowRenderSpec rs;
        // The TARGET's name, not the unit's (source) identity -- the preview
        // shows what will be written, and TargetTable() is the same expression
        // the executor writes through.
        rs.qualifiedTable = QualifiedTarget(unit.dataSpec.TargetTable());
        rs.columns        = unit.layout.columns;
        rs.pkColumns      = unit.layout.pkColumns;
        rs.dialect        = tgt_.GetDialect();
        for (const RowChange& r : unit.rows.Sample()) {
            wxString s = RenderRowDml(r, rs);
            if (!s.IsEmpty()) unit.dml.push_back(std::move(s));
        }
        unit.dmlTruncated = unit.rows.Truncated();
    }
}

bool SyncEngine::BuildPlan(const SyncScope& scope, SyncPlan& out, wxString& err,
                           const std::atomic<bool>& stop,
                           const BuildProgress& progress)
{
    out = SyncPlan{};

    // Not load-bearing for the catalog reads below (they all pass the database
    // explicitly), but it makes the compare's session state EXPLICIT instead of
    // a side effect of whichever call happened to run last — and the data
    // compare that follows reads through session-bound SQL on PostgreSQL.
    if (!BindSessions(err)) return false;

    const bool sameDialect = src_.GetDialect() == tgt_.GetDialect();
    // Cross-engine structure sync: column-level Add/Modify goes through
    // Compare()/Plan() the same as same-dialect, verdict-gated per column
    // (SyncTypeMap/DialectProfile, A1/A3). Cross-engine whole-table CREATE is
    // now supported too (A5): DiffSchema translates every column's type through
    // the same per-column Interpret(src)->Render(tgt) path and hands a target-
    // native schema to the target driver's RenderCreateTable — refusing the
    // whole table (a Finding) only when a column's type is Unmappable, and
    // omitting FKs with a warning. So there is no longer a blanket "整表自动建表暂不
    // 支持" caveat to emit up front; per-table Findings carry the specifics.

    // ---- table universe: union of src + tgt, filtered by scope.tables --------
    // Indeterminate progress (total==0) around each ListTables call: on a large
    // schema, or a connection routed through an SSH tunnel, this step alone can
    // take long enough to read as a UI hang without it — the per-table loop
    // below doesn't start emitting until both calls return.
    std::vector<TableInfo> st, tt;
    if (progress) progress(L"读取源表清单", 0, 0);
    if (!src_.ListTables(srcDb_, st, err)) return false;
    if (progress) progress(L"读取目标表清单", 0, 0);
    if (!tgt_.ListTables(tgtDb_, tt, err)) return false;

    // CROSS-ENGINE TABLE IDENTITY. MySQL/SQLite produce BARE names
    // ("OpenIddictApplications"); PostgreSQL produces SCHEMA-QUALIFIED ones
    // ("public.OpenIddictApplications"). When the two ENDPOINTS are different
    // engines the only identity component BOTH can express is the bare table
    // name, so a MySQL `widgets` and a PG `public.widgets` are the SAME table.
    // Keyed on the full QualifiedName, they were NOT: existence detection
    // reported a table the auto-create had JUST created on PG as still missing
    // (bare src key != public-qualified tgt key) and re-emitted its CREATE,
    // which on a live server fails `relation "..." already exists`. That is the
    // P0 the fresh cross-engine auto-create surfaced — before it, a whole-table
    // cross-engine CREATE was refused, so this collision could never be emitted.
    //
    // keyOf() is the MEMBERSHIP key: cross-engine it is the bare name so the two
    // sides line up; SAME-dialect it is the FULL identity, preserving the
    // PostgreSQL public.orders vs archive.orders distinction (QualifiedName.h)
    // — cross-engine MySQL has no second schema to conflate with.
    auto keyOf = [sameDialect](const QualifiedName& n) -> wxString {
        return sameDialect ? n.Key() : n.Name();
    };

    // Each side's REAL spelling of a table, looked up by the shared key. This is
    // the second half of the fix and the half the membership key alone cannot
    // supply: the TARGET must be introspected with ITS OWN schema-qualified,
    // exact-case identity (`public."OpenIddictApplications"`). Reconstructing a
    // bare name and re-resolving it is lossy — PG's to_regclass CASE-FOLDS an
    // unquoted identifier, so a bare camelCase name resolves to NULL, the target
    // schema reads back empty, and DiffSchema re-emits createTable even though
    // membership matched. Carrying the ListTables identity through avoids that
    // round-trip entirely.
    std::map<wxString, QualifiedName> srcByKey, tgtByKey;
    for (const auto& t : st) srcByKey.emplace(keyOf(t.Qualified()), t.Qualified());
    for (const auto& t : tt) tgtByKey.emplace(keyOf(t.Qualified()), t.Qualified());

    std::set<wxString> want;
    for (const auto& t : scope.tables) want.insert(keyOf(t));

    // Scope matching accepts an UNQUALIFIED request as "any schema", so a caller
    // that names `orders` without a schema still selects what it always did,
    // while a caller that names `archive.orders` gets exactly that one. An empty
    // scope means every table, as before.
    auto included = [&](const wxString& key, const QualifiedName& n) {
        if (want.empty()) return true;
        if (want.count(key) > 0) return true;
        return want.count(n.Name()) > 0;
    };

    std::vector<wxString> tableKeys;            // src order first, then tgt-only
    std::set<wxString> seen;
    for (const auto& t : st) {
        const wxString k = keyOf(t.Qualified());
        if (included(k, t.Qualified()) && seen.insert(k).second) tableKeys.push_back(k);
    }
    for (const auto& t : tt) {
        const wxString k = keyOf(t.Qualified());
        if (included(k, t.Qualified()) && seen.insert(k).second) tableKeys.push_back(k);
    }

    const int total = static_cast<int>(tableKeys.size());
    // Topo sort works over IDENTITY STRINGS (QualifiedName::Key()), matching
    // what NormForeignKey::refTable now carries for PostgreSQL -- a FK pointing
    // at another schema's table must order against THAT table, not against a
    // same-named one in this schema.
    std::map<wxString, std::set<wxString>> refs;
    std::vector<SyncPlan::TableUnit> units;

    for (int i = 0; i < total; ++i) {
        if (stop.load()) { err = L"已取消"; return false; }
        if (progress) progress(L"比对", i, total);

        auto sIt = srcByKey.find(tableKeys[i]);
        auto tIt = tgtByKey.find(tableKeys[i]);
        const bool srcHas = sIt != srcByKey.end();
        const bool tgtHas = tIt != tgtByKey.end();

        // The unit identity is the SOURCE's own spelling (it drives createSchema
        // and the data source); a target-only table (a DROP candidate) falls back
        // to the target's. Each side is introspected with ITS OWN identity so PG
        // reads its schema-qualified, exact-case name directly instead of
        // re-resolving a bare name through case-folding to_regclass.
        const QualifiedName table = srcHas ? sIt->second : tIt->second;

        TableSchema srcSchema, tgtSchema;
        if (srcHas && !src_.GetTableSchema(srcDb_, sIt->second, srcSchema, err)) return false;
        if (tgtHas && !tgt_.GetTableSchema(tgtDb_, tIt->second, tgtSchema, err)) return false;

        // refTable and the unit keys are all SOURCE-side spellings here, so the
        // topo sort lines up without normalization: a MySQL FK names a bare
        // parent whose unit key is also bare; a PG FK names public.parent whose
        // unit key is also public.parent.
        for (const auto& fk : srcSchema.foreignKeys)
            if (!fk.refTable.IsEmpty()) refs[table.Key()].insert(fk.refTable);

        SyncPlan::TableUnit unit;
        unit.table = table;

        // ---- structure (A4/A5: Compare() + Plan(), same call for either
        // dialect pairing — DiffSchema decides what's actionable per column via
        // the verdict gate, and now also translates a cross-engine whole-table
        // CREATE when every column maps; see the note above this loop) ----
        bool structureRan = false;
        SchemaDelta delta;   // stays default/Empty() when structure isn't run
        if (scope.structure) {
            // Source-missing table = DROP TABLE candidate; only plan it when the
            // caller opted in (safety red line).
            if (!srcHas && !scope.dropMissingTables) {
                out.warnings.push_back(L"表 " + table.Display() +
                    L" 目标有·源无，未勾选删除，已跳过");
            } else {
                delta = Compare(srcSchema, tgtSchema);
                structureRan = true;
                for (const auto& f : delta.findings) out.warnings.push_back(FormatFinding(f));
                if (!delta.Empty()) {
                    std::vector<wxString> ddl;
                    if (!Plan(delta, ddl, err)) return false;
                    unit.ddl     = std::move(ddl);
                    unit.changes = delta.ToChangeSet();
                }
            }
        }

        // Will the target actually have this table once the structure phase
        // runs? createTable makes one — for same-dialect always, and for
        // cross-dialect whenever DiffSchema's translation succeeded (A5). A
        // cross-engine table refused for an Unmappable column sets no
        // createTable, so tableWillExist stays false and its data is skipped.
        const bool tableWillExist = tgtHas || (structureRan && delta.createTable);

        // ---- data (T11: structured RowChangeSet, not a statement list) ----
        if (scope.data && srcHas)
            PlanTableData(srcSchema, tgtSchema, tgtHas, tableWillExist, unit,
                          out.warnings, err, stop);
        if (!err.IsEmpty()) return false;

        // A table with no changes is still worth carrying when its data was
        // REFUSED: "无主键（已排除）" is a row the compare screen must show, and
        // a unit that never reaches the plan cannot be shown at all. That was
        // the reason the PK verdict was previously only recoverable by parsing
        // warning text.
        const bool dataNote = unit.dataVerdict != DataVerdict::NotRequested &&
                              unit.dataVerdict != DataVerdict::Ok;
        if (!unit.ddl.empty() || unit.HasData() || dataNote)
            units.push_back(std::move(unit));
    }

    // ---- FK topo order (referenced tables first) -----------------------------
    std::vector<wxString> unitTables;
    for (const auto& u : units) unitTables.push_back(u.table.Key());
    const std::vector<size_t> order = TopoOrder(unitTables, refs);
    for (size_t idx : order) out.units.push_back(std::move(units[idx]));

    // ---- MySQL target: relax FK checks around the whole run ------------------
    if (tgt_.GetDialect() == Dialect::MySQL) {
        out.preamble.push_back(L"SET FOREIGN_KEY_CHECKS=0");
        out.postamble.push_back(L"SET FOREIGN_KEY_CHECKS=1");
    }

    if (progress) progress(L"比对", total, total);
    return true;
}

wxString SyncEngine::QualifiedTarget(const QualifiedName& table) const
{
    return QualifiedTableSql(tgtDb_, table, tgt_.GetDialect());
}

bool SyncEngine::Execute(const SyncPlan& plan, const DataExecPlan& data,
                         bool useTransaction, DataExecResult& dataResult,
                         wxString& err, const std::atomic<bool>& stop,
                         const RunProgress& progress)
{
    dataResult = DataExecResult{};

    // BEFORE THE GUARD, AND BEFORE ANY STATEMENT. Two reasons this cannot move
    // down: (1) on PostgreSQL UseDatabase is a RECONNECT, which would discard
    // the preamble's session state (and an open transaction) if it ran later;
    // (2) failing here must leave the target untouched, and the postamble has
    // nothing to restore because nothing was relaxed yet. See BindSessions().
    if (!BindSessions(err)) return false;

    // THE RESTORE GUARD. Constructed before any statement runs and destroyed
    // after ExecuteRun() has returned by ANY path — success, per-table failure,
    // whole-run failure, cancellation, or an exception escaping the run. This is
    // why the run body is a separate function: every `return` in it is inside
    // this guard's scope, so the postamble cannot be returned past. (It was a
    // trailing statement list nothing executed; a trailing statement is exactly
    // how it got missed, and re-adding one would invite the same regression.)
    PostambleGuard restore(tgt_, plan.postamble);

    const bool ok = ExecuteRun(plan, data, useTransaction, dataResult, err, stop,
                               progress);

    // Deterministic restore point: after the data phase (the inserts are what
    // need the relaxation most, so the postamble must NOT run between DDL and
    // data), and after the transaction is already committed or rolled back.
    // Calling it here rather than leaving it to ~PostambleGuard is what lets a
    // restore failure be *reported*; the destructor still covers the throw path.
    restore.Run();

    if (!restore.Error().IsEmpty()) {
        const wxString note = L"目标会话状态未能恢复（" + restore.Error() +
            L"）。该连接的会话设置（如 MySQL FOREIGN_KEY_CHECKS）可能仍处于放宽状态，"
            L"建议断开并重新连接后再执行其他语句";
        if (ok) {
            // The run itself succeeded but the session is left dirty. That is a
            // reportable failure in its own right — silently returning success
            // is what would let an unconstrained connection go unnoticed.
            err = note;
            return false;
        }
        // A restore failure must never MASK the real error: the original error
        // is what the operator has to act on, so it stays first and intact.
        err += L"；另外：" + note;
    }
    return ok;
}

bool SyncEngine::ExecuteRun(const SyncPlan& plan, const DataExecPlan& data,
                            bool useTransaction, DataExecResult& dataResult,
                            wxString& err, const std::atomic<bool>& stop,
                            const RunProgress& progress)
{
    const Dialect dialect = tgt_.GetDialect();

    // ---- phase 1: preamble + every table's DDL, in FK topological order -----
    // All DDL before any data, so a table referenced by an INSERT already
    // exists. Pointers into `plan` rather than copies: the strings outlive this
    // call and copying them doubled peak memory on large plans.
    std::vector<const wxString*> stmts;
    for (const auto& s : plan.preamble) stmts.push_back(&s);
    for (const auto& u : plan.units)
        for (const auto& s : u.ddl) stmts.push_back(&s);

    const int total = static_cast<int>(stmts.size());

    if (total > 0 && useTransaction) {
        const wxString begin = BeginSql(dialect);
        if (!begin.IsEmpty()) {
            QueryResult r; wxString e;
            tgt_.Execute(begin, r, e);   // best effort; DDL may implicit-commit
        }
    }

    for (int i = 0; i < total; ++i) {
        if (stop.load()) {
            if (useTransaction) { QueryResult r; wxString e; tgt_.Execute(L"ROLLBACK", r, e); }
            err = L"已取消";
            return false;
        }
        QueryResult r; wxString e;
        const bool ok = tgt_.Execute(*stmts[i], r, e);
        if (progress) progress(i + 1, total, *stmts[i]);
        if (!ok) {
            if (useTransaction) { QueryResult rr; wxString ee; tgt_.Execute(L"ROLLBACK", rr, ee); }
            err = e;
            if (useTransaction && dialect == Dialect::MySQL)
                err += L"（注意：MySQL DDL 已提交部分无法回滚，建议重新比对）";
            return false;
        }
    }

    if (total > 0 && useTransaction) {
        QueryResult r; wxString e;
        if (!tgt_.Execute(L"COMMIT", r, e)) {
            err = e;
            // THE ROLLBACK THIS PATH USED TO SKIP.
            //
            // The two failure exits above (cancellation, rejected statement)
            // both ROLLBACK before returning; this one did not, and returned
            // with the transaction — as far as this code could tell — still
            // OPEN. That matters because of WHOSE connection it is: the
            // interactive path (SyncRunnerDialog) uses a clone that dies with
            // the worker, but the AUTOMATION path (ui::MainFrame_Automation)
            // uses the ConnectionTree's own long-lived connection, the same one
            // the user later opens a SQL editor on, so anything left open
            // persists for the life of the application.
            //
            // WHAT THE ENGINES ACTUALLY DO — measured, not assumed
            // (mysql_pg_live_mysqltx_test M5/M6), because the whole shape of
            // this handling depends on it:
            //
            //   PostgreSQL — a failed COMMIT has ALREADY ended the transaction.
            //     The backend goes straight to `idle`, and this ROLLBACK is a
            //     no-op that SUCCEEDS with a server-side `WARNING: there is no
            //     transaction in progress`. Nothing is appended.
            //   MySQL — the only reachable COMMIT failure is the connection
            //     dying, which ends the session and makes InnoDB discard the
            //     transaction. The ROLLBACK cannot even be sent (the driver has
            //     already closed the dead handle).
            //
            // So on BOTH engines covered by tests the missing ROLLBACK left
            // nothing open — M6 pins that, and records that it passes with or
            // without this statement. This is therefore DEFENSIVE CONSISTENCY,
            // not a fix for an observed leak: it exists for the dialects this
            // suite cannot reach (SQL Server, Oracle, DM), where a failed
            // COMMIT's effect on transaction state is not established, and so
            // that all three failure exits close what they opened.
            QueryResult rr; wxString re;
            if (!tgt_.Execute(L"ROLLBACK", rr, re) && tgt_.IsConnected()) {
                // APPENDED, never substituted — the same rule PostambleGuard's
                // note follows below. The COMMIT error is what the operator has
                // to act on; demoting it to make room for a secondary failure is
                // how the actionable half gets lost.
                //
                // Gated on IsConnected() because the claim this note makes must
                // be TRUE. On a live connection a refused ROLLBACK really does
                // leave the disposition unknown. On a DEAD one it does not: the
                // session is gone, the server has already rolled the transaction
                // back (verified in M5a — the killed session's rows were
                // discarded), and the COMMIT error above already says the
                // connection was lost. Emitting "状态未知" there would be
                // verbose AND false, which is worse than saying nothing.
                err += L"；回滚也失败（" +
                       (re.IsEmpty() ? wxString(L"无错误信息") : re) +
                       L"），目标连接上的事务处置状态未知，建议断开并重新连接";
            }
            // Same honesty note the statement-failure path carries, for the same
            // reason: on MySQL every DDL statement in this phase committed
            // implicitly as it ran, so a failed COMMIT does not undo them. The
            // user must not be left believing the target is untouched.
            if (dialect == Dialect::MySQL)
                err += L"（注意：MySQL DDL 已提交部分无法回滚，建议重新比对）";
            return false;
        }
    }

    // ---- phase 2: data, streamed (never from plan.units[].dml) --------------
    // The jobs keep the plan's FK topological order; DataSyncExec walks it
    // forward for INSERT/UPDATE and backward for DELETE.
    std::vector<TableExecJob> jobs;
    for (const auto& u : plan.units) {
        if (u.dataVerdict != DataVerdict::Ok) continue;   // refused verdicts never run
        const TableDataSpec* allow = data.Find(u.table);
        if (!allow || !allow->AnyWrite()) continue;       // nothing approved
        TableExecJob job{u.dataSpec, *allow};
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) return true;

    DataSyncOptions base;   // category flags are set per sweep by the executor
    return ExecuteDataSync(src_, tgt_, jobs, base, dataResult, err, stop,
        [&](const wxString& table, const wxString& phase, long long applied) {
            if (progress)
                progress(total, total,
                         table + L"：" + phase + wxString::Format(L" %lld 行", applied));
        });
}

bool SyncEngine::Execute(const SyncPlan& plan, bool useTransaction, wxString& err,
                         const std::atomic<bool>& stop, const RunProgress& progress)
{
    // Fail closed. See the header: this signature cannot express which data
    // categories the user approved, so running data through it would mean either
    // guessing (and deleting rows nobody checked) or executing the 500-row
    // preview as if it were the whole diff.
    for (const auto& u : plan.units) {
        if (u.HasData()) {
            err = L"该同步计划包含数据变更，必须使用带 DataExecPlan 的 Execute 重载"
                  L"（按表按类别授权），否则无法确定用户勾选了哪些类别。表：" + u.table.Display();
            return false;
        }
    }
    DataExecResult ignored;
    return Execute(plan, DataExecPlan{}, useTransaction, ignored, err, stop, progress);
}

} // namespace db::sync
