// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "db/SqlKeywords.h"

namespace db {
namespace {

// Common SQL reserved words shared by MySQL and PostgreSQL.
const wchar_t* const kCommon[] = {
    L"SELECT", L"FROM", L"WHERE", L"AND", L"OR", L"NOT", L"NULL", L"AS",
    L"DISTINCT", L"ORDER", L"BY", L"GROUP", L"HAVING", L"LIMIT", L"OFFSET",
    L"INSERT", L"INTO", L"VALUES", L"UPDATE", L"SET", L"DELETE", L"CREATE",
    L"TABLE", L"VIEW", L"INDEX", L"DROP", L"ALTER", L"ADD", L"COLUMN",
    L"PRIMARY", L"KEY", L"FOREIGN", L"REFERENCES", L"UNIQUE", L"DEFAULT",
    L"JOIN", L"LEFT", L"RIGHT", L"INNER", L"OUTER", L"FULL", L"CROSS", L"ON",
    L"USING", L"UNION", L"ALL", L"EXCEPT", L"INTERSECT", L"CASE", L"WHEN",
    L"THEN", L"ELSE", L"END", L"BETWEEN", L"LIKE", L"IN", L"EXISTS", L"IS",
    L"ASC", L"DESC", L"BEGIN", L"COMMIT", L"ROLLBACK", L"TRUNCATE", L"GRANT",
    L"REVOKE", L"WITH", L"RECURSIVE", L"CAST", L"NULLS", L"FIRST", L"LAST",
    nullptr
};

const wchar_t* const kMySql[] = {
    L"AUTO_INCREMENT", L"UNSIGNED", L"ZEROFILL", L"ENGINE", L"CHARSET",
    L"COLLATE", L"REPLACE", L"IGNORE", L"DUPLICATE", L"SHOW", L"DATABASES",
    L"TABLES", L"COLUMNS", L"STATUS", L"USE", L"LOCK", L"UNLOCK", L"STRAIGHT_JOIN",
    // types
    L"INT", L"INTEGER", L"BIGINT", L"TINYINT", L"SMALLINT", L"MEDIUMINT",
    L"VARCHAR", L"CHAR", L"TEXT", L"LONGTEXT", L"MEDIUMTEXT", L"TINYTEXT",
    L"DATETIME", L"TIMESTAMP", L"DATE", L"TIME", L"YEAR", L"DECIMAL", L"FLOAT",
    L"DOUBLE", L"BLOB", L"JSON", L"ENUM", L"BOOLEAN", L"BIT",
    nullptr
};

const wchar_t* const kPg[] = {
    L"RETURNING", L"ILIKE", L"LATERAL", L"WINDOW", L"OVER", L"PARTITION",
    L"SERIAL", L"BIGSERIAL", L"SMALLSERIAL", L"SETOF", L"TABLESAMPLE",
    L"CONFLICT", L"DO", L"NOTHING", L"MATERIALIZED",
    // types
    L"INTEGER", L"INT", L"BIGINT", L"SMALLINT", L"VARCHAR", L"CHARACTER",
    L"TEXT", L"TIMESTAMP", L"TIMESTAMPTZ", L"DATE", L"TIME", L"INTERVAL",
    L"NUMERIC", L"REAL", L"DOUBLE", L"PRECISION", L"BYTEA", L"JSON", L"JSONB",
    L"UUID", L"BOOLEAN", L"ARRAY", L"MONEY", L"CIDR", L"INET",
    nullptr
};

const wchar_t* const kMySqlFn[] = {
    L"NOW", L"CURDATE", L"CURTIME", L"SYSDATE", L"CONCAT", L"CONCAT_WS",
    L"COALESCE", L"IFNULL", L"NULLIF", L"IF", L"CONVERT", L"COUNT", L"SUM",
    L"AVG", L"MIN", L"MAX", L"GROUP_CONCAT", L"SUBSTRING", L"SUBSTR", L"LENGTH",
    L"CHAR_LENGTH", L"LOWER", L"UPPER", L"TRIM", L"LTRIM", L"RTRIM", L"REPLACE",
    L"LOCATE", L"INSTR", L"LEFT", L"RIGHT", L"LPAD", L"RPAD", L"ROUND", L"FLOOR",
    L"CEIL", L"ABS", L"MOD", L"POWER", L"DATE_FORMAT", L"STR_TO_DATE",
    L"DATEDIFF", L"DATE_ADD", L"DATE_SUB", L"YEAR", L"MONTH", L"DAY", L"HOUR",
    L"MINUTE", L"SECOND", L"UNIX_TIMESTAMP", L"FROM_UNIXTIME", L"JSON_EXTRACT",
    L"JSON_OBJECT", L"JSON_ARRAY", L"JSON_UNQUOTE", L"ROW_NUMBER", L"RANK",
    nullptr
};

const wchar_t* const kPgFn[] = {
    L"now", L"current_date", L"current_timestamp", L"coalesce", L"nullif",
    L"count", L"sum", L"avg", L"min", L"max", L"string_agg", L"array_agg",
    L"substring", L"length", L"char_length", L"lower", L"upper", L"trim",
    L"ltrim", L"rtrim", L"btrim", L"replace", L"position", L"strpos", L"left",
    L"right", L"lpad", L"rpad", L"round", L"floor", L"ceil", L"abs", L"mod",
    L"power", L"to_char", L"to_date", L"to_timestamp", L"date_part",
    L"date_trunc", L"extract", L"age", L"generate_series", L"coalesce",
    L"jsonb_build_object", L"json_build_object", L"jsonb_extract_path",
    L"jsonb_array_elements", L"row_number", L"rank", L"dense_rank", L"lag",
    L"lead", L"nextval", L"currval", nullptr
};

std::vector<wxString> Build(const wchar_t* const* a, const wchar_t* const* b = nullptr)
{
    std::vector<wxString> v;
    for (int i = 0; a[i]; ++i) v.emplace_back(a[i]);
    if (b) for (int i = 0; b[i]; ++i) v.emplace_back(b[i]);
    return v;
}

} // namespace

const std::vector<wxString>& Keywords(Dialect d)
{
    static const std::vector<wxString> mysql = Build(kCommon, kMySql);
    static const std::vector<wxString> pg    = Build(kCommon, kPg);
    return d == Dialect::Postgres ? pg : mysql;
}

const std::vector<wxString>& Functions(Dialect d)
{
    static const std::vector<wxString> mysql = Build(kMySqlFn);
    static const std::vector<wxString> pg    = Build(kPgFn);
    return d == Dialect::Postgres ? pg : mysql;
}

wxString FunctionSignature(Dialect d, const wxString& nameUpper)
{
    // Shared signatures (upper-cased key); a few dialect-specific ones follow.
    struct Sig { const wchar_t* name; const wchar_t* sig; };
    static const Sig kSig[] = {
        { L"COUNT",        L"COUNT(expr | *)" },
        { L"SUM",          L"SUM(expr)" },
        { L"AVG",          L"AVG(expr)" },
        { L"MIN",          L"MIN(expr)" },
        { L"MAX",          L"MAX(expr)" },
        { L"COALESCE",     L"COALESCE(val1, val2, …)" },
        { L"NULLIF",       L"NULLIF(a, b)" },
        { L"CONCAT",       L"CONCAT(str1, str2, …)" },
        { L"CONCAT_WS",    L"CONCAT_WS(sep, str1, str2, …)" },
        { L"SUBSTRING",    L"SUBSTRING(str, pos, len)" },
        { L"SUBSTR",       L"SUBSTR(str, pos, len)" },
        { L"LENGTH",       L"LENGTH(str)" },
        { L"CHAR_LENGTH",  L"CHAR_LENGTH(str)" },
        { L"LOWER",        L"LOWER(str)" },
        { L"UPPER",        L"UPPER(str)" },
        { L"TRIM",         L"TRIM(str)" },
        { L"REPLACE",      L"REPLACE(str, from, to)" },
        { L"ROUND",        L"ROUND(number, decimals)" },
        { L"FLOOR",        L"FLOOR(number)" },
        { L"CEIL",         L"CEIL(number)" },
        { L"ABS",          L"ABS(number)" },
        { L"CAST",         L"CAST(expr AS type)" },
        { L"LEFT",         L"LEFT(str, len)" },
        { L"RIGHT",        L"RIGHT(str, len)" },
        { L"LPAD",         L"LPAD(str, len, pad)" },
        { L"RPAD",         L"RPAD(str, len, pad)" },
        { L"NOW",          L"NOW()" },
        { L"ROW_NUMBER",   L"ROW_NUMBER() OVER (…)" },
        { L"RANK",         L"RANK() OVER (…)" },
    };
    static const Sig kMy[] = {
        { L"IFNULL",       L"IFNULL(expr, alt)" },
        { L"IF",           L"IF(cond, then, else)" },
        { L"GROUP_CONCAT", L"GROUP_CONCAT(expr [SEPARATOR sep])" },
        { L"DATE_FORMAT",  L"DATE_FORMAT(date, format)" },
        { L"STR_TO_DATE",  L"STR_TO_DATE(str, format)" },
        { L"DATEDIFF",     L"DATEDIFF(date1, date2)" },
        { L"DATE_ADD",     L"DATE_ADD(date, INTERVAL n unit)" },
        { L"JSON_EXTRACT", L"JSON_EXTRACT(json, path)" },
    };
    static const Sig kPgSig[] = {
        { L"STRING_AGG",   L"string_agg(expr, delimiter)" },
        { L"ARRAY_AGG",    L"array_agg(expr)" },
        { L"TO_CHAR",      L"to_char(value, format)" },
        { L"TO_DATE",      L"to_date(str, format)" },
        { L"DATE_TRUNC",   L"date_trunc(field, source)" },
        { L"DATE_PART",    L"date_part(field, source)" },
        { L"GENERATE_SERIES", L"generate_series(start, stop [, step])" },
    };
    const wxString up = nameUpper.Upper();
    const Sig* extra = (d == Dialect::Postgres) ? kPgSig : kMy;
    const size_t nExtra = (d == Dialect::Postgres) ? (sizeof kPgSig / sizeof *kPgSig)
                                                   : (sizeof kMy / sizeof *kMy);
    for (size_t i = 0; i < nExtra; ++i)
        if (up == extra[i].name) return extra[i].sig;
    for (const Sig& s : kSig)
        if (up == s.name) return s.sig;
    return wxString();
}

} // namespace db
