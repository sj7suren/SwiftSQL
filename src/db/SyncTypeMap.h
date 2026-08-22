// SyncTypeMap.h — cross-engine (MySQL <-> PostgreSQL) column type mapping for
// the cross-database synchronization suite (see docs/design/cross-db-sync.md).
//
// Deliberately placed in `namespace db` (like SchemaModel.h), NOT `namespace
// db::sync`: CanonicalType/TypeVerdict are a foundational type-system concept
// that BOTH the sync engine (db::sync::SchemaDelta / SyncEngine) and the
// dialect-editor abstraction (db::DialectProfile::Interpret/Render, ADR-014)
// need to share. DialectProfile's hard rule is "no SchemaChangeSet/SyncPlan/
// anything sync-*orchestration*-specific" — this file has zero knowledge of
// SchemaChangeSet, SyncPlan, or SchemaDelta, so depending on it does not violate
// that rule. The sync-specific consumer types (Finding, SchemaDelta) live in the
// separate db::sync::SchemaDelta.h instead.
//
// Pure value types + pure functions: no connection, no wx UI beyond wxString.
#pragma once

#include <wx/string.h>
#include <vector>

#include "db/SchemaModel.h"   // ColKind / NormColumn

namespace db {

// A canonical, engine-neutral description of a column's type — the common
// currency Interpret() produces and Render() consumes. `kind` is the existing
// SchemaModel ColKind (already shared across engines); the extra fields carry
// just enough width/semantics to tell apart pairs that share a ColKind but are
// NOT interchangeable (MySQL TIMESTAMP vs DATETIME both being ColKind::Timestamp
// is exactly this case — see withTimeZone below).
struct CanonicalType {
    ColKind   kind = ColKind::Other;
    long long length = -1;             // char length / numeric precision; -1 = N/A
    int       scale = -1;              // numeric scale; -1 = N/A
    bool      isUnsigned = false;      // MySQL UNSIGNED; PostgreSQL never sets this
    // For ColKind::Timestamp only: distinguishes the "auto-converts, effectively
    // timezone-aware" variant (MySQL TIMESTAMP, PG timestamptz) from the "naive
    // wall-clock" variant (MySQL DATETIME, PG timestamp without time zone). See
    // the CompareCanonical() comment for why "both true" is still Lossy, not
    // Equivalent.
    bool      withTimeZone = false;
    // True for the binary family (ColKind::Binary/Blob) — kept as an explicit
    // field (rather than re-deriving from `kind` at every call site) so the
    // fixed-length-vs-unbounded distinction below has a stable home to grow into.
    bool      binaryVariant = false;
    std::vector<wxString> allowedValues;   // ENUM/SET member list ("" = not enum)
    wxString  rawType;                 // origin engine-native text, for diagnostics only —
                                       // NEVER re-emitted into another engine's DDL verbatim
};

// The verdict CompareCanonical() (or a Render() failure) assigns to a column
// type pairing. Only Identical/Equivalent may ever become an automatic ALTER —
// see MayAutoAlter() below, which is the single predicate the rest of the sync
// engine is required to gate on (SchemaDelta::AddColumnChange enforces this
// structurally; see SchemaDelta.h).
enum class TypeVerdict { Identical, Equivalent, Lossy, Unmappable };

// The ONE predicate that decides whether a verdict may drive an automatic
// ALTER/ADD. Identical = byte-identical within one engine (rawType equal).
// Equivalent = different text, same engine-neutral semantics, safe to convert
// automatically. Lossy/Unmappable must never auto-ALTER — the caller degrades to
// a Finding (see SchemaDelta.h) and leaves the column for a human.
constexpr bool MayAutoAlter(TypeVerdict v)
{
    return v == TypeVerdict::Identical || v == TypeVerdict::Equivalent;
}

// ---- MySQL ----

// MySQL information_schema.columns (data_type, column_type) -> ColKind. This is
// the SAME classification MySqlSql.cpp's detail::MapColKind already performs for
// GetTableSchema; re-exported here (rather than duplicated) so SyncTypeMap and
// MySqlDriver.cpp/MySqlSql.cpp share one source of truth. Declared here, defined
// in MySqlSql.cpp (unchanged) — see the .cpp for the forwarding shim.
// (Not re-declared: callers needing MySQL ColKind classification already
// #include db/MySqlSql.h and call db::detail::MapColKind directly.)

// A MySQL-sourced NormColumn (kind already set by detail::MapColKind, rawType =
// column_type, e.g. "bigint unsigned", "enum('a','b')", "timestamp") -> the
// canonical type. Parses UNSIGNED / enum member list / TIMESTAMP-vs-DATETIME out
// of rawType since NormColumn carries no dedicated fields for them.
CanonicalType InterpretMySqlColumn(const NormColumn& c);

// Canonical type -> a MySQL type-text fragment ("bigint", "enum('a','b')",
// "varchar(80)"...). False = no safe MySQL rendering exists for this canonical
// type (e.g. ColKind::Uuid with no native MySQL type); `why` explains. `out`
// carries ONLY the type text (no column name / NOT NULL / DEFAULT).
bool RenderMySqlType(const CanonicalType& t, wxString& out, wxString& why);

// ---- PostgreSQL ----

// PostgreSQL information_schema.columns.data_type -> ColKind (the PG-side
// counterpart to MySqlSql.cpp's MapColKind; PG's data_type is a bare category
// name — "character varying", "timestamp with time zone" — never a MySQL-style
// column_type with an inline width). Used by PgSchemaRead.cpp (A2) to give
// PG-introspected columns a real ColKind instead of the previous permanent
// ColKind::Other.
ColKind MapPgColKind(const wxString& dataType);

// A PostgreSQL-sourced NormColumn (kind = MapPgColKind(data_type), rawType =
// data_type verbatim) -> the canonical type. Parses "with/without time zone" out
// of rawType for the Timestamp/Time kinds.
CanonicalType InterpretPgColumn(const NormColumn& c);

// Canonical type -> a PostgreSQL type-text fragment ("bigint", "text",
// "numeric(10,2)"...). False = no safe PG rendering exists (e.g. MySQL UNSIGNED
// integers — PG has no unsigned integer type); `why` explains.
bool RenderPgType(const CanonicalType& t, wxString& out, wxString& why);

// ---- cross-engine comparison ----

// Compare two canonical types (order-independent) and classify the pairing.
// `reason` is always filled (both success and skip cases) — it is the text a
// Finding.reason carries when a column is skipped, and a short audit note when
// it is not. See the .cpp for the documented judgment calls (unsigned integers,
// TIMESTAMP/timestamptz, ENUM).
TypeVerdict CompareCanonical(const CanonicalType& a, const CanonicalType& b, wxString& reason);

} // namespace db
