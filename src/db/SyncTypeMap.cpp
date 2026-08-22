// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncTypeMap.cpp — see header. MySQL <-> PostgreSQL canonical type mapping.
// Pure functions; no connection, no vendor header.
#include "db/SyncTypeMap.h"

namespace db {
namespace {

// Debug/reason-text label for a ColKind — used only inside CompareCanonical's
// `reason` strings, never emitted into DDL.
wxString ColKindName(ColKind k)
{
    switch (k) {
    case ColKind::Integer:   return L"Integer";
    case ColKind::Decimal:   return L"Decimal";
    case ColKind::Float:     return L"Float";
    case ColKind::Boolean:   return L"Boolean";
    case ColKind::Char:      return L"Char";
    case ColKind::Varchar:   return L"Varchar";
    case ColKind::Text:      return L"Text";
    case ColKind::Binary:    return L"Binary";
    case ColKind::Blob:      return L"Blob";
    case ColKind::Date:      return L"Date";
    case ColKind::Time:      return L"Time";
    case ColKind::Timestamp: return L"Timestamp";
    case ColKind::Json:      return L"Json";
    case ColKind::Enum:      return L"Enum";
    case ColKind::Uuid:      return L"Uuid";
    default:                 return L"Other";
    }
}

// "enum('a','b','c')" / "set('x','y')" -> {a,b,c}. Best-effort: an unparsable
// rawType yields an empty list (CompareCanonical then treats it as "unknown
// members" rather than crashing on it).
std::vector<wxString> ParseMySqlEnumValues(const wxString& rawType)
{
    std::vector<wxString> out;
    const int open = rawType.Find(L'(');
    if (open == wxNOT_FOUND) return out;
    const int close = rawType.Find(L')', /*fromEnd=*/true);
    if (close == wxNOT_FOUND || close <= open) return out;
    wxString inner = rawType.Mid(open + 1, close - open - 1);
    wxString rest = inner;
    while (!rest.IsEmpty()) {
        int comma = rest.Find(L',');
        wxString tok = (comma == wxNOT_FOUND) ? rest : rest.Left(comma);
        tok.Trim(true).Trim(false);
        if (tok.StartsWith(L"'") && tok.EndsWith(L"'") && tok.length() >= 2)
            tok = tok.Mid(1, tok.length() - 2);
        out.push_back(tok);
        if (comma == wxNOT_FOUND) break;
        rest = rest.Mid(comma + 1);
    }
    return out;
}

} // namespace

// ---- MySQL ----

CanonicalType InterpretMySqlColumn(const NormColumn& c)
{
    CanonicalType t;
    t.kind    = c.kind;
    t.length  = c.length;
    t.scale   = c.scale;
    t.rawType = c.rawType;

    const wxString ct = c.rawType.Lower();
    t.isUnsigned    = ct.Contains(L"unsigned");
    t.binaryVariant = (c.kind == ColKind::Binary || c.kind == ColKind::Blob);

    // MySQL TIMESTAMP is stored as UTC and converted on read/write per session
    // time zone (effectively "timezone-aware"); DATETIME is a naive wall-clock
    // value with no such conversion. column_type for both is the bare keyword
    // (optionally with a fractional-seconds precision, "timestamp(3)"), so
    // StartsWith is enough to tell them apart.
    if (c.kind == ColKind::Timestamp)
        t.withTimeZone = ct.StartsWith(L"timestamp");

    if (c.kind == ColKind::Enum)
        t.allowedValues = ParseMySqlEnumValues(c.rawType);

    return t;
}

bool RenderMySqlType(const CanonicalType& t, wxString& out, wxString& why)
{
    switch (t.kind) {
    case ColKind::Integer:   out = L"bigint"; break;    // widest safe default; caller may
                                                        // still prefer int/smallint by length
    case ColKind::Decimal:
        out = (t.length >= 0)
                  ? (t.scale >= 0 ? wxString::Format(L"decimal(%lld,%d)", t.length, t.scale)
                                  : wxString::Format(L"decimal(%lld)", t.length))
                  : wxString(L"decimal");
        break;
    case ColKind::Float:     out = L"double"; break;
    case ColKind::Boolean:   out = L"tinyint(1)"; break;
    case ColKind::Char:      out = (t.length >= 0) ? wxString::Format(L"char(%lld)", t.length) : wxString(L"char(1)"); break;
    case ColKind::Varchar:   out = (t.length >= 0) ? wxString::Format(L"varchar(%lld)", t.length) : wxString(L"varchar(255)"); break;
    case ColKind::Text:      out = L"text"; break;
    case ColKind::Binary:    out = (t.length >= 0) ? wxString::Format(L"varbinary(%lld)", t.length) : wxString(L"varbinary(255)"); break;
    case ColKind::Blob:      out = L"blob"; break;
    case ColKind::Date:      out = L"date"; break;
    case ColKind::Time:      out = L"time"; break;
    case ColKind::Timestamp: out = t.withTimeZone ? L"timestamp" : L"datetime"; break;
    case ColKind::Json:      out = L"json"; break;
    case ColKind::Enum: {
        if (t.allowedValues.empty()) { why = L"ENUM 取值列表未知，无法在 MySQL 端生成安全的 enum(...) 定义"; return false; }
        wxString vals;
        for (size_t i = 0; i < t.allowedValues.size(); ++i) {
            wxString e = t.allowedValues[i]; e.Replace(L"'", L"''");
            vals += (i ? L",'" : L"'") + e + L"'";
        }
        out = L"enum(" + vals + L")";
        break;
    }
    case ColKind::Uuid:
        // MySQL has no native UUID type; char(36) is the conventional textual
        // representation. Render succeeds (a workable degrade), but this pairing
        // is still classified Lossy by CompareCanonical (format, not storage,
        // equivalence) — Render()'s success/fail is orthogonal to that verdict.
        out = L"char(36)";
        break;
    default:
        why = L"该类型种类（" + ColKindName(t.kind) + L"）在 MySQL 没有已知的安全映射";
        return false;
    }
    if (t.isUnsigned &&
        (t.kind == ColKind::Integer || t.kind == ColKind::Decimal || t.kind == ColKind::Float))
        out += L" unsigned";
    return true;
}

// ---- PostgreSQL ----

ColKind MapPgColKind(const wxString& dataType)
{
    const wxString t = dataType.Lower();
    if (t == L"smallint" || t == L"integer" || t == L"bigint")
        return ColKind::Integer;
    if (t == L"numeric" || t == L"decimal")
        return ColKind::Decimal;
    if (t == L"real" || t == L"double precision")
        return ColKind::Float;
    if (t == L"boolean")
        return ColKind::Boolean;
    if (t == L"character")
        return ColKind::Char;
    if (t == L"character varying")
        return ColKind::Varchar;
    if (t == L"text")
        return ColKind::Text;
    if (t == L"bytea")
        return ColKind::Binary;
    if (t == L"date")
        return ColKind::Date;
    if (t == L"time without time zone" || t == L"time with time zone")
        return ColKind::Time;
    if (t == L"timestamp without time zone" || t == L"timestamp with time zone")
        return ColKind::Timestamp;
    if (t == L"json" || t == L"jsonb")
        return ColKind::Json;
    if (t == L"uuid")
        return ColKind::Uuid;
    return ColKind::Other;   // custom/domain/composite/array types — honest gate
}

CanonicalType InterpretPgColumn(const NormColumn& c)
{
    CanonicalType t;
    t.kind       = c.kind;
    t.length     = c.length;
    t.scale      = c.scale;
    t.rawType    = c.rawType;
    t.isUnsigned = false;   // PostgreSQL has no unsigned integer types
    t.binaryVariant = (c.kind == ColKind::Binary || c.kind == ColKind::Blob);

    if (c.kind == ColKind::Timestamp || c.kind == ColKind::Time) {
        const wxString rt = c.rawType.Lower();
        t.withTimeZone = rt.Contains(L"with time zone") && !rt.Contains(L"without time zone");
    }
    return t;
}

bool RenderPgType(const CanonicalType& t, wxString& out, wxString& why)
{
    if (t.isUnsigned) {
        why = L"PostgreSQL 没有无符号整数类型；需人工改用更宽的有符号类型并加 CHECK 约束";
        return false;
    }
    switch (t.kind) {
    case ColKind::Integer:   out = L"bigint"; break;
    case ColKind::Decimal:
        out = (t.length >= 0)
                  ? (t.scale >= 0 ? wxString::Format(L"numeric(%lld,%d)", t.length, t.scale)
                                  : wxString::Format(L"numeric(%lld)", t.length))
                  : wxString(L"numeric");
        break;
    case ColKind::Float:     out = L"double precision"; break;
    case ColKind::Boolean:   out = L"boolean"; break;
    case ColKind::Char:      out = (t.length >= 0) ? wxString::Format(L"character(%lld)", t.length) : wxString(L"character(1)"); break;
    case ColKind::Varchar:   out = (t.length >= 0) ? wxString::Format(L"character varying(%lld)", t.length) : wxString(L"character varying"); break;
    case ColKind::Text:      out = L"text"; break;
    case ColKind::Binary:
    case ColKind::Blob:      out = L"bytea"; break;
    case ColKind::Date:      out = L"date"; break;
    case ColKind::Time:      out = t.withTimeZone ? L"time with time zone" : L"time without time zone"; break;
    case ColKind::Timestamp: out = t.withTimeZone ? L"timestamp with time zone" : L"timestamp without time zone"; break;
    case ColKind::Json:      out = L"jsonb"; break;
    case ColKind::Enum:
        // No CREATE TYPE machinery here (out of scope — a PG native enum needs a
        // companion CREATE TYPE statement this layer doesn't emit); degrade to
        // text. Render still succeeds (a workable fallback); CompareCanonical is
        // what flags this pairing Lossy so it never auto-ALTERs.
        out = L"text";
        break;
    case ColKind::Uuid:      out = L"uuid"; break;
    default:
        why = L"该类型种类（" + ColKindName(t.kind) + L"）在 PostgreSQL 没有已知的安全映射";
        return false;
    }
    return true;
}

// ---- cross-engine comparison ----

TypeVerdict CompareCanonical(const CanonicalType& a, const CanonicalType& b, wxString& reason)
{
    // Byte-identical native text is the strongest possible compatibility signal,
    // whichever engines it came from. This is usually a same-engine comparison,
    // but cross-engine callers can legitimately land here too by coincidence —
    // MySQL's column_type and PG's data_type spell several keywords identically
    // ("bigint", "smallint", "text", "boolean", "json"...) — and that is fine:
    // an exact-text match is at least as trustworthy as the kind-based
    // Equivalent verdict below, so Identical (not Equivalent) is still correct.
    if (!a.rawType.IsEmpty() && a.rawType == b.rawType) {
        reason = L"原始类型文本完全一致";
        return TypeVerdict::Identical;
    }

    // MySQL unsigned integers <-> PostgreSQL: PG has no unsigned integer type at
    // all, so there is no automatic widening that is both safe and exact (widen
    // to bigint and you still need an app-level CHECK to reject negative values
    // MySQL would have rejected at the column level). Judgment call: Unmappable,
    // not Lossy — this needs a human decision (widen type? add CHECK? reject?),
    // not a silent best-effort ALTER.
    if (a.isUnsigned != b.isUnsigned) {
        reason = L"MySQL 无符号整数在 PostgreSQL 无对应类型（PG 无 unsigned），"
                 L"需人工改用更宽的有符号类型并按需补 CHECK 约束";
        return TypeVerdict::Unmappable;
    }

    if (a.kind != b.kind) {
        // ENUM has no clean PostgreSQL equivalent without a companion CREATE TYPE
        // (out of scope here — see RenderPgType). Judgment call: Lossy (not
        // Unmappable) degrading to a string type — the *data* is representable,
        // only the value-list constraint is lost, so this is a real but bounded
        // loss a human can consciously accept, worth surfacing rather than
        // blocking outright.
        const bool enumToString =
            (a.kind == ColKind::Enum && (b.kind == ColKind::Text || b.kind == ColKind::Varchar || b.kind == ColKind::Char)) ||
            (b.kind == ColKind::Enum && (a.kind == ColKind::Text || a.kind == ColKind::Varchar || a.kind == ColKind::Char));
        if (enumToString) {
            reason = L"ENUM 在目标引擎无原生等价物，降级为字符串类型，取值约束丢失";
            return TypeVerdict::Lossy;
        }
        reason = L"跨引擎类型种类不匹配（" + ColKindName(a.kind) + L" vs " + ColKindName(b.kind) +
                 L"），无已知的安全映射规则";
        return TypeVerdict::Unmappable;
    }

    // Same ColKind on both sides: per-kind refinement for the pairs that need it.
    switch (a.kind) {
    case ColKind::Timestamp:
        // Judgment call (per the task brief): MySQL TIMESTAMP and PostgreSQL
        // timestamptz are NOT declared Equivalent even though both are
        // "timezone-aware" in the sense of auto-converting through UTC. They
        // still differ enough to be risky for an unattended ALTER: MySQL
        // TIMESTAMP's range is 1970-01-01 to 2038-01-19 (32-bit epoch) vs PG
        // timestamptz's full 4713 BC-294276 AD range, and the two engines'
        // DST-fold/gap handling during the UTC<->local conversion is not
        // guaranteed identical at the boundary. Lossy: flagged for human review,
        // never auto-ALTERed (MayAutoAlter(Lossy) == false).
        if (a.withTimeZone && b.withTimeZone) {
            reason = L"两侧均为“带时区语义”的时间戳（MySQL TIMESTAMP / PG timestamptz），"
                     L"但取值范围（MySQL 限 1970–2038）与 DST 边界转换算法不完全一致，按 Lossy 处理，需人工核实";
            return TypeVerdict::Lossy;
        }
        if (!a.withTimeZone && !b.withTimeZone) {
            reason = L"均为“无时区语义”的挂钟时间戳（DATETIME / timestamp without time zone），语义等价";
            return TypeVerdict::Equivalent;
        }
        reason = L"一侧带时区语义、一侧不带，直接互转会改变取值语义";
        return TypeVerdict::Lossy;

    case ColKind::Binary:
        // MySQL's BINARY/VARBINARY are length-enforced (fixed/max byte count);
        // PostgreSQL's bytea is a single unbounded type with no length modifier
        // at all — the length constraint is silently dropped either direction.
        reason = L"定长 BINARY/VARBINARY 与不限长的 bytea 之间会丢失长度约束";
        return TypeVerdict::Lossy;

    case ColKind::Enum:
        if (!a.allowedValues.empty() && a.allowedValues == b.allowedValues) {
            reason = L"枚举取值列表完全一致";
            return TypeVerdict::Equivalent;
        }
        reason = L"枚举取值列表不同或未知，需人工核对后再同步";
        return TypeVerdict::Lossy;

    case ColKind::Uuid:
        // Same ColKind::Uuid on both sides only happens when both are native PG
        // uuid columns (MySQL has none) — same-dialect, already caught by the
        // rawType-identical branch above. Reaching here (uuid vs uuid, rawType
        // differs) is defensive; treat as Equivalent like any other same-kind
        // structured type.
        reason = L"类型种类相同（Uuid），视为跨引擎语义等价";
        return TypeVerdict::Equivalent;

    default:
        // Integer/Decimal/Float/Boolean/Char/Varchar/Text/Blob/Date/Time/Json:
        // same ColKind implies the same value domain across these two engines
        // (widths/precision differences are a data-shape concern the diff engine
        // still compares separately via NormColumn.length/scale before deciding
        // an actual MODIFY is needed — CompareCanonical only answers "is this
        // pairing safe to auto-ALTER at all").
        reason = L"类型种类相同，视为跨引擎语义等价";
        return TypeVerdict::Equivalent;
    }
}

} // namespace db
