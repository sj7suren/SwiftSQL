// PgStream.cpp — see header. Byte-order ORDER BY (ADR-015 T2 defense 1) +
// parameterized batch INSERT (T4) for the libpq driver.
#include "db/PgStream.h"

#include "db/DbDriver.h"   // QuoteIdent / Dialect

#include <algorithm>
#include <string>

namespace db {
namespace pgstream {

namespace {

struct ResultCloser {
    PGresult* res;
    explicit ResultCloser(PGresult* r) : res(r) {}
    ~ResultCloser() { if (res) PQclear(res); }
    ResultCloser(const ResultCloser&) = delete;
    ResultCloser& operator=(const ResultCloser&) = delete;
};

bool Contains(const std::vector<wxString>& v, const wxString& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Execute one batch of `n` rows starting at `off` as a single PQexecParams.
bool ExecChunk(PGconn* conn, const wxString& head,
               const std::vector<std::vector<Cell>>& rows,
               size_t off, size_t n, size_t ncols, wxString& err)
{
    const size_t np = n * ncols;

    wxString sql = head;
    sql.reserve(head.length() + np * 8);
    for (size_t r = 0; r < n; ++r) {
        sql += (r ? L", (" : L" (");
        for (size_t c = 0; c < ncols; ++c)
            sql += wxString::Format(c ? L", $%lu" : L"$%lu",
                                    static_cast<unsigned long>(r * ncols + c + 1));
        sql += L")";
    }

    // bufs owns the bytes; vals points into it (nullptr = SQL NULL). Both are
    // sized up front so no reallocation can dangle a pointer in vals.
    std::vector<std::string> bufs(np);
    std::vector<const char*> vals(np, nullptr);
    for (size_t r = 0; r < n; ++r) {
        const std::vector<Cell>& row = rows[off + r];
        for (size_t c = 0; c < ncols; ++c) {
            const size_t i = r * ncols + c;
            const Cell&  cell = row[c];
            if (cell.kind == CellKind::Null) continue;   // vals[i] stays nullptr
            if (cell.kind == CellKind::Binary) {
                // Cell::text is prefix-less hex; bytea's text input form wants
                // the \x prefix. Sent as a text parameter, so backslash handling
                // never depends on standard_conforming_strings — parameters are
                // not parsed as SQL literals.
                bufs[i] = "\\x" + std::string(cell.text.utf8_str());
            } else {
                const wxScopedCharBuffer u = cell.text.utf8_str();
                bufs[i].assign(u.data(), u.length());
            }
            vals[i] = bufs[i].c_str();
        }
    }

    // paramTypes = nullptr: let the server infer each parameter's type from the
    // INSERT target column, which is what makes a text-format bytea/numeric/
    // timestamp parameter land correctly without us mapping types by hand.
    PGresult* res = PQexecParams(conn, sql.utf8_str(), static_cast<int>(np),
                                 nullptr, vals.data(), nullptr, nullptr, 0);
    ResultCloser guard(res);
    if (!res) { err = L"批量执行失败：无法获取结果"; return false; }

    const ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        err = wxString::FromUTF8(PQresultErrorMessage(res)).Trim();
        if (err.IsEmpty()) err = L"批量执行失败";
        return false;
    }
    return true;
}

} // namespace

wxString OrderByClause(const StreamOptions& opt)
{
    if (opt.orderBy.empty()) return wxString();
    wxString s = L" ORDER BY ";
    for (size_t i = 0; i < opt.orderBy.size(); ++i) {
        if (i) s += L", ";
        s += QuoteIdent(opt.orderBy[i], Dialect::Postgres);
        if (opt.binaryOrder && Contains(opt.binaryOrderCols, opt.orderBy[i]))
            s += L" COLLATE \"C\"";
    }
    return s;
}

bool ExecuteBatch(PGconn* conn, const wxString& sql,
                  const std::vector<std::vector<Cell>>& rows, wxString& err)
{
    if (!conn)        { err = L"未连接"; return false; }
    if (rows.empty()) return true;

    const size_t ncols = rows.front().size();
    if (ncols == 0) { err = L"批量执行：行不含任何列"; return false; }
    for (const auto& r : rows)
        if (r.size() != ncols) { err = L"批量执行：各行列数不一致"; return false; }

    // ~500 rows per round trip, lowered when a wide table would otherwise blow
    // past libpq's 65535-parameter limit for one PQexecParams.
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

} // namespace pgstream
} // namespace db
