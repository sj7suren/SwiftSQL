// PgRoutineRead.cpp — see the header, especially the prosrc-vs-
// pg_get_functiondef argument. Pure db::IConnection consumer; no PGconn*, no
// libpq include, no DDL rendering.
#include "db/PgRoutineRead.h"

namespace db {
namespace pgroutine {

namespace {

wxString Esc(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return e;
}

bool LooksLikePermissionError(const wxString& err)
{
    wxString e = err;
    e.MakeLower();
    return e.Contains(L"permission denied") || e.Contains(L"42501") ||
           e.Contains(L"must be owner") || e.Contains(L"权限");
}

// PG < 11 has no pg_proc.prokind. Distinguishing that from a real failure keeps
// an old server on the "functions only" path instead of reporting its catalog
// unreadable.
bool LooksLikeMissingProkind(const wxString& err)
{
    wxString e = err;
    e.MakeLower();
    return e.Contains(L"prokind");
}

// The argument type list is aggregated server-side with a UNIT SEPARATOR rather
// than a comma. oidvectortypes() (what psql's \df uses) comma-joins, and a
// comma-joined list cannot be split safely the moment a type name contains one.
// Splitting on \x1F is unambiguous regardless of what format_type returns.
const wchar_t kSep = L'\x1F';

std::vector<wxString> SplitUnitSep(const wxString& s)
{
    std::vector<wxString> out;
    if (s.IsEmpty()) return out;
    wxString cur;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == kSep) { out.push_back(cur); cur.clear(); }
        else cur += s[i];
    }
    out.push_back(cur);
    return out;
}

// provolatile is a single char in the catalog.
wxString VolatilityText(const wxString& v)
{
    if (v == L"i") return L"immutable";
    if (v == L"s") return L"stable";
    if (v == L"v") return L"volatile";
    return wxString();
}

wxString NullToEmpty(const wxString& v)
{
    return v == L"NULL" ? wxString() : v;
}

// The shared column list. `prokindExpr` is spliced in so the PG<11 fallback can
// substitute a literal 'f' for the missing column while every other column,
// filter and ordering stays byte-identical between the two queries — one shape
// to reason about, not two.
wxString BuildQuery(const wxString& schemaFilter, const wxString& prokindExpr,
                    const wxString& extraWhere)
{
    wxString sql =
        L"SELECT n.nspname, p.proname, " + prokindExpr + L", "
        // prosrc: STORED body text, not rendered DDL. See the header.
        L"COALESCE(p.prosrc,''), "
        L"l.lanname, "
        L"CASE WHEN p.prosecdef THEN 1 ELSE 0 END, "
        L"COALESCE(p.provolatile::text,''), "
        L"COALESCE(pg_catalog.format_type(p.prorettype, NULL),''), "
        // Ordered argument types, unit-separator joined. proargtypes is cast to
        // oid[] before unnest because oidvector is not itself an anyarray on
        // every server version.
        //
        // NOTE — this correlated subquery is deliberately part of the SAME
        // statement as the routine list, not a second pass like MySQL's
        // information_schema.PARAMETERS query. That is what makes this reader
        // structurally immune to the partial-read hazard MySqlRoutineRead.h
        // describes: the signature cannot fail while the routine list succeeds,
        // so this reader can never hand the compare a routine with a silently
        // empty argument list. If a future edit splits this into a second
        // Execute(), it MUST also degrade the whole side on failure.
        L"COALESCE((SELECT string_agg(pg_catalog.format_type(a.t, NULL), chr(31) "
        L"                            ORDER BY a.ord) "
        L"           FROM unnest(p.proargtypes::oid[]) WITH ORDINALITY AS a(t, ord)), '') "
        L"FROM pg_catalog.pg_proc p "
        L"JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        L"JOIN pg_catalog.pg_language  l ON l.oid = p.prolang "
        // Internal / C functions have no comparable body (prosrc holds a C
        // symbol name), so they are excluded rather than body-diffed.
        L"WHERE l.lanname IN ('sql','plpgsql') "
        L"AND n.nspname NOT IN ('pg_catalog','information_schema') ";
    if (!extraWhere.IsEmpty()) sql += extraWhere + L" ";
    if (!schemaFilter.IsEmpty())
        sql += L"AND n.nspname = '" + Esc(schemaFilter) + L"' ";
    sql += L"ORDER BY n.nspname, p.proname";
    return sql;
}

} // namespace

sync::RoutineReadStatus ReadRoutines(IConnection& conn, const wxString& schemaFilter,
                               std::vector<sync::RoutineDef>& out, wxString& detail)
{
    out.clear();
    detail.clear();

    QueryResult r;
    wxString err;

    // Modern path: prokind tells functions from procedures, and filtering to
    // ('f','p') drops aggregates and window functions — the very objects
    // pg_get_functiondef() throws on.
    bool ok = conn.Execute(BuildQuery(schemaFilter, L"p.prokind::text",
                                      L"AND p.prokind IN ('f','p')"), r, err);

    if (!ok && LooksLikeMissingProkind(err)) {
        // PG < 11: no prokind, and no procedures either — everything in pg_proc
        // that is not an aggregate or a window function is a function.
        wxString e2;
        ok = conn.Execute(BuildQuery(schemaFilter, L"'f'::text",
                                     L"AND NOT p.proisagg AND NOT p.proiswindow"), r, e2);
        if (!ok) err = e2;
    }

    if (!ok) {
        detail = err;
        return LooksLikePermissionError(err) ? sync::RoutineReadStatus::PermissionDenied
                                             : sync::RoutineReadStatus::Unsupported;
    }

    // ===================================================================
    // WHY THERE IS NO PRIVILEGE PROBE HERE, AND WHAT KEEPS IT UNNECESSARY
    // ===================================================================
    // MySqlRoutineRead cannot trust an empty routine list: MySQL 8 filters
    // information_schema.ROUTINES per row, so "no routines" and "no privilege
    // to see routines" are the SAME response, and it has to ask SHOW GRANTS
    // which one it is. PostgreSQL does not have that ambiguity, and this reader
    // depends on it — an empty `r.rows` below is reported as Ok with an empty
    // vector, exactly the answer that turned out to be unsafe on MySQL 8.
    //
    // That is safe here because pg_catalog.pg_proc is WORLD-READABLE: routine
    // privileges (EXECUTE) govern CALLING a function, not SEEING its catalog
    // row, and prosrc is a plain column of a readable table. So an empty result
    // means the schema really has no functions.
    //
    // MEASURED, not assumed — the same reasoning was "documented behaviour" on
    // MySQL 5.x right up until 8.0 changed it. tests/mysql_pg_live_routines.cpp
    // hazard 7g creates a role with no routine privileges at all (including the
    // EXECUTE that PUBLIC holds by default), SET ROLEs to it, and asserts this
    // reader still returns every routine WITH its body. Observed on PostgreSQL:
    // status=Ok, 2 routines, both bodyReadable with real prosrc text.
    //
    // THE OBLIGATION, for whoever edits this next: the empty-result-is-Ok
    // shortcut is licensed ONLY by that immunity. If hazard 7g ever fails — a
    // future PG version restricting pg_proc, or a switch to a filtered view such
    // as information_schema.routines (which IS privilege-filtered, unlike
    // pg_proc) — then this reader needs MySqlRoutineRead's second signal too,
    // and returning Ok on empty becomes the same silent mass-false-positive bug.
    // Do not "simplify" the query onto information_schema without reading that.
    for (const auto& row : r.rows) {
        if (row.size() < 9) continue;

        sync::RoutineDef d;
        d.schema = row[0];
        d.name   = row[1];
        d.kind   = (row[2] == L"p") ? sync::RoutineKind::Procedure
                                    : sync::RoutineKind::Function;
        d.body            = NullToEmpty(row[3]);
        d.language        = row[4];
        d.securityDefiner = (row[5] == L"1");
        d.volatility      = VolatilityText(NullToEmpty(row[6]));
        d.returnType      = NullToEmpty(row[7]);
        d.argTypes        = SplitUnitSep(NullToEmpty(row[8]));

        // PG procedures have no return type; the catalog reports `void`, which
        // is not a signature component and would otherwise read as a difference
        // against a MySQL procedure's empty one.
        if (d.kind == sync::RoutineKind::Procedure || d.returnType == L"void")
            d.returnType.clear();

        // Argument MODES are deliberately left empty. ADR-013 Q2 keys the
        // signature on proargtypes, which is PG's own notion of a function's
        // identity — and proargtypes carries input arguments only (OUT
        // parameters live in proallargtypes/proargmodes). Reporting modes from a
        // different column than the one the signature came from would let a pair
        // match on types and then "differ" on modes derived from a longer list.
        // An empty mode means "the catalog did not say" and never counts as a
        // difference; see RoutineCompare.cpp.

        // Unlike MySQL, PostgreSQL does not hide prosrc from a user who can see
        // the row: pg_proc is world-readable, so a listed routine's body is a
        // real body. An empty one is genuinely empty.
        d.bodyReadable = true;

        out.push_back(std::move(d));
    }

    return sync::RoutineReadStatus::Ok;
}

} // namespace pgroutine
} // namespace db
