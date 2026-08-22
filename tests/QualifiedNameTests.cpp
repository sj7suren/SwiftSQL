// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// QualifiedNameTests.cpp — the identity type's contract.
//
// db::QualifiedName replaced a bare `wxString name` as the identity of a table
// throughout the sync suite. Three properties have to hold or something above it
// breaks, and each is pinned here:
//
//   1. ROUND TRIP. Key() is what ui::TableKey wraps and what
//      SchemaChangeSet::table carries; Parse() is how a renderer gets the schema
//      back. If those are not exact inverses, a table key produced in one layer
//      resolves to a different table in another -- the wrong-table bug in a new
//      costume. Includes the identifiers PostgreSQL allows but nobody expects.
//
//   2. TWO ENGINE MODELS, HONESTLY. A MySQL key must be BYTE-IDENTICAL to the
//      bare name it replaced (nothing persisted or compared against it changes),
//      while a PostgreSQL key must distinguish two same-named tables.
//
//   3. NO POSITIONS. The same guarantee ui::StableId makes -- an index can never
//      become an identity -- must not have a back door through this type.
#include "db/QualifiedName.h"

#include <cstdio>
#include <type_traits>

using db::QualifiedName;

static int g_checks = 0, g_fails = 0;

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n       want=[%s]\n        got=[%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

// ---------------------------------------------------------------------------
// 1. Positions are not identities (compile-time)
// ---------------------------------------------------------------------------
static void TestNotConstructibleFromIndex()
{
    std::printf("[TestNotConstructibleFromIndex]\n");

    // QualifiedName must not become the loophole that ui::StableId closed. A
    // wxString has no implicit conversion from an integer, so these are already
    // true -- asserting them makes it a BUILD failure if someone later adds a
    // convenience constructor that changes it.
    static_assert(!std::is_constructible_v<QualifiedName, int>,
                  "QualifiedName must never be constructible from an index");
    static_assert(!std::is_constructible_v<QualifiedName, unsigned>, "");
    static_assert(!std::is_constructible_v<QualifiedName, long long>, "");
    static_assert(!std::is_constructible_v<QualifiedName, std::size_t>, "");

    // The dangerous direction stays closed: no implicit decay back to a string,
    // so no caller can silently drop the schema half by assigning it away.
    static_assert(!std::is_convertible_v<QualifiedName, wxString>,
                  "QualifiedName must not implicitly decay to wxString");

    ExpectTrue("compile-time guarantees hold", true);
}

// ---------------------------------------------------------------------------
// 2. The two engine models
// ---------------------------------------------------------------------------
static void TestEngineModels()
{
    std::printf("[TestEngineModels]\n");

    // MySQL / SQLite: unqualified, and the key is exactly the old bare name.
    const QualifiedName my(L"orders");
    ExpectTrue("unqualified reports no schema", !my.HasSchema());
    ExpectStr("MySQL key == the bare name it replaced", my.Key(), L"orders");
    ExpectStr("MySQL display == the bare name", my.Display(), L"orders");
    ExpectStr("name half", my.Name(), L"orders");
    ExpectStr("schema half is empty, not invented", my.Schema(), wxString());

    // PostgreSQL: qualified, and `public` is spelled out rather than elided --
    // eliding it would make public.orders and an unqualified `orders` collide
    // again, which is the whole defect.
    const QualifiedName pub(L"public", L"orders");
    const QualifiedName arc(L"archive", L"orders");
    ExpectStr("PG public key", pub.Key(), L"public.orders");
    ExpectStr("PG archive key", arc.Key(), L"archive.orders");
    ExpectTrue("two same-named PG tables are NOT equal", pub != arc);
    ExpectTrue("their keys differ", pub.Key() != arc.Key());

    // The comparison that used to conflate them.
    ExpectTrue("same name half is not sufficient for identity",
               pub.Name() == arc.Name() && pub != arc);

    // A qualified name is distinct from the bare one, so an unqualified key can
    // never accidentally match a qualified table.
    ExpectTrue("qualified != unqualified", pub != QualifiedName(L"orders"));

    // Ordering is total and schema-major (what std::set/std::map rely on).
    ExpectTrue("ordering is schema-major", arc < pub);
}

// ---------------------------------------------------------------------------
// 3. Round trip
// ---------------------------------------------------------------------------
static void CheckRoundTrip(const char* label, const QualifiedName& n)
{
    const QualifiedName back = QualifiedName::Parse(n.Key());
    ++g_checks;
    if (back != n) {
        ++g_fails;
        std::printf("  FAIL round-trip %s\n       key=[%s]\n"
                    "       schema want=[%s] got=[%s]\n"
                    "       name   want=[%s] got=[%s]\n", label,
                    (const char*)n.Key().utf8_str(),
                    (const char*)n.Schema().utf8_str(),
                    (const char*)back.Schema().utf8_str(),
                    (const char*)n.Name().utf8_str(),
                    (const char*)back.Name().utf8_str());
    } else {
        std::printf("  ok   round-trip %s\n", label);
    }
}

static void TestRoundTrip()
{
    std::printf("[TestRoundTrip]\n");

    CheckRoundTrip("bare", QualifiedName(L"orders"));
    CheckRoundTrip("qualified", QualifiedName(L"public", L"orders"));
    CheckRoundTrip("other schema", QualifiedName(L"archive", L"orders"));
    CheckRoundTrip("empty", QualifiedName());

    // The cases that break a naive split-on-'.'. PostgreSQL genuinely allows all
    // of these via double-quoted identifiers, so they are not theoretical:
    //   CREATE TABLE "my.table" (...);
    CheckRoundTrip("dot in the table name", QualifiedName(L"my.table"));
    CheckRoundTrip("dot in both halves", QualifiedName(L"a.b", L"c.d"));
    CheckRoundTrip("backslash in the name", QualifiedName(L"we\\ird"));
    CheckRoundTrip("backslash and dot", QualifiedName(L"s\\.1", L"t.2"));
    CheckRoundTrip("trailing backslash", QualifiedName(L"sch\\", L"tbl"));
    CheckRoundTrip("unicode", QualifiedName(L"模式", L"订单"));

    // The ambiguity the escaping exists to remove, stated as an assertion:
    // a table literally named "a.b" must NOT parse back as schema `a`, table `b`.
    const QualifiedName dotted(L"a.b");                 // one bare name
    const QualifiedName split(L"a", L"b");              // schema a, table b
    ExpectTrue("dotted bare name and a.b pair have DIFFERENT keys",
               dotted.Key() != split.Key());
    ExpectTrue("dotted bare name round-trips as unqualified",
               !QualifiedName::Parse(dotted.Key()).HasSchema());
    ExpectTrue("the a.b pair round-trips as qualified",
               QualifiedName::Parse(split.Key()).HasSchema());
    ExpectStr("a.b pair parses back to schema a",
              QualifiedName::Parse(split.Key()).Schema(), L"a");
}

// ---------------------------------------------------------------------------
// 4. Key() is stable across constructions -- it is a map key
// ---------------------------------------------------------------------------
static void TestKeyStability()
{
    std::printf("[TestKeyStability]\n");

    ExpectTrue("equal names produce equal keys",
               QualifiedName(L"s", L"t").Key() == QualifiedName(L"s", L"t").Key());
    ExpectTrue("wchar_t* and wxString constructions agree",
               QualifiedName(L"orders").Key() == QualifiedName(wxString(L"orders")).Key());
    ExpectTrue("IsEmpty tracks the NAME half",
               QualifiedName().IsEmpty() && !QualifiedName(L"t").IsEmpty());
    // A schema with no table is still "empty" -- there is no table to identify.
    ExpectTrue("schema alone is not an identity",
               QualifiedName(L"public", wxString()).IsEmpty());
}

int main()
{
    std::printf("== QualifiedNameTests ==\n");
    TestNotConstructibleFromIndex();
    TestEngineModels();
    TestRoundTrip();
    TestKeyStability();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
