// RoutineCompare.cpp — see the header. Pure: no IConnection, no globals.
#include "db/RoutineCompare.h"

#include "db/DbDriver.h"          // db::Dialect
#include "db/MySqlSql.h"          // db::detail::MapColKind
#include "db/RoutineNormalize.h"
#include "db/SyncTypeMap.h"       // CanonicalType / CompareCanonical / MapPgColKind

#include <algorithm>
#include <map>
#include <vector>

namespace db::sync {

namespace {

// ---------------------------------------------------------------------------
// small text helpers
// ---------------------------------------------------------------------------

wxString Squeeze(const wxString& in)
{
    wxString out;
    bool sp = false;
    for (size_t i = 0; i < in.length(); ++i) {
        const wxUniChar c = in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = !out.empty(); continue; }
        if (sp) { out += ' '; sp = false; }
        out += c;
    }
    return out;
}

std::vector<wxString> SplitWords(const wxString& s)
{
    std::vector<wxString> w;
    wxString cur;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == ' ') { if (!cur.IsEmpty()) { w.push_back(cur); cur.clear(); } }
        else cur += s[i];
    }
    if (!cur.IsEmpty()) w.push_back(cur);
    return w;
}

// Remove every parenthesized run (length/precision/enum member list). Nesting is
// tracked so `decimal(10,2)` and a hypothetical nested form both survive.
wxString StripParens(const wxString& s)
{
    wxString out;
    int depth = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        const wxUniChar c = s[i];
        if (c == '(') { ++depth; out += ' '; continue; }
        if (c == ')') { if (depth > 0) --depth; continue; }
        if (depth == 0) out += c;
    }
    return out;
}

// Engine alias resolution. Deliberately MODEST: it resolves spellings of the
// SAME type (int4 == integer) and MySQL's text-family widths, and it does NOT
// invent cross-engine equivalences (bytea is not mapped onto blob, tinyint is
// not mapped onto smallint). Inventing them would make the KEY assert a
// semantic claim, and the key must only assert "same spelling of same type".
// Genuinely cross-engine type differences are instead caught by the
// same-name/same-arity re-join below and reported as SignatureDiffers, with
// both declarations shown — a true statement rather than a guessed match.
wxString ResolveAlias(const wxString& base)
{
    struct Alias { const wchar_t* from; const wchar_t* to; };
    static const Alias kAliases[] = {
        { L"int",                        L"integer"    },
        { L"int4",                       L"integer"    },
        { L"integer",                    L"integer"    },
        { L"int8",                       L"bigint"     },
        { L"int2",                       L"smallint"   },
        { L"bool",                       L"boolean"    },
        { L"character varying",          L"varchar"    },
        { L"varchar",                    L"varchar"    },
        { L"character",                  L"char"       },
        { L"bpchar",                     L"char"       },
        { L"float4",                     L"real"       },
        { L"float8",                     L"double"     },
        { L"double precision",           L"double"     },
        { L"decimal",                    L"numeric"    },
        { L"dec",                        L"numeric"    },
        { L"fixed",                      L"numeric"    },
        { L"tinytext",                   L"text"       },
        { L"mediumtext",                 L"text"       },
        { L"longtext",                   L"text"       },
        { L"datetime",                   L"timestamp"  },
        { L"timestamp without time zone",L"timestamp"  },
        { L"timestamp with time zone",   L"timestamptz"},
        { L"timestamptz",                L"timestamptz"},
        { L"time without time zone",     L"time"       },
        { L"time with time zone",        L"timetz"     },
        { L"timetz",                     L"timetz"     },
    };
    for (const auto& a : kAliases)
        if (base == a.from) return a.to;
    return base;
}

wxString FoldName(const wxString& name, bool fold)
{
    if (!fold) return name;
    wxString n = name;
    n.MakeLower();
    return n;
}

wxString KindToken(RoutineKind k)
{
    return k == RoutineKind::Function ? L"F" : L"P";
}

wxString KindLabel(RoutineKind k)
{
    return k == RoutineKind::Function ? L"函数" : L"存储过程";
}

// ---------------------------------------------------------------------------
// canonical typing, used ONLY to explain a difference (never to key)
// ---------------------------------------------------------------------------

bool DialectModelled(Dialect d)
{
    return d == Dialect::MySQL || d == Dialect::Postgres;
}

// Build a CanonicalType from an engine-native argument type spelling. Mirrors
// what the schema readers hand SyncTypeMap for a COLUMN: the same rawType
// spelling, the same ColKind classifier, the same Interpret* entry point — so a
// routine argument is typed by exactly the code that types a column, rather than
// by a second, drifting implementation.
bool CanonicalOf(const wxString& rawType, Dialect d, CanonicalType& out)
{
    if (!DialectModelled(d) || rawType.IsEmpty()) return false;

    wxString raw = Squeeze(rawType);
    NormColumn nc;
    nc.rawType = raw;

    // data_type == the type name without the length/precision suffix, which is
    // how both information_schema.columns and PG's data_type actually spell it.
    wxString base = Squeeze(StripParens(raw));
    wxString baseLower = base; baseLower.MakeLower();

    // Parse a length/precision out of the parens for the width-sensitive
    // verdicts (varchar(50) vs varchar(100)).
    const size_t lp = raw.find('(');
    if (lp != wxString::npos) {
        const size_t rp = raw.find(')', lp);
        if (rp != wxString::npos) {
            const wxString inner = raw.Mid(lp + 1, rp - lp - 1);
            const size_t comma = inner.find(',');
            long long len = -1; long sc = -1;
            if (comma == wxString::npos) {
                if (inner.ToLongLong(&len)) nc.length = len;
            } else {
                if (inner.Left(comma).ToLongLong(&len)) nc.length = len;
                if (inner.Mid(comma + 1).ToLong(&sc))   nc.scale  = static_cast<int>(sc);
            }
        }
    }

    if (d == Dialect::MySQL) {
        nc.kind = db::detail::MapColKind(baseLower, raw);
        out = InterpretMySqlColumn(nc);
    } else {
        nc.kind = MapPgColKind(baseLower);
        nc.rawType = baseLower;      // PG's data_type carries no inline width
        out = InterpretPgColumn(nc);
        out.length = nc.length;
        out.scale  = nc.scale;
    }
    return true;
}

// Case/space-insensitive raw comparison — the two spellings come from two
// catalogs of the SAME engine, which are internally consistent, so this only
// absorbs formatting noise and never a semantic difference.
bool RawTypeSame(const wxString& a, const wxString& b)
{
    wxString x = Squeeze(a), y = Squeeze(b);
    x.MakeLower(); y.MakeLower();
    return x == y;
}

// ---------------------------------------------------------------------------
// verdict assignment for ONE matched pair
// ---------------------------------------------------------------------------

// Compare the DECLARATIONS. Returns true when they differ, filling `why`.
//
// The rule differs by engine pairing, and that is principled rather than
// pragmatic:
//
//   SAME ENGINE — raw text is truth. SyncTypeMap.h's own definition of
//   TypeVerdict::Identical is "byte-identical within one engine (rawType
//   equal)". So ANY declaration difference is a real difference the user must
//   see; varchar(50) -> varchar(100) is a change to this routine, not noise.
//
//   CROSS ENGINE — raw text differs BY CONSTRUCTION (`int(11)` vs `integer`
//   describe the same thing), so raw comparison would report every routine as
//   different, i.e. pure noise. Here the canonical mapper earns its keep: only
//   a pairing it refuses to call auto-alterable (Lossy/Unmappable) is reported.
bool DeclarationDiffers(const RoutineDef& s, const RoutineDef& t,
                        const RoutineCompareOptions& opt, wxString& why)
{
    if (s.kind != t.kind) {
        why = wxString::Format(L"源端为%s，目标端为%s", KindLabel(s.kind), KindLabel(t.kind));
        return true;
    }

    if (s.argTypes.size() != t.argTypes.size()) {
        why = wxString::Format(L"参数个数不同：源端 %d 个，目标端 %d 个",
                               (int)s.argTypes.size(), (int)t.argTypes.size());
        return true;
    }

    // Argument modes (IN/OUT/INOUT). Not part of the key — see the header — so
    // this is where a mode-only change surfaces, and it is a real one: turning
    // an IN parameter into an OUT parameter changes every call site.
    for (size_t i = 0; i < s.argTypes.size(); ++i) {
        const wxString sm = i < s.argModes.size() ? s.argModes[i] : wxString();
        const wxString tm = i < t.argModes.size() ? t.argModes[i] : wxString();
        // An empty mode means "the catalog did not say", not "IN" — comparing it
        // against a stated IN would manufacture a difference out of a gap in the
        // reader. Only two STATED and different modes count.
        if (!sm.IsEmpty() && !tm.IsEmpty() && sm != tm) {
            why = wxString::Format(L"参数 %d 模式不同：%s vs %s", (int)i + 1, sm, tm);
            return true;
        }
    }

    for (size_t i = 0; i < s.argTypes.size(); ++i) {
        const wxString& a = s.argTypes[i];
        const wxString& b = t.argTypes[i];
        if (RawTypeSame(a, b)) continue;

        if (opt.sameEngine) {
            why = wxString::Format(L"参数 %d 类型不同：%s vs %s", (int)i + 1, a, b);
            return true;
        }
        CanonicalType ca, cb;
        if (CanonicalOf(a, opt.srcDialect, ca) && CanonicalOf(b, opt.tgtDialect, cb)) {
            wxString reason;
            const TypeVerdict v = CompareCanonical(ca, cb, reason);
            if (!MayAutoAlter(v)) {
                why = wxString::Format(L"参数 %d 类型不兼容：%s vs %s（%s）",
                                       (int)i + 1, a, b, reason);
                return true;
            }
            continue;   // Identical/Equivalent across engines: not a difference
        }
        // An unmodelled dialect: we cannot judge equivalence, so we report the
        // raw difference rather than silently calling it equal.
        why = wxString::Format(L"参数 %d 类型不同：%s vs %s（该数据库类型无法判定等价性）",
                               (int)i + 1, a, b);
        return true;
    }

    if (!RawTypeSame(s.returnType, t.returnType)) {
        if (s.returnType.IsEmpty() || t.returnType.IsEmpty() || opt.sameEngine) {
            why = wxString::Format(L"返回类型不同：%s vs %s",
                                   s.returnType.IsEmpty() ? L"(无)" : s.returnType,
                                   t.returnType.IsEmpty() ? L"(无)" : t.returnType);
            return true;
        }
        CanonicalType ca, cb;
        if (CanonicalOf(s.returnType, opt.srcDialect, ca) &&
            CanonicalOf(t.returnType, opt.tgtDialect, cb)) {
            wxString reason;
            if (!MayAutoAlter(CompareCanonical(ca, cb, reason))) {
                why = wxString::Format(L"返回类型不兼容：%s vs %s（%s）",
                                       s.returnType, t.returnType, reason);
                return true;
            }
        } else {
            why = wxString::Format(L"返回类型不同：%s vs %s", s.returnType, t.returnType);
            return true;
        }
    }

    // SECURITY DEFINER changes WHOSE privileges the body runs with. Two bodies
    // that are character-for-character identical do not do the same thing if one
    // of them is definer-rights and the other is not, so this can never be
    // folded into "identical".
    //
    // SAME-ENGINE ONLY, for the same reason the volatility check below is — and
    // this gate was added after a live MySQL->PostgreSQL run showed why. The two
    // engines have OPPOSITE DEFAULTS: a MySQL routine created without a SQL
    // SECURITY clause is DEFINER, a PostgreSQL function created without one is
    // INVOKER. So cross-engine this test fires on literally every matched pair
    // and reports 执行权限不同 about two servers' defaults rather than about the
    // user's routines — a 100% false-positive rate against a feature whose
    // stated release bar is zero. (Observed: tests/mysql_pg_live_routines.cpp
    // hazard 7c, where the only matched pair reported SignatureDiffers instead
    // of the NotComparable the cross-engine rule requires.) Within ONE engine
    // the flag is a real, comparable declaration and the reasoning above stands.
    if (opt.sameEngine && s.securityDefiner != t.securityDefiner) {
        why = wxString::Format(L"执行权限不同：源端 %s，目标端 %s",
                               s.securityDefiner ? L"DEFINER" : L"INVOKER",
                               t.securityDefiner ? L"DEFINER" : L"INVOKER");
        return true;
    }

    // Volatility is PG-only and same-engine-only: it is a planner-visible
    // contract (an IMMUTABLE function may be folded into an index expression),
    // so a change is real. Cross-engine there is nothing to compare it against.
    if (opt.sameEngine && !s.volatility.IsEmpty() && !t.volatility.IsEmpty() &&
        s.volatility != t.volatility) {
        why = wxString::Format(L"易变性不同：%s vs %s", s.volatility, t.volatility);
        return true;
    }

    return false;
}

// Decide the verdict for a pair whose declarations already matched.
void JudgeBodies(const RoutineDef& s, const RoutineDef& t,
                 const RoutineCompareOptions& opt, RoutinePair& p)
{
    // Different procedural languages: we do not claim, ever. Cross-engine this
    // is unconditional (a PL/pgSQL body and a MySQL body are different
    // languages by definition); same-engine it still happens (PG `sql` vs
    // `plpgsql`), and the same reasoning applies.
    if (!opt.sameEngine) {
        p.verdict = RoutineVerdict::NotComparable;
        p.reason  = L"跨引擎：过程语言不同，不对函数体作同/异判断，请人工对照两侧源码";
        return;
    }

    wxString sl = s.language, tl = t.language;
    sl.MakeLower(); tl.MakeLower();
    if (!sl.IsEmpty() && !tl.IsEmpty() && sl != tl) {
        p.verdict = RoutineVerdict::NotComparable;
        p.reason  = wxString::Format(L"过程语言不同：%s vs %s，不作同/异判断",
                                     s.language, t.language);
        return;
    }

    // A body we could not READ can never yield a verdict. Two routines whose
    // bodies are both hidden by privilege would otherwise compare equal (both
    // empty) and be reported 相同 — a confident claim of sameness about text
    // nobody ever saw. See RoutineDef::bodyReadable.
    if (!s.bodyReadable || !t.bodyReadable) {
        p.verdict = RoutineVerdict::NotComparable;
        p.reason  = L"函数体不可见（权限不足），仅能比较签名；请用有权限的账号重试或人工对照";
        return;
    }

    const NormalizeRules rules = RulesFor(opt.srcDialect);
    const NormalizeResult ns = NormalizeRoutineBody(s.body, rules);
    const NormalizeResult nt = NormalizeRoutineBody(t.body, rules);

    // Degrade to "can't tell", never to "differs" — the normalizer's contract.
    if (!ns.Ok() || !nt.Ok()) {
        p.verdict = RoutineVerdict::NotComparable;
        p.reason  = L"无法解析函数体（" +
                    (!ns.Ok() ? L"源端：" + ns.reason : L"目标端：" + nt.reason) +
                    L"），请人工对照";
        return;
    }

    if (ns.text == nt.text) {
        p.verdict = RoutineVerdict::Identical;
        p.reason  = L"签名与函数体一致（已忽略空白与注释差异）";
    } else {
        p.verdict = RoutineVerdict::BodyDiffers;
        p.reason  = L"签名相同，函数体不同";
    }
}

void Judge(const RoutineDef& s, const RoutineDef& t,
           const RoutineCompareOptions& opt, RoutinePair& p)
{
    wxString why;
    if (DeclarationDiffers(s, t, opt, why)) {
        p.verdict = RoutineVerdict::SignatureDiffers;
        p.reason  = why;
        return;
    }
    JudgeBodies(s, t, opt, p);
}

void Tally(RoutineDiffStat& st, RoutineVerdict v)
{
    switch (v) {
    case RoutineVerdict::Identical:        ++st.identical;        break;
    case RoutineVerdict::SourceOnly:       ++st.sourceOnly;       break;
    case RoutineVerdict::TargetOnly:       ++st.targetOnly;       break;
    case RoutineVerdict::SignatureDiffers: ++st.signatureDiffers; break;
    case RoutineVerdict::BodyDiffers:      ++st.bodyDiffers;      break;
    case RoutineVerdict::NotComparable:    ++st.notComparable;    break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// public surface
// ---------------------------------------------------------------------------

RoutineCompareOptions MakeRoutineCompareOptions(Dialect src, Dialect tgt)
{
    RoutineCompareOptions o;
    o.enabled     = true;
    o.srcDialect  = src;
    o.tgtDialect  = tgt;
    o.sameEngine  = (src == tgt);
    // Fold only when MySQL is on either side: MySQL routine names are
    // case-insensitive, so NOT folding would report `getTotal` and `gettotal` as
    // two unrelated routines. In a PG↔PG compare folding is the greater risk —
    // a quoted "MyFunc" and a "myfunc" are genuinely distinct there, and folding
    // would collide them into one pair with a fabricated verdict.
    o.foldNameCase = (src == Dialect::MySQL || tgt == Dialect::MySQL);
    o.qualifyBySchema = false;
    return o;
}

wxString NormalizeArgType(const wxString& raw)
{
    wxString s = Squeeze(raw);
    if (s.IsEmpty()) return s;
    s.MakeLower();

    // Array suffix is part of the type's identity: integer and integer[] are
    // different parameters.
    bool array = false;
    while (s.length() >= 2 && s.EndsWith(L"[]")) {
        array = true;
        s = Squeeze(s.Left(s.length() - 2));
    }

    s = Squeeze(StripParens(s));

    bool isUnsigned = false;
    std::vector<wxString> keep;
    const std::vector<wxString> words = SplitWords(s);
    for (size_t i = 0; i < words.size(); ++i) {
        const wxString& w = words[i];
        if (w == L"unsigned") { isUnsigned = true; continue; }
        if (w == L"signed" || w == L"zerofill") continue;   // MySQL noise / default
        // `character set utf8mb4` / `collate x` say nothing about the TYPE's
        // identity for signature purposes; drop the keyword and its argument.
        if (w == L"collate") { ++i; continue; }
        if (w == L"character" && i + 1 < words.size() && words[i + 1] == L"set") {
            i += 2;
            continue;
        }
        keep.push_back(w);
    }

    wxString base;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i) base += ' ';
        base += keep[i];
    }
    base = ResolveAlias(base);

    if (isUnsigned) base += L" unsigned";   // KEPT: part of MySQL's type meaning
    if (array)      base += L"[]";
    return base;
}

wxString RoutineMatchKey(const RoutineDef& r, const RoutineCompareOptions& opt)
{
    wxString k;
    if (opt.qualifyBySchema) k += FoldName(r.schema, opt.foldNameCase) + L".";
    k += KindToken(r.kind);
    k += L":";
    k += FoldName(r.name, opt.foldNameCase);
    k += L"(";
    for (size_t i = 0; i < r.argTypes.size(); ++i) {
        if (i) k += L",";
        k += NormalizeArgType(r.argTypes[i]);
    }
    k += L")";
    return k;
}

RoutineDiffSet CompareRoutines(const std::vector<RoutineDef>& source,
                               const std::vector<RoutineDef>& target,
                               RoutineReadStatus sourceStatus,
                               RoutineReadStatus targetStatus,
                               const RoutineCompareOptions& opt)
{
    RoutineDiffSet out;
    out.sourceStatus = sourceStatus;
    out.targetStatus = targetStatus;
    out.sameEngine   = opt.sameEngine;

    // ADR-013 Q4/Q5: a non-Ok side, or the feature switched off, yields NO
    // pairs. Not an error, not a partial list — the categories simply render
    // their "不可见" label with zero children, and the table diff (which does
    // not read this type at all) is untouched.
    if (!opt.enabled || !out.Comparable()) return out;

    // ---- index both sides by key, detecting ambiguity ----------------------
    std::map<wxString, std::vector<size_t>> srcByKey, tgtByKey;
    for (size_t i = 0; i < source.size(); ++i) srcByKey[RoutineMatchKey(source[i], opt)].push_back(i);
    for (size_t i = 0; i < target.size(); ++i) tgtByKey[RoutineMatchKey(target[i], opt)].push_back(i);

    std::vector<bool> srcUsed(source.size(), false), tgtUsed(target.size(), false);
    std::vector<RoutinePair> pairs;

    auto oneSided = [&](const RoutineDef& r, bool isSource, RoutineVerdict v,
                        const wxString& key, const wxString& reason) {
        RoutinePair p;
        p.verdict = v;
        p.kind    = r.kind;
        p.key     = key;
        p.reason  = reason;
        if (isSource) { p.hasSource = true; p.source = r; }
        else          { p.hasTarget = true; p.target = r; }
        pairs.push_back(std::move(p));
    };

    // ---- matched keys ------------------------------------------------------
    for (const auto& [key, sidx] : srcByKey) {
        const auto it = tgtByKey.find(key);
        if (it == tgtByKey.end()) continue;
        const auto& tidx = it->second;

        // Two routines on one side normalized to the SAME key. We refuse to pick
        // one: an arbitrary pairing would produce a confident verdict about a
        // pair we invented. Every entry involved is reported NotComparable, so
        // the user sees all of them and decides.
        if (sidx.size() > 1 || tidx.size() > 1) {
            const wxString why = L"归一化签名冲突（存在同名同参的多个重载），无法确定配对，请人工对照";
            for (size_t i : sidx) { srcUsed[i] = true; oneSided(source[i], true,  RoutineVerdict::NotComparable, key, why); }
            for (size_t i : tidx) { tgtUsed[i] = true; oneSided(target[i], false, RoutineVerdict::NotComparable, key, why); }
            continue;
        }

        const RoutineDef& s = source[sidx[0]];
        const RoutineDef& t = target[tidx[0]];
        srcUsed[sidx[0]] = true;
        tgtUsed[tidx[0]] = true;

        RoutinePair p;
        p.kind      = s.kind;
        p.key       = key;
        p.hasSource = true;  p.source = s;
        p.hasTarget = true;  p.target = t;
        Judge(s, t, opt, p);
        pairs.push_back(std::move(p));
    }

    // ---- re-join leftovers that are obviously the same routine -------------
    //
    // A routine whose KIND changed (function -> procedure), or whose parameter
    // types changed in a way the lexical key cannot see through (MySQL
    // `tinyint` vs PG `smallint`), lands here as one leftover on each side.
    // Reporting those as an unrelated deletion PLUS an unrelated addition is
    // technically defensible but practically useless — the user has to notice
    // the two rows are about the same routine.
    //
    // The re-join is deliberately timid: it fires ONLY when a name has exactly
    // one leftover on each side AND both have the same parameter count. With PG
    // overloading, a looser rule could marry two genuinely unrelated overloads
    // and hide that one was dropped.
    {
        std::map<wxString, std::vector<size_t>> sLeft, tLeft;
        for (size_t i = 0; i < source.size(); ++i)
            if (!srcUsed[i]) sLeft[FoldName(source[i].name, opt.foldNameCase)].push_back(i);
        for (size_t i = 0; i < target.size(); ++i)
            if (!tgtUsed[i]) tLeft[FoldName(target[i].name, opt.foldNameCase)].push_back(i);

        for (const auto& [name, sv] : sLeft) {
            const auto it = tLeft.find(name);
            if (it == tLeft.end() || sv.size() != 1 || it->second.size() != 1) continue;
            const RoutineDef& s = source[sv[0]];
            const RoutineDef& t = target[it->second[0]];
            if (s.argTypes.size() != t.argTypes.size()) continue;

            srcUsed[sv[0]] = true;
            tgtUsed[it->second[0]] = true;

            RoutinePair p;
            p.kind      = s.kind;
            p.key       = RoutineMatchKey(s, opt);
            p.hasSource = true;  p.source = s;
            p.hasTarget = true;  p.target = t;
            Judge(s, t, opt, p);
            // Judge() can only have found a difference here — the two did not
            // share a key — but if the declarations somehow matched, the honest
            // label is still "签名不同" rather than a body verdict on a pair the
            // key refused to join.
            if (p.verdict == RoutineVerdict::Identical ||
                p.verdict == RoutineVerdict::BodyDiffers) {
                p.verdict = RoutineVerdict::SignatureDiffers;
                p.reason  = L"声明不同（按名称与参数个数配对）：" + s.Signature() +
                            L" vs " + t.Signature();
            }
            pairs.push_back(std::move(p));
        }
    }

    // ---- genuinely one-sided ----------------------------------------------
    for (size_t i = 0; i < source.size(); ++i)
        if (!srcUsed[i])
            oneSided(source[i], true, RoutineVerdict::SourceOnly,
                     RoutineMatchKey(source[i], opt),
                     L"目标端不存在同名同参的" + KindLabel(source[i].kind));
    for (size_t i = 0; i < target.size(); ++i)
        if (!tgtUsed[i])
            oneSided(target[i], false, RoutineVerdict::TargetOnly,
                     RoutineMatchKey(target[i], opt),
                     L"源端不存在同名同参的" + KindLabel(target[i].kind));

    // ---- stable order: differences first, then identical, then by key ------
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const RoutinePair& a, const RoutinePair& b) {
                         const bool ad = RoutineVerdictIsDifference(a.verdict);
                         const bool bd = RoutineVerdictIsDifference(b.verdict);
                         if (ad != bd) return ad;
                         return a.key < b.key;
                     });

    for (const auto& p : pairs) Tally(out.stat, p.verdict);
    out.pairs = std::move(pairs);
    return out;
}

} // namespace db::sync
