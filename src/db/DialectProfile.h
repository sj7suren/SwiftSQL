// DialectProfile.h — extensible per-dialect column/field editor abstraction for
// the Table Design view (ADR-014). This is a *separate* bounded context from the
// sync engine's RenderSchemaChange/SchemaChangeSet: sync = "diff existing
// structures, rawType is truth"; table design = "synthesize a new column from the
// parts the user typed, support rename, and ask which attributes a type accepts".
//
// Pure db/ layer: depends only on wx/string + DbDriver.h (Dialect / QuoteIdent /
// DbType, transitively SchemaModel.h / ColKind). No UI, no vendor headers. Concrete
// profiles (MySqlProfile, PgProfile, …) live in their own .cpp files and share the
// detail:: fragment primitives (QuoteIdent / RenderDefault / JoinIdents / …) with
// the sync path — the sole coupling point, locked down by golden-DDL unit tests.
#pragma once

#include <wx/string.h>
#include <utility>
#include <vector>

#include "core/DbTypes.h"     // DbType (registry key)
#include "db/DbDriver.h"      // Dialect / QuoteIdent (+ transitively SchemaModel.h / ColKind)
#include "db/SyncTypeMap.h"   // CanonicalType/TypeVerdict — foundational type-system
                              // value objects (namespace db, NOT db::sync), shared with
                              // the sync engine. Deliberately NOT SchemaDelta.h/SchemaDiff.h:
                              // DialectProfile must never reference SchemaChangeSet/SyncPlan/
                              // anything sync-*orchestration*-specific (one-directional: sync
                              // consumes DialectProfile, never the reverse).

namespace db {

// ---- forward metadata: data-driven type catalog + attribute catalog ----

// One row of a dialect's type catalog. Drives the type-name autocomplete dropdown
// and whether the length/precision cells are editable for the selected type.
// aliasOf names the canonical type when this entry is a synonym ("" otherwise).
struct TypeDescriptor {
    wxString name;                  // "varchar", "int8", "timestamptz"…
    ColKind  kind = ColKind::Other; // drives ApplicableAttrs filtering
    bool     takesLength = false;   // length cell editable (varchar(N), decimal(P,…))
    bool     takesScale = false;    // scale cell editable (decimal(P,S))
    bool     lengthRequired = false;// length is mandatory (e.g. varchar with no default)
    wxString category;              // grouping label for the dropdown ("字符串"…)
    wxString aliasOf;               // canonical type name when this is a synonym ("" = none)
};

// One row of a dialect's attribute catalog — a long-tail property surfaced in the
// right-hand dialect-difference panel. editor picks the control; appliesTo gates
// the property by the selected type's ColKind (empty = applies to every type);
// core=true marks properties promoted to ColumnModel's strong-typed core.
struct AttrDescriptor {
    wxString              id;                 // stable key, also the ColumnModel.attrs key
    wxString              label;             // localized display label
    enum Editor { Bool, Text, Int, Choice, Expr };
    Editor                editor = Text;      // control kind for the property cell
    std::vector<wxString> choices;            // candidate values (Choice editor)
    std::vector<ColKind>  appliesTo;          // gate by type kind ([] = universal)
    bool                  core = false;       // true = mirrored by a core ColumnModel field
};

// ---- the edit model: strong-typed core + long-tail attribute bag ----

// The editor's working model of one column. Strong-typed core fields carry the
// "≥3-dialect-shared" essentials; everything dialect-specific lives in the attrs
// bag, keyed by AttrDescriptor.id. (This is NOT ColumnInfo — that stays frozen as
// a GetColumns introspection DTO; edit-time properties belong here.)
struct ColumnModel {
    // --- strong-typed core ---
    wxString name;             // current column name
    wxString origName;         // name before edit ("" for a freshly added column); a
                               // non-empty origName differing from name signals a rename
    wxString type;             // type name as chosen from TypeDescriptor.name
    wxString length;           // length / precision text ("80", "10"); "" = N/A
    wxString scale;            // numeric scale text ("2"); "" = N/A
    bool     notNull = false;
    wxString defaultVal;       // default value/expression text ("" = none)
    wxString comment;          // column COMMENT ("" = none)
    wxString collation;        // per-column collation ("" = inherit)
    bool     autoIncrement = false;
    wxString generatedExpr;    // generated-column expression ("" = not generated)
    bool     generatedStored = false;  // STORED vs VIRTUAL when generatedExpr set

    // --- long-tail attribute bag (key = AttrDescriptor.id) ---
    std::vector<std::pair<wxString, wxString>> attrs;

    // Value of a bag attribute by id ("" when absent).
    wxString Attr(const wxString& id) const;
};

// One pending column change within a TableEdit.
struct ColumnEdit {
    enum Op { Add, Modify, Drop };
    Op          op = Add;
    ColumnModel model;         // desired column (Add/Modify); Drop keys on model.name
    // Positional hint. For Add: AFTER <afterColumn> ("" = append). For Modify with
    // `positioned` (a column reorder, MySQL AFTER/FIRST): "" = FIRST, else AFTER <col>.
    wxString    afterColumn;
    bool        positioned = false;  // Modify carries a FIRST/AFTER clause (reorder)
};

// ---- index edit model (ADR-014, table-scope extension) ----

// The editor's working model of one index. Richer than the sync path's NormIndex
// (which carries only name/columns/unique/primary): the design view also edits the
// dialect index *type* (UNIQUE / FULLTEXT / SPATIAL — from IndexTypes()), the
// storage *method* (BTREE / HASH — from IndexMethods()), and a per-index comment.
// `type` is the single index-kind selector ("" = a plain, non-unique index).
struct IndexModel {
    wxString              name;
    std::vector<wxString> columns;
    wxString              type;      // "" / "UNIQUE" / "FULLTEXT" / "SPATIAL" (dialect)
    wxString              method;    // "" / "BTREE" / "HASH" (dialect); "" = engine default
    wxString              comment;
    wxString              origName;  // name before edit ("" = freshly added index)
};

// One pending index change within a TableEdit. Drop keys on model.name.
struct IndexEdit {
    enum Op { Add, Drop };
    Op         op = Add;
    IndexModel model;
};

// ---- foreign-key edit model (ADR-014, table-scope extension) ----

// The editor's working model of one foreign key. Richer than the sync path's
// NormForeignKey: the design view also edits the referenced *schema* (refDb — a
// column the "referenced Schema" picker fills) which NormForeignKey omits. The
// onDelete/onUpdate actions come from FkActions().
struct ForeignKeyModel {
    wxString              name;
    std::vector<wxString> columns;
    wxString              refDb;       // referenced schema/db ("" = table's own schema)
    wxString              refTable;
    std::vector<wxString> refColumns;
    wxString              onDelete;    // "" / RESTRICT / CASCADE / SET NULL / … (FkActions())
    wxString              onUpdate;
    wxString              origName;    // name before edit ("" = freshly added FK)
};

// One pending foreign-key change within a TableEdit. Drop keys on model.name.
struct ForeignKeyEdit {
    enum Op { Add, Drop };
    Op              op = Add;
    ForeignKeyModel model;
};

// ---- trigger edit model (ADR-014, table-scope extension; drives the 触发器 tab) ----

// The editor's working model of one trigger. A deliberately simple timing/event/body
// shape that fits MySQL / SQLite (CREATE TRIGGER … FOR EACH ROW <body>). Dialects
// whose trigger model does not fit this shape (PG's function-based triggers, SQL
// Server, Oracle) report SupportsTriggers()=false and the tab is hidden there.
// List order (the 上移/下移 buttons) is a pure UI concern → no field here.
struct TriggerModel {
    wxString name;
    wxString timing;    // BEFORE / AFTER / INSTEAD OF (TriggerTimings())
    wxString event;     // INSERT / UPDATE / DELETE (TriggerEvents())
    wxString body;      // the FOR EACH ROW body (a single statement or BEGIN … END)
    wxString origName;  // name before edit ("" = freshly added trigger)
};

// One pending trigger change within a TableEdit. Drop keys on model.name.
struct TriggerEdit {
    enum Op { Add, Drop };
    Op           op = Add;
    TriggerModel model;
};

// ---- table-level options (ADR-014, table-scope extension; drives the 选项 tab) ----

// Whole-table storage/collation options. Every field is dialect-optional ("" = omit
// that clause); which fields a dialect actually emits is decided by its
// RenderOptionsChange override. TableOptionSpecs() supplies the UI's per-field
// dropdown candidates (e.g. MySQL engines).
struct TableOptions {
    wxString engine;        // MySQL storage engine (InnoDB / MyISAM / …)
    wxString charset;       // default character set
    wxString collation;     // default collation
    wxString rowFormat;     // MySQL ROW_FORMAT (DYNAMIC / COMPRESSED / …)
    wxString tablespace;    // PostgreSQL / Oracle tablespace
    wxString maxRows;       // MySQL MAX_ROWS ("" / "0" = unlimited/omit)
    wxString autoIncrement; // MySQL AUTO_INCREMENT start ("" / "0" = omit)
    wxString avgRowLength;  // MySQL AVG_ROW_LENGTH ("" / "0" = omit)
    wxString checksum;      // MySQL CHECKSUM ("" = omit; "0" / "1" only)
};

// A batch of edits against one table — the unit RenderAlter consumes. Grows from
// "columns only" toward "whole table" (ADR-014 table-scope extension): each new
// edit category is an independent vector (or a has*/value pair for the singletons)
// so a TableEdit can carry a mix.
struct TableEdit {
    wxString                     db;
    wxString                     table;
    std::vector<ColumnEdit>      columns;
    std::vector<IndexEdit>       indexes;
    std::vector<ForeignKeyEdit>  fks;
    std::vector<TriggerEdit>     triggers;
    bool                         hasOptions = false;   // 选项 tab edited
    TableOptions                 options;
    bool                         hasComment = false;   // 注释 tab edited
    wxString                     comment;              // new table comment (when hasComment)
};

// A whole-table snapshot — the input to RenderCreate (the TABLE DDL tab's full
// CREATE TABLE). Unlike TableEdit (a set of deltas), this is the complete desired
// structure; the primary key is a plain ordered column list here (not per-column).
struct TableModel {
    wxString                     db;
    wxString                     table;
    std::vector<ColumnModel>     columns;
    std::vector<wxString>        primaryKey;   // ordered PK columns ("" = no PK)
    std::vector<IndexModel>      indexes;      // secondary indexes (PK excluded)
    std::vector<ForeignKeyModel> fks;
    std::vector<TriggerModel>    triggers;
    TableOptions                 options;      // engine/charset/… (dialect-optional)
    bool                         hasOptions = false;
    wxString                     comment;      // table comment ("" = none)
};

// ---- the abstraction ----

// A per-dialect (per-DbType) strategy: forward metadata (Types / Attributes /
// ApplicableAttrs) drives the editor UI; backward rendering (RenderTypeSpec /
// RenderColumnBody / RenderAlter) turns an edit model into this dialect's DDL.
// Concrete profiles subclass one of the two Template-Method bases below rather
// than this directly.
class DialectProfile {
public:
    virtual ~DialectProfile() = default;

    // Which SQL dialect this profile renders (drives identifier quoting via Q()).
    virtual Dialect GetDialect() const = 0;

    // --- forward metadata ---
    virtual const std::vector<TypeDescriptor>& Types() const = 0;
    virtual const std::vector<AttrDescriptor>& Attributes() const = 0;

    // Index-kind candidates for the design view's index-type dropdown, in display
    // order. "" is the implicit plain (non-unique) index and is NOT listed here; a
    // dialect lists its extra kinds, e.g. MySQL → {"UNIQUE","FULLTEXT","SPATIAL"}.
    // Base default: none (only a plain index) — concrete profiles override.
    virtual std::vector<wxString> IndexTypes() const { return {}; }
    // Storage-method candidates for the index "method" dropdown (MySQL → {"BTREE",
    // "HASH"}). Empty ⇒ the method cell is hidden/disabled. Base default: none.
    virtual std::vector<wxString> IndexMethods() const { return {}; }

    // Referential-action candidates for the FK ON DELETE / ON UPDATE dropdowns.
    // "" (the engine default, usually NO ACTION/RESTRICT) is implicit and not listed.
    // Base default: the four portable actions; dialects trim/extend (MySQL has no
    // real SET DEFAULT, SQLite adds it).
    virtual std::vector<wxString> FkActions() const
    { return { L"RESTRICT", L"CASCADE", L"SET NULL", L"NO ACTION" }; }

    // Descriptors for the table-level 选项 tab (engine / charset / tablespace …),
    // reusing the CREATE-DATABASE option descriptor shape. Base default: none (the
    // tab shows empty for dialects without alterable table options); MySQL populates
    // engine + charset + collation + row_format.
    virtual std::vector<DbCreateOption> TableOptionSpecs() const { return {}; }

    // Whether the design view's 触发器 tab is offered for this dialect. True where the
    // simple timing/event/body model renders correct DDL (MySQL / SQLite); false where
    // the trigger model differs too much (PG functions, SQL Server, Oracle) — the tab
    // is hidden there. Base default: true.
    virtual bool SupportsTriggers() const { return true; }

    // Whether existing columns can be reordered via ALTER (MySQL: MODIFY … AFTER/FIRST).
    // Base default: false — PG/SQL Server/Oracle/SQLite must rebuild the table to
    // reorder, so the design view's 上移/下移 produces no reorder DDL there.
    virtual bool SupportsColumnReorder() const { return false; }
    // Candidate trigger timings / events for the tab's dropdowns.
    virtual std::vector<wxString> TriggerTimings() const { return { L"BEFORE", L"AFTER" }; }
    virtual std::vector<wxString> TriggerEvents() const
    { return { L"INSERT", L"UPDATE", L"DELETE" }; }

    // Attributes applicable to the given type name: an attribute passes when its
    // appliesTo is empty (universal) or contains the type's ColKind. Non-virtual —
    // shared filtering over Types() + Attributes(). Unknown type → only universals.
    std::vector<AttrDescriptor> ApplicableAttrs(const wxString& typeName) const;

    // --- backward rendering ---
    // Just the type portion of a definition: "varchar(80)", "decimal(10,2)"…
    virtual wxString RenderTypeSpec(const ColumnModel& c) const = 0;
    // A whole column definition body: `name` <typespec> [NOT NULL] [DEFAULT …]
    // [COMMENT …] … — the piece spliced after ADD COLUMN and into CREATE TABLE.
    // Concrete profiles build this from the detail:: fragment primitives.
    virtual wxString RenderColumnBody(const ColumnModel& c) const = 0;
    // Render a TableEdit to this dialect's statements (no trailing ';'). The two
    // bases implement the whole-ALTER shell; concretes fill clause bodies.
    virtual bool RenderAlter(const TableEdit& edit,
                             std::vector<wxString>& stmts, wxString& err) const = 0;

    // Render a full CREATE TABLE (+ any follow-on CREATE INDEX / COMMENT / trigger
    // statements) from a whole-table snapshot — the TABLE DDL tab. SeparateAlter-
    // Profile provides a generic implementation (columns + PK + inline FK, then
    // indexes/comment/triggers as separate statements reusing the RenderAlter hooks);
    // the MySQL family overrides it to inline indexes/FKs/options/comment. Each
    // pushed string is one statement (no trailing ';').
    virtual bool RenderCreate(const TableModel& model,
                              std::vector<wxString>& stmts, wxString& err) const = 0;

    // --- cross-engine type mapping (published, read-only; the cross-db sync
    // engine is the consumer — see db/SyncTypeMap.h's file banner for why this
    // lives in `namespace db`, not `db::sync`) ---

    // This dialect's column -> the canonical, engine-neutral type the sync
    // engine compares across dialects. Base default (DialectProfile.cpp): a
    // generic passthrough (kind/length/scale/rawType copied as-is, no
    // dialect-specific unsigned/timezone/enum parsing) — an honest baseline for
    // a dialect that hasn't wired real cross-engine semantics yet. MySQL and
    // PostgreSQL override with SyncTypeMap's InterpretMySqlColumn/
    // InterpretPgColumn (A1); SQLite/SQL Server/Oracle keep the base default
    // (out of A3's scope — not wired to SyncTypeMap yet).
    virtual CanonicalType Interpret(const NormColumn& c) const;

    // Canonical type -> this dialect's type text. Base default (DialectProfile.
    // cpp): always false ("unsupported") — a dialect must opt in before the
    // sync engine ever treats a cross-engine column pairing involving it as
    // auto-alterable (MayAutoAlter gates on Render succeeding too, via
    // SchemaDelta::AddColumnChange upstream). `why` explains the refusal.
    virtual bool Render(const CanonicalType& t, wxString& out, wxString& why) const;

protected:
    // Quote an identifier for this dialect.
    wxString Q(const wxString& id) const { return QuoteIdent(id, GetDialect()); }
    // db.table (or bare table when db is empty), each part quoted.
    wxString QualifiedTable(const TableEdit& edit) const;

    // The uniform FK constraint body — shared by both RenderAlter shells since
    //   [CONSTRAINT <name>] FOREIGN KEY (<cols>) REFERENCES [<refDb>.]<refTable>
    //   (<refCols>) [ON DELETE <a>] [ON UPDATE <a>]
    // is standard SQL across every dialect. Only the ADD placement (inline clause
    // vs standalone ALTER) and the DROP verb (DROP FOREIGN KEY vs DROP CONSTRAINT)
    // differ, and those live in the two bases. Mirrors detail::FkDef (the sync
    // path's primitive), adding the referenced-schema qualifier the editor tracks.
    wxString RenderFkBody(const ForeignKeyModel& fk) const;

    // Append the whole-table singleton edits (options, comment) to stmts. Called by
    // BOTH RenderAlter shells after their column/index/FK work, so the extension
    // lives in one place instead of being duplicated in each shell.
    void RenderTableExtras(const TableEdit& edit, std::vector<wxString>& stmts) const;

    // Options change (选项 tab). Default: no-op — a dialect with alterable table
    // options overrides (MySQL → ALTER TABLE … ENGINE=… DEFAULT CHARSET=… …).
    // qualifiedTable is the already-quoted db.table.
    virtual void RenderOptionsChange(const wxString& qualifiedTable,
                                     const TableOptions& opts,
                                     std::vector<wxString>& stmts) const;

    // Table comment change (注释 tab). Default: no-op (dialects without a table
    // comment — SQLite / SQL Server — safely emit nothing). MySQL overrides with
    // ALTER TABLE … COMMENT=…; PG / Oracle with COMMENT ON TABLE … IS …. comment is
    // the raw (unescaped) text; the override escapes it. qualifiedTable is quoted.
    virtual void RenderCommentChange(const wxString& qualifiedTable,
                                     const wxString& comment,
                                     std::vector<wxString>& stmts) const;

    // Trigger change (触发器 tab). Default (DialectProfile.cpp): the MySQL/SQLite form
    //   Add  → CREATE TRIGGER <name> <timing> <event> ON <qt> FOR EACH ROW <body>
    //   Drop → DROP TRIGGER <name>
    // Dialects with SupportsTriggers()=false override this to an honest "-- " skip
    // note. qualifiedTable is the already-quoted db.table.
    virtual void RenderTriggerChange(const wxString& qualifiedTable,
                                     const TriggerEdit& te,
                                     std::vector<wxString>& stmts) const;
};

// Template-Method base for the MySQL family (MySQL / MariaDB / OceanBase): one
// ALTER TABLE with the per-column ADD/MODIFY/CHANGE/DROP COLUMN clauses comma-
// joined. Concretes supply ModifyClause (which chooses MODIFY vs CHANGE and, for a
// rename, splices old→new in one clause).
class InlineAlterProfile : public DialectProfile {
public:
    bool RenderAlter(const TableEdit& edit,
                     std::vector<wxString>& stmts, wxString& err) const override;

protected:
    // The full clause for a Modify column, e.g.
    //   MODIFY COLUMN `c` int NOT NULL      (no rename)
    //   CHANGE COLUMN `old` `new` int       (rename)
    // Return "" to emit nothing for this column. Excludes the leading "ALTER TABLE".
    virtual wxString ModifyClause(const ColumnModel& c) const = 0;

    // The clause for one index Add/Drop, folded into the SAME ALTER TABLE as the
    // column clauses (MySQL allows `ALTER TABLE t ADD COLUMN …, ADD INDEX …`).
    // Excludes the leading "ALTER TABLE"; return "" to emit nothing. Default: ""
    // (no index support) — the MySQL family overrides.
    virtual wxString IndexClause(const IndexEdit& ie) const { (void)ie; return wxString(); }

    // The clause for one FK Add/Drop, folded into the same ALTER TABLE. Default
    // (DialectProfile.cpp) is the MySQL-family form:
    //   Add  → ADD <RenderFkBody>
    //   Drop → DROP FOREIGN KEY <name>
    // Uniform across MySQL/MariaDB/OceanBase, so the base implements it; excludes
    // the leading "ALTER TABLE". Return "" to emit nothing.
    virtual wxString FkClause(const ForeignKeyEdit& fe) const;
};

// Template-Method base for the standalone-ALTER dialects (PostgreSQL / SQL Server /
// Oracle / SQLite): each column change is its own statement. The shell handles the
// syntactically-uniform Add / Drop; a Modify is delegated wholesale to the concrete
// (ADR-014 #4). Add / Drop stay base-owned because "ALTER TABLE t ADD/DROP COLUMN"
// is identical across these dialects.
class SeparateAlterProfile : public DialectProfile {
public:
    bool RenderAlter(const TableEdit& edit,
                     std::vector<wxString>& stmts, wxString& err) const override;
    // Generic full CREATE TABLE: columns + PRIMARY KEY + inline FK constraints, then
    // secondary indexes / table comment / triggers as separate follow-on statements
    // (reusing RenderIndexChange / RenderCommentChange / RenderTriggerChange).
    bool RenderCreate(const TableModel& model,
                      std::vector<wxString>& stmts, wxString& err) const override;

protected:
    // The ADD clause for one column, excluding the leading "ALTER TABLE <t> ".
    // Default: standard "ADD COLUMN <body>" (PostgreSQL / SQLite). SQL Server and
    // Oracle have no COLUMN keyword after ADD — they override to "ADD <body>".
    virtual wxString AddColumnClause(const ColumnModel& c) const;

    // Emit the complete statement(s) for one Modify column into `stmts`. The
    // concrete owns everything a modify entails and decides its own SQL shapes:
    //   - a rename via "ALTER TABLE … RENAME COLUMN old TO new" (PG/Oracle/SQLite)
    //     or "EXEC sp_rename 'tbl.old','new','COLUMN'" (SQL Server);
    //   - the type/attribute change ("ALTER COLUMN … TYPE / SET NOT NULL /
    //     SET|DROP DEFAULT", "COMMENT ON COLUMN …", …).
    // Each pushed string is a whole statement (no trailing ';'); the base does NOT
    // wrap them — this is what lets SQL Server emit sp_rename instead of ALTER TABLE.
    // qualifiedTable is the already-quoted db.table for statements that need it.
    virtual void RenderColumnChange(const wxString& qualifiedTable,
                                    const ColumnModel& c,
                                    std::vector<wxString>& stmts) const = 0;

    // Emit whole statement(s) for one index Add/Drop. Unlike the MySQL family,
    // these dialects create/drop indexes as standalone statements, not ALTER TABLE
    // clauses. Default (DialectProfile.cpp): ANSI
    //   ADD  → CREATE [UNIQUE] INDEX <name> ON <qt> (<cols>)
    //   Drop → DROP INDEX <name>
    // Concretes override for quirks (PG `USING <method>`, SQL Server `DROP INDEX
    // <name> ON <qt>`, …). qualifiedTable is the already-quoted db.table.
    virtual void RenderIndexChange(const wxString& qualifiedTable,
                                   const IndexEdit& ie,
                                   std::vector<wxString>& stmts) const;

    // Emit whole statement(s) for one FK Add/Drop. Default (DialectProfile.cpp):
    //   Add  → ALTER TABLE <qt> ADD <RenderFkBody>
    //   Drop → ALTER TABLE <qt> DROP CONSTRAINT <name>
    // Standard SQL for PG / SQL Server / Oracle. SQLite (which cannot add a FK to an
    // existing table) overrides to an honest "-- " skip note. qualifiedTable is the
    // already-quoted db.table.
    virtual void RenderFkChange(const wxString& qualifiedTable,
                                const ForeignKeyEdit& fe,
                                std::vector<wxString>& stmts) const;
};

// Factory: per-DbType singleton, mirroring CreateConnection(DbType). Implemented by
// the registry (T6, DialectRegistry.cpp) — declared here only.
const DialectProfile& GetDialectProfile(DbType type);

// Overload keyed by the coarser `Dialect` enum (db::Dialect — MySQL/Postgres/
// Sqlite/SqlServer/Oracle) rather than DbType. Dialect already IS the same
// family grouping DbType maps down to (DbType::MariaDB/OceanBase -> MySQL
// family, DbType::KingBase -> PG family, DbType::DM -> Oracle family) — this
// overload exists so a caller that only has an IConnection::GetDialect()
// result (the cross-db sync engine, A4) doesn't need to reverse-engineer a
// representative DbType just to look up a profile.
const DialectProfile& GetDialectProfile(Dialect d);

} // namespace db
