// SqliteStream.cpp — see header. Parameterized batch INSERT (ADR-015 T4) for the
// SQLite driver, i.e. the apply half that lets a SQLite file be a sync target.
#include "db/SqliteStream.h"

#include <algorithm>
#include <string>

namespace db {
namespace sqlitestream {

namespace {

// RAII: finalize the prepared statement on every exit path, including a throw
// from wxString/std::vector work in the bind loop. Mirrors SqliteStmtGuard in
// SqliteDriver.cpp (that one is file-local to the driver TU).
struct StmtCloser {
    sqlite3_stmt* st;
    explicit StmtCloser(sqlite3_stmt* s) : st(s) {}
    ~StmtCloser() { if (st) sqlite3_finalize(st); }
    StmtCloser(const StmtCloser&) = delete;
    StmtCloser& operator=(const StmtCloser&) = delete;
};

// One hex digit -> value, or -1. Cell::text for a Binary cell is prefix-less
// uppercase hex (StreamRows' convention); lowercase is accepted too so a
// hand-built Cell doesn't silently truncate. Duplicated from MySqlStream.cpp
// rather than shared: that header is driver-private and drags in mysql.h, and a
// ten-line total function is a cheaper dependency than a cross-driver one.
int HexVal(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

// Hex digits -> raw bytes. A stray non-hex character or an odd digit count can
// only come from a malformed Cell; the pair is skipped rather than guessed at,
// which keeps this total (it never throws or reads out of range).
std::string HexToBytes(const wxString& hex)
{
    std::string out;
    out.reserve(hex.length() / 2);
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        const int hi = HexVal(static_cast<wchar_t>(hex[i].GetValue()));
        const int lo = HexVal(static_cast<wchar_t>(hex[i + 1].GetValue()));
        if (hi < 0 || lo < 0) continue;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

wxString DbError(sqlite3* db)
{
    const char* e = db ? sqlite3_errmsg(db) : nullptr;
    return e && *e ? wxString::FromUTF8(e) : wxString(L"批量执行失败");
}

// Execute one chunk of `n` rows starting at `off`: one prepared statement whose
// VALUES carries n tuples of ncols anonymous `?` placeholders, bound and stepped
// once.
bool ExecChunk(sqlite3* db, const wxString& head,
               const std::vector<std::vector<Cell>>& rows,
               size_t off, size_t n, size_t ncols, wxString& err)
{
    wxString tuple = L" (";
    for (size_t c = 0; c < ncols; ++c) tuple += (c ? L", ?" : L"?");
    tuple += L")";

    // Anonymous `?` rather than numbered `?NNN`: SQLite assigns them ascending
    // 1-based indices in textual order, which is exactly the (row, column)
    // order the bind loop below walks. Numbering them would add a few hundred
    // KB of statement text per batch and buy nothing.
    wxString sql = head;
    sql.reserve(head.length() + n * (tuple.length() + 1));
    for (size_t r = 0; r < n; ++r) {
        if (r) sql += L",";
        sql += tuple;
    }

    sqlite3_stmt* st = nullptr;
    const wxScopedCharBuffer q = sql.utf8_str();
    if (sqlite3_prepare_v2(db, q.data(), static_cast<int>(q.length()),
                           &st, nullptr) != SQLITE_OK) {
        err = DbError(db);
        return false;
    }
    StmtCloser guard(st);

    // bufs owns every bound byte. Sized up front and only ever assigned
    // element-wise, so the vector never reallocates and no SQLITE_STATIC pointer
    // handed to sqlite can dangle before the sqlite3_step below. (SQLITE_STATIC
    // rather than SQLITE_TRANSIENT precisely because we can make that promise —
    // TRANSIENT would copy every value a second time.)
    std::vector<std::string> bufs(n * ncols);

    for (size_t r = 0; r < n; ++r) {
        const std::vector<Cell>& row = rows[off + r];
        for (size_t c = 0; c < ncols; ++c) {
            const size_t i = r * ncols + c;
            const int    p = static_cast<int>(i + 1);   // parameters are 1-based
            const Cell&  cell = row[c];

            int rc;
            if (cell.kind == CellKind::Null) {
                rc = sqlite3_bind_null(st, p);
            } else if (cell.kind == CellKind::Binary) {
                bufs[i] = HexToBytes(cell.text);
                rc = sqlite3_bind_blob(st, p, bufs[i].c_str(),
                                       static_cast<int>(bufs[i].size()),
                                       SQLITE_STATIC);
            } else {
                // Numeric cells bind as text too: SQLite applies the target
                // column's type affinity to a bound value, so '42' lands in an
                // INTEGER column as the integer 42 and '1.5' in a REAL column as
                // 1.5. Going through text avoids having to guess int vs real vs
                // decimal from the digits, which is what MySqlStream does for the
                // same reason.
                const wxScopedCharBuffer u = cell.text.utf8_str();
                bufs[i].assign(u.data(), u.length());
                rc = sqlite3_bind_text(st, p, bufs[i].c_str(),
                                       static_cast<int>(bufs[i].size()),
                                       SQLITE_STATIC);
            }

            // c_str(), never a conditional nullptr, and an EXPLICIT byte length.
            //
            // Both halves of that are load-bearing, and this is the exact spot
            // where the equivalent MySQL defect lived. MySqlStream.cpp used to
            // pass `empty() ? nullptr : data()` for MYSQL_BIND::buffer, and
            // libmariadb reads a null buffer as SQL NULL regardless of is_null:
            // against a live MySQL 8.0.36 target, 234 of 700 rows whose column
            // was '' arrived as NULL, the diff then saw a real difference on
            // every run, and the sync never converged.
            //
            // sqlite3_bind_text/blob have precisely the same trap:
            // sqlite3_bind_text(st, p, nullptr, ...) binds SQL NULL, not an empty
            // string. std::string::c_str() is guaranteed to return a valid
            // pointer even when the string is empty, so an empty Cell binds a
            // real zero-length TEXT/BLOB and stays distinguishable from a Null
            // Cell, which took the sqlite3_bind_null branch above.
            //
            // The explicit length (rather than -1) matters for BLOBs and for any
            // text carrying an embedded 0x00: -1 would make sqlite strlen() the
            // buffer and silently truncate at the first zero byte. Byte-exact
            // round-tripping of binary is asserted in
            // tests/sqlite_batch_sync_test.cpp case [2].
            if (rc != SQLITE_OK) { err = DbError(db); return false; }
        }
    }

    // A single step runs the whole multi-row INSERT. SQLITE_ROW would mean the
    // caller passed something that returns rows (e.g. INSERT … RETURNING); it is
    // not an error, and finalize below discards the rest either way.
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        err = DbError(db);
        return false;
    }
    return true;
}

} // namespace

bool ExecuteBatch(sqlite3* db, const wxString& sql,
                  const std::vector<std::vector<Cell>>& rows, wxString& err)
{
    if (!db)          { err = L"未连接"; return false; }
    if (rows.empty()) return true;

    const size_t ncols = rows.front().size();
    if (ncols == 0) { err = L"批量执行：行不含任何列"; return false; }
    for (const auto& r : rows)
        if (r.size() != ncols) { err = L"批量执行：各行列数不一致"; return false; }

    // The per-statement parameter ceiling, read off THIS connection rather than
    // hardcoded — see the header. A negative third argument queries without
    // changing anything. The 999 fallback is SQLite's historical default and is
    // only reached if the query somehow returns a non-positive value.
    const int  lim       = sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1);
    const size_t maxParams = lim > 0 ? static_cast<size_t>(lim) : 999;

    // A table so wide that a SINGLE row overruns the cap cannot be batched at
    // all, and lowering the chunk to 1 would still fail — with sqlite's own
    // "too many SQL variables", which says nothing about why. Fail here instead,
    // naming the numbers, so the operator can raise the limit or narrow the
    // column set knowingly.
    if (ncols > maxParams) {
        err = wxString::Format(
            L"批量执行：单行需要 %llu 个参数，超过本连接的 SQLite 参数上限 %llu",
            static_cast<unsigned long long>(ncols),
            static_cast<unsigned long long>(maxParams));
        return false;
    }

    // ~500 rows per statement, lowered when a wide table would otherwise blow
    // past that cap. Same shape as MySqlStream/PgStream, which lower against
    // their engines' fixed 65535.
    size_t chunk = 500;
    if (ncols * chunk > maxParams)
        chunk = std::max<size_t>(1, maxParams / ncols);

    for (size_t off = 0; off < rows.size(); off += chunk) {
        const size_t n = std::min(chunk, rows.size() - off);
        if (!ExecChunk(db, sql, rows, off, n, ncols, err)) return false;
    }
    return true;
}

} // namespace sqlitestream
} // namespace db
