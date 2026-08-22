// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MySqlRoutinePrivilegeTests.cpp — DEF-R3-02: an empty MySQL routine list is
// AMBIGUOUS, and MySqlRoutineRead's resolution of that ambiguity.
//
// ===========================================================================
// THE DEFECT THIS FILE OWNS
// ===========================================================================
// Found by OBSERVATION, not by reasoning — it survived three rounds of
// construction-time review because the real behaviour is not what anyone
// predicted. Measured on a real MySQL 8.0.36 with an account holding only
// `GRANT SELECT ON db.*` and no routine privileges:
//
//   information_schema.ROUTINES    -> SUCCEEDS, returns ZERO ROWS
//   information_schema.PARAMETERS  -> SUCCEEDS, returns ZERO ROWS
//
// No error. No NULL bodies. The rows are simply filtered out, because 8.0 backs
// information_schema with the data dictionary and applies privilege filtering
// per row. The documented 5.x behaviour (list the routines, blank the body) is
// gone, and both of the reader's existing defenses miss it: the blank-body
// heuristic has nothing to list, and the PARAMETERS degradation requires a
// FAILED query.
//
// So ReadRoutines used to answer Ok with an empty vector — the "genuinely empty
// catalog" answer — and a restricted source compared against a privileged target
// reported EVERY target routine as 仅目标端存在: confident, silent, mass false
// positives, against a feature whose stated release bar is zero.
//
// THE RULING, and therefore what this file pins. An empty list cannot be
// answered from the list, so the reader acquires a SECOND SIGNAL (SHOW GRANTS
// FOR CURRENT_USER()) and answers from that instead:
//
//   privileges HELD          -> Ok             (a real empty catalog)
//   privileges ABSENT        -> PermissionDenied, naming what is missing
//   privileges INDETERMINATE -> Unsupported    (degrade visibly; never guess Ok)
//
// The third line is the one with teeth. SHOW GRANTS output is free-form and
// version-dependent, and roles / ALL PRIVILEGES / wildcards complicate parsing
// it, so "we could not tell" is a real and reachable state. Resolving it towards
// Ok would reinstate exactly the silent false positives the fix exists to
// remove, so every doubtful input below must land on a NON-Ok status.
//
// Why Unsupported and not a hedged PermissionDenied: both are non-Ok, so the
// SAFETY is identical (the category degrades, zero pairs are emitted) and only
// the explanation differs. PermissionDenied is a claim ABOUT THE ACCOUNT
// (「不可见（权限不足）」) which in the indeterminate case we have specifically
// not established — asserting it would send a fully-privileged user chasing a
// grant they already hold, the same species of confident-wrong-answer merely
// pointed the other way. Unsupported means "something stopped us reading; the
// list is not available", which is exactly and only what is known.
//
// Live counterpart: tests/mysql_pg_live_routines.cpp hazards 7d/7e/7f measure
// this against the real server. This file pins the LOGIC so it is asserted in
// CI, where no MySQL is reachable.
//
// Pure logic — links swiftsql::db, no connection, no GUI.
#include "db/MySqlRoutineRead.h"
#include "db/RoutineDiff.h"
#include "db/DbDriver.h"

#include <cstdio>
#include <vector>
#include <wx/string.h>

using namespace db;
using namespace db::sync;

namespace {

int g_checks = 0;
int g_fails  = 0;

void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

const char* StatusName(RoutineReadStatus s)
{
    switch (s) {
    case RoutineReadStatus::Ok:               return "Ok";
    case RoutineReadStatus::PermissionDenied: return "PermissionDenied";
    case RoutineReadStatus::Unsupported:      return "Unsupported";
    }
    return "?";
}

// A MySQL 8 server that hides routines it will not let you see: ROUTINES and
// PARAMETERS both succeed and both return nothing, which is precisely the
// measured behaviour. `grants_` is what SHOW GRANTS FOR CURRENT_USER() replies.
class GrantStub : public IConnection {
public:
    bool                  grantsOk_ = true;
    std::vector<wxString> grants_;
    int                   grantQueries_ = 0;

    // Non-empty only when a test wants the populated path.
    std::vector<std::vector<wxString>> routines_;

    bool Execute(const wxString& sql, QueryResult& out, wxString& err) override
    {
        out = QueryResult{};
        if (sql.Contains(L"SHOW GRANTS")) {
            ++grantQueries_;
            if (!grantsOk_) { err = L"SELECT command denied for SHOW GRANTS"; return false; }
            for (const auto& g : grants_) out.rows.push_back({ g });
            return true;
        }
        if (sql.Contains(L"information_schema.ROUTINES")) {
            for (const auto& r : routines_)
                out.rows.push_back({ r[0], r[1], r[2], r[3], L"DEFINER", L"SQL", L"" });
            return true;   // succeeds with zero rows — the whole point
        }
        if (sql.Contains(L"information_schema.PARAMETERS"))
            return true;   // ditto
        err = L"unexpected query";
        return false;
    }

    Dialect GetDialect() const override { return Dialect::MySQL; }
    bool Connect(const core::ConnectionProfile&, wxString&) override { return true; }
    void Disconnect() override {}
    bool IsConnected() const override { return true; }
    bool ListDatabases(std::vector<wxString>&, wxString&) override { return true; }
    bool ListTables(const wxString&, std::vector<TableInfo>&, wxString&) override { return true; }
    bool GetColumns(const wxString&, const wxString&, std::vector<ColumnInfo>&,
                    wxString&) override { return true; }
    bool GetForeignKeys(const wxString&, const wxString&, std::vector<ForeignKey>&,
                        wxString&) override { return true; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<IndexInfo>&,
                    wxString&) override { return true; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&, wxString&)
        override { return true; }
    wxString ServerVersion() const override { return L"8.0.36-stub"; }
};

// Read an empty catalog under one set of grants and report what came back.
RoutineReadStatus ReadUnder(std::vector<wxString> grants, wxString& detail,
                            bool grantsOk = true)
{
    GrantStub c;
    c.grants_   = std::move(grants);
    c.grantsOk_ = grantsOk;
    std::vector<RoutineDef> defs;
    const RoutineReadStatus s =
        db::mysqlroutine::ReadRoutines(c, L"app", defs, detail);
    // Invariant across every case below: the reader never hands back routines it
    // could not read, whatever status it chose.
    ExpectTrue("  ^ no routines are returned for an empty catalog", defs.empty());
    return s;
}

void Expect(const char* name, RoutineReadStatus got, RoutineReadStatus want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s (got %s, want %s)\n", name,
                    StatusName(got), StatusName(want));
    } else {
        std::printf("  ok   %s -> %s\n", name, StatusName(got));
    }
}

// ---------------------------------------------------------------------------
// (a) privileges HELD -> Ok. THE REGRESSION RISK.
//
// The way to get this fix wrong is to make it too aggressive. A database that
// genuinely has no routines, read by an account that plainly could have seen
// them, must still report Ok — otherwise every empty schema on every server
// starts telling the user their credentials are broken, a false positive of the
// opposite sign.
// ---------------------------------------------------------------------------
void TestPrivilegesHeld()
{
    std::printf("\n-- (a) empty list + routine privileges HELD => Ok --\n");
    wxString d;

    Expect("global ALL PRIVILEGES",
           ReadUnder({ L"GRANT ALL PRIVILEGES ON *.* TO `dev`@`%` WITH GRANT OPTION" }, d),
           RoutineReadStatus::Ok);
    ExpectTrue("  ^ a clean read carries no detail text", d.IsEmpty());

    Expect("global EXECUTE among many privileges",
           ReadUnder({ L"GRANT SELECT, INSERT, EXECUTE, CREATE ROUTINE, "
                       L"ALTER ROUTINE ON *.* TO `dev`@`%`" }, d),
           RoutineReadStatus::Ok);

    Expect("schema-scoped EXECUTE alone",
           ReadUnder({ L"GRANT USAGE ON *.* TO `ro`@`%`",
                       L"GRANT SELECT, EXECUTE ON `app`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::Ok);

    Expect("schema-scoped CREATE ROUTINE alone",
           ReadUnder({ L"GRANT USAGE ON *.* TO `ro`@`%`",
                       L"GRANT CREATE ROUTINE ON `app`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::Ok);

    // MySQL database names are matched case-insensitively on the platforms this
    // ships to, and SHOW GRANTS echoes back whatever case the GRANT used.
    Expect("schema name matched case-insensitively",
           ReadUnder({ L"GRANT ALTER ROUTINE ON `APP`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::Ok);

    // A routine-level grant is a routine privilege by construction.
    Expect("routine-level GRANT EXECUTE ON PROCEDURE",
           ReadUnder({ L"GRANT EXECUTE ON PROCEDURE `app`.`p_one` TO `ro`@`%`" }, d),
           RoutineReadStatus::Ok);

    // Column lists must not be mistaken for privilege names, in either
    // direction: a column literally named `execute` is not the EXECUTE privilege.
    Expect("a column named `execute` is not the EXECUTE privilege",
           ReadUnder({ L"GRANT USAGE ON *.* TO `ro`@`%`",
                       L"GRANT SELECT (`id`, `execute`) ON `app`.`t1` TO `ro`@`%`" }, d),
           RoutineReadStatus::PermissionDenied);
}

// ---------------------------------------------------------------------------
// (b) privileges ABSENT -> PermissionDenied. THE DEFECT ITSELF.
//
// This is the measured 8.0.36 case. The empty list is empty BECAUSE of the
// privilege state, so the privilege state is what gets reported — and the detail
// has to name the missing privileges or the user cannot act on it.
//
// Note the accepted trade-off: an account with no routine privileges pointed at
// a genuinely empty database is ALSO told "permission". That is deliberate. It
// is recoverable (retry with a better account); the alternative is a silent
// screenful of wrong 仅目标端存在 rows, which is not.
// ---------------------------------------------------------------------------
void TestPrivilegesAbsent()
{
    std::printf("\n-- (b) empty list + routine privileges ABSENT => PermissionDenied --\n");
    wxString d;

    // The exact grants the live probe creates (hazard 7d).
    Expect("the measured case: GRANT SELECT ON db.* and nothing else",
           ReadUnder({ L"GRANT USAGE ON *.* TO `swiftsql_xs_ro`@`%`",
                       L"GRANT SELECT ON `app`.* TO `swiftsql_xs_ro`@`%`" }, d),
           RoutineReadStatus::PermissionDenied);
    ExpectTrue("  ^ the detail names the missing privileges",
               d.Contains(L"EXECUTE") && d.Contains(L"CREATE ROUTINE") &&
               d.Contains(L"ALTER ROUTINE"));
    ExpectTrue("  ^ ...and names the database it is talking about",
               d.Contains(L"app"));

    Expect("USAGE alone (a bare account) is not a routine privilege",
           ReadUnder({ L"GRANT USAGE ON *.* TO `ro`@`%`" }, d),
           RoutineReadStatus::PermissionDenied);

    Expect("routine privileges on ANOTHER schema do not count",
           ReadUnder({ L"GRANT USAGE ON *.* TO `ro`@`%`",
                       L"GRANT ALL PRIVILEGES ON `other`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::PermissionDenied);

    // A grant scoped to one TABLE cannot confer a routine privilege however
    // sweeping its privilege list looks.
    Expect("a table-scoped ALL PRIVILEGES is not a routine privilege",
           ReadUnder({ L"GRANT ALL PRIVILEGES ON `app`.`t1` TO `ro`@`%`" }, d),
           RoutineReadStatus::PermissionDenied);

    // SELECT/INSERT/UPDATE/DELETE on the schema is the classic read-only
    // reporting account, and is the shape that produced the false positives.
    Expect("full DML on the schema is still not a routine privilege",
           ReadUnder({ L"GRANT SELECT, INSERT, UPDATE, DELETE, SHOW VIEW "
                       L"ON `app`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::PermissionDenied);
}

// ---------------------------------------------------------------------------
// (c) privileges INDETERMINATE -> Unsupported. NEVER Ok.
//
// Every input here is one the parser cannot resolve with confidence. The single
// assertion that matters is the same for all of them: the status is not Ok. Ok
// is the silent mass-false-positive state and is never the fallback for "we
// could not tell".
// ---------------------------------------------------------------------------
void TestIndeterminate()
{
    std::printf("\n-- (c) empty list + privileges INDETERMINATE => Unsupported --\n");
    wxString d;

    // The probe itself failed. Must NOT be a hard error, and must NOT be Ok.
    Expect("SHOW GRANTS failing is indeterminate, not an error and not Ok",
           ReadUnder({}, d, /*grantsOk=*/false),
           RoutineReadStatus::Unsupported);
    ExpectTrue("  ^ and says why it could not tell", !d.IsEmpty());

    Expect("SHOW GRANTS returning nothing at all",
           ReadUnder({}, d),
           RoutineReadStatus::Unsupported);

    // A role grant has no ON clause, and MySQL 8 does not expand a role's
    // privileges in SHOW GRANTS FOR CURRENT_USER() (that needs USING). The
    // account may well hold routine privileges through the role that we simply
    // cannot see, so this must never resolve to Absent.
    Expect("an unexpanded ROLE grant",
           ReadUnder({ L"GRANT USAGE ON *.* TO `ro`@`%`",
                       L"GRANT `app_reader`@`%` TO `ro`@`%`" }, d),
           RoutineReadStatus::Unsupported);

    // A LIKE wildcard in the schema pattern MIGHT match `app`. Resolving that
    // means reimplementing the server's escaping rules; we decline and degrade.
    Expect("a wildcard schema pattern that might have matched",
           ReadUnder({ L"GRANT EXECUTE ON `ap%`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::Unsupported);

    Expect("an underscore wildcard in the schema pattern",
           ReadUnder({ L"GRANT EXECUTE ON `ap_`.* TO `ro`@`%`" }, d),
           RoutineReadStatus::Unsupported);

    // Output we do not recognize at all — a future version, a proxy grant, a
    // localized server. Unparsed is not "no privileges".
    Expect("a line that is not a GRANT at all",
           ReadUnder({ L"something entirely unexpected" }, d),
           RoutineReadStatus::Unsupported);

    Expect("a GRANT with no TO clause",
           ReadUnder({ L"GRANT SELECT ON `app`.*" }, d),
           RoutineReadStatus::Unsupported);

    // THE CORE CLAIM OF THIS SECTION, restated so it cannot be lost in the noise
    // above: not one indeterminate input may produce Ok.
    const std::vector<std::vector<wxString>> doubtful = {
        {},
        { L"GRANT `r`@`%` TO `u`@`%`" },
        { L"GRANT EXECUTE ON `a%`.* TO `u`@`%`" },
        { L"GRANT PROXY ON ''@'' TO `u`@`%` WITH GRANT OPTION" },
        { L"gibberish" },
    };
    bool anyOk = false;
    for (const auto& g : doubtful) {
        wxString dd;
        if (ReadUnder(g, dd) == RoutineReadStatus::Ok) anyOk = true;
    }
    ExpectTrue("NO indeterminate grant output ever resolves to Ok", !anyOk);
}

// ---------------------------------------------------------------------------
// The non-empty path is untouched by all of the above, and must not pay for it.
// ---------------------------------------------------------------------------
void TestNonEmptyPathUnchanged()
{
    std::printf("\n-- the populated path is unchanged and unprobed --\n");

    GrantStub c;
    c.grants_   = { L"GRANT USAGE ON *.* TO `ro`@`%`" };   // no routine privilege
    c.routines_ = { { L"f_one", L"FUNCTION", L"int", L"BEGIN RETURN 1; END" } };

    std::vector<RoutineDef> defs;
    wxString detail;
    const RoutineReadStatus s =
        db::mysqlroutine::ReadRoutines(c, L"app", defs, detail);

    // A listed routine with a readable body is a real answer regardless of what
    // SHOW GRANTS would have said, so the verdict is Ok...
    Expect("a listed, readable routine reports Ok even for a bare account",
           s, RoutineReadStatus::Ok);
    ExpectTrue("  ^ and the routine comes back", defs.size() == 1);
    // ...and the probe is never issued. It runs ONLY on the empty path, so the
    // common case pays nothing for this fix.
    ExpectTrue("  ^ and SHOW GRANTS was never issued", c.grantQueries_ == 0);
}

// ---------------------------------------------------------------------------
// The MySQL 5.x-shaped defense still stands. Both server generations have to be
// handled, and the empty-list fix must not have cannibalized the blank-body one:
// 5.x lists the routines and NULLs the body, which is a DIFFERENT observation
// with a different (and still correct) answer.
//
// Not hypothetical on 8.0 either — measured in hazard 7e, an 8.0.36 account
// holding EXECUTE but not SHOW_ROUTINE gets exactly this: all three rows listed,
// every body blank.
// ---------------------------------------------------------------------------
void TestBlankBodyDefenseIntact()
{
    std::printf("\n-- the 5.x blank-body defense is untouched --\n");

    GrantStub c;
    c.grants_   = { L"GRANT ALL PRIVILEGES ON *.* TO `dev`@`%`" };
    c.routines_ = {
        { L"f_one",   L"FUNCTION",  L"int", L"" },   // body withheld
        { L"p_two",   L"PROCEDURE", L"",    L"" },
    };

    std::vector<RoutineDef> defs;
    wxString detail;
    const RoutineReadStatus s =
        db::mysqlroutine::ReadRoutines(c, L"app", defs, detail);

    Expect("every listed routine has a hidden body => PermissionDenied",
           s, RoutineReadStatus::PermissionDenied);
    ExpectTrue("  ^ with an explanation", !detail.IsEmpty());

    // A PARTIAL loss stays Ok — the readable ones are genuinely comparable, and
    // the withheld one degrades on its own via bodyReadable.
    GrantStub c2;
    c2.grants_   = { L"GRANT ALL PRIVILEGES ON *.* TO `dev`@`%`" };
    c2.routines_ = {
        { L"f_one", L"FUNCTION", L"int", L"" },
        { L"f_two", L"FUNCTION", L"int", L"BEGIN RETURN 2; END" },
    };
    std::vector<RoutineDef> d2;
    wxString det2;
    Expect("a PARTIAL body loss stays Ok",
           db::mysqlroutine::ReadRoutines(c2, L"app", d2, det2),
           RoutineReadStatus::Ok);
    ExpectTrue("  ^ the withheld body is marked unreadable, not empty",
               d2.size() == 2 && !d2[0].bodyReadable && d2[1].bodyReadable);
}

} // namespace

int main()
{
    std::printf("=== MySqlRoutinePrivilegeTests (DEF-R3-02) ===\n");
    TestPrivilegesHeld();
    TestPrivilegesAbsent();
    TestIndeterminate();
    TestNonEmptyPathUnchanged();
    TestBlankBodyDefenseIntact();
    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
