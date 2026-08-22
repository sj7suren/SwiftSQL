// SyncSeqFix.cpp — see header. Per-dialect identity/sequence repair.
#include "db/SyncSeqFix.h"

#include "db/DbDriver.h"   // db::Dialect (full definition), QuoteIdent

#include <climits>

namespace db::sync {
namespace {

// Embed a name inside a SQL string literal (pg_get_serial_sequence takes its
// arguments as text, not identifiers).
wxString SqlStr(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}

} // namespace

bool BuildSequenceFixes(const TableSchema& schema, Dialect tgtDialect,
                        const wxString& qualifiedTable, long long maxValue,
                        std::vector<SeqFix>& out, wxString& why)
{
    out.clear();
    why.clear();

    std::vector<const NormColumn*> identity;
    for (const auto& c : schema.columns)
        if (c.autoIncrement) identity.push_back(&c);

    // No identity column: nothing to repair. Not an error — see the header's
    // note on the two different false returns.
    if (identity.empty()) return false;

    for (const NormColumn* c : identity) {
        SeqFix fix;
        fix.column = c->name;

        switch (tgtDialect) {

        case Dialect::Postgres: {
            // Self-computing and idempotent: safe to re-run, and correct even if
            // the sync wrote fewer rows than the table already had.
            //
            // The three-argument setval matters. Two-arg setval(seq, 0) is an
            // ERROR for a sequence with MINVALUE 1, which is exactly the state an
            // empty table leaves us in. With is_called = false the sequence's
            // NEXT nextval() returns the given value, so the empty-table case
            // correctly resets to 1 rather than blowing up.
            const wxString col   = QuoteIdent(c->name, tgtDialect);
            const wxString maxQ  = L"(SELECT MAX(" + col + L") FROM " + qualifiedTable + L")";
            fix.statement = L"SELECT setval(pg_get_serial_sequence(" +
                            SqlStr(qualifiedTable) + L", " + SqlStr(c->name) + L"), " +
                            L"COALESCE(" + maxQ + L", 1), " + maxQ + L" IS NOT NULL)";
            fix.selfComputing = true;
            break;
        }

        case Dialect::MySQL: {
            // ALTER TABLE ... AUTO_INCREMENT accepts no subquery, so the value
            // must be a literal. Refuse rather than guess: an AUTO_INCREMENT set
            // too low is the very duplicate-key bug this file prevents.
            if (maxValue < 0) {
                why = L"MySQL 的 ALTER TABLE ... AUTO_INCREMENT 只接受字面量，"
                      L"但本次同步未提供已写入的最大自增值，无法安全推进自增计数器";
                out.clear();
                return false;
            }
            if (maxValue == LLONG_MAX) {
                why = L"已写入的最大自增值已达 int64 上限，无法再推进自增计数器";
                out.clear();
                return false;
            }
            fix.statement = wxString::Format(L"ALTER TABLE %s AUTO_INCREMENT = %lld",
                                             qualifiedTable, maxValue + 1);
            break;
        }

        case Dialect::SqlServer: {
            if (maxValue < 0) {
                why = L"SQL Server 的 DBCC CHECKIDENT RESEED 需要字面量种子值，"
                      L"但本次同步未提供已写入的最大标识值";
                out.clear();
                return false;
            }
            // RESEED sets the *current* value; the next identity is seed + 1,
            // so the max itself is the correct seed (unlike MySQL's max + 1).
            fix.statement = wxString::Format(L"DBCC CHECKIDENT (N%s, RESEED, %lld)",
                                             SqlStr(qualifiedTable), maxValue);
            break;
        }

        case Dialect::Sqlite:
            // SQLite's INTEGER PRIMARY KEY / AUTOINCREMENT allocates from
            // max(rowid) (or sqlite_sequence, which the INSERTs themselves
            // maintain). Nothing to repair — and emitting a manual
            // sqlite_sequence UPDATE would be worse than doing nothing.
            continue;

        case Dialect::Oracle:
        default:
            // Oracle identity columns are backed by a system-generated sequence
            // whose name is not derivable from the column alone, and a plain
            // sequence + trigger arrangement is not discoverable from
            // TableSchema at all. Honest refusal beats a wrong ALTER.
            why = L"Oracle 目标端的标识列/序列名无法从表结构推导，请手工推进对应 sequence";
            out.clear();
            return false;
        }

        out.push_back(std::move(fix));
    }

    return !out.empty();
}

} // namespace db::sync
