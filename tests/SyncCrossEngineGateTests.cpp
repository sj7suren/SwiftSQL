// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCrossEngineGateTests.cpp -- table-driven regression test for
// ui::CrossEngineSupported (src/ui/SyncCrossEngineGate.{h,cpp}), the wizard-
// entry policy gate that decides which db::Dialect pairs SyncWizardDialog
// allows into the compare/sync flow at all (see SelectionValid() in
// SyncWizardDialog.cpp). Documented policy: same-dialect is always allowed;
// MySQL<->Postgres is the one cross-engine pairing exercised end-to-end and
// is allowed; every other pairing that touches Sqlite/SqlServer/Oracle while
// the two dialects differ must stay blocked until a later phase validates
// it (see SyncCrossEngineGate.h's comment for the full rationale).
//
// This function used to be an anonymous-namespace free function inside
// SyncWizardDialog.cpp -- a ~870-line wx GUI dialog TU (wx/wx.h, ConnEntry,
// SyncDiffPanel, SyncRunnerDialog, Theme, UiFonts, ...) that is not
// link-compatible with this test suite's lightweight "compile one pure TU"
// pattern (contrast SyncDiffModel.cpp, which is explicitly documented as
// "pure, non-wx" and testable exactly that way). It has been extracted
// verbatim (semantics unchanged) into its own pure, non-wx TU
// (SyncCrossEngineGate.h/.cpp, which depends only on the db::Dialect enum)
// specifically so this table-driven test can exercise the REAL function --
// see the QA handoff report for why this was judged an acceptable, minimal,
// clearly-scoped src/ exception rather than an invasive refactor.
//
// Same dependency-free harness as the other tests/ files: an
// ExpectTrue/ExpectEq-style assert loop, non-zero exit on any failure, no
// doctest/Catch2. Links only swiftsql::db (db::Dialect lives in
// db/DbDriver.h; SyncCrossEngineGate.cpp is compiled directly into this
// binary -- see tests/CMakeLists.txt).
#include "ui/SyncCrossEngineGate.h"

#include <cstdio>

using db::Dialect;

static int g_checks = 0;
static int g_fails  = 0;

static const char* DialectName(Dialect d)
{
    switch (d) {
    case Dialect::MySQL:     return "MySQL";
    case Dialect::Postgres:  return "Postgres";
    case Dialect::Sqlite:    return "Sqlite";
    case Dialect::SqlServer: return "SqlServer";
    case Dialect::Oracle:    return "Oracle";
    }
    return "?";
}

static void Expect(Dialect a, Dialect b, bool want)
{
    ++g_checks;
    const bool got = ui::CrossEngineSupported(a, b);
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL CrossEngineSupported(%s, %s): want=%d got=%d\n",
                     DialectName(a), DialectName(b), (int)want, (int)got);
    } else {
        std::printf("  ok   CrossEngineSupported(%s, %s) == %d\n",
                     DialectName(a), DialectName(b), (int)want);
    }
}

// Full 5x5 matrix over every db::Dialect pair, both directions (the function
// is not documented/asserted to be symmetric by inspection alone, so both
// orderings are exercised explicitly rather than assumed).
static void TestFullDialectMatrix()
{
    const Dialect all[] = { Dialect::MySQL, Dialect::Postgres, Dialect::Sqlite,
                             Dialect::SqlServer, Dialect::Oracle };

    for (Dialect a : all) {
        for (Dialect b : all) {
            bool want;
            if (a == b) {
                want = true;   // same-dialect always allowed
            } else {
                const bool aIsMyOrPg = (a == Dialect::MySQL || a == Dialect::Postgres);
                const bool bIsMyOrPg = (b == Dialect::MySQL || b == Dialect::Postgres);
                want = aIsMyOrPg && bIsMyOrPg;   // only the MySQL<->Postgres pairing
            }
            Expect(a, b, want);
        }
    }
}

// Named spot checks for the pairings the wizard's user-facing gate message
// explicitly promises (MVP is same-engine plus MySQL<->PostgreSQL) -- kept
// separate from the generated matrix above so a change to the matrix's own
// derivation logic can't silently drift from the documented contract.
static void TestDocumentedContractSpotChecks()
{
    Expect(Dialect::MySQL,     Dialect::MySQL,     true);
    Expect(Dialect::Postgres,  Dialect::Postgres,  true);
    Expect(Dialect::Sqlite,    Dialect::Sqlite,    true);
    Expect(Dialect::SqlServer, Dialect::SqlServer, true);
    Expect(Dialect::Oracle,    Dialect::Oracle,    true);

    Expect(Dialect::MySQL,    Dialect::Postgres, true);
    Expect(Dialect::Postgres, Dialect::MySQL,    true);

    Expect(Dialect::MySQL,     Dialect::Sqlite,    false);
    Expect(Dialect::MySQL,     Dialect::SqlServer, false);
    Expect(Dialect::MySQL,     Dialect::Oracle,    false);
    Expect(Dialect::Postgres,  Dialect::Sqlite,    false);
    Expect(Dialect::Postgres,  Dialect::SqlServer, false);
    Expect(Dialect::Postgres,  Dialect::Oracle,    false);
    Expect(Dialect::Sqlite,    Dialect::SqlServer, false);
    Expect(Dialect::Sqlite,    Dialect::Oracle,    false);
    Expect(Dialect::SqlServer, Dialect::Oracle,    false);
}

int main()
{
    TestFullDialectMatrix();
    TestDocumentedContractSpotChecks();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
