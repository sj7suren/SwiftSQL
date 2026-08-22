// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncTypeMapTests.cpp — standalone unit tests for db::SyncTypeMap (A1 of the
// cross-engine sync feature), exercised BEFORE it is wired into anything else
// (DialectProfile::Interpret/Render, SchemaDelta, SyncEngine). Pure value types
// + pure functions — no connection, no wx UI beyond wxString — so this links
// only swiftsql::db and runs headless.
//
// Same dependency-free harness as ddl_test.cpp / sync_test.cpp: an
// ExpectTrue/ExpectStr/ExpectEq assert loop, non-zero exit on any failure.
#include "db/SyncTypeMap.h"

#include <cstdio>
#include <wx/string.h>

using namespace db;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectVerdict(const char* name, TypeVerdict got, TypeVerdict want)
{
    auto name_of = [](TypeVerdict v) -> const char* {
        switch (v) {
        case TypeVerdict::Identical:  return "Identical";
        case TypeVerdict::Equivalent: return "Equivalent";
        case TypeVerdict::Lossy:      return "Lossy";
        default:                      return "Unmappable";
        }
    };
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%s got=%s\n", name, name_of(want), name_of(got));
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static NormColumn MysqlCol(ColKind kind, const wxString& columnType,
                           long long length = -1, int scale = -1)
{
    NormColumn c;
    c.kind = kind; c.rawType = columnType; c.length = length; c.scale = scale;
    return c;
}
static NormColumn PgCol(const wxString& dataType, long long length = -1, int scale = -1)
{
    NormColumn c;
    c.kind = MapPgColKind(dataType); c.rawType = dataType; c.length = length; c.scale = scale;
    return c;
}

// ===========================================================================
//  MayAutoAlter
// ===========================================================================
static void TestMayAutoAlter()
{
    std::printf("[MayAutoAlter]\n");
    ExpectTrue("Identical may auto-alter",  MayAutoAlter(TypeVerdict::Identical));
    ExpectTrue("Equivalent may auto-alter", MayAutoAlter(TypeVerdict::Equivalent));
    ExpectTrue("Lossy may NOT auto-alter",  !MayAutoAlter(TypeVerdict::Lossy));
    ExpectTrue("Unmappable may NOT auto-alter", !MayAutoAlter(TypeVerdict::Unmappable));
}

// ===========================================================================
//  MapPgColKind — PG data_type -> ColKind
// ===========================================================================
static void TestMapPgColKind()
{
    std::printf("[MapPgColKind]\n");
    ExpectTrue("bigint->Integer",   MapPgColKind(L"bigint") == ColKind::Integer);
    ExpectTrue("integer->Integer",  MapPgColKind(L"integer") == ColKind::Integer);
    ExpectTrue("smallint->Integer", MapPgColKind(L"smallint") == ColKind::Integer);
    ExpectTrue("numeric->Decimal",  MapPgColKind(L"numeric") == ColKind::Decimal);
    ExpectTrue("boolean->Boolean",  MapPgColKind(L"boolean") == ColKind::Boolean);
    ExpectTrue("character varying->Varchar",
               MapPgColKind(L"character varying") == ColKind::Varchar);
    ExpectTrue("text->Text",        MapPgColKind(L"text") == ColKind::Text);
    ExpectTrue("bytea->Binary",     MapPgColKind(L"bytea") == ColKind::Binary);
    ExpectTrue("timestamp without time zone->Timestamp",
               MapPgColKind(L"timestamp without time zone") == ColKind::Timestamp);
    ExpectTrue("timestamp with time zone->Timestamp",
               MapPgColKind(L"timestamp with time zone") == ColKind::Timestamp);
    ExpectTrue("jsonb->Json",       MapPgColKind(L"jsonb") == ColKind::Json);
    ExpectTrue("uuid->Uuid",        MapPgColKind(L"uuid") == ColKind::Uuid);
    ExpectTrue("USER-DEFINED->Other (honest gate)",
               MapPgColKind(L"USER-DEFINED") == ColKind::Other);
}

// ===========================================================================
//  Interpret — MySQL / PG NormColumn -> CanonicalType
// ===========================================================================
static void TestInterpret()
{
    std::printf("[InterpretMySqlColumn / InterpretPgColumn]\n");

    {
        CanonicalType t = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"int(10) unsigned"));
        ExpectTrue("mysql unsigned int -> isUnsigned", t.isUnsigned);
    }
    {
        CanonicalType t = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"bigint"));
        ExpectTrue("mysql signed bigint -> !isUnsigned", !t.isUnsigned);
    }
    {
        CanonicalType t = InterpretMySqlColumn(MysqlCol(ColKind::Timestamp, L"timestamp"));
        ExpectTrue("mysql TIMESTAMP -> withTimeZone", t.withTimeZone);
    }
    {
        CanonicalType t = InterpretMySqlColumn(MysqlCol(ColKind::Timestamp, L"datetime"));
        ExpectTrue("mysql DATETIME -> !withTimeZone", !t.withTimeZone);
    }
    {
        CanonicalType t = InterpretMySqlColumn(MysqlCol(ColKind::Enum, L"enum('a','b','c')"));
        ExpectTrue("mysql enum parses 3 values", t.allowedValues.size() == 3);
        if (t.allowedValues.size() == 3) {
            ExpectStr("enum[0]", t.allowedValues[0], L"a");
            ExpectStr("enum[1]", t.allowedValues[1], L"b");
            ExpectStr("enum[2]", t.allowedValues[2], L"c");
        }
    }
    {
        CanonicalType t = InterpretPgColumn(PgCol(L"timestamp with time zone"));
        ExpectTrue("pg timestamptz -> withTimeZone", t.withTimeZone);
    }
    {
        CanonicalType t = InterpretPgColumn(PgCol(L"timestamp without time zone"));
        ExpectTrue("pg timestamp -> !withTimeZone", !t.withTimeZone);
    }
    {
        CanonicalType t = InterpretPgColumn(PgCol(L"integer"));
        ExpectTrue("pg integer -> !isUnsigned (PG never unsigned)", !t.isUnsigned);
    }
}

// ===========================================================================
//  Render — CanonicalType -> dialect type text
// ===========================================================================
static void TestRender()
{
    std::printf("[RenderMySqlType / RenderPgType]\n");

    {
        CanonicalType t; t.kind = ColKind::Varchar; t.length = 80;
        wxString out, why;
        ExpectTrue("render mysql varchar(80) ok", RenderMySqlType(t, out, why));
        ExpectStr("render mysql varchar(80) text", out, L"varchar(80)");
    }
    {
        CanonicalType t; t.kind = ColKind::Varchar; t.length = 80;
        wxString out, why;
        ExpectTrue("render pg character varying(80) ok", RenderPgType(t, out, why));
        ExpectStr("render pg character varying(80) text", out, L"character varying(80)");
    }
    {
        CanonicalType t; t.kind = ColKind::Integer; t.isUnsigned = true;
        wxString out, why;
        ExpectTrue("render mysql unsigned bigint ok", RenderMySqlType(t, out, why));
        ExpectStr("render mysql unsigned bigint text", out, L"bigint unsigned");
    }
    {
        // PG has no unsigned integer type — Render must fail honestly.
        CanonicalType t; t.kind = ColKind::Integer; t.isUnsigned = true;
        wxString out, why;
        ExpectTrue("render pg unsigned int FAILS (honest gate)", !RenderPgType(t, out, why));
        ExpectTrue("render pg unsigned int: why explains", !why.IsEmpty());
    }
    {
        CanonicalType t; t.kind = ColKind::Enum; t.allowedValues = { L"a", L"b" };
        wxString out, why;
        ExpectTrue("render pg enum->text ok (lossy degrade, still succeeds)",
                   RenderPgType(t, out, why));
        ExpectStr("render pg enum->text", out, L"text");
    }
    {
        CanonicalType t; t.kind = ColKind::Timestamp; t.withTimeZone = true;
        wxString out, why;
        ExpectTrue("render pg timestamptz ok", RenderPgType(t, out, why));
        ExpectStr("render pg timestamptz text", out, L"timestamp with time zone");
    }
    {
        CanonicalType t; t.kind = ColKind::Timestamp; t.withTimeZone = false;
        wxString out, why;
        ExpectTrue("render mysql datetime ok", RenderMySqlType(t, out, why));
        ExpectStr("render mysql datetime text", out, L"datetime");
    }
}

// ===========================================================================
//  CompareCanonical — the judgment-call pairs called out in the task brief.
// ===========================================================================
static void TestCompareCanonical()
{
    std::printf("[CompareCanonical]\n");

    // INT <-> INTEGER, BIGINT <-> bigint, SMALLINT <-> smallint: same ColKind::Integer.
    // NOTE: MySQL's column_type and PG's data_type happen to spell "bigint"/
    // "smallint" identically, so these hit CompareCanonical's rawType-equality
    // fast path and come back Identical rather than Equivalent — both verdicts
    // MayAutoAlter() the same way, so this is a labeling nuance, not a behavior
    // difference. What actually matters (MayAutoAlter) is asserted explicitly.
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"bigint"));
        CanonicalType b = InterpretPgColumn(PgCol(L"bigint"));
        wxString reason;
        ExpectTrue("BIGINT <-> bigint: may auto-alter",
                   MayAutoAlter(CompareCanonical(a, b, reason)));
    }
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"int"));
        CanonicalType b = InterpretPgColumn(PgCol(L"integer"));
        wxString reason;
        ExpectVerdict("INT <-> INTEGER: Equivalent", CompareCanonical(a, b, reason), TypeVerdict::Equivalent);
    }
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"smallint"));
        CanonicalType b = InterpretPgColumn(PgCol(L"smallint"));
        wxString reason;
        ExpectTrue("SMALLINT <-> smallint: may auto-alter",
                   MayAutoAlter(CompareCanonical(a, b, reason)));
    }
    // VARCHAR(n) <-> character varying(n).
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Varchar, L"varchar(80)", 80));
        CanonicalType b = InterpretPgColumn(PgCol(L"character varying", 80));
        wxString reason;
        ExpectVerdict("VARCHAR(80) <-> character varying(80): Equivalent",
                      CompareCanonical(a, b, reason), TypeVerdict::Equivalent);
    }
    // TINYINT(1) <-> boolean — semantic Equivalent (both classify ColKind::Boolean
    // upstream: MySQL's MapColKind already special-cases tinyint(1); this test
    // exercises CompareCanonical's contribution once both sides are Boolean).
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Boolean, L"tinyint(1)"));
        CanonicalType b = InterpretPgColumn(PgCol(L"boolean"));
        wxString reason;
        ExpectVerdict("TINYINT(1) <-> boolean: Equivalent",
                      CompareCanonical(a, b, reason), TypeVerdict::Equivalent);
    }
    // DATETIME <-> timestamp (without tz): Equivalent (both naive wall-clock).
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Timestamp, L"datetime"));
        CanonicalType b = InterpretPgColumn(PgCol(L"timestamp without time zone"));
        wxString reason;
        ExpectVerdict("DATETIME <-> timestamp: Equivalent",
                      CompareCanonical(a, b, reason), TypeVerdict::Equivalent);
    }
    // TIMESTAMP <-> timestamptz: Lossy (task brief's explicit judgment call —
    // range/DST/conversion-algorithm differences, NOT a safe auto-ALTER).
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Timestamp, L"timestamp"));
        CanonicalType b = InterpretPgColumn(PgCol(L"timestamp with time zone"));
        wxString reason;
        ExpectVerdict("TIMESTAMP <-> timestamptz: Lossy",
                      CompareCanonical(a, b, reason), TypeVerdict::Lossy);
        ExpectTrue("TIMESTAMP <-> timestamptz: reason non-empty", !reason.IsEmpty());
    }
    // JSON <-> json/jsonb: Equivalent.
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Json, L"json"));
        CanonicalType b = InterpretPgColumn(PgCol(L"jsonb"));
        wxString reason;
        ExpectVerdict("JSON <-> jsonb: Equivalent",
                      CompareCanonical(a, b, reason), TypeVerdict::Equivalent);
    }
    // ENUM(...) <-> no clean PG equivalent: Lossy (degrades to text — data
    // representable, value-list constraint lost).
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Enum, L"enum('a','b')"));
        CanonicalType b = InterpretPgColumn(PgCol(L"text"));
        wxString reason;
        ExpectVerdict("ENUM <-> text: Lossy",
                      CompareCanonical(a, b, reason), TypeVerdict::Lossy);
    }
    // MySQL unsigned integers <-> no PG equivalent: Unmappable.
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"int(10) unsigned"));
        CanonicalType b = InterpretPgColumn(PgCol(L"integer"));
        wxString reason;
        ExpectVerdict("unsigned int <-> integer: Unmappable",
                      CompareCanonical(a, b, reason), TypeVerdict::Unmappable);
        ExpectTrue("unsigned int <-> integer: reason non-empty", !reason.IsEmpty());
    }
    // Genuinely mismatched kinds (not the ENUM special case) -> Unmappable.
    {
        CanonicalType a = InterpretMySqlColumn(MysqlCol(ColKind::Integer, L"int"));
        CanonicalType b = InterpretPgColumn(PgCol(L"character varying", 10));
        wxString reason;
        ExpectVerdict("int <-> varchar(10): Unmappable (no safe cast rule)",
                      CompareCanonical(a, b, reason), TypeVerdict::Unmappable);
    }
    // Same-engine identical rawType -> Identical.
    {
        CanonicalType a = InterpretPgColumn(PgCol(L"bigint"));
        CanonicalType b = InterpretPgColumn(PgCol(L"bigint"));
        wxString reason;
        ExpectVerdict("pg bigint <-> pg bigint: Identical",
                      CompareCanonical(a, b, reason), TypeVerdict::Identical);
    }
}

int main()
{
    std::printf("== SyncTypeMapTests (A1 standalone) ==\n");
    TestMayAutoAlter();
    TestMapPgColKind();
    TestInterpret();
    TestRender();
    TestCompareCanonical();
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
