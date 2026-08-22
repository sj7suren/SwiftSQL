// MySqlRoutineRead.cpp — see the header. Pure db::IConnection consumer; no
// MYSQL* handle, no libmariadb include, no DDL rendering.
#include "db/MySqlRoutineRead.h"

#include <map>

namespace db {
namespace mysqlroutine {

namespace {

wxString Esc(const wxString& s)
{
    wxString e = s;
    e.Replace(L"'", L"''");
    return e;
}

// Does this server error mean "you may not look"? Matching on text is crude,
// but the alternative — treating every failure as a privilege problem — would
// label a network drop as "权限不足" and send the user chasing the wrong thing.
// Anything unrecognized stays Unsupported, whose detail carries the raw text.
bool LooksLikePermissionError(const wxString& err)
{
    wxString e = err;
    e.MakeLower();
    return e.Contains(L"denied") || e.Contains(L"privilege") ||
           e.Contains(L"permission") || e.Contains(L"1370") || e.Contains(L"1044") ||
           e.Contains(L"拒绝") || e.Contains(L"权限");
}

// The driver's QueryResult renders a SQL NULL as the literal text "NULL"
// (db/DbDriver.h). Every query below therefore wraps nullable columns in
// COALESCE so an absent value arrives as an empty string — but a column that
// slips through would hand us the four characters N-U-L-L as if they were data,
// and for a routine BODY that would mean two invisible bodies comparing equal.
// Belt and braces: treat the literal "NULL" as absent too.
wxString NullToEmpty(const wxString& v)
{
    return v == L"NULL" ? wxString() : v;
}

// ===========================================================================
// The routine-privilege probe — the second signal an empty list needs
// ===========================================================================
// MySQL 8 filters information_schema.ROUTINES per row, so an empty list is
// AMBIGUOUS: it means either "this database has no routines" or "you may not
// see this database's routines", and the list alone cannot tell them apart.
// Everything below exists to acquire the missing bit from a second source:
// what the server says the account has been granted.
//
// It is deliberately conservative in ONE direction. Answering Granted when the
// account actually has nothing sends us back to the silent-mass-false-positive
// behaviour this whole unit is defending against, so Granted is returned only
// on an unambiguous, positively matched grant. Everything doubtful — an
// unparsed line, a role grant, a wildcard database pattern, a failed query —
// lands on Indeterminate, which the caller degrades rather than trusts.
enum class RoutinePriv {
    Granted,        // a grant positively conferring a routine privilege was seen
    Absent,         // grants parsed cleanly and NONE conferred one
    Indeterminate   // SHOW GRANTS failed, or its output could not be read with confidence
};

// Strip MySQL's backtick/quote wrapping from one identifier, undoubling any
// escaped quote inside it.
wxString Unquote(const wxString& s)
{
    wxString t = s;
    t.Trim(true).Trim(false);
    if (t.length() >= 2) {
        const wxChar q = t[0];
        if ((q == L'`' || q == L'\'' || q == L'"') && t.Last() == q) {
            t = t.Mid(1, t.length() - 2);
            wxString dbl; dbl << q << q;
            t.Replace(dbl, wxString(q));
        }
    }
    return t;
}

// Find the last '.' that is NOT inside a backtick-quoted identifier, so a
// database literally named `my.db` splits at the right dot.
size_t LastUnquotedDot(const wxString& s)
{
    size_t found = wxString::npos;
    bool inTick = false;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == L'`') inTick = !inTick;
        else if (s[i] == L'.' && !inTick) found = i;
    }
    return found;
}

// Split a privilege list on commas that are outside parentheses and backticks.
// Column-level grants render as `SELECT (`a`, `b`), UPDATE`, so a naive split
// would produce the fragments "SELECT (`a`" and "`b`)" and could mistake a
// COLUMN named `execute` for the EXECUTE privilege.
std::vector<wxString> SplitPrivileges(const wxString& privs)
{
    std::vector<wxString> out;
    wxString cur;
    int depth = 0;
    bool inTick = false;
    for (size_t i = 0; i < privs.length(); ++i) {
        const wxChar c = privs[i];
        if (c == L'`') inTick = !inTick;
        if (!inTick) {
            if (c == L'(') ++depth;
            else if (c == L')') { if (depth > 0) --depth; }
            else if (c == L',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        }
        cur += c;
    }
    out.push_back(cur);
    for (auto& p : out) {
        // Drop any column list, then normalize whitespace and case so
        // "ALTER  ROUTINE" and "alter routine" both compare equal.
        const size_t paren = p.find(L'(');
        if (paren != wxString::npos) p = p.Mid(0, paren);
        p.Trim(true).Trim(false);
        p.MakeUpper();
        while (p.Replace(L"  ", L" ")) {}
    }
    return out;
}

// Does this privilege token confer the ability to SEE routines?
//
// EXECUTE / CREATE ROUTINE / ALTER ROUTINE are the three routine privileges;
// holding any one of them makes a schema's routines visible in
// information_schema.ROUTINES. ALL [PRIVILEGES] is the superset spelling.
bool IsRoutinePrivilege(const wxString& p)
{
    return p == L"EXECUTE" || p == L"CREATE ROUTINE" || p == L"ALTER ROUTINE" ||
           p == L"ALL PRIVILEGES" || p == L"ALL";
}

// Would this database pattern possibly match `database` without matching it
// literally? MySQL grant scopes may carry LIKE wildcards (`GRANT ... ON
// \`app\_%\`.*`), and resolving them properly means reimplementing the server's
// escaping rules. We do not: a wildcard we did not match literally makes the
// whole verdict Indeterminate, which degrades visibly instead of guessing.
bool HasWildcard(const wxString& dbPattern)
{
    return dbPattern.Contains(L"%") || dbPattern.Contains(L"_");
}

// Read SHOW GRANTS FOR CURRENT_USER() and decide whether this account holds a
// routine privilege on `database` (or globally).
//
// CHEAP AND FAILURE-TOLERANT BY CONTRACT. One query, called only when the
// routine list came back empty, and every failure mode — the query erroring, an
// empty result, a line we cannot parse — resolves to Indeterminate rather than
// propagating an error. A privilege probe must never be the thing that breaks a
// compare.
RoutinePriv ProbeRoutinePrivilege(IConnection& conn, const wxString& database,
                                  wxString& why)
{
    QueryResult r;
    wxString err;
    if (!conn.Execute(L"SHOW GRANTS FOR CURRENT_USER()", r, err)) {
        why = err.IsEmpty() ? wxString(L"SHOW GRANTS 执行失败") : err;
        return RoutinePriv::Indeterminate;
    }
    if (r.rows.empty()) {
        why = L"SHOW GRANTS 未返回任何行";
        return RoutinePriv::Indeterminate;
    }

    bool fuzzy = false;      // at least one line we could not read with confidence
    bool parsedAny = false;  // at least one line we COULD read

    for (const auto& row : r.rows) {
        if (row.empty()) { fuzzy = true; continue; }

        wxString line = row[0];
        line.Trim(true).Trim(false);

        wxString head = line.Mid(0, 6);
        head.MakeUpper();
        if (head != L"GRANT ") { fuzzy = true; continue; }

        // "GRANT `role`@`%` TO `user`@`%`" — a ROLE grant, which has no ON
        // clause. MySQL 8 does not expand a role's privileges here (that needs
        // SHOW GRANTS ... USING), so the account may well hold routine
        // privileges we simply cannot see. Never Absent once one of these
        // appears.
        const size_t onAt = line.Upper().find(L" ON ");
        if (onAt == wxString::npos) { fuzzy = true; continue; }

        const size_t toAt = line.Upper().find(L" TO ", onAt + 4);
        if (toAt == wxString::npos) { fuzzy = true; continue; }

        const wxString privs = line.Mid(6, onAt - 6);
        wxString scope = line.Mid(onAt + 4, toAt - (onAt + 4));
        scope.Trim(true).Trim(false);

        // A routine-level grant names its object kind: "ON PROCEDURE db.p".
        bool routineLevel = false;
        wxString upScope = scope.Upper();
        if (upScope.StartsWith(L"PROCEDURE ")) { routineLevel = true; scope = scope.Mid(10); }
        else if (upScope.StartsWith(L"FUNCTION ")) { routineLevel = true; scope = scope.Mid(9); }
        scope.Trim(false);

        const size_t dot = LastUnquotedDot(scope);
        if (dot == wxString::npos) {
            // e.g. "GRANT PROXY ON ''@''" — not a schema scope at all. Cannot
            // confer a routine privilege, but we also cannot claim to have
            // understood it.
            fuzzy = true;
            continue;
        }

        const wxString dbPart  = Unquote(scope.Mid(0, dot));
        const wxString objPart = Unquote(scope.Mid(dot + 1));

        const bool globalScope = (dbPart == L"*");
        const bool thisSchema  = dbPart.IsSameAs(database, /*caseSensitive=*/false);
        if (!globalScope && !thisSchema) {
            if (HasWildcard(dbPart)) fuzzy = true;   // might have matched; we can't tell
            parsedAny = true;
            continue;
        }

        // A TABLE-level grant ("ON db.t1") cannot carry a routine privilege, so
        // only "*" — the whole schema — counts on the non-routine path.
        if (!routineLevel && objPart != L"*") { parsedAny = true; continue; }

        for (const auto& p : SplitPrivileges(privs))
            if (IsRoutinePrivilege(p)) { why.clear(); return RoutinePriv::Granted; }

        parsedAny = true;
    }

    if (fuzzy) {
        why = L"SHOW GRANTS 中存在无法确定解读的授权行（角色授权 / 通配库名 / 非常规格式）";
        return RoutinePriv::Indeterminate;
    }
    if (!parsedAny) {
        why = L"SHOW GRANTS 未返回任何可解析的库级授权";
        return RoutinePriv::Indeterminate;
    }
    why.clear();
    return RoutinePriv::Absent;
}

// The empty-list verdict, isolated so the reasoning sits in one place.
//
// The three-way split is the entire fix for the MySQL 8 hazard, and the middle
// case is the one that is easy to get wrong: an empty list from an account we
// KNOW can see routines is a real answer and must stay Ok, or every legitimately
// empty database starts reporting a permission problem it does not have.
sync::RoutineReadStatus EmptyListVerdict(IConnection& conn, const wxString& database,
                                         wxString& detail)
{
    wxString why;
    switch (ProbeRoutinePrivilege(conn, database, why)) {
    case RoutinePriv::Granted:
        // The account demonstrably CAN see routines and saw none. Genuinely
        // empty catalog — the pre-MySQL-8 meaning of an empty list, now earned
        // rather than assumed.
        detail.clear();
        return sync::RoutineReadStatus::Ok;

    case RoutinePriv::Absent:
        // Empty list from an account holding no routine privilege at all. On
        // MySQL 8 that list is empty BECAUSE of the privilege state, so report
        // the privilege state. (On a genuinely empty database this account would
        // also be told "permission" — accepted deliberately: the user is told to
        // retry with a better account, which is recoverable, whereas the
        // alternative is a silent screenful of wrong 仅目标端存在 rows.)
        detail = L"当前账号在数据库 " + database +
                 L" 上没有任何例程权限（EXECUTE / CREATE ROUTINE / ALTER ROUTINE）。"
                 L"MySQL 8 会按行过滤 information_schema.ROUTINES，因此“没有函数/存储过程”"
                 L"与“无权查看函数/存储过程”无法区分；请改用具备例程权限的账号重新比对。";
        return sync::RoutineReadStatus::PermissionDenied;

    case RoutinePriv::Indeterminate:
    default:
        // We could not establish EITHER answer. Do NOT fall back to Ok: Ok here
        // is precisely the silent mass-false-positive state, and an unverified
        // guess in that direction is worse than no comparison.
        //
        // Unsupported rather than PermissionDenied, deliberately.
        // PermissionDenied is a CLAIM ABOUT THE ACCOUNT — 「不可见（权限不足）」 —
        // and here we specifically do not know that it is true; asserting it
        // would send a fully-privileged user chasing a grant they already have,
        // which is the same species of confident-wrong-answer this fix exists to
        // remove, merely pointed the other way. Unsupported's documented meaning
        // in RoutineDiff.h is "something stopped us reading; the list is not
        // available", which is exactly and only what we know. Both statuses are
        // non-Ok, so the SAFETY is identical — the category degrades visibly and
        // emits zero pairs either way; only the explanation differs, and this is
        // the honest one.
        detail = L"information_schema.ROUTINES 返回空列表，且无法确认当前账号是否拥有例程权限（" +
                 why + L"）。MySQL 8 会按行过滤例程，空列表既可能是“确实没有例程”，"
                 L"也可能是“无权查看”；为避免把目标端的全部例程误报为“仅目标端存在”，"
                 L"本侧不作比较。";
        return sync::RoutineReadStatus::Unsupported;
    }
}

} // namespace

sync::RoutineReadStatus ReadRoutines(IConnection& conn, const wxString& database,
                               std::vector<sync::RoutineDef>& out, wxString& detail)
{
    out.clear();
    detail.clear();

    const wxString db = Esc(database);

    // ---- the routines themselves ------------------------------------------
    //
    // ROUTINE_DEFINITION is the routine's INNER BODY (`BEGIN … END`), not a
    // CREATE statement — which is exactly why this reader uses it instead of
    // SHOW CREATE PROCEDURE. SHOW CREATE returns deployable DDL, and this
    // feature is forbidden to hold deployable DDL (RoutineDiff.h). Reading the
    // body only is both the honest compare input and the structural guarantee.
    QueryResult r;
    wxString err;
    if (!conn.Execute(wxString::Format(
            L"SELECT ROUTINE_NAME, ROUTINE_TYPE, "
            L"COALESCE(DTD_IDENTIFIER,''), COALESCE(ROUTINE_DEFINITION,''), "
            L"COALESCE(SECURITY_TYPE,''), COALESCE(ROUTINE_BODY,''), "
            L"COALESCE(EXTERNAL_LANGUAGE,'') "
            L"FROM information_schema.ROUTINES "
            L"WHERE ROUTINE_SCHEMA='%s' "
            L"ORDER BY ROUTINE_TYPE, ROUTINE_NAME", db), r, err)) {
        detail = err;
        return LooksLikePermissionError(err) ? sync::RoutineReadStatus::PermissionDenied
                                             : sync::RoutineReadStatus::Unsupported;
    }

    // Index by name+type so the parameter pass can find its routine. MySQL does
    // not support overloading, so name+type is unique within a schema — this is
    // the one engine where that shortcut is actually safe.
    std::map<wxString, size_t> byKey;

    for (const auto& row : r.rows) {
        if (row.size() < 7) continue;

        sync::RoutineDef d;
        d.schema     = database;
        d.name       = row[0];
        d.kind       = (row[1] == L"FUNCTION") ? sync::RoutineKind::Function
                                               : sync::RoutineKind::Procedure;
        d.returnType = NullToEmpty(row[2]);
        d.body       = NullToEmpty(row[3]);
        d.securityDefiner = (row[4] == L"DEFINER");

        // ROUTINE_BODY is 'SQL' for every routine MySQL can store;
        // EXTERNAL_LANGUAGE is only populated for the (rare) external-language
        // routines. Prefer the specific one when present.
        const wxString ext = NullToEmpty(row[6]);
        d.language = ext.IsEmpty() ? NullToEmpty(row[5]) : ext;
        if (d.language.IsEmpty()) d.language = L"SQL";

        // A stored routine cannot legally have an empty body, so an empty one
        // here means MySQL withheld it (no privilege) rather than that it is
        // blank. Marking it unreadable is what stops the compare layer from
        // reporting two withheld bodies as identical.
        d.bodyReadable = !d.body.IsEmpty();

        // A procedure has no return type; MySQL leaves DTD_IDENTIFIER empty for
        // one, but normalize explicitly so the compare never sees a stray value.
        if (d.kind == sync::RoutineKind::Procedure) d.returnType.clear();

        byKey[row[1] + L"\x1F" + row[0]] = out.size();
        out.push_back(std::move(d));
    }

    if (out.empty()) {
        // NOT automatically "genuinely empty catalog" — that was the defect.
        //
        // MySQL 8 backs information_schema with the data dictionary and filters
        // ROUTINES per row, so an account without routine privileges gets zero
        // rows and no error: byte-for-byte the same response as an empty
        // database. Measured on 8.0.36; see the header. Neither of this unit's
        // other defenses can fire here — the blank-body heuristic has nothing to
        // list, and the PARAMETERS degradation needs a FAILED query.
        //
        // So ask a second source what this account is allowed to see, and let
        // that decide. This is the only place the probe runs: one extra query,
        // only on the empty path, never in the common case.
        return EmptyListVerdict(conn, database, detail);
    }

    // ---- parameters --------------------------------------------------------
    //
    // ORDINAL_POSITION 0 is a FUNCTION's return value, not a parameter, so it is
    // filtered out here rather than being silently prepended to the argument
    // list — which would shift every argument by one and make every function
    // mismatch its counterpart.
    QueryResult pr;
    if (conn.Execute(wxString::Format(
            L"SELECT SPECIFIC_NAME, ROUTINE_TYPE, ORDINAL_POSITION, "
            L"COALESCE(PARAMETER_MODE,''), COALESCE(DTD_IDENTIFIER,'') "
            L"FROM information_schema.PARAMETERS "
            L"WHERE SPECIFIC_SCHEMA='%s' AND ORDINAL_POSITION > 0 "
            L"ORDER BY SPECIFIC_NAME, ORDINAL_POSITION", db), pr, err)) {
        for (const auto& row : pr.rows) {
            if (row.size() < 5) continue;
            const auto it = byKey.find(row[1] + L"\x1F" + row[0]);
            if (it == byKey.end()) continue;
            sync::RoutineDef& d = out[it->second];
            d.argTypes.push_back(NullToEmpty(row[4]));
            wxString mode = NullToEmpty(row[3]);
            mode.MakeUpper();
            d.argModes.push_back(mode);
        }
    } else {
        // Parameters unreadable while ROUTINES was readable. We must NOT continue
        // with empty argument lists, and the reason is asymmetry.
        //
        // RoutineMatchKey() (RoutineCompare.cpp) builds the matching key from
        // name + argTypes, and the name-based re-join pass additionally requires
        // s.argTypes.size() == t.argTypes.size(). If PARAMETERS is readable on
        // ONE side and not the other — the realistic case, a restricted
        // production account compared against a full development one — then
        // every source key carries a signature and every target key carries an
        // empty one. Nothing matches, the re-join is blocked by the arity test,
        // and EVERY routine is emitted twice: once SourceOnly, once TargetOnly.
        // A screen of pure false positives, against a feature whose bar is zero.
        //
        // (A SYMMETRIC failure happens to be benign — both sides degrade to
        // name-only keys and still match. We do not rely on that: a reader
        // cannot see the other side, so it cannot know which case it is in.)
        //
        // So the side degrades instead, exactly as the all-bodies-withheld case
        // below does: the category renders its "不可见" label with zero children
        // and `detail` on screen. Losing a comparison the user can retry with
        // better credentials beats handing them confident wrong pairings.
        detail = err;
        out.clear();
        return LooksLikePermissionError(err) ? sync::RoutineReadStatus::PermissionDenied
                                             : sync::RoutineReadStatus::Unsupported;
    }

    // ---- side-wide privilege verdict --------------------------------------
    //
    // Every routine listed but not one body visible is a privilege state, not a
    // coincidence. Report the whole side PermissionDenied so the UI shows the
    // "不可见（权限不足）" label instead of a list of routines it can say
    // nothing about. A PARTIAL loss stays Ok: the readable ones are genuinely
    // comparable, and the withheld ones degrade individually via bodyReadable.
    bool anyBody = false;
    for (const auto& d : out)
        if (d.bodyReadable) { anyBody = true; break; }
    if (!anyBody) {
        detail = L"information_schema.ROUTINES 未返回任何函数体（当前账号缺少查看权限）";
        return sync::RoutineReadStatus::PermissionDenied;
    }

    return sync::RoutineReadStatus::Ok;
}

} // namespace mysqlroutine
} // namespace db
