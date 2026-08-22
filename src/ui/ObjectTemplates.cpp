// ObjectTemplates.cpp — see ObjectTemplates.h.
#include "ui/ObjectTemplates.h"

#include <vector>

namespace ui {

using db::Dialect;

// ---------------------------------------------------------------------------
bool ObjectCreateSupported(ObjectKind kind, Dialect d)
{
    switch (kind) {
    case ObjectKind::View:
        // Single-statement DDL — safe on every engine (SQLite gets DROP+CREATE,
        // which is still two clean single statements).
        return true;
    case ObjectKind::Function:
    case ObjectKind::Procedure:
        // Only where SplitSqlScript keeps the routine body intact: MySQL via
        // DELIMITER, PostgreSQL via $$ dollar-quoting.
        return d == Dialect::MySQL || d == Dialect::Postgres;
    case ObjectKind::Package:
        // Oracle/DM only conceptually, but the splitter can't keep a PL/SQL
        // package body intact yet → gated off.
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
static wxString ViewTemplate(Dialect d)
{
    switch (d) {
    case Dialect::SqlServer:
        // T-SQL has no CREATE OR REPLACE; CREATE OR ALTER is the idempotent form
        // (SQL Server 2016 SP1+).
        return L"CREATE OR ALTER VIEW view_name AS\n"
               L"SELECT id, name\n"
               L"FROM   some_table;\n";
    case Dialect::Sqlite:
        // SQLite has no OR REPLACE for views → DROP IF EXISTS + CREATE.
        return L"DROP VIEW IF EXISTS view_name;\n"
               L"CREATE VIEW view_name AS\n"
               L"SELECT id, name\n"
               L"FROM   some_table;\n";
    case Dialect::MySQL:
    case Dialect::Postgres:
    case Dialect::Oracle:
    default:
        return L"CREATE OR REPLACE VIEW view_name AS\n"
               L"SELECT id, name\n"
               L"FROM   some_table;\n";
    }
}

static wxString ProcedureTemplate(Dialect d)
{
    switch (d) {
    case Dialect::MySQL:
        return L"DROP PROCEDURE IF EXISTS proc_name;\n"
               L"DELIMITER $$\n"
               L"CREATE PROCEDURE proc_name()\n"
               L"BEGIN\n"
               L"    -- TODO: 存储过程逻辑\n"
               L"    SELECT 1;\n"
               L"END$$\n"
               L"DELIMITER ;\n";
    case Dialect::Postgres:
        return L"CREATE OR REPLACE PROCEDURE proc_name()\n"
               L"LANGUAGE plpgsql\n"
               L"AS $$\n"
               L"BEGIN\n"
               L"    -- TODO: 存储过程逻辑\n"
               L"    NULL;\n"
               L"END;\n"
               L"$$;\n";
    case Dialect::Oracle:
        // Runs only once a PL/SQL-aware splitter lands (see ObjectCreateSupported).
        return L"CREATE OR REPLACE PROCEDURE proc_name AS\n"
               L"BEGIN\n"
               L"    NULL;\n"
               L"END proc_name;\n"
               L"/\n";
    case Dialect::SqlServer:
        return L"CREATE OR ALTER PROCEDURE proc_name AS\n"
               L"BEGIN\n"
               L"    SET NOCOUNT ON;\n"
               L"    -- TODO: 存储过程逻辑\n"
               L"END;\n";
    default:
        return L"CREATE PROCEDURE proc_name()\nBEGIN\n    NULL;\nEND;\n";
    }
}

static wxString FunctionTemplate(Dialect d)
{
    switch (d) {
    case Dialect::MySQL:
        return L"DROP FUNCTION IF EXISTS func_name;\n"
               L"DELIMITER $$\n"
               L"CREATE FUNCTION func_name(p_in INT) RETURNS INT DETERMINISTIC\n"
               L"BEGIN\n"
               L"    -- TODO: 函数逻辑\n"
               L"    RETURN p_in;\n"
               L"END$$\n"
               L"DELIMITER ;\n";
    case Dialect::Postgres:
        return L"CREATE OR REPLACE FUNCTION func_name(p_in INT) RETURNS INT\n"
               L"LANGUAGE plpgsql\n"
               L"AS $$\n"
               L"BEGIN\n"
               L"    -- TODO: 函数逻辑\n"
               L"    RETURN p_in;\n"
               L"END;\n"
               L"$$;\n";
    case Dialect::Oracle:
        // Oracle uses RETURN (not RETURNS) and NUMBER as the canonical numeric type.
        return L"CREATE OR REPLACE FUNCTION func_name(p_in NUMBER) RETURN NUMBER AS\n"
               L"BEGIN\n"
               L"    RETURN p_in;\n"
               L"END func_name;\n"
               L"/\n";
    case Dialect::SqlServer:
        return L"CREATE OR ALTER FUNCTION func_name(@p_in INT) RETURNS INT AS\n"
               L"BEGIN\n"
               L"    RETURN @p_in;\n"
               L"END;\n";
    default:
        return L"CREATE FUNCTION func_name() RETURNS INT\nBEGIN\n    RETURN 0;\nEND;\n";
    }
}

static wxString PackageTemplate(Dialect /*d*/)
{
    // Oracle / DM only. Spec + body, both CREATE OR REPLACE.
    return L"CREATE OR REPLACE PACKAGE pkg_name AS\n"
           L"    PROCEDURE do_work;\n"
           L"END pkg_name;\n"
           L"/\n\n"
           L"CREATE OR REPLACE PACKAGE BODY pkg_name AS\n"
           L"    PROCEDURE do_work IS\n"
           L"    BEGIN\n"
           L"        NULL;\n"
           L"    END do_work;\n"
           L"END pkg_name;\n"
           L"/\n";
}

wxString ObjectTemplate(ObjectKind kind, Dialect d)
{
    switch (kind) {
    case ObjectKind::View:      return ViewTemplate(d);
    case ObjectKind::Procedure: return ProcedureTemplate(d);
    case ObjectKind::Function:  return FunctionTemplate(d);
    case ObjectKind::Package:   return PackageTemplate(d);
    }
    return wxString();
}

wxString ObjectNewTitle(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::View:      return L"新建视图";
    case ObjectKind::Function:  return L"新建函数";
    case ObjectKind::Procedure: return L"新建存储过程";
    case ObjectKind::Package:   return L"新建包";
    }
    return L"新建对象";
}

// ---------------------------------------------------------------------------
// Modify-flow DDL wrapping.
// ---------------------------------------------------------------------------
namespace {

// Scan a MySQL "DEFINER=" operand (the <user> or <host> part) starting at i,
// skipping leading blanks. Accepts a backtick/single/double-quoted string or a
// bare token (up to blank / '@'). Returns one-past-end, or npos if malformed.
size_t ScanDefinerPart(const wxString& s, size_t i)
{
    const size_t n = s.length();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) return wxString::npos;
    const wxChar c = s[i];
    if (c == '`' || c == '\'' || c == '"') {
        const wxChar close = c;
        for (++i; i < n; ++i)
            if (s[i] == close) return i + 1;   // closing quote consumed
        return wxString::npos;                 // unterminated
    }
    while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '@') ++i;
    return i;
}

// Remove a MySQL "DEFINER=<user>@<host>" clause (SHOW CREATE … emits it and a
// re-run under a different account then fails with a privilege error). Conservative:
// strips only that one segment (the first "DEFINER=" — the "SQL SECURITY DEFINER"
// keyword form has no '=' and is left intact). Returns the input unchanged if no
// well-formed DEFINER= clause is found.
wxString StripDefiner(const wxString& in)
{
    const wxString up = in.Upper();
    const size_t at = up.find(L"DEFINER");
    if (at == wxString::npos) return in;
    size_t i = at + 7;                          // past "DEFINER"
    while (i < in.length() && (in[i] == ' ' || in[i] == '\t')) ++i;
    if (i >= in.length() || in[i] != '=') return in;   // not the "DEFINER=" form
    ++i;
    const size_t userEnd = ScanDefinerPart(in, i);
    if (userEnd == wxString::npos) return in;
    size_t j = userEnd;
    while (j < in.length() && (in[j] == ' ' || in[j] == '\t')) ++j;
    if (j >= in.length() || in[j] != '@') return in;
    const size_t hostEnd = ScanDefinerPart(in, j + 1);
    if (hostEnd == wxString::npos) return in;

    wxString left  = in.Left(at);
    wxString right = in.Mid(hostEnd);
    left.Trim(true);        // drop the blank before "DEFINER"
    right.Trim(false);      // and the blank after the host
    return left + L" " + right;
}

// Insert "OR REPLACE " / "OR ALTER " right after the leading CREATE keyword, if
// the DDL isn't already in a replace form.
wxString EnsureCreateReplace(const wxString& ddlIn, bool useOrAlter)
{
    wxString ddl = ddlIn;
    ddl.Trim(true).Trim(false);
    const wxString up = ddl.Upper();
    if (up.find(L"OR REPLACE") != wxString::npos ||
        up.find(L"OR ALTER") != wxString::npos)
        return ddl;
    // Find the first "CREATE" token and splice after it.
    const size_t at = up.find(L"CREATE");
    if (at == wxString::npos) return ddl;
    const size_t after = at + 6;   // length of "CREATE"
    const wxString kw = useOrAlter ? L"CREATE OR ALTER" : L"CREATE OR REPLACE";
    return ddl.Left(at) + kw + ddl.Mid(after);
}

} // namespace

wxString ObjectModifyDdl(ObjectKind kind, Dialect d,
                         const wxString& name, const wxString& fetchedDdl)
{
    wxString ddl = fetchedDdl;
    ddl.Trim(true).Trim(false);
    if (ddl.IsEmpty()) return ObjectTemplate(kind, d);

    // MySQL's SHOW CREATE … prefixes a DEFINER=`user`@`host` clause; strip it so a
    // re-run under a different account doesn't fail with a privilege error.
    if (d == Dialect::MySQL) ddl = StripDefiner(ddl);

    switch (kind) {
    case ObjectKind::View:
        return EnsureCreateReplace(ddl, /*useOrAlter*/ d == Dialect::SqlServer)
               + (ddl.EndsWith(L";") ? L"\n" : L";\n");

    case ObjectKind::Function:
    case ObjectKind::Procedure: {
        const wxString type = (kind == ObjectKind::Procedure) ? L"PROCEDURE" : L"FUNCTION";
        if (d == Dialect::MySQL) {
            // Bare body from SHOW CREATE … → DROP-IF-EXISTS + DELIMITER wrap so a
            // re-run replaces rather than errors "already exists".
            return L"DROP " + type + L" IF EXISTS " + name + L";\n"
                   L"DELIMITER $$\n" + ddl + L"$$\nDELIMITER ;\n";
        }
        if (d == Dialect::Postgres) {
            // pg_get_functiondef already yields CREATE OR REPLACE FUNCTION … .
            return EnsureCreateReplace(ddl, false) + L"\n";
        }
        // Oracle / DM / SQL Server: fetched source may start at the routine keyword
        // without CREATE; make a best-effort replace form (execution still pending
        // a block-aware splitter — see ObjectCreateSupported).
        if (d == Dialect::SqlServer)
            return EnsureCreateReplace(ddl, /*useOrAlter*/ true) + L"\n";
        return EnsureCreateReplace(ddl, false) + L"\n";
    }

    case ObjectKind::Package:
        return EnsureCreateReplace(ddl, false) + L"\n";
    }
    return ddl;
}

// ---------------------------------------------------------------------------
// Object-name parsing (for renaming the tab after a successful run).
// ---------------------------------------------------------------------------
namespace {

bool IsWordChar(wxUniChar c)
{
    const wchar_t v = static_cast<wchar_t>(c.GetValue());
    return (v >= L'a' && v <= L'z') || (v >= L'A' && v <= L'Z') ||
           (v >= L'0' && v <= L'9') || v == L'_' || v == L'$' || v == L'.';
}

// Split into "words": runs of identifier chars (keeping '.' so a schema-qualified
// name stays one word) plus quoted/backticked segments (quotes stripped). Any
// other character is a separator.
std::vector<wxString> TokenizeWords(const wxString& sql)
{
    std::vector<wxString> words;
    wxString cur;
    const size_t n = sql.length();
    auto push = [&] { if (!cur.IsEmpty()) { words.push_back(cur); cur.clear(); } };
    for (size_t i = 0; i < n; ) {
        const wxUniChar c = sql[i];
        if (c == '`' || c == '"' || c == '[') {
            const wxUniChar close = (c == '[') ? wxUniChar(']') : c;
            ++i;
            while (i < n && sql[i] != close) { cur += sql[i]; ++i; }
            if (i < n) ++i;   // skip closing quote
            continue;         // keep accumulating (so `db`.`v` → db.v)
        }
        if (IsWordChar(c)) { cur += c; ++i; continue; }
        push();
        ++i;
    }
    push();
    return words;
}

wxString StripQualifier(wxString w)
{
    if (w.Contains(L".")) w = w.AfterLast('.');
    return w;
}

bool IsTypeKeyword(const wxString& up)
{
    return up == L"VIEW" || up == L"FUNCTION" || up == L"PROCEDURE" ||
           up == L"PROC" || up == L"PACKAGE" || up == L"TRIGGER";
}

} // namespace

wxString ParseObjectName(const wxString& sql)
{
    const std::vector<wxString> w = TokenizeWords(sql);
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i].Upper() != L"CREATE") continue;
        size_t j = i + 1;
        if (j < w.size() && w[j].Upper() == L"OR") {
            ++j;
            if (j < w.size() && (w[j].Upper() == L"REPLACE" || w[j].Upper() == L"ALTER"))
                ++j;
        }
        // Skip MySQL/engine noise (ALGORITHM, DEFINER value, SQL SECURITY, …) up to
        // the object-type keyword.
        while (j < w.size() && !IsTypeKeyword(w[j].Upper())) ++j;
        if (j >= w.size()) continue;
        const wxString type = w[j].Upper();
        ++j;
        if (type == L"PACKAGE" && j < w.size() && w[j].Upper() == L"BODY") ++j;
        if (j < w.size()) return StripQualifier(w[j]);
    }
    return wxString();
}

// ---------------------------------------------------------------------------
// Rename-by-rebuild DDL surgery (BuildRenamedCreateDdl).
// ---------------------------------------------------------------------------
namespace {

inline bool IsSpaceCh(wxUniChar c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// First whole-word (identifier-boundary) occurrence of `word` in the uppercased
// text `up`, at or after `from`. Avoids matching REVIEW/OVERVIEW when looking for
// VIEW, or a substring inside a longer identifier.
size_t FindKeyword(const wxString& up, const wxString& word, size_t from)
{
    const size_t wl = word.length();
    for (;;) {
        const size_t at = up.find(word, from);
        if (at == wxString::npos) return at;
        const bool lok = (at == 0) || !IsWordChar(up[at - 1]);
        const size_t end = at + wl;
        const bool rok = (end >= up.length()) || !IsWordChar(up[end]);
        if (lok && rok) return at;
        from = at + 1;
    }
}

// Scan ONE identifier segment starting at p: a backtick/double-quote/bracket-quoted
// run (quotes included in the span), or a bare run of identifier chars. Returns
// one-past-end, or p itself when there's no identifier there.
size_t ScanIdentSeg(const wxString& s, size_t p)
{
    const size_t n = s.length();
    if (p >= n) return p;
    const wxUniChar c = s[p];
    if (c == '`' || c == '"' || c == '[') {
        const wxUniChar close = (c == '[') ? wxUniChar(']') : c;
        size_t q = p + 1;
        while (q < n && s[q] != close) ++q;
        if (q < n) ++q;   // consume the closing quote
        return q;
    }
    size_t q = p;
    while (q < n && IsWordChar(s[q]) && s[q] != '.') ++q;
    return q;
}

// Remove a leading "OR REPLACE"/"OR ALTER" right after the first CREATE, so the
// rebuilt CREATE fails (rather than clobbers) if the new name already exists.
wxString StripOrReplaceAfterCreate(const wxString& in)
{
    const wxString up = in.Upper();
    const size_t c = FindKeyword(up, L"CREATE", 0);
    if (c == wxString::npos) return in;
    const size_t n = in.length();
    size_t i = c + 6;                                   // just past "CREATE"
    size_t p = i;
    while (p < n && IsSpaceCh(in[p])) ++p;
    size_t t1 = p;
    while (t1 < n && IsWordChar(in[t1])) ++t1;
    if (up.SubString(p, (t1 ? t1 - 1 : 0)) != L"OR" || t1 == p) return in;
    size_t p2 = t1;
    while (p2 < n && IsSpaceCh(in[p2])) ++p2;
    size_t t2 = p2;
    while (t2 < n && IsWordChar(in[t2])) ++t2;
    const wxString w2 = up.SubString(p2, (t2 ? t2 - 1 : 0));
    if (w2 != L"REPLACE" && w2 != L"ALTER") return in;
    return in.Left(i) + in.Mid(t2);                     // "CREATE" + " <rest>"
}

} // namespace

bool BuildRenamedCreateDdl(ObjectKind kind, Dialect d, const wxString& fetchedDdl,
                           const wxString& newNameSql, wxString& out)
{
    wxString ddl = fetchedDdl;
    ddl.Trim(true).Trim(false);
    if (ddl.IsEmpty()) return false;

    if (d == Dialect::MySQL) ddl = StripDefiner(ddl);
    ddl = StripOrReplaceAfterCreate(ddl);

    wxString keyword;
    switch (kind) {
    case ObjectKind::View:      keyword = L"VIEW";      break;
    case ObjectKind::Function:  keyword = L"FUNCTION";  break;
    case ObjectKind::Procedure: keyword = L"PROCEDURE"; break;
    default: return false;   // packages not handled by rename-by-rebuild
    }

    const wxString up = ddl.Upper();
    const size_t kw = FindKeyword(up, keyword, 0);
    if (kw == wxString::npos) return false;
    const size_t n = ddl.length();
    size_t i = kw + keyword.length();
    while (i < n && IsSpaceCh(ddl[i])) ++i;             // to the object name
    const size_t nameStart = i;
    size_t nameEnd = ScanIdentSeg(ddl, i);
    if (nameEnd == nameStart) return false;             // no identifier → abort
    // A schema/owner qualifier ("owner"."name" / `db`.`v`): skip the '.' and the
    // trailing segment so the WHOLE qualified name is replaced by newNameSql.
    if (nameEnd < n && ddl[nameEnd] == '.') {
        const size_t after = ScanIdentSeg(ddl, nameEnd + 1);
        if (after > nameEnd + 1) nameEnd = after;
    }
    out = ddl.Left(nameStart) + newNameSql + ddl.Mid(nameEnd);
    return true;
}

} // namespace ui
