// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MySqlStream.cpp — see header. Byte-order ORDER BY (ADR-015 T2 defense 1) +
// parameterized batch INSERT (T4) for the MySQL wire driver.
#include "db/MySqlStream.h"

#include "db/DbDriver.h"   // QuoteIdent / Dialect

#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>

namespace db {
namespace mysqlstream {

namespace {

// RAII: close the prepared statement on every exit path, including a throw from
// wxString/std::vector work in the bind loop.
struct StmtCloser {
    MYSQL_STMT* st;
    explicit StmtCloser(MYSQL_STMT* s) : st(s) {}
    ~StmtCloser() { if (st) mysql_stmt_close(st); }
    StmtCloser(const StmtCloser&) = delete;
    StmtCloser& operator=(const StmtCloser&) = delete;
};

bool Contains(const std::vector<wxString>& v, const wxString& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// One hex digit -> value, or -1. Cell::text for a Binary cell is prefix-less
// uppercase hex (StreamRows' convention); lowercase is accepted too so a
// hand-built Cell doesn't silently truncate.
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

wxString StmtError(MYSQL_STMT* st)
{
    const char* e = mysql_stmt_error(st);
    return e && *e ? wxString::FromUTF8(e) : wxString(L"批量执行失败");
}

// Execute one batch of `n` rows starting at `off`. Prepared + bound + executed
// as a single round trip.
bool ExecChunk(MYSQL* conn, const wxString& head,
               const std::vector<std::vector<Cell>>& rows,
               size_t off, size_t n, size_t ncols, wxString& err)
{
    wxString tuple = L" (";
    for (size_t c = 0; c < ncols; ++c) tuple += (c ? L", ?" : L"?");
    tuple += L")";

    wxString sql = head;
    sql.reserve(head.length() + n * tuple.length() + n);
    for (size_t r = 0; r < n; ++r) {
        if (r) sql += L",";
        sql += tuple;
    }

    MYSQL_STMT* st = mysql_stmt_init(conn);
    if (!st) { err = L"批量执行：无法创建预处理语句"; return false; }
    StmtCloser guard(st);

    const wxScopedCharBuffer q = sql.utf8_str();
    if (mysql_stmt_prepare(st, q.data(), static_cast<unsigned long>(q.length())) != 0) {
        err = StmtError(st);
        return false;
    }

    // is_null's element type differs across connector versions (my_bool vs
    // bool); deriving it from the struct keeps this compiling either way.
    using NullFlag = std::remove_pointer<decltype(MYSQL_BIND::is_null)>::type;

    const size_t np = n * ncols;
    std::vector<MYSQL_BIND>    binds(np);
    std::vector<NullFlag>      nulls(np, static_cast<NullFlag>(0));
    std::vector<unsigned long> lens(np, 0);
    std::vector<std::string>   bufs(np);   // owns every bound byte
    std::memset(binds.data(), 0, np * sizeof(MYSQL_BIND));

    for (size_t r = 0; r < n; ++r) {
        const std::vector<Cell>& row = rows[off + r];
        for (size_t c = 0; c < ncols; ++c) {
            const size_t i = r * ncols + c;
            const Cell&  cell = row[c];
            if (cell.kind == CellKind::Null) {
                nulls[i] = static_cast<NullFlag>(1);
                binds[i].buffer_type = MYSQL_TYPE_NULL;
            } else if (cell.kind == CellKind::Binary) {
                bufs[i] = HexToBytes(cell.text);
                binds[i].buffer_type = MYSQL_TYPE_BLOB;
            } else {
                // Numeric cells bind as text too: the server casts to the target
                // column's type, and going through a string avoids having to
                // guess int vs decimal vs float from the text.
                const wxScopedCharBuffer u = cell.text.utf8_str();
                bufs[i].assign(u.data(), u.length());
                binds[i].buffer_type = MYSQL_TYPE_STRING;
            }
            lens[i] = static_cast<unsigned long>(bufs[i].size());
            // c_str(), never a conditional nullptr.
            //
            // This line used to read `bufs[i].empty() ? nullptr : ...data()`,
            // which silently turned every EMPTY STRING into a SQL NULL: the
            // client library treats a null buffer pointer as NULL no matter what
            // is_null says, and is_null is 0 here for a genuinely empty value.
            // Observed against a real MySQL 8.0.36 target (PostgreSQL → MySQL,
            // mysql_pg_live_exec3.cpp item B3): 234 of 700 rows whose `note` was
            // '' arrived as NULL, and because the diff then saw a real
            // difference every single run, the table produced 234 phantom
            // UPDATEs forever and the sync never converged. Against a NOT NULL
            // target column it would instead fail the whole batch outright.
            //
            // std::string::c_str() is guaranteed to return a valid pointer even
            // when the string is empty, so the guard bought nothing and cost
            // correctness. PgStream.cpp's equivalent line has always used
            // c_str() and is why the PG target never had this bug.
            binds[i].buffer        = const_cast<char*>(bufs[i].c_str());
            binds[i].buffer_length = lens[i];
            binds[i].length        = &lens[i];
            binds[i].is_null       = &nulls[i];
        }
    }

    if (mysql_stmt_bind_param(st, binds.data()) != 0) { err = StmtError(st); return false; }
    if (mysql_stmt_execute(st) != 0)                  { err = StmtError(st); return false; }
    return true;
}

} // namespace

wxString HexDigits(const char* data, unsigned long len)
{
    static const wchar_t* kHex = L"0123456789ABCDEF";
    wxString h;
    h.reserve(len * 2);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    for (unsigned long i = 0; i < len; ++i) {
        h += kHex[p[i] >> 4];
        h += kHex[p[i] & 0x0F];
    }
    return h;
}

wxString HexLiteral(const char* data, unsigned long len)
{
    if (len == 0) return L"''";
    return L"0x" + HexDigits(data, len);
}

wxString OrderByClause(const StreamOptions& opt)
{
    if (opt.orderBy.empty()) return wxString();
    wxString s = L" ORDER BY ";
    for (size_t i = 0; i < opt.orderBy.size(); ++i) {
        if (i) s += L", ";
        const wxString col = QuoteIdent(opt.orderBy[i], Dialect::MySQL);
        if (opt.binaryOrder && Contains(opt.binaryOrderCols, opt.orderBy[i]))
            s += L"CONVERT(" + col + L" USING utf8mb4) COLLATE utf8mb4_bin";
        else
            s += col;
    }
    return s;
}

bool ExecuteBatch(MYSQL* conn, const wxString& sql,
                  const std::vector<std::vector<Cell>>& rows, wxString& err)
{
    if (!conn)      { err = L"未连接"; return false; }
    if (rows.empty()) return true;

    const size_t ncols = rows.front().size();
    if (ncols == 0) { err = L"批量执行：行不含任何列"; return false; }
    for (const auto& r : rows)
        if (r.size() != ncols) { err = L"批量执行：各行列数不一致"; return false; }

    // ~500 rows per round trip, lowered when a wide table would otherwise blow
    // past MySQL's 65535-placeholder limit for one prepared statement.
    const size_t kMaxParams = 65535;
    size_t chunk = 500;
    if (ncols * chunk > kMaxParams)
        chunk = std::max<size_t>(1, kMaxParams / ncols);

    for (size_t off = 0; off < rows.size(); off += chunk) {
        const size_t n = std::min(chunk, rows.size() - off);
        if (!ExecChunk(conn, sql, rows, off, n, ncols, err)) return false;
    }
    return true;
}

} // namespace mysqlstream
} // namespace db
