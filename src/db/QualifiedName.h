// QualifiedName.h — the identity of ONE table, honestly shaped for engines that
// have a schema layer and engines that do not.
//
// ===========================================================================
// WHY THIS TYPE EXISTS
// ===========================================================================
// Table identity used to be a bare `wxString name`. Everything keyed on it:
// db::sync::SyncPlan::TableUnit::table, ui::TableKey, SyncSelection's map,
// TableDataSpec, DataExecPlan::Find. QA flagged it twice as a LATENT defect —
// "two same-named tables in different PostgreSQL schemas would conflate" — and
// graded it latent because the wizard pairs one database against one database,
// so two same-named tables cannot both appear.
//
// Measured against a real PostgreSQL 15 server, that grading was too generous.
// With `public.orders` (3 cols) and `archive.orders` (4 cols) in one database:
//
//   ListTables(db)                  -> "orders", "orders"   (indistinguishable)
//   GetTableSchema(db, "orders")    -> public's 3 columns   (silently)
//   GetTableSchema(db, "only_here") -> TRUE, ZERO COLUMNS   (archive-only table)
//   StreamRows(db, "orders")        -> public's 2 rows      (search_path roulette)
//
// The third line is the dangerous one and it was never latent: an empty
// TableSchema is DiffSchema's convention for "this table does not exist", so a
// table living in any non-`public` schema was introspected as ABSENT while
// ListTables simultaneously reported it as PRESENT. The layer below the UI never
// had the identity the UI was being careful with.
//
// ===========================================================================
// BEING HONEST ABOUT TWO DIFFERENT DATA MODELS
// ===========================================================================
// MySQL and PostgreSQL do not have the same shape here, and pretending they do
// is how the bug got in.
//
//   PostgreSQL: database > SCHEMA > table. A connection binds to one database;
//               `schema` is a real namespace inside it, and two tables may share
//               a name across schemas. `schema` is MEANINGFUL and must be
//               carried, or the name is ambiguous.
//   MySQL:      database > table. There is no second layer — MySQL's own
//               `information_schema.TABLES.TABLE_SCHEMA` column is a synonym for
//               the DATABASE, not an additional namespace. A table name is
//               already unique within a database.
//
// So this type does NOT force a schema onto MySQL, and does not invent a
// synthetic one (`""`, `"def"`, or the database name repeated) to make the two
// look alike. The schema component is genuinely OPTIONAL:
//
//   * Unqualified (`schema_` empty) — the name is complete on its own. This is
//     what MySQL, SQLite and SQL Server drivers produce, and it is not a
//     degraded or "missing" state.
//   * Qualified   (`schema_` set)   — the name means nothing without it. This is
//     what the PostgreSQL driver produces, always, including for `public`.
//
// Key() renders exactly that distinction: `orders` on MySQL, `public.orders` and
// `archive.orders` on PostgreSQL. A MySQL key is therefore BYTE-IDENTICAL to what
// this program produced before this type existed, which is what makes the change
// safe for anything already persisted (see the Key() contract below).
//
// ===========================================================================
// THE IMPLICIT CONSTRUCTOR, AND WHY IT IS NOT THE HAZARD IT LOOKS LIKE
// ===========================================================================
// Construction from a bare wxString is implicit. That is a deliberate, bounded
// concession: this type threads through ~170 call sites across six drivers, and
// requiring an explicit conversion at every one of them would have meant editing
// MySqlDriver.cpp (978/1000) and PgDriver.cpp (940/1000) far past the charter's
// line ceiling for no correctness gain.
//
// It is safe because an implicit conversion here can only produce an UNQUALIFIED
// name — i.e. exactly the behaviour that call site already had. It cannot
// silently STRIP a schema: there is no implicit conversion back to wxString, so
// `wxString s = tbl;` does not compile and every place that wants a bare string
// must say which half it means (`.Name()`) or ask for the identity (`.Key()`).
// The dangerous direction is the one that is closed.
//
// NOTE ON ui::TableKey. That type's deleted integral constructors are a separate
// and still-load-bearing guarantee ("a row index can never become an identity").
// This type does not weaken it: ui::TableKey still takes only an explicit
// wxString, and what changed is merely WHICH string — Key() instead of a bare
// name. QualifiedName has no integral constructor to abuse either.
#pragma once

#include <wx/string.h>

namespace db {

class QualifiedName {
public:
    QualifiedName() = default;

    // Unqualified — see the header note. Implicit on purpose.
    QualifiedName(const wxString& name) : name_(name) {}
    QualifiedName(const wchar_t* name) : name_(name) {}
    QualifiedName(const char* name) : name_(name) {}

    // Qualified. Two arguments cannot be written by accident.
    QualifiedName(wxString schema, wxString name)
        : schema_(std::move(schema)), name_(std::move(name)) {}

    const wxString& Schema() const { return schema_; }
    const wxString& Name() const { return name_; }

    // True when the schema component is meaningful (PostgreSQL). False is NOT an
    // error state — it is the correct and complete answer for MySQL/SQLite.
    bool HasSchema() const { return !schema_.IsEmpty(); }
    bool IsEmpty() const { return name_.IsEmpty(); }

    // ---- THE IDENTITY STRING ------------------------------------------------
    // The stable key. `schema.name` when qualified, `name` when not.
    //
    // ROUND-TRIP CONTRACT: Key() and Parse() are exact inverses for every name
    // this program can produce, and Parse(Key(x)) == x is pinned by
    // QualifiedNameTests. Everything that produces or consumes a ui::TableKey
    // goes through this pair, so the grid, the selection model, the execution
    // spec and the plan all spell one table the same way.
    //
    // A dot inside an identifier (`my.table`) would make the rendering ambiguous.
    // That is not hypothetical on PostgreSQL, where `CREATE TABLE "a.b"` is
    // legal, so the dot is ESCAPED rather than hoped about — see Parse().
    wxString Key() const
    {
        if (!HasSchema()) return Escape(name_);
        return Escape(schema_) + L"." + Escape(name_);
    }

    // Recover a QualifiedName from Key(). Splits on the first UNESCAPED dot.
    static QualifiedName Parse(const wxString& key)
    {
        for (size_t i = 0; i < key.length(); ++i) {
            if (key[i] == L'\\') { ++i; continue; }      // escaped char, skip pair
            if (key[i] == L'.')
                return QualifiedName(Unescape(key.substr(0, i)),
                                     Unescape(key.substr(i + 1)));
        }
        return QualifiedName(Unescape(key));
    }

    // Dialect-neutral display text. NOT SQL and never quoted for execution —
    // rendering a real statement is the driver's job (QuoteIdent).
    wxString Display() const
    {
        return HasSchema() ? schema_ + L"." + name_ : name_;
    }

    // Identity comparison is over BOTH components. Two tables named `orders` in
    // two schemas are not equal, which is the entire point of this type.
    bool operator==(const QualifiedName& o) const
    {
        return schema_ == o.schema_ && name_ == o.name_;
    }
    bool operator!=(const QualifiedName& o) const { return !(*this == o); }

    bool operator<(const QualifiedName& o) const
    {
        const int c = schema_.Cmp(o.schema_);
        return c != 0 ? c < 0 : name_.Cmp(o.name_) < 0;
    }

    // Convenience for the very common "does this name match a bare string?"
    // question inside a single-schema driver. Compares the NAME half only, so it
    // must not be used to decide identity across schemas.
    bool NameIs(const wxString& n) const { return name_ == n; }

private:
    static wxString Escape(const wxString& s)
    {
        wxString out;
        out.reserve(s.length());
        for (size_t i = 0; i < s.length(); ++i) {
            if (s[i] == L'\\' || s[i] == L'.') out += L'\\';
            out += s[i];
        }
        return out;
    }

    static wxString Unescape(const wxString& s)
    {
        wxString out;
        out.reserve(s.length());
        for (size_t i = 0; i < s.length(); ++i) {
            if (s[i] == L'\\' && i + 1 < s.length()) ++i;
            out += s[i];
        }
        return out;
    }

    wxString schema_;   // empty = unqualified; see the header's two-models note
    wxString name_;
};

} // namespace db
