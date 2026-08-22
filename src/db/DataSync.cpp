// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DataSync.cpp — see header. Sorted-merge data diff.
//
// StreamRows is a *push* API (it calls a sink per row). To lock-step merge two
// push streams without buffering either table, each side runs on its own thread
// feeding a bounded queue; the merge thread pulls one row at a time from both.
// This is uniform for single- and composite-PK tables (it sidesteps StreamRows'
// single-column keyset-paging limit — see the note in MySqlDriver::StreamRows),
// keeps client memory flat, and stays cancellable via the stop flag.
#include "db/DataSync.h"
#include "core/CrashLog.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace db::sync {

namespace {

// ---- SQL builders (target dialect) ----------------------------------------

// Table-name rendering moved to DbDriver.h's QualifiedTableSql() so this file
// and DataSyncExec.cpp cannot spell the target table two different ways -- see
// the note there.

wxString BuildInsert(const wxString& db, const TableSchema& s,
                     const std::vector<Cell>& row, Dialect d)
{
    wxString cols, vals;
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (i) { cols += L", "; vals += L", "; }
        cols += QuoteIdent(s.columns[i].name, d);
        vals += RenderLiteral(i < row.size() ? row[i] : Cell{}, d);
    }
    return L"INSERT INTO " + QualifiedTableSql(db, s.name, d) + L" (" + cols +
           L") VALUES (" + vals + L")";
}

wxString BuildWhereByPk(const TableSchema& s, const std::vector<Cell>& row,
                        const std::vector<size_t>& pkIdx, Dialect d)
{
    wxString w;
    for (size_t k = 0; k < pkIdx.size(); ++k) {
        const size_t i = pkIdx[k];
        if (k) w += L" AND ";
        w += QuoteIdent(s.columns[i].name, d) + L" = " +
             RenderLiteral(i < row.size() ? row[i] : Cell{}, d);
    }
    return w;
}

wxString BuildUpdate(const wxString& db, const TableSchema& s,
                     const std::vector<Cell>& srow,
                     const std::vector<size_t>& pkIdx, Dialect d)
{
    wxString set;
    bool first = true;
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (std::find(pkIdx.begin(), pkIdx.end(), i) != pkIdx.end()) continue;
        if (!first) set += L", ";
        first = false;
        set += QuoteIdent(s.columns[i].name, d) + L" = " +
               RenderLiteral(i < srow.size() ? srow[i] : Cell{}, d);
    }
    return L"UPDATE " + QualifiedTableSql(db, s.name, d) + L" SET " + set +
           L" WHERE " + BuildWhereByPk(s, srow, pkIdx, d);
}

wxString BuildDelete(const wxString& db, const TableSchema& s,
                     const std::vector<Cell>& trow,
                     const std::vector<size_t>& pkIdx, Dialect d)
{
    return L"DELETE FROM " + QualifiedTableSql(db, s.name, d) + L" WHERE " +
           BuildWhereByPk(s, trow, pkIdx, d);
}

// ---- cell comparison -------------------------------------------------------
//
// CmpCodePoints / CmpCell / the CompareOp canonicalization all live in
// SyncCellCompare.cpp. The merge below never compares two cells directly: it
// goes through the per-column CompareRules the layout carries, so a cross-engine
// coercion (MySQL tinyint(1) 1 vs PostgreSQL boolean 't') compares EQUAL instead
// of reporting a phantom UPDATE — and, in a key column, matches as the same row
// instead of degenerating into DELETE + INSERT. `rules == nullptr` (and
// rules->allRaw) select the historical raw path exactly; see SyncCellCompare.h
// for why canonicalizing BOTH sides is the only variant that also keeps the
// sorted merge's ordering assumption intact.

// The op for column `i`, or Raw when no rules were supplied (same-engine, and
// the legacy single-schema DiffAndEmitData path, which cannot coerce anything
// because it streams both sides through the SAME schema).
CompareOp OpAt(const CompareRules* rules, size_t i)
{
    return rules ? rules->At(i) : CompareOp::Raw;
}

int CmpPk(const std::vector<Cell>& a, const std::vector<Cell>& b,
          const std::vector<size_t>& pkIdx, const CompareRules* rules)
{
    for (size_t i : pkIdx) {
        const Cell& ca = i < a.size() ? a[i] : Cell{};
        const Cell& cb = i < b.size() ? b[i] : Cell{};
        const int c = CompareCells(ca, cb, OpAt(rules, i));
        if (c) return c;
    }
    return 0;
}

// Compare two already-extracted key tuples (compact vectors, PK order) — the
// backwards check, which compares ONE side against ITSELF. It uses the same
// canonicalization as the cross-side compare precisely because that
// canonicalization is order-preserving on each side individually: if it were
// not, this net would stop catching a server that ignored binaryOrder.
int CmpKeys(const std::vector<Cell>& a, const std::vector<Cell>& b,
            const std::vector<size_t>& pkIdx, const CompareRules* rules)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int c = CompareCells(a[i], b[i],
                                   OpAt(rules, i < pkIdx.size() ? pkIdx[i] : i));
        if (c) return c;
    }
    return 0;
}

// Pull a row's PK cells into a compact tuple, for the backwards check. Copies
// only the key columns, not the row.
void ExtractKey(const std::vector<Cell>& row, const std::vector<size_t>& pkIdx,
                std::vector<Cell>& out)
{
    out.clear();
    out.reserve(pkIdx.size());
    for (size_t i : pkIdx) out.push_back(i < row.size() ? row[i] : Cell{});
}

// ---- defense 2: which PK column types can we guarantee an order for? -------

// Deterministic = every engine orders this type the same way, and the same way
// CmpCell does; collation is not involved. Collated = a text type, whose order
// IS collation-dependent and must be forced to byte order. Unknown = we cannot
// tell, which is treated exactly like "cannot guarantee".
enum class KeyOrder { Deterministic, Collated, Unknown };

// Fallback classification when a driver left ColKind::Other (the IConnection
// base-class GetTableSchema does this for every column, so any engine without a
// dedicated schema reader lands here). Substring matching on the engine-native
// type text, deliberately conservative: anything unrecognized is Unknown, i.e.
// refused, rather than optimistically assumed orderable.
KeyOrder ClassifyRawType(const wxString& raw)
{
    const wxString t = raw.Lower();
    // Checked first: these CONTAIN substrings the later arms would match
    // ("set(" would never, but "enum" and "json" must not fall through to a
    // text arm and be treated as merely collation-dependent — no COLLATE fixes
    // them). MySQL orders ENUM by member ordinal while everyone else orders the
    // label text, and JSON/XML/geometry have no cross-engine total order at all.
    if (t.Contains(L"enum") || t.Contains(L"set(") || t.Contains(L"json") ||
        t.Contains(L"xml"))
        return KeyOrder::Unknown;
    if (t.Contains(L"uuid") || t.Contains(L"uniqueidentifier") || t.Contains(L"guid"))
        return KeyOrder::Deterministic;
    if (t.Contains(L"binary") || t.Contains(L"blob") || t.Contains(L"bytea") ||
        t.Contains(L"raw") || t.Contains(L"image"))
        return KeyOrder::Deterministic;
    if (t.Contains(L"int") || t.Contains(L"serial") || t.Contains(L"numeric") ||
        t.Contains(L"decimal") || t.Contains(L"float") || t.Contains(L"double") ||
        t.Contains(L"real") || t.Contains(L"money") || t.Contains(L"bool") ||
        t.Contains(L"bit"))
        return KeyOrder::Deterministic;
    if (t.Contains(L"date") || t.Contains(L"time") || t.Contains(L"year"))
        return KeyOrder::Deterministic;
    // After the numeric arms so "character varying"/"nvarchar"/"clob" land here
    // but "numeric" does not.
    if (t.Contains(L"char") || t.Contains(L"text") || t.Contains(L"clob") ||
        t.Contains(L"string"))
        return KeyOrder::Collated;
    return KeyOrder::Unknown;
}

KeyOrder ClassifyKeyOrder(const NormColumn& c)
{
    switch (c.kind) {
    // Numbers, instants and byte strings: one order, everywhere, always. These
    // are ~95% of real primary keys — surrogate integer ids and UUIDs — and
    // refusing them would make this gate useless in practice.
    case ColKind::Integer:  case ColKind::Decimal: case ColKind::Float:
    case ColKind::Boolean:  case ColKind::Date:    case ColKind::Time:
    case ColKind::Timestamp:case ColKind::Binary:  case ColKind::Blob:
    case ColKind::Uuid:
        return KeyOrder::Deterministic;
    // Text: orderable, but only once both sides are forced to byte order.
    case ColKind::Char: case ColKind::Varchar: case ColKind::Text:
        return KeyOrder::Collated;
    // ENUM/JSON have no agreed cross-engine order (see ClassifyRawType).
    case ColKind::Enum: case ColKind::Json:
        return KeyOrder::Unknown;
    case ColKind::Other:
    default:
        return ClassifyRawType(c.rawType);
    }
}

// Can this engine be made to return a text column in byte order?
// MySQL: CONVERT(col USING utf8mb4) COLLATE utf8mb4_bin. PostgreSQL:
// COLLATE "C". SQLite: nothing to force — its default BINARY collation already
// IS byte order, so a text PK is safe there without driver support. SQL Server
// and Oracle have no equivalent wired up, so a text PK against them is refused
// rather than attempted.
bool CanForceByteOrder(Dialect d)
{
    return d == Dialect::MySQL || d == Dialect::Postgres || d == Dialect::Sqlite;
}

wxString DialectName(Dialect d)
{
    switch (d) {
    case Dialect::MySQL:     return L"MySQL";
    case Dialect::Postgres:  return L"PostgreSQL";
    case Dialect::Sqlite:    return L"SQLite";
    case Dialect::SqlServer: return L"SQL Server";
    case Dialect::Oracle:    return L"Oracle";
    }
    return L"该数据库";
}

// Any non-PK column differs (would need an UPDATE).
//
// `rules == nullptr` or allRaw (every same-engine sync) takes the first loop,
// which is the pre-fix comparison unchanged — so the common case pays nothing at
// all for the cross-engine machinery, not even a per-cell branch.
bool RowsDiffer(const std::vector<Cell>& a, const std::vector<Cell>& b,
                size_t ncols, const std::vector<size_t>& pkIdx,
                const CompareRules* rules)
{
    const bool raw = !rules || rules->allRaw;
    for (size_t i = 0; i < ncols; ++i) {
        if (std::find(pkIdx.begin(), pkIdx.end(), i) != pkIdx.end()) continue;
        const Cell& ca = i < a.size() ? a[i] : Cell{};
        const Cell& cb = i < b.size() ? b[i] : Cell{};
        if (raw ? (ca.kind != cb.kind || ca.text != cb.text)
                : !CellsEqual(ca, cb, rules->At(i)))
            return true;
    }
    return false;
}

// ---- push→pull bridge: one background streamer + bounded queue ------------
class RowCursor {
public:
    RowCursor(IConnection& conn, wxString db, QualifiedName table,
              std::vector<wxString> cols, std::vector<wxString> orderBy,
              std::vector<wxString> binOrderCols, size_t cap,
              const std::atomic<bool>& extStop)
        : conn_(conn), db_(std::move(db)), table_(std::move(table)),
          cols_(std::move(cols)), orderBy_(std::move(orderBy)),
          binOrderCols_(std::move(binOrderCols)),
          cap_(std::max<size_t>(cap, 1)), extStop_(extStop)
    {
        th_ = core::CrashLog::GuardedThread(L"数据同步", [this] { Run(); });
    }
    ~RowCursor()
    {
        Stop();
        if (th_.joinable()) th_.join();
    }

    // 1 = row produced, 0 = end of stream, -1 = error (Error() set),
    // -2 = cancelled (the caller's stop flag went up while we were waiting).
    int Next(std::vector<Cell>& out)
    {
        std::unique_lock<std::mutex> lk(m_);
        while (true) {
            if (!q_.empty()) {
                out = std::move(q_.front());
                q_.pop_front();
                cvProd_.notify_one();
                return 1;
            }
            if (done_)      return failed_ ? -1 : 0;
            if (Cancelled()) return -2;
            // Timed, not indefinite: the caller's stop flag is a plain atomic
            // that nobody notifies this cv on, so it has to be polled. Without
            // this, a cancel issued while the server has gone quiet is only
            // noticed when the next row finally arrives — which is the gap that
            // made the UI's join unbounded. 50 ms is the worst-case ack latency.
            cvCons_.wait_for(lk, std::chrono::milliseconds(50),
                             [this] { return !q_.empty() || done_ || stop_; });
        }
    }

    void Stop()
    {
        bool firstStop = false, running = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            firstStop = !stop_;
            running   = !done_;
            stop_     = true;
        }
        cvProd_.notify_all();
        cvCons_.notify_all();
        // The producer thread may be parked in a socket read inside StreamRows,
        // where the sink's "return false" cannot take effect until a row shows
        // up. Cancel() aborts the in-flight query driver-side (MySQL issues KILL
        // QUERY over a side connection), which is what bounds the join below —
        // otherwise the destructor waits for the server. Only on the first stop
        // of a still-running stream; if the race makes the stream finish first,
        // Cancel() lands on an idle connection and is a no-op. Default impl is a
        // no-op, so drivers that don't support it just keep the old behavior.
        if (firstStop && running) conn_.Cancel();
    }
    const wxString& Error() const { return err_; }

private:
    // Caller must hold m_.
    bool Cancelled() const { return stop_ || extStop_.load(); }

    void Run()
    {
        StreamOptions opt;
        opt.columns = cols_;
        opt.orderBy = orderBy_;
        // Defense 1: make the server sort by bytes, so both sides agree with
        // each other and with CmpCell. Always on for the merge — it costs
        // nothing for the integer keys that carry no binOrderCols_.
        opt.binaryOrder     = true;
        opt.binaryOrderCols = binOrderCols_;
        opt.limit   = -1;                 // full stream; the queue bounds memory
        wxString e;
        const bool ok = conn_.StreamRows(db_, table_, opt,
            [this](const std::vector<Cell>& row) -> bool {
                std::unique_lock<std::mutex> lk(m_);
                // Poll-wait rather than wait(): the external stop flag is an
                // atomic this cv is never notified on, and a producer blocked on
                // a full queue must still notice a cancel. Loops (never falls
                // through with a full queue) so no row is ever dropped.
                while (q_.size() >= cap_ && !Cancelled())
                    cvProd_.wait_for(lk, std::chrono::milliseconds(50),
                                     [this] { return q_.size() < cap_ || stop_; });
                if (Cancelled()) return false;   // cancellation → end StreamRows
                q_.push_back(row);
                cvCons_.notify_one();
                return true;
            }, e);
        std::lock_guard<std::mutex> lk(m_);
        if (!ok && !stop_) { failed_ = true; err_ = e; }
        done_ = true;
        cvCons_.notify_all();
    }

    IConnection&              conn_;
    wxString                  db_;
    QualifiedName             table_;
    std::vector<wxString>     cols_, orderBy_, binOrderCols_;
    size_t                    cap_;
    const std::atomic<bool>&  extStop_;   // the caller's cancel flag
    std::thread               th_;
    std::mutex                m_;
    std::condition_variable   cvProd_, cvCons_;
    std::deque<std::vector<Cell>> q_;
    bool                      done_ = false, failed_ = false, stop_ = false;
    wxString                  err_;
};

// Resolve PK column names to their positions in the schema column order.
bool PkIndices(const TableSchema& s, std::vector<size_t>& out)
{
    out.clear();
    for (const wxString& pk : s.primaryKey) {
        bool found = false;
        for (size_t i = 0; i < s.columns.size(); ++i)
            if (s.columns[i].name == pk) { out.push_back(i); found = true; break; }
        if (!found) return false;
    }
    return true;
}

// Same, over a plain name list (the layout's streamed column order).
bool PkIndicesIn(const std::vector<wxString>& cols,
                 const std::vector<wxString>& pk, std::vector<size_t>& out)
{
    out.clear();
    for (const wxString& k : pk) {
        bool found = false;
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i] == k) { out.push_back(i); found = true; break; }
        if (!found) return false;
    }
    return true;
}

// ---- the shared merge core -------------------------------------------------
//
// Extracted from DiffAndEmitData so the statement-rendering path (legacy, for
// SQL preview and the live tests) and the structured RowChange path run the
// EXACT SAME merge, with the exact same three ordering defenses. Forking them
// would mean the regression this file's header describes — a desynced merge
// inventing INSERTs and DELETEs — could be reintroduced on one path while the
// other stayed covered by DataSyncOrderingTests.

enum class MergeOp { Insert, Update, Delete };

struct MergeParams {
    wxString              srcDb, tgtDb;
    // TWO names, never one -- one shared `table` opened BOTH cursors, so the
    // target was read under the SOURCE's name. See DataDiffSpec::TargetTable().
    QualifiedName         srcTable, tgtTable;
    std::vector<wxString> cols;           // streamed on BOTH sides, in this order
    std::vector<wxString> orderBy;        // PK column names
    std::vector<wxString> binOrderCols;   // subset of orderBy needing COLLATE
    std::vector<size_t>   pkIdx;          // PK positions within `cols`
    // Per-column comparison plan, indexed like `cols`. nullptr = compare raw
    // (the legacy path, whose two streams share one schema and so cannot differ
    // in type at all).
    const CompareRules*   rules = nullptr;
};

// Return false to abort the table. For Insert only `srow` is non-null, for
// Delete only `trow`, for Update both (the source row is the desired state, the
// target row is what is there now).
using MergeEmit = std::function<bool(MergeOp, const std::vector<Cell>* srow,
                                     const std::vector<Cell>* trow)>;

bool MergeRows(IConnection& src, IConnection& tgt, const MergeParams& p,
               const DataSyncOptions& opt, const MergeEmit& emit,
               RowStat& stat, wxString& err, const std::atomic<bool>& stop)
{
    const size_t ncols = p.cols.size();
    const size_t cap   = static_cast<size_t>(std::max<long long>(64, opt.batchRows));

    RowCursor sc(src, p.srcDb, p.srcTable, p.cols, p.orderBy, p.binOrderCols, cap, stop);
    RowCursor tc(tgt, p.tgtDb, p.tgtTable, p.cols, p.orderBy, p.binOrderCols, cap, stop);

    std::vector<Cell> srow, trow;
    std::vector<Cell> sPrevKey, tPrevKey, curKey;   // for the backwards check
    long long scanned = 0;

    auto fireProgress = [&] {
        if (opt.progress) opt.progress(p.srcTable.Key(), scanned, opt.rowsEstimated);
    };

    // Advance one cursor, and enforce defense 3 on it: a key that moves
    // BACKWARDS means the server did not actually return the order the merge is
    // built on. Continuing from there produces INSERTs for rows that exist and
    // DELETEs for rows that don't — silently. So the table is aborted instead.
    // Returns the cursor status, or -3 for "ordering violated" (err set).
    // Only ever called on a cursor whose current `row` is valid (status 1), so
    // there is always a previous key to compare against.
    auto step = [&](RowCursor& cur, std::vector<Cell>& row,
                    std::vector<Cell>& prevKey, const wxChar* side) -> int
    {
        ExtractKey(row, p.pkIdx, prevKey);   // `row` still holds the key we consumed
        const int st = cur.Next(row);
        if (st != 1) return st;
        ++scanned;
        if ((scanned & 0xFFF) == 0) fireProgress();   // every 4096 rows
        ExtractKey(row, p.pkIdx, curKey);
        if (CmpKeys(curKey, prevKey, p.pkIdx, p.rules) < 0) {
            err = wxString::Format(
                L"表 %s 的%s端未按主键升序返回（已读 %lld 行后主键回退）。"
                L"排序与比对假设不一致，已中止该表的数据比对以避免产生错误的 "
                L"INSERT/DELETE。", p.srcTable.Display(), side, scanned);
            return -3;
        }
        return st;
    };

    // Sink refusal is an abort, not a skip: the two callers use it for "this row
    // could not be executed" and "cancelled", and continuing past either would
    // silently drop changes.
    auto fire = [&](MergeOp op, const std::vector<Cell>* s,
                    const std::vector<Cell>* t) -> bool {
        if (!emit) return true;
        if (emit(op, s, t)) return true;
        if (err.IsEmpty()) err = L"表 " + p.srcTable.Display() + L" 的数据同步已中止";
        return false;
    };

    int ss = sc.Next(srow);
    if (ss == 1) ++scanned;
    int ts = tc.Next(trow);
    if (ts == 1) ++scanned;

    while (!stop.load()) {
        // -3/-2 checked before the generic `< 0` arm: both are negative, but
        // they carry their own message rather than the cursor's.
        if (ss == -3 || ts == -3) return false;              // err already set
        if (ss == -2 || ts == -2) { err = L"已取消"; return false; }
        if (ss < 0) { err = sc.Error(); return false; }
        if (ts < 0) { err = tc.Error(); return false; }
        if (ss == 0 && ts == 0) break;

        int cmp;
        if (ss == 1 && ts == 1) cmp = CmpPk(srow, trow, p.pkIdx, p.rules);
        else if (ss == 1)       cmp = -1;   // only source rows remain → INSERT
        else                    cmp = 1;    // only target rows remain → DELETE

        if (cmp < 0) {
            ++stat.inserts;
            // THE CATEGORY GATE. A disabled category never reaches the sink, so
            // no statement, RowChange or bound batch row for it is ever
            // constructed anywhere downstream. See DataSyncExec.h.
            if (opt.insert && !fire(MergeOp::Insert, &srow, nullptr)) return false;
            ss = step(sc, srow, sPrevKey, L"源");
        } else if (cmp > 0) {
            ++stat.deletes;
            if (opt.deleteMissing && !fire(MergeOp::Delete, nullptr, &trow)) return false;
            ts = step(tc, trow, tPrevKey, L"目标");
        } else {
            if (RowsDiffer(srow, trow, ncols, p.pkIdx, p.rules)) {
                ++stat.updates;
                if (opt.update && !fire(MergeOp::Update, &srow, &trow)) return false;
            } else {
                ++stat.unchanged;
            }
            ss = step(sc, srow, sPrevKey, L"源");
            // Short-circuit so a source-side violation keeps its own message
            // rather than being overwritten by the target's.
            if (ss != -3) ts = step(tc, trow, tPrevKey, L"目标");
        }
    }
    if (stop.load()) { err = L"已取消"; return false; }
    fireProgress();   // final, exact count — the gauge always lands on 100%
    return true;
}

} // namespace

bool CheckDataDiffOrdering(const TableSchema& schema, Dialect srcDialect,
                           Dialect tgtDialect, std::vector<Finding>& out,
                           std::vector<wxString>& binaryOrderCols)
{
    binaryOrderCols.clear();

    auto refuse = [&](const wxString& column, const wxString& reason) {
        Finding f;
        f.table   = schema.name.Key();
        f.column  = column;
        f.verdict = TypeVerdict::Unmappable;
        f.reason  = reason;
        out.push_back(std::move(f));
    };

    if (schema.primaryKey.empty()) {
        refuse(wxString(), L"表 " + schema.name.Display() +
               L" 无主键，无法按主键有序比对数据");
        return false;
    }

    bool ok = true;
    for (const wxString& pk : schema.primaryKey) {
        const NormColumn* c = schema.FindColumn(pk);
        if (!c) {
            refuse(pk, L"主键列 " + pk + L" 不在表结构中，无法判断排序是否可保证");
            ok = false;
            continue;
        }
        switch (ClassifyKeyOrder(*c)) {
        case KeyOrder::Deterministic:
            break;                       // collation-independent — nothing to do
        case KeyOrder::Collated:
            // Text key: safe only if BOTH engines can be pinned to byte order.
            if (CanForceByteOrder(srcDialect) && CanForceByteOrder(tgtDialect)) {
                // Listed for every collatable engine, including SQLite, whose
                // driver ignores the flag because BINARY is already its default —
                // one list keeps both StreamOptions identical.
                binaryOrderCols.push_back(pk);
            } else {
                const Dialect bad = CanForceByteOrder(srcDialect) ? tgtDialect
                                                                  : srcDialect;
                refuse(pk, L"主键列 " + pk + L" 为文本类型（" + c->rawType +
                       L"），而 " + DialectName(bad) +
                       L" 端无法强制按字节序返回；两端排序规则不一致会导致比对"
                       L"静默错位并产生错误的 INSERT/DELETE，已拒绝该表的数据比对");
                ok = false;
            }
            break;
        case KeyOrder::Unknown:
            refuse(pk, L"主键列 " + pk + L" 的类型（" + c->rawType +
                   L"）无法确定两端排序是否一致，已拒绝该表的数据比对");
            ok = false;
            break;
        }
    }
    if (!ok) binaryOrderCols.clear();
    return ok;
}

bool DiffAndEmitData(IConnection& src, IConnection& tgt,
                     const wxString& srcDb, const wxString& tgtDb,
                     const TableSchema& schema, const DataSyncOptions& opt,
                     const StmtSink& emit, RowStat& stat, wxString& err,
                     const std::atomic<bool>& stop)
{
    if (schema.primaryKey.empty()) {
        err = L"表 " + schema.name.Display() + L" 无主键，无法安全比对数据";
        return false;
    }
    std::vector<size_t> pkIdx;
    if (!PkIndices(schema, pkIdx)) {
        err = L"主键列与表结构不匹配：" + schema.name.Display();
        return false;
    }

    // Defense 2, enforced here too and not only by the caller: a table whose PK
    // order cannot be guaranteed on both sides is refused outright rather than
    // diffed and silently mis-merged.
    std::vector<Finding>  findings;
    std::vector<wxString> binOrderCols;
    if (!CheckDataDiffOrdering(schema, src.GetDialect(), tgt.GetDialect(),
                               findings, binOrderCols)) {
        err = findings.empty() ? (L"表 " + schema.name.Display() + L" 的主键排序无法保证一致")
                               : findings.front().reason;
        return false;
    }

    MergeParams p;
    p.srcDb = srcDb; p.tgtDb = tgtDb;
    // One schema, two servers (legacy text path): both sides genuinely ARE the
    // same name -- said by assigning twice, not by sharing one field.
    p.srcTable = schema.name; p.tgtTable = schema.name;
    p.cols.reserve(schema.columns.size());
    for (const auto& c : schema.columns) p.cols.push_back(c.name);
    p.orderBy      = schema.primaryKey;
    p.binOrderCols = binOrderCols;
    p.pkIdx        = pkIdx;

    const Dialect td = tgt.GetDialect();

    // Statement rendering, unchanged byte-for-byte from before the T9 split —
    // this path exists for SQL preview and the live tests, and its output is
    // asserted verbatim by sync_test.
    return MergeRows(src, tgt, p, opt,
        [&](MergeOp op, const std::vector<Cell>* srow,
            const std::vector<Cell>* trow) -> bool {
            switch (op) {
            case MergeOp::Insert: emit(BuildInsert(tgtDb, schema, *srow, td)); break;
            case MergeOp::Update: emit(BuildUpdate(tgtDb, schema, *srow, pkIdx, td)); break;
            case MergeOp::Delete: emit(BuildDelete(tgtDb, schema, *trow, pkIdx, td)); break;
            }
            return true;
        }, stat, err, stop);
}

bool EmitInsertAll(IConnection& src, IConnection& tgt,
                   const wxString& srcDb, const wxString& tgtDb,
                   const TableSchema& schema, const DataSyncOptions& opt,
                   const StmtSink& emit, RowStat& stat, wxString& err,
                   const std::atomic<bool>& stop)
{
    std::vector<wxString> cols;
    cols.reserve(schema.columns.size());
    for (const auto& c : schema.columns) cols.push_back(c.name);
    const Dialect td = tgt.GetDialect();

    StreamOptions o;
    o.columns = cols;
    o.orderBy = schema.primaryKey;   // ordering optional here; nice-to-have
    o.limit   = -1;

    wxString e;
    const bool ok = src.StreamRows(srcDb, schema.name, o,
        [&](const std::vector<Cell>& row) -> bool {
            if (stop.load()) return false;
            ++stat.inserts;
            if (opt.insert) emit(BuildInsert(tgtDb, schema, row, td));
            return true;
        }, e);

    if (stop.load()) { err = L"已取消"; return false; }
    if (!ok) { err = e; return false; }
    return true;
}

// ===========================================================================
//  T9 — the structured path.
// ===========================================================================

wxString EncodeRowKey(const std::vector<wxString>& pkColumns,
                      const std::vector<Cell>& keyCells)
{
    wxString s;
    const size_t n = std::min(pkColumns.size(), keyCells.size());
    for (size_t i = 0; i < n; ++i) {
        if (i) s += wxChar(0x1F);
        s += pkColumns[i];
        s += L"=";
        // A NULL key and an empty-string key are DIFFERENT rows. Rendering both
        // as "" would collide them, and a collision here means the user's
        // per-row exclusion silently applies to the wrong row.
        s += keyCells[i].kind == CellKind::Null ? wxString(L"\\0") : keyCells[i].text;
    }
    return s;
}

namespace {

Finding LayoutFinding(const QualifiedName& table, const wxString& column,
                      const wxString& reason)
{
    Finding f;
    f.table   = table.Key();
    f.column  = column;
    f.verdict = TypeVerdict::Unmappable;
    f.reason  = reason;
    return f;
}

// An identity bridge for a value that is ALREADY target-native (a DELETE's key,
// which came off the target stream). Still routed through ConvertCell — on its
// passthrough fast path — so no code path in this file ever puts a cell into a
// RowChange without going through the gate.
ColumnBridge IdentityBridge(size_t idx, const wxString& column, Dialect tgtDialect)
{
    ColumnBridge b;
    b.srcIdx      = idx;
    b.column      = column;
    b.toDialect   = tgtDialect;
    b.passthrough = true;
    b.op          = BridgeOp::Identity;
    return b;
}

} // namespace

bool BuildRowLayout(const DataDiffSpec& spec, Dialect srcDialect,
                    Dialect tgtDialect, RowLayout& out,
                    std::vector<Finding>& findings)
{
    out = RowLayout{};
    const QualifiedName& table = spec.SourceTable();

    std::vector<ColumnBridge> bridges;
    bool ok = BuildBridges(spec.srcSchema, srcDialect, spec.tgtSchema, tgtDialect,
                           bridges, findings);

    // BuildBridges numbers srcIdx against the FULL source schema, skipping any
    // column that has no target counterpart — so the indices are sparse. Both
    // sides are streamed with exactly the bridged columns instead (which also
    // stops the target query from asking for a column the target does not
    // have), so the indices are renumbered to match that narrower row.
    out.valueBridges.reserve(bridges.size());
    out.columns.reserve(bridges.size());
    for (size_t k = 0; k < bridges.size(); ++k) {
        ColumnBridge b = bridges[k];
        b.srcIdx = k;
        out.columns.push_back(b.column);
        out.valueBridges.push_back(std::move(b));
    }

    if (out.valueBridges.empty()) {
        findings.push_back(LayoutFinding(table, wxString(),
            L"两端没有可对应的列，无法同步该表的数据"));
        return false;
    }

    // The comparison plan, derived once from the bridges — never per cell, and
    // never by re-reading a type. All-Raw (hence free) whenever the dialects
    // match, because BuildBridges made every bridge passthrough.
    BuildCompareRules(out.valueBridges, out.compare);

    // A freshly-created target takes every source row as an INSERT; no key is
    // read, compared or emitted, so a missing PK is not an obstacle there.
    if (spec.targetMissing) return ok;

    if (spec.srcSchema.primaryKey.empty()) {
        findings.push_back(LayoutFinding(table, wxString(),
            L"表 " + table.Display() + L" 无主键，已排除数据比对"));
        return false;
    }

    for (const wxString& pk : spec.srcSchema.primaryKey) {
        size_t at = out.columns.size();
        for (size_t k = 0; k < out.columns.size(); ++k)
            if (out.columns[k] == pk) { at = k; break; }
        if (at == out.columns.size()) {
            // The PK exists on the source but did not survive bridging, so
            // every UPDATE/DELETE this table would produce is missing part of
            // its WHERE clause. Refuse the table rather than emit those.
            findings.push_back(LayoutFinding(table, pk,
                L"主键列在目标端缺失或无法转换，拒绝同步该表数据（否则 UPDATE/DELETE 的 WHERE 条件不完整）"));
            return false;
        }
        // Defense 2, extended to COERCED keys. Both servers sorted by the RAW
        // key; the merge compares the CANONICAL one. That is sound only while
        // the canonicalization is order-preserving on each side — which every
        // CompareOp in use today provably is (SyncCellCompare.h carries the
        // proof per op), and which an unclassified future coercion would not be.
        // Such a key is refused with a Finding rather than merged, because a
        // merge over two sequences that are no longer sorted by its own
        // comparator invents INSERTs and DELETEs and reports success.
        if (!KeyOrderSafe(out.compare.At(at))) {
            findings.push_back(LayoutFinding(table, pk,
                L"主键列 " + pk + L" 需要跨引擎取值转换，且该转换无法保证与两端服务器的"
                L"排序一致；继续比对会使有序归并静默错位并产生错误的 INSERT/DELETE，"
                L"已拒绝该表的数据比对"));
            return false;
        }
        out.pkColumns.push_back(pk);
        out.pkIdx.push_back(at);
        out.keyBridges.push_back(out.valueBridges[at]);
        out.tgtKeyBridges.push_back(IdentityBridge(at, pk, tgtDialect));
    }
    return ok;
}

bool StreamRowChanges(IConnection& src, IConnection& tgt,
                      const DataDiffSpec& spec, const RowLayout& layout,
                      const DataSyncOptions& opt, const RowChangeSink& sink,
                      RowStat& stat, wxString& err,
                      const std::atomic<bool>& stop)
{
    if (layout.Empty()) { err = L"未构建列映射，无法同步数据"; return false; }
    const QualifiedName& table = spec.SourceTable();

    // Fill one builder from a streamed row. The ONLY place a cell enters a
    // RowChange in the structured path — and it never handles a verdict itself,
    // because AddCell/AddKeyCell compute it inside the gate.
    auto emitRow = [&](RowChange::Op op, const std::vector<Cell>* srow,
                       const std::vector<Cell>* trow) -> bool {
        const bool isDelete = (op == RowChange::Op::Delete);
        const std::vector<Cell>& keyRow = isDelete ? *trow : *srow;
        const std::vector<ColumnBridge>& kb =
            isDelete ? layout.tgtKeyBridges : layout.keyBridges;

        std::vector<Cell> rawKey;
        ExtractKey(keyRow, layout.pkIdx, rawKey);
        // Encoded ONCE, over the RAW (pre-bridge) key cells, and then handed to
        // BOTH the builder (which stores it as RowChange::RowKey()) and the
        // sink. That is what makes the encoding single-sourced: a consumer never
        // has to re-derive it from RowChange::Key(), whose cells are
        // post-conversion and only reproduce this string while the key bridges
        // are passthrough.
        const wxString rowKey = EncodeRowKey(layout.pkColumns, rawKey);

        RowChangeBuilder b(op, table.Key(), rowKey);
        // The merge holds the target row at the moment it classifies an Update,
        // so this is the only place the "before" side exists at all. Attached by
        // pointer — nothing is copied unless a consumer opts in (RowChangeSet
        // does, for sampled rows only), so the execute pass pays nothing.
        if (op == RowChange::Op::Update && trow) b.AttachPriorValues(trow);
        for (const ColumnBridge& k : kb)
            b.AddKeyCell(k.srcIdx < keyRow.size() ? keyRow[k.srcIdx] : Cell{}, k);
        if (!isDelete)
            for (const ColumnBridge& v : layout.valueBridges)
                b.AddCell(v.srcIdx < srow->size() ? (*srow)[v.srcIdx] : Cell{}, v);

        return !sink || sink(std::move(b), rowKey);
    };

    // ---- target table does not exist yet: every source row is an INSERT ----
    if (spec.targetMissing) {
        StreamOptions o;
        o.columns = layout.columns;
        o.orderBy = spec.srcSchema.primaryKey;   // nice-to-have, not required
        o.limit   = -1;
        bool aborted = false;
        wxString e;
        const bool ok = src.StreamRows(spec.srcDb, table, o,
            [&](const std::vector<Cell>& row) -> bool {
                if (stop.load()) return false;
                ++stat.inserts;
                if (!opt.insert) return true;
                if (!emitRow(RowChange::Op::Insert, &row, nullptr)) {
                    aborted = true;
                    return false;
                }
                return true;
            }, e);
        if (stop.load()) { err = L"已取消"; return false; }
        if (aborted) {
            if (err.IsEmpty()) err = L"表 " + table.Display() + L" 的数据同步已中止";
            return false;
        }
        if (!ok) { err = e; return false; }
        return true;
    }

    // ---- both sides exist: the sorted merge, with all three defenses -------
    std::vector<Finding>  findings;
    std::vector<wxString> binOrderCols;
    if (!CheckDataDiffOrdering(spec.srcSchema, src.GetDialect(), tgt.GetDialect(),
                               findings, binOrderCols)) {
        err = findings.empty() ? (L"表 " + table.Display() + L" 的主键排序无法保证一致")
                               : findings.front().reason;
        return false;
    }

    MergeParams p;
    p.srcDb        = spec.srcDb;
    p.tgtDb        = spec.tgtDb;
    // THE FIX: each side named from its own authority. TargetTable() is the
    // same expression the applier writes through -- see DataDiffSpec.
    p.srcTable     = spec.SourceTable();
    p.tgtTable     = spec.TargetTable();
    p.cols         = layout.columns;
    p.orderBy      = spec.srcSchema.primaryKey;
    p.binOrderCols = binOrderCols;
    p.pkIdx        = layout.pkIdx;
    // Borrowed, not copied: `layout` outlives this call by contract (both passes
    // hold it), and the merge only reads it.
    p.rules        = &layout.compare;

    return MergeRows(src, tgt, p, opt,
        [&](MergeOp op, const std::vector<Cell>* srow,
            const std::vector<Cell>* trow) -> bool {
            switch (op) {
            case MergeOp::Insert: return emitRow(RowChange::Op::Insert, srow, nullptr);
            case MergeOp::Update: return emitRow(RowChange::Op::Update, srow, trow);
            case MergeOp::Delete: return emitRow(RowChange::Op::Delete, nullptr, trow);
            }
            return true;
        }, stat, err, stop);
}

bool DiffRowChanges(IConnection& src, IConnection& tgt,
                    const DataDiffSpec& spec, const DataSyncOptions& opt,
                    RowLayout& layout, RowChangeSet& out, wxString& err,
                    const std::atomic<bool>& stop)
{
    out = RowChangeSet{};

    std::vector<Finding> findings;
    const bool laidOut = BuildRowLayout(spec, src.GetDialect(), tgt.GetDialect(),
                                        layout, findings);
    // Fold the column-level verdicts into the set BEFORE deciding whether to
    // scan. AddFinding latches the set non-executable for any !MayAutoAlter
    // verdict, so a table refused at the column level reaches the UI as a
    // greyed-out row with a reason attached rather than as a lost error string.
    for (Finding& f : findings) out.AddFinding(std::move(f));
    if (!laidOut) {
        err.Clear();          // not an engine failure — a per-table verdict
        return true;
    }

    RowStat raw;
    const bool ok = StreamRowChanges(src, tgt, spec, layout, opt,
        [&](RowChangeBuilder&& b, const wxString&) -> bool {
            // Accept() is the single door: it counts, caps the sample at 500,
            // absorbs the builder's Findings, and latches blocked_ when the gate
            // refused the row. Its false return is NOT an abort — a blocked row
            // is a per-row verdict, and the rest of the table must still be
            // counted so the review screen shows the true totals.
            out.Accept(std::move(b));
            return true;
        }, raw, err, stop);

    // `raw` used to die here. Its unchanged count is what distinguishes a clean
    // compare from one that read nothing at all (see RowChangeStat::unchanged).
    out.SetUnchanged(raw.unchanged);
    return ok;
}

} // namespace db::sync
