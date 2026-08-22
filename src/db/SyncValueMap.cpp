// SyncValueMap.cpp — see header. Cross-engine cell VALUE conversion.
//
// Two-layer split, and the reason for it: everything expensive (rawType parsing,
// CanonicalType comparison, integer-range resolution, family classification)
// happens in BuildBridges, once per column pair. ConvertCell does no parsing of
// type text whatsoever — it switches on a precomputed BridgeOp, and in the common
// case does not even get that far (one `passthrough` test).
#include "db/SyncValueMap.h"

#include "db/DbDriver.h"       // db::Dialect (full definition), QuoteIdent
#include "db/DialectProfile.h" // GetDialectProfile(Dialect).Interpret

#include <climits>

namespace db::sync {
namespace {

// ---- kind families -------------------------------------------------------
// Value-level families, deliberately COARSER than SyncTypeMap's type-level
// comparison. A column pair may be too risky to auto-ALTER (TypeVerdict::Lossy —
// e.g. VARBINARY(16) -> bytea drops the length constraint) while its VALUES
// still cross perfectly. The schema ladder and the value ladder answer different
// questions; conflating them would block data sync on type nitpicks.
bool IsIntFamily(ColKind k)  { return k == ColKind::Integer || k == ColKind::Boolean; }
bool IsNumFamily(ColKind k)  { return k == ColKind::Decimal || k == ColKind::Float; }
bool IsBinFamily(ColKind k)  { return k == ColKind::Binary  || k == ColKind::Blob; }
bool IsTemporal(ColKind k)   { return k == ColKind::Date || k == ColKind::Time || k == ColKind::Timestamp; }
bool IsTextFamily(ColKind k)
{
    return k == ColKind::Char || k == ColKind::Varchar || k == ColKind::Text ||
           k == ColKind::Json || k == ColKind::Enum    || k == ColKind::Uuid;
}

// ---- integer range resolution -------------------------------------------
// Resolved ONCE per column at build time from the engine-native type text.
struct IntRange {
    long long lo = LLONG_MIN;
    long long hi = LLONG_MAX;
    bool      unsigned64 = false;   // hi is really 2^64-1 (MySQL BIGINT UNSIGNED)
    bool      known = false;
};

// `raw` is the engine-native type text (MySQL column_type "bigint unsigned",
// PG data_type "integer"). Order matters: "bigint"/"smallint"/"tinyint" must be
// tested before the bare "int" substring, which they all... do not contain, but
// "integer" does, so bigint-before-int is still the safe habit.
IntRange ResolveIntRange(const CanonicalType& t)
{
    IntRange r;
    const wxString s = t.rawType.Lower();
    const bool u = t.isUnsigned;

    auto set = [&](long long lo, long long hi) { r.lo = lo; r.hi = hi; r.known = true; };

    if (t.kind == ColKind::Boolean)          set(0, 1);
    else if (s.Contains(L"tinyint"))         u ? set(0, 255)        : set(-128, 127);
    else if (s.Contains(L"smallint"))        u ? set(0, 65535)      : set(-32768, 32767);
    else if (s.Contains(L"mediumint"))       u ? set(0, 16777215)   : set(-8388608, 8388607);
    else if (s.Contains(L"bigint")) {
        if (u) { r.lo = 0; r.hi = LLONG_MAX; r.unsigned64 = true; r.known = true; }
        else   set(LLONG_MIN, LLONG_MAX);
    }
    else if (s.Contains(L"int") || s.Contains(L"serial"))
                                             u ? set(0, 4294967295LL) : set(-2147483648LL, 2147483647LL);
    // Unknown integer spelling: leave `known` false. The caller then assumes the
    // widest possible source / refuses to narrow the target, which errs toward
    // checking more values rather than fewer.
    return r;
}

// True when a value legal for `src` might not fit `tgt` — the only case worth
// paying a per-value range check for.
bool NarrowerThan(const IntRange& src, const IntRange& tgt)
{
    if (!tgt.known) return false;                 // can't police an unknown target
    if (!src.known) return true;                  // unknown source: check every value
    if (src.unsigned64 && !tgt.unsigned64) return true;
    return src.lo < tgt.lo || src.hi > tgt.hi;
}

// ---- per-value helpers ---------------------------------------------------

// MySQL's zero date. Stored as a legal DATE/DATETIME value by MySQL, storable by
// no other engine we target. Ruling: Unrepresentable — NOT coerced to NULL.
bool IsZeroDate(const wxString& s)
{
    return s.StartsWith(L"0000-00-00");
}

// The text a driver produces for a boolean-ish value, normalized. Returns
// -1 when the text is not recognizably boolean (the caller then refuses the
// value rather than guessing).
int BoolValue(const wxString& raw)
{
    const wxString s = raw.Lower();
    if (s == L"1" || s == L"t" || s == L"true"  || s == L"y" || s == L"yes") return 1;
    if (s == L"0" || s == L"f" || s == L"false" || s == L"n" || s == L"no")  return 0;
    return -1;
}

// Post-decode Unicode sanity check.
//
// Honest scope note: the driver has already decoded bytes into a wxString by the
// time a Cell reaches us, so a byte-level UTF-8 validator is no longer possible
// here. What IS detectable is the evidence a failed decode leaves behind — the
// U+FFFD replacement character, a NUL (legal in a MySQL blob/text byte string,
// rejected outright by PostgreSQL text), or a string that will not round-trip
// back to UTF-8. Any of those => Unrepresentable, per the ruling.
bool HasInvalidUnicode(const wxString& s)
{
    if (s.IsEmpty()) return false;
    for (const auto ch : s) {
        const wxUint32 c = static_cast<wxUint32>(ch.GetValue());
        if (c == 0xFFFD || c == 0) return true;                 // decode failure / embedded NUL
        if (c >= 0xD800 && c <= 0xDFFF) return true;            // lone surrogate
    }
    return s.ToUTF8().length() == 0;                            // non-empty but unencodable
}

// Interpret one column through its dialect's profile. MySQL/PG override
// Interpret with SyncTypeMap's InterpretMySqlColumn/InterpretPgColumn; the other
// dialects use the base passthrough. We consume this, never extend it.
CanonicalType Interpret(const NormColumn& c, Dialect d)
{
    return GetDialectProfile(d).Interpret(c);
}

Finding MakeFinding(const wxString& table, const wxString& column,
                    ValueVerdict v, const wxString& reason)
{
    Finding f;
    f.table   = table;
    f.column  = column;
    f.verdict = ToTypeVerdict(v);
    f.reason  = reason;
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// BuildBridges — all type reasoning lives here, once per table.
// ---------------------------------------------------------------------------
bool BuildBridges(const TableSchema& src, Dialect srcDialect,
                  const TableSchema& tgt, Dialect tgtDialect,
                  std::vector<ColumnBridge>& out, std::vector<Finding>& findings)
{
    out.clear();
    bool ok = true;
    const bool sameEngine = (srcDialect == tgtDialect);

    for (size_t i = 0; i < src.columns.size(); ++i) {
        const NormColumn& sc = src.columns[i];
        const NormColumn* tc = tgt.FindColumn(sc.name);
        if (!tc) {
            // Not fatal to the table: a source column with no target counterpart
            // simply is not synced. It IS an audit-trail entry — silently
            // dropping a column's data is the same class of wrongness as
            // silently NULLing a date.
            findings.push_back(MakeFinding(src.name.Key(), sc.name, ValueVerdict::Unrepresentable,
                L"目标表缺少同名列，该列数据不会被同步"));
            ok = false;
            continue;
        }

        ColumnBridge b;
        b.srcIdx     = i;
        b.column     = sc.name;
        b.toDialect  = tgtDialect;

        // ---- same-engine fast path -----------------------------------------
        // No Interpret call, no rawType parsing, no comparison. Every bridge is
        // an identity passthrough. This is what keeps the common case (MySQL ->
        // MySQL, PG -> PG) from paying anything at all for the cross-engine
        // machinery below.
        if (sameEngine) {
            out.push_back(std::move(b));
            continue;
        }

        b.from = Interpret(sc, srcDialect);
        b.to   = Interpret(*tc, tgtDialect);

        const ColKind fk = b.from.kind, tk = b.to.kind;

        // ---- boolean coercions --------------------------------------------
        if (tk == ColKind::Boolean && IsIntFamily(fk) && tgtDialect == Dialect::Postgres) {
            b.op = BridgeOp::BoolToPgBool; b.passthrough = false;
        }
        else if (fk == ColKind::Boolean && IsIntFamily(tk) && srcDialect == Dialect::Postgres) {
            b.op = BridgeOp::PgBoolToInt; b.passthrough = false;
        }
        // ---- integers: per-VALUE range decision ----------------------------
        // Ruling: unsigned overflow is decided per value, not per column. A
        // MySQL BIGINT UNSIGNED column whose every actual value fits PG's bigint
        // must sync; only the individual offending values are refused. So we
        // never reject the column here — we arm a cheap bounds check.
        else if (IsIntFamily(fk) && IsIntFamily(tk)) {
            const IntRange sr = ResolveIntRange(b.from);
            const IntRange tr = ResolveIntRange(b.to);
            if (NarrowerThan(sr, tr)) {
                b.op = BridgeOp::IntRange;
                b.passthrough = false;
                b.minValue = tr.lo;
                b.maxValue = tr.hi;
                b.maxIsUnsigned64 = tr.unsigned64;
                if (b.from.isUnsigned != b.to.isUnsigned)
                    findings.push_back(MakeFinding(src.name.Key(), sc.name, ValueVerdict::Coerced,
                        L"源列为无符号整数而目标列不是：逐值判定，超出目标范围的行会被单独拒绝（不影响其余行）"));
            }
        }
        // ---- binary ---------------------------------------------------------
        else if (IsBinFamily(fk) && IsBinFamily(tk)) {
            b.op = BridgeOp::BinaryReframe; b.passthrough = false;
        }
        // ---- temporal -------------------------------------------------------
        // Armed in BOTH directions, not just MySQL-as-source. The zero-date
        // guard only has something to do when MySQL is the source, but the
        // COMPARISON canonicalization is needed either way: a PG->MySQL
        // timestamp column left passthrough would compare `2024-01-01
        // 00:00:00+00` against `2024-01-01 00:00:00` as raw text and report a
        // false-positive UPDATE on every row.
        //
        // The split below is the whole timezone ruling, made ONCE PER COLUMN
        // from the canonical types so that neither ConvertCell nor the compare
        // layer ever has to re-derive it per cell.
        else if (IsTemporal(fk) && IsTemporal(tk)) {
            b.passthrough = false;
            if (b.from.withTimeZone || b.to.withTimeZone) {
                // MySQL TIMESTAMP / PG timestamptz on at least one side. Both
                // render through a SESSION timezone that the cell text does not
                // record, so equal text may be different instants and different
                // text may be the same instant. Undecidable from what we have;
                // refused rather than guessed. See BridgeOp::TemporalTzAmbiguous.
                b.op = BridgeOp::TemporalTzAmbiguous;
                findings.push_back(MakeFinding(src.name.Key(), sc.name, ValueVerdict::Coerced,
                    L"列的时间类型在某一端带时区语义（MySQL TIMESTAMP / PG timestamptz），"
                    L"两端渲染出的文本取决于各自会话时区，仅凭取值文本无法判定是否为同一时刻："
                    L"取值仍按原样搬运，但比对只能按文本进行，可能把同一时刻报成差异；"
                    L"该列不可用作主键排序，需人工确认两端会话时区一致"));
            } else {
                b.op = BridgeOp::TemporalGuard;
            }
        }
        // ---- text -----------------------------------------------------------
        else if (IsTextFamily(fk) && IsTextFamily(tk)) {
            b.op = BridgeOp::TextValidate; b.passthrough = false;
        }
        // ---- exact numerics -------------------------------------------------
        else if (IsNumFamily(fk) && IsNumFamily(tk)) {
            // Decimal/Float text crosses verbatim; no engine-specific framing.
        }
        // ---- no value-level mapping exists ----------------------------------
        else {
            wxString why;
            CompareCanonical(b.from, b.to, why);
            findings.push_back(MakeFinding(src.name.Key(), sc.name, ValueVerdict::Unrepresentable,
                L"两端类型种类不属于同一可转换族，无法安全搬运取值：" + why));
            ok = false;
            continue;
        }

        out.push_back(std::move(b));
    }

    // A target column the source has no counterpart for is only a problem when
    // it cannot take a value on its own (NOT NULL, no default, not generated).
    for (const auto& tcol : tgt.columns) {
        if (src.FindColumn(tcol.name)) continue;
        if (tcol.notNull && !tcol.hasDefault && !tcol.autoIncrement) {
            findings.push_back(MakeFinding(src.name.Key(), tcol.name, ValueVerdict::Unrepresentable,
                L"目标列 NOT NULL 且无默认值，源表没有对应列，INSERT 必然失败"));
            ok = false;
        }
    }

    return ok;
}

// ---------------------------------------------------------------------------
// ConvertCell — the hot path. 10M cells go through here.
// ---------------------------------------------------------------------------
ConvertedCell ConvertCell(const Cell& in, const ColumnBridge& bridge)
{
    // Fast path: one bool test. Same-engine sync and every no-op cross-engine
    // column land here and do zero work.
    if (bridge.passthrough) return { in, ValueVerdict::Exact, wxString() };

    // NULL is universal and is never converted into anything else (and the empty
    // string is never converted into NULL). Cell distinguishes the two; so do we,
    // in every branch, by short-circuiting before any op-specific logic.
    if (in.kind == CellKind::Null) return { in, ValueVerdict::Exact, wxString() };

    switch (bridge.op) {

    case BridgeOp::IntRange: {
        long long v = 0;
        if (in.text.ToLongLong(&v)) {
            if (v >= bridge.minValue && (bridge.maxIsUnsigned64 || v <= bridge.maxValue))
                return { in, ValueVerdict::Exact, wxString() };
            return { Cell(), ValueVerdict::Unrepresentable,
                     wxString::Format(L"整数值 %s 超出目标列可表示范围 [%lld, %lld]",
                                      in.text, bridge.minValue, bridge.maxValue) };
        }
        // Doesn't fit a signed 64-bit: only a MySQL BIGINT UNSIGNED value above
        // LLONG_MAX can legitimately look like this. It fits only if the target
        // is itself an unsigned 64-bit column.
        unsigned long long uv = 0;
        if (in.text.ToULongLong(&uv)) {
            if (bridge.maxIsUnsigned64) return { in, ValueVerdict::Exact, wxString() };
            return { Cell(), ValueVerdict::Unrepresentable,
                     wxString::Format(L"无符号整数值 %s 超出目标列上界 %lld", in.text, bridge.maxValue) };
        }
        return { Cell(), ValueVerdict::Unrepresentable, L"整数列取值无法解析为整数：" + in.text };
    }

    case BridgeOp::BoolToPgBool: {
        const int b = BoolValue(in.text);
        if (b < 0)
            return { Cell(), ValueVerdict::Unrepresentable,
                     L"目标为 PostgreSQL boolean，源值既非 0 也非 1：" + in.text };
        // CellKind::Numeric renders bare (unquoted) via RenderLiteral — exactly
        // what a PG boolean literal needs. TRUE/FALSE rather than 't'/'f' so the
        // emitted SQL is readable in the preview.
        Cell out;
        out.kind = CellKind::Numeric;
        out.text = b ? L"TRUE" : L"FALSE";
        return { out, ValueVerdict::Coerced, L"tinyint(1) " + in.text + L" -> PostgreSQL boolean " + out.text };
    }

    case BridgeOp::PgBoolToInt: {
        const int b = BoolValue(in.text);
        if (b < 0)
            return { Cell(), ValueVerdict::Unrepresentable,
                     L"源为 PostgreSQL boolean，取值无法识别：" + in.text };
        Cell out;
        out.kind = CellKind::Numeric;
        out.text = b ? L"1" : L"0";
        return { out, ValueVerdict::Coerced, L"PostgreSQL boolean " + in.text + L" -> tinyint(1) " + out.text };
    }

    case BridgeOp::BinaryReframe: {
        // Cell::Binary already holds bare uppercase hex with no prefix
        // (SyncTypes.h:17); the dialect framing is applied by RenderLiteral,
        // which emits PG's '\xDEADBEEF' bytea form. So the payload itself needs
        // no rewriting — but the framing genuinely changes across engines
        // (0x… -> '\x…'), so the honest verdict is Coerced, not Exact.
        //
        // Deliberately NOT emitting an explicit ::bytea cast: that would require
        // changing RenderLiteral, whose output is contractually byte-for-byte
        // shared with the MySQL dump path (SqlLiteral.cpp header comment), and
        // PG accepts the bare '\x…' literal in INSERT/UPDATE position anyway.
        if (in.kind != CellKind::Binary)
            return { in, ValueVerdict::Exact, wxString() };
        return { in, ValueVerdict::Coerced, L"二进制值按目标方言重新加框（十六进制载荷不变）" };
    }

    // Both temporal ops EMIT identically — the tz ruling is purely a comparison
    // concern, and carrying the value across as-is is what this has always done.
    // Widening the guard to the tz case costs nothing and keeps a MySQL
    // TIMESTAMP column's zero date refused rather than shipped.
    case BridgeOp::TemporalGuard:
    case BridgeOp::TemporalTzAmbiguous: {
        if (IsZeroDate(in.text))
            return { Cell(), ValueVerdict::Unrepresentable,
                     L"MySQL 零日期 " + in.text + L" 在目标引擎不可表示；"
                     L"按设计不会被静默转成 NULL，需人工决定如何处理该行" };
        return { in, ValueVerdict::Exact, wxString() };
    }

    case BridgeOp::TextValidate: {
        if (HasInvalidUnicode(in.text))
            return { Cell(), ValueVerdict::Unrepresentable,
                     L"文本包含无效的 UTF-8 序列/嵌入 NUL，目标引擎无法存储" };
        return { in, ValueVerdict::Exact, wxString() };
    }

    case BridgeOp::Identity:
    default:
        return { in, ValueVerdict::Exact, wxString() };
    }
}

} // namespace db::sync
