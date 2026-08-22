// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SchemaDiff.cpp — see header. Pure structural diff; no connection, no wx UI.
#include "db/SchemaDiff.h"
#include "db/DialectProfile.h"   // GetDialectProfile(Dialect) + Interpret/Render (A3)

namespace db::sync {

namespace {

// ---- normalized signatures (match by shape, not by name) ------------------
// An index/FK is "the same" if its column set + kind matches. A changed index
// (same name, different columns) therefore surfaces naturally as Drop(old) +
// Add(new) because the two signatures differ. Dialect-agnostic — unchanged by
// A4 (index/FK signatures never depended on rawType/kind).

wxString JoinCols(const std::vector<wxString>& cols)
{
    wxString s;
    for (size_t i = 0; i < cols.size(); ++i) s += (i ? L"," : L"") + cols[i];
    return s;
}

wxString IndexSig(const NormIndex& ix)
{
    // primary/unique are load-bearing: a UNIQUE index and a PRIMARY KEY on the
    // same columns are different objects.
    return JoinCols(ix.columns) + L"|u=" + (ix.unique ? L"1" : L"0") +
           L"|p=" + (ix.primary ? L"1" : L"0");
}

wxString FkSig(const NormForeignKey& fk)
{
    return JoinCols(fk.columns) + L"=>" + fk.refTable + L"(" +
           JoinCols(fk.refColumns) + L")";
}

// Non-type facets every Modify decision (either dialect path) checks the same
// way: nullability, default, auto-increment.
bool NonTypeFacetsDiffer(const NormColumn& s, const NormColumn& t)
{
    if (s.notNull != t.notNull)             return true;
    if (s.hasDefault != t.hasDefault)       return true;
    if (s.hasDefault && t.hasDefault && s.defaultExpr != t.defaultExpr)
        return true;
    if (s.autoIncrement != t.autoIncrement) return true;
    return false;
}

// Same-dialect: exact-text compare (rawType is the true value within one
// engine) — byte-for-byte identical to the pre-A4 ColumnDiffers same-dialect
// branch. This is what keeps every same-dialect golden/unit test unchanged.
bool ColumnDiffersSameDialect(const NormColumn& s, const NormColumn& t)
{
    if (s.rawType != t.rawType) return true;
    return NonTypeFacetsDiffer(s, t);
}

// Cross-dialect: two columns are type-equal iff the SOURCE, translated into the
// TARGET dialect by the SAME canonical round-trip the CREATE/Add path uses
// (Interpret(src) -> tgtProfile.Render), equals what the target already holds.
// This is the whole point of the unification: the previous version compared the
// source-canonical shape (kind/length/scale) against the target-canonical shape,
// which called MySQL `int` (canonical length 10, from numeric_precision) and its
// OWN correct auto-create translation PG `bigint` (canonical length 64)
// DIFFERENT — a phantom Modify against a freshly auto-created table, whose PG
// renderer then dragged a `DROP DEFAULT` onto the identity column and errored.
// TranslateSchemaForCreate says "int becomes bigint, equivalent"; this now says
// the same thing because it asks the same function, so the two can never
// disagree and a sync-then-resync is a no-op.
//
// Both sides are normalized through the TARGET dialect's Render so their type
// TEXT is commensurable: PG introspection stores data_type WITHOUT the length
// modifier ("character varying"), while Render emits it inline ("character
// varying(80)") — reading the target's rawType directly would mis-compare on
// spelling alone. Rendering the target through its own Interpret+Render is
// idempotent on the target's native types, so the comparison reduces to exactly
// "is the target already what we would have created". Genuine differences still
// surface wherever Render encodes them: varchar(50) vs varchar(80) render to
// distinct text (Modify), decimal(10,2) vs decimal(12,2) likewise. Integer/float
// WIDTH differences collapse (all integer widths render to "bigint", all floats
// to "double precision") — which is correct, not blind: the create/alter path
// can only ever express those widened forms, so treating them as equal is the
// only self-consistent choice and is what makes idempotency hold.
bool ColumnDiffersCrossDialect(const NormColumn& s, const NormColumn& t,
                               const DialectProfile& srcProfile,
                               const DialectProfile& tgtProfile)
{
    const CanonicalType sCanon = srcProfile.Interpret(s);
    const CanonicalType tCanon = tgtProfile.Interpret(t);
    wxString sText, tText, why;
    const bool sOk = tgtProfile.Render(sCanon, sText, why);
    const bool tOk = tgtProfile.Render(tCanon, tText, why);
    if (sOk && tOk) {
        if (sText != tText) return true;   // genuinely different target-native type
    } else {
        // Defensive: a MayAutoAlter pairing whose Render nonetheless refuses
        // (e.g. an ENUM whose member list wasn't captured). Fall back to the
        // engine-neutral canonical-shape compare rather than assert equality.
        if (sCanon.kind != tCanon.kind)     return true;
        if (sCanon.length != tCanon.length) return true;
        if (sCanon.scale != tCanon.scale)   return true;
    }
    return NonTypeFacetsDiffer(s, t);
}

// ---- cross-engine whole-table CREATE translation (A5) ---------------------
// Build a target-dialect-native copy of `source` for a cross-engine CREATE
// TABLE, so the target driver's EXISTING RenderCreateTable (which renders each
// column's rawType verbatim) produces correct target DDL with no new render
// path. The per-column type translation is the SAME A4 path the cross-engine
// Add/Modify branches use — srcProfile.Interpret(NormColumn) -> CanonicalType,
// tgtProfile.Render(CanonicalType) -> target text — NOT a fork of the type
// logic.
//
// Rulings implemented here (all decided upstream; see the file's call site):
//  1. Any column whose type is Unmappable to the target ⇒ refuse the WHOLE
//     table (return false, fill `refusal` naming the column + its source type).
//     A table missing a column is not a table; a partial CREATE is worse than
//     an honest refusal — the same discipline the value layer uses.
//  2. Foreign keys are OMITTED from the CREATE and a warning is appended: an FK
//     may reference a table not yet on the target, so the correct order is
//     create-all-tables-then-add-FKs (same reason same-dialect sync topologically
//     orders and defers). No cross-dialect FK translation is attempted here.
//  3. Indexes are carried as-is (column lists are portable; the target driver's
//     RenderCreateTable owns the CREATE INDEX / inline syntax). An index over an
//     unmappable column can't survive because ruling 1 already refused the table.
//  4. Identity / AUTO_INCREMENT: the NormColumn.autoIncrement flag rides through
//     unchanged, so the target driver expresses it in its own dialect
//     (AUTO_INCREMENT / GENERATED … AS IDENTITY) best-effort — never a refusal.
//  5. PRIMARY KEY is carried (source.primaryKey copied), essential for the
//     subsequent data sync which needs a key.
//  6. Column DEFAULTs are OMITTED and warned (same discipline as ruling 2): a
//     default is source-dialect TEXT whose validity in the target is not
//     something this layer can guarantee — a MySQL `tinyint(1) DEFAULT 1` is an
//     integer literal that a PostgreSQL `boolean` column rejects outright, and a
//     PG `boolean DEFAULT false` is not a valid MySQL `tinyint` default either
//     (both observed against live servers). Rather than emit a default that is
//     silently wrong OR breaks the whole CREATE, drop it — the column stays
//     usable (the data sync supplies every value explicitly) and the warning
//     tells the user which table needs its defaults re-added by hand.
//
// `out` is only meaningful when this returns true.
bool TranslateSchemaForCreate(const TableSchema& source,
                              const DialectProfile& srcProfile,
                              const DialectProfile& tgtProfile,
                              const wxString& tableKey,
                              TableSchema& out,
                              Finding& refusal,
                              std::vector<Finding>& warnings)
{
    out = source;              // carries name, primaryKey (ruling 5), indexes (ruling 3),
                               // engine/charset/collation/comment
    out.columns.clear();
    out.foreignKeys.clear();   // ruling 2: FKs never enter the CREATE

    bool droppedDefault = false;
    for (const NormColumn& sc : source.columns) {
        // Exactly the cross-engine Add path: interpret in the source profile,
        // render in the target profile. A false Render == this column's type has
        // no safe target expression.
        CanonicalType canon = srcProfile.Interpret(sc);
        wxString typeText, why;
        if (!tgtProfile.Render(canon, typeText, why)) {
            refusal = { tableKey, sc.name, TypeVerdict::Unmappable,
                L"表 " + tableKey + L" 无法自动建表：列 " + sc.name +
                L"（源类型 " + sc.rawType + L"）无法映射到目标方言" +
                (why.IsEmpty() ? wxString() : L"（" + why + L"）") +
                L"，已拒绝整表建表（请人工建表后重试）" };
            return false;      // ruling 1: one Unmappable column refuses the whole table
        }
        NormColumn tc = sc;
        tc.rawType = typeText;   // target-dialect-native now
        tc.kind    = canon.kind; // keep kind consistent with the rendered text
        // ruling 6: a source-dialect default literal/expression is not portable —
        // drop it (autoIncrement columns never carry a value default anyway).
        if (tc.hasDefault && !tc.autoIncrement) droppedDefault = true;
        tc.hasDefault  = false;
        tc.defaultExpr = wxString();
        out.columns.push_back(std::move(tc));
    }

    // ruling 2: name the table so the user knows FKs need manual addition.
    if (!source.foreignKeys.empty())
        warnings.push_back({ tableKey, wxString(), TypeVerdict::Lossy,
            L"表 " + tableKey + L" 的外键未随自动建表迁移（跨引擎自动建表不携带外键）；"
            L"请在所有表建好后手工添加外键" });

    // ruling 6: name the table so the user knows defaults were not carried.
    if (droppedDefault)
        warnings.push_back({ tableKey, wxString(), TypeVerdict::Lossy,
            L"表 " + tableKey + L" 的列默认值未随跨引擎自动建表迁移"
            L"（默认值表达式跨方言不保证有效）；如需请人工补充" });

    return true;
}

} // namespace

SchemaDiffResult DiffSchema(const TableSchema& source, const TableSchema& target,
                            Dialect srcDialect, Dialect tgtDialect)
{
    SchemaDiffResult res;
    SchemaChangeSet& ch = res.changes;
    // The change set is keyed by IDENTITY (schema-qualified where the engine
    // has schemas), matching SyncPlan::TableUnit::table and ui::TableKey.
    ch.table = (source.name.IsEmpty() ? target.name : source.name).Key();
    const bool sameDialect = (srcDialect == tgtDialect);

    const bool srcExists = !source.columns.empty();
    const bool tgtExists = !target.columns.empty();

    // ---- whole-table create / drop short-circuits -------------------------
    if (srcExists && !tgtExists) {
        if (sameDialect) {
            // Same-engine: source rawType renders as-is in the target driver.
            ch.createTable  = true;
            ch.createSchema = source;
            return res;
        }
        // Cross-engine whole-table CREATE (A5): translate every column's type
        // through the SAME A4 per-column Interpret(src)->Render(tgt) path the
        // cross-engine Add/Modify branches use, then hand the target-native
        // schema to the target driver's existing RenderCreateTable — no new
        // render path. Any Unmappable column refuses the whole table (ruling 1);
        // FKs are omitted with a warning (ruling 2); indexes/PK/identity ride
        // through (rulings 3/4/5). See TranslateSchemaForCreate.
        const DialectProfile& sp = GetDialectProfile(srcDialect);
        const DialectProfile& tp = GetDialectProfile(tgtDialect);
        TableSchema translated;
        Finding     refusal;
        if (!TranslateSchemaForCreate(source, sp, tp, ch.table, translated,
                                      refusal, res.warnings)) {
            res.warnings.push_back(refusal);   // honest refusal — NOT a createTable
            return res;
        }
        ch.createTable  = true;
        ch.createSchema = std::move(translated);
        return res;
    }
    if (!srcExists && tgtExists) {
        ch.dropTable = true;   // opt-in: the caller decides whether to keep it
        return res;
    }
    if (!srcExists && !tgtExists) return res;   // nothing on either side

    // Per-side profiles, only fetched when actually needed (cross-dialect).
    const DialectProfile* srcProfile = sameDialect ? nullptr : &GetDialectProfile(srcDialect);
    const DialectProfile* tgtProfile = sameDialect ? nullptr : &GetDialectProfile(tgtDialect);

    // ---- column-by-column diff (aligned by name) --------------------------
    for (size_t i = 0; i < source.columns.size(); ++i) {
        const NormColumn& sc = source.columns[i];
        const wxString afterHint = (i > 0) ? source.columns[i - 1].name : wxString();
        const NormColumn* tc = target.FindColumn(sc.name);

        if (!tc) {
            // ---- Add ----
            if (sameDialect) {
                ColumnChange cc;
                cc.op = ColumnChange::Op::Add;
                cc.column = sc;
                cc.afterColumn = afterHint;
                ch.columns.push_back(std::move(cc));
                continue;
            }
            // Cross-dialect Add: the source's rawType is FOREIGN text to the
            // target — never hand it across verbatim (that is exactly the bug
            // PgSql.cpp's PgTypeText ternary fix (A2) closed off on the
            // rendering side). Translate via Interpret+Render first.
            CanonicalType srcCanon = srcProfile->Interpret(sc);
            wxString typeText, why;
            if (!tgtProfile->Render(srcCanon, typeText, why)) {
                res.warnings.push_back({ ch.table, sc.name, TypeVerdict::Unmappable, why });
                continue;   // honest skip — no ColumnChange for an unrenderable Add
            }
            NormColumn desired = sc;
            desired.rawType = typeText;   // target-dialect-native text now
            desired.kind    = srcCanon.kind;
            ColumnChange cc;
            cc.op = ColumnChange::Op::Add;
            cc.column = desired;
            cc.afterColumn = afterHint;
            ch.columns.push_back(std::move(cc));
            res.warnings.push_back({ ch.table, sc.name, TypeVerdict::Equivalent,
                L"新增列，已映射到目标方言类型 " + typeText });
            continue;
        }

        // ---- both sides have this column: Modify? ----
        if (sameDialect) {
            if (ColumnDiffersSameDialect(sc, *tc)) {
                ColumnChange cc;
                cc.op = ColumnChange::Op::Modify;
                cc.column = sc;                 // desired definition
                cc.hasCurrent = true;
                cc.current    = *tc;            // target's definition right now
                ch.columns.push_back(std::move(cc));
            }
            continue;
        }

        CanonicalType a = srcProfile->Interpret(sc);
        CanonicalType b = tgtProfile->Interpret(*tc);
        wxString reason;
        const TypeVerdict verdict = CompareCanonical(a, b, reason);

        if (!MayAutoAlter(verdict)) {
            // Structurally cannot become a ColumnChange — Lossy/Unmappable is
            // always a Finding, never a Modify. This is the "skip and warn"
            // path (see the golden test's file banner for the pre-A4 shape).
            res.warnings.push_back({ ch.table, sc.name, verdict, reason });
            continue;
        }
        if (!ColumnDiffersCrossDialect(sc, *tc, *srcProfile, *tgtProfile))
            continue;   // safe to compare AND already equal — nothing to do, no Finding

        wxString typeText, why;
        if (!tgtProfile->Render(a, typeText, why)) {
            // Render can still refuse even for a MayAutoAlter verdict in edge
            // cases (e.g. an ENUM whose member list wasn't captured) — degrade
            // honestly rather than emit unrenderable DDL.
            res.warnings.push_back({ ch.table, sc.name, TypeVerdict::Unmappable, why });
            continue;
        }
        NormColumn desired = sc;
        desired.rawType = typeText;
        desired.kind    = a.kind;
        ColumnChange cc;
        cc.op = ColumnChange::Op::Modify;
        cc.column = desired;
        // The target's CURRENT column, verbatim — its own dialect's rawType, not
        // a translation. A side-by-side view wants to show what is actually
        // there, which for a cross-engine Modify is precisely the interesting
        // half ("character varying(80)" vs the desired "varchar(80)").
        cc.hasCurrent = true;
        cc.current    = *tc;
        ch.columns.push_back(std::move(cc));
        res.warnings.push_back({ ch.table, sc.name, verdict, reason });   // audit trail
    }
    // Columns the target has but the source doesn't → Drop (opt-in downstream).
    // Name-only; no type text involved, so this is dialect-agnostic.
    for (const NormColumn& tc : target.columns) {
        if (!source.FindColumn(tc.name)) {
            ColumnChange cc;
            cc.op     = ColumnChange::Op::Drop;
            cc.column = tc;                 // name identifies the drop
            // A Drop's "current" IS the target column; setting it makes the flag
            // uniform ("hasCurrent => `current` is what the target has today")
            // so a consumer never special-cases the op to know whether to trust
            // the field.
            cc.hasCurrent = true;
            cc.current    = tc;
            ch.columns.push_back(std::move(cc));
        }
    }

    // ---- index diff (by signature) — dialect-agnostic, unchanged ----------
    for (const NormIndex& si : source.indexes) {
        const wxString sig = IndexSig(si);
        bool found = false;
        for (const NormIndex& ti : target.indexes)
            if (IndexSig(ti) == sig) { found = true; break; }
        if (!found) { IndexChange ic; ic.op = IndexChange::Op::Add; ic.index = si; ch.indexes.push_back(std::move(ic)); }
    }
    for (const NormIndex& ti : target.indexes) {
        const wxString sig = IndexSig(ti);
        bool found = false;
        for (const NormIndex& si : source.indexes)
            if (IndexSig(si) == sig) { found = true; break; }
        if (!found) { IndexChange ic; ic.op = IndexChange::Op::Drop; ic.index = ti; ch.indexes.push_back(std::move(ic)); }
    }

    // ---- foreign-key diff (by signature) — dialect-agnostic, unchanged ----
    for (const NormForeignKey& sf : source.foreignKeys) {
        const wxString sig = FkSig(sf);
        bool found = false;
        for (const NormForeignKey& tf : target.foreignKeys)
            if (FkSig(tf) == sig) { found = true; break; }
        if (!found) { FkChange fc; fc.op = FkChange::Op::Add; fc.fk = sf; ch.foreignKeys.push_back(std::move(fc)); }
    }
    for (const NormForeignKey& tf : target.foreignKeys) {
        const wxString sig = FkSig(tf);
        bool found = false;
        for (const NormForeignKey& sf : source.foreignKeys)
            if (FkSig(sf) == sig) { found = true; break; }
        if (!found) { FkChange fc; fc.op = FkChange::Op::Drop; fc.fk = tf; ch.foreignKeys.push_back(std::move(fc)); }
    }

    // ---- table options → a Finding, never a silent structural rewrite -----
    auto optWarn = [&](const wxString& what, const wxString& a, const wxString& b) {
        if (!a.IsEmpty() && !b.IsEmpty() && a != b)
            res.warnings.push_back({ ch.table, wxString(), TypeVerdict::Lossy,
                L"表 " + ch.table + L" 选项差异：" + what + L" " + b + L" → " + a +
                L"（未自动同步）" });
    };
    optWarn(L"engine",    source.engine,    target.engine);
    optWarn(L"charset",   source.charset,   target.charset);
    optWarn(L"collation", source.collation, target.collation);
    optWarn(L"comment",   source.comment,   target.comment);

    return res;
}

} // namespace db::sync
