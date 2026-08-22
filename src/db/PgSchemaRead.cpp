// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// PgSchemaRead.cpp — see header. Real PostgreSQL introspection driving A2 of
// the cross-engine sync feature; ColKind assignment delegates to
// SyncTypeMap::MapPgColKind (A1) instead of the previous permanent
// ColKind::Other. Pure db::IConnection consumer — no PGconn*, no libpq header.
#include "db/PgSchemaRead.h"
#include "db/SyncTypeMap.h"

#include <algorithm>
#include <vector>

namespace db {
namespace pgschema {

namespace {

wxString Esc(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return e;
}

// Resolve which schema a request actually refers to.
//
// A QUALIFIED name is authoritative and is returned unchanged — no search_path,
// no 'public' fallback, no "did you mean" guessing. That is the whole contract:
// asking for archive.orders must never read public.orders.
//
// An UNQUALIFIED name is resolved the way PostgreSQL itself would, by casting to
// ::regclass and reading back the namespace. This is deliberately NOT the old
// hard-coded 'public': a session whose search_path points elsewhere would then
// have had its table declared non-existent. Returning the schema we truly read
// also means out.name always reports where the answer came from.
//
// Empty return = the table does not exist in any visible schema, which is a real
// and honest answer (DiffSchema's "absent" convention), distinct from the old
// behaviour of claiming success with zero columns.
wxString ResolveSchema(IConnection& conn, const QualifiedName& table)
{
    if (table.HasSchema()) return table.Schema();

    QueryResult r; wxString ig;
    if (!conn.Execute(wxString::Format(
            L"SELECT n.nspname FROM pg_class c "
            L"JOIN pg_namespace n ON n.oid = c.relnamespace "
            L"WHERE c.oid = to_regclass('%s')", Esc(table.Name())), r, ig))
        return wxString();
    if (r.rows.empty() || r.rows[0].empty()) return wxString();
    return r.rows[0][0];
}

// Ordered primary key for one (schema, table). Replaces the delegation to
// IConnection::GetPrimaryKey, whose PG override is 'public'-scoped AND returns
// columns in table-ordinal order rather than key order. Key ORDER matters here:
// the data diff streams both sides ORDER BY the PK and merges them in lock-step,
// so a composite key read in the wrong order desynchronizes the merge.
void ReadPrimaryKey(IConnection& conn, const wxString& schema,
                    const wxString& table, std::vector<wxString>& out)
{
    QueryResult r; wxString ig;
    if (!conn.Execute(wxString::Format(
            L"SELECT a.attname "
            L"FROM pg_index i "
            L"JOIN pg_class c ON c.oid = i.indrelid "
            L"JOIN pg_namespace n ON n.oid = c.relnamespace "
            L"JOIN unnest(i.indkey) WITH ORDINALITY k(attnum, ord) ON TRUE "
            L"JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.attnum "
            L"WHERE i.indisprimary AND n.nspname='%s' AND c.relname='%s' "
            L"ORDER BY k.ord", Esc(schema), Esc(table)), r, ig))
        return;
    for (const auto& row : r.rows)
        if (!row.empty()) out.push_back(row[0]);
}

void ReadIndexes(IConnection& conn, const wxString& schema, const wxString& table,
                 const std::vector<wxString>& pk, std::vector<NormIndex>& out)
{
    QueryResult r; wxString ig;
    if (!conn.Execute(wxString::Format(
            L"SELECT i.relname, "
            L"  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
            L"   FROM unnest(ix.indkey) WITH ORDINALITY k(attnum, ord) "
            L"   JOIN pg_attribute a ON a.attrelid=t.oid AND a.attnum=k.attnum), "
            L"  ix.indisunique, ix.indisprimary "
            L"FROM pg_index ix JOIN pg_class i ON i.oid=ix.indexrelid "
            L"JOIN pg_class t ON t.oid=ix.indrelid "
            L"JOIN pg_namespace n ON n.oid=t.relnamespace "
            L"WHERE n.nspname='%s' AND t.relname='%s' ORDER BY i.relname",
            Esc(schema), Esc(table)), r, ig))
        return;
    for (const auto& row : r.rows) {
        if (row.size() < 4) continue;
        NormIndex ni;
        ni.name   = row[0];
        ni.unique = (row[2] == L"t");
        // pg_index.indisprimary is authoritative — the previous code inferred
        // primary-ness by comparing column SETS against the PK, which misfires
        // for a unique index over exactly the PK's columns.
        ni.primary = (row[3] == L"t");
        wxString rest = row[1];                 // comma-joined → vector
        while (true) {
            wxString part = rest.BeforeFirst(L',');
            part.Trim(true).Trim(false);
            if (!part.IsEmpty()) ni.columns.push_back(part);
            if (!rest.Contains(L",")) break;
            rest = rest.AfterFirst(L',');
        }
        out.push_back(std::move(ni));
    }
    (void)pk;
}

// Foreign keys for one (schema, table), with the referenced table's OWN schema
// carried on refTable. A FK may point across schemas, so recording only the bare
// relname would reintroduce the very ambiguity this file exists to remove — and
// refTable feeds SyncEngine's FK topological sort, which orders what executes.
void ReadForeignKeys(IConnection& conn, const wxString& schema,
                     const wxString& table, std::vector<NormForeignKey>& out)
{
    QueryResult r; wxString ig;
    if (!conn.Execute(wxString::Format(
            L"SELECT con.conname, "
            L"  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
            L"   FROM unnest(con.conkey) WITH ORDINALITY k(attnum, ord) "
            L"   JOIN pg_attribute a ON a.attrelid=con.conrelid AND a.attnum=k.attnum), "
            L"  fn.nspname, f.relname, "
            L"  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
            L"   FROM unnest(con.confkey) WITH ORDINALITY k(attnum, ord) "
            L"   JOIN pg_attribute a ON a.attrelid=con.confrelid AND a.attnum=k.attnum) "
            L"FROM pg_constraint con "
            L"JOIN pg_class c ON c.oid=con.conrelid "
            L"JOIN pg_namespace n ON n.oid=c.relnamespace "
            L"JOIN pg_class f ON f.oid=con.confrelid "
            L"JOIN pg_namespace fn ON fn.oid=f.relnamespace "
            L"WHERE con.contype='f' AND n.nspname='%s' AND c.relname='%s' "
            L"ORDER BY con.conname", Esc(schema), Esc(table)), r, ig))
        return;

    auto split = [](const wxString& s) {
        std::vector<wxString> v;
        wxString rest = s;
        while (true) {
            wxString part = rest.BeforeFirst(L',');
            part.Trim(true).Trim(false);
            if (!part.IsEmpty()) v.push_back(part);
            if (!rest.Contains(L",")) break;
            rest = rest.AfterFirst(L',');
        }
        return v;
    };

    for (const auto& row : r.rows) {
        if (row.size() < 5) continue;
        NormForeignKey nf;
        nf.name       = row[0];
        nf.columns    = split(row[1]);
        nf.refTable   = QualifiedName(row[2], row[3]).Key();
        nf.refColumns = split(row[4]);
        out.push_back(std::move(nf));
    }
}

} // namespace

bool GetTableSchema(IConnection& conn, const QualifiedName& table,
                    TableSchema& out, wxString& err)
{
    out = TableSchema{};

    const wxString schema = ResolveSchema(conn, table);
    if (schema.IsEmpty()) {
        // Not found in any visible schema. Report it as an EMPTY schema with a
        // true return, which is DiffSchema's documented "table does not exist"
        // convention — the same answer the caller would get for a genuinely
        // absent table, and now reached only when the table really is absent.
        out.name = table;
        return true;
    }

    // out.name is the RESOLVED identity, not the requested one: an unqualified
    // request that landed in `archive` comes back saying so, and every consumer
    // downstream (the plan unit, the selection key, the executor) then carries
    // the schema it was actually read from.
    out.name = QualifiedName(schema, table.Name());

    const wxString sc = Esc(schema);
    const wxString tb = Esc(table.Name());

    // One pass over information_schema.columns yields data_type (== rawType,
    // used verbatim by PgSql.cpp's PgTypeText for same-dialect rendering),
    // char/numeric length + scale, nullability, default text, and an
    // auto-increment signal (nextval-backed serial OR PG10+ GENERATED ... AS
    // IDENTITY).
    QueryResult r;
    if (!conn.Execute(wxString::Format(
            L"SELECT column_name, data_type, "
            L"COALESCE(character_maximum_length, numeric_precision, -1), "
            L"COALESCE(numeric_scale, -1), "
            L"is_nullable, COALESCE(column_default, ''), ordinal_position, "
            L"CASE WHEN column_default LIKE 'nextval(%%' "
            L"      OR COALESCE(is_identity, 'NO') = 'YES' THEN 1 ELSE 0 END "
            L"FROM information_schema.columns "
            L"WHERE table_schema='%s' AND table_name='%s' "
            L"ORDER BY ordinal_position", sc, tb), r, err))
        return false;

    for (const auto& row : r.rows) {
        if (row.size() < 8) continue;
        NormColumn nc;
        nc.name       = row[0];
        nc.rawType    = row[1];                 // PG data_type, verbatim — PgTypeText truth
        long long len = -1; row[2].ToLongLong(&len);
        long scale = -1;    row[3].ToLong(&scale);
        nc.length     = len;
        if (len >= 0 && scale >= 0) nc.scale = static_cast<int>(scale);
        nc.notNull    = (row[4] == L"NO");
        nc.hasDefault = !row[5].IsEmpty();
        nc.defaultExpr = nc.hasDefault ? row[5] : wxString();
        long ord = 0; row[6].ToLong(&ord); nc.ordinal = static_cast<int>(ord);
        nc.autoIncrement = (row[7] == L"1");
        // Real ColKind (A1's MapPgColKind), replacing the previous permanent
        // ColKind::Other every PG-introspected column used to carry.
        nc.kind = MapPgColKind(nc.rawType);
        out.columns.push_back(std::move(nc));
    }

    ReadPrimaryKey(conn, schema, table.Name(), out.primaryKey);
    ReadIndexes(conn, schema, table.Name(), out.primaryKey, out.indexes);
    ReadForeignKeys(conn, schema, table.Name(), out.foreignKeys);

    // PG has no storage "engine"; charset/collation/comment are best-effort.
    QueryResult cr; wxString ig;
    if (conn.Execute(wxString::Format(
            L"SELECT COALESCE(obj_description("
            L"  (quote_ident(table_schema)||'.'||quote_ident(table_name))::regclass, "
            L"  'pg_class'), '') "
            L"FROM information_schema.tables "
            L"WHERE table_schema='%s' AND table_name='%s'", sc, tb), cr, ig))
        if (!cr.rows.empty() && !cr.rows[0].empty())
            out.comment = cr.rows[0][0];

    return true;
}

} // namespace pgschema
} // namespace db
