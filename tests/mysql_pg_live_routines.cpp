// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// mysql_pg_live_routines.cpp — hazard 7 of the live cross-engine test: the
// compare-only functions & stored procedures diff (ADR-013), exercised against
// real MySQL and PostgreSQL catalogs. Driven from mysql_pg_live_test.cpp; split
// into its own TU purely to keep both files under the charter's 1000-line
// ceiling.
//
// Three claims are checked, and one QUESTION is answered:
//
//   * a routine present only on the source reports SourceOnly;
//   * SAME-ENGINE (MySQL -> MySQL) bodies differing only in whitespace and
//     comments must report Identical — the normalizer's zero-false-positive
//     bar, which is the release gate for this whole feature;
//   * CROSS-ENGINE (MySQL -> PostgreSQL) body pairs must ALWAYS report
//     NotComparable — never "same", never "different". PL/pgSQL and MySQL's
//     routine dialect are different languages; either claim would be a lie.
//
// And the question, which is the highest-priority open item in the project:
// when a MySQL account can see tables and list routines but holds NO routine
// privileges, do information_schema.ROUTINES.ROUTINE_DEFINITION and
// information_schema.PARAMETERS *error*, or do they *return NULL/empty*?
// MySqlRoutineRead.cpp's PARAMETERS fix assumes the former (it degrades the
// whole side only when the query FAILS). If the server instead returns rows
// silently — MySQL's documented habit elsewhere — that fix never fires and the
// mass false positives it was written to prevent are still there, showing up as
// "every routine has no arguments". This file resolves it by observation and
// prints the raw catalog response either way; see the OBS/ANSWER lines.
#include "mysql_pg_live.h"

#include "db/MySqlRoutineRead.h"
#include "db/PgRoutineRead.h"
#include "db/RoutineCompare.h"

#include <memory>

using namespace db;
using namespace db::sync;

namespace mplive {

namespace {

const char* VerdictName(RoutineVerdict v)
{
    switch (v) {
    case RoutineVerdict::Identical:        return "Identical";
    case RoutineVerdict::SourceOnly:       return "SourceOnly";
    case RoutineVerdict::TargetOnly:       return "TargetOnly";
    case RoutineVerdict::SignatureDiffers: return "SignatureDiffers";
    case RoutineVerdict::BodyDiffers:      return "BodyDiffers";
    case RoutineVerdict::NotComparable:    return "NotComparable";
    }
    return "?";
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

void ShowPairs(const RoutineDiffSet& set, const char* tag)
{
    std::printf("  OBS  %s: sourceStatus=%s targetStatus=%s sameEngine=%d pairs=%d\n",
                tag, StatusName(set.sourceStatus), StatusName(set.targetStatus),
                (int)set.sameEngine, (int)set.pairs.size());
    for (const auto& p : set.pairs)
        std::printf("  OBS    %s %-16s key=%s reason=%s\n",
                    tag, VerdictName(p.verdict),
                    (const char*)p.key.utf8_str(),
                    (const char*)p.reason.utf8_str());
}

// ---------------------------------------------------------------------------
// Routine fixtures. Bodies are deliberately trivial — what is under test is the
// normalizer and the verdict layer, not SQL semantics.
// ---------------------------------------------------------------------------

// MySQL side. `fn_shared` also exists on the PG target and in the same-engine
// second database; `fn_srconly` exists nowhere else.
bool CreateMySqlRoutines(IConnection& c, bool withComments)
{
    bool ok = true;
    // Deliberately NOT wrapped in DELIMITER: single-statement bodies, so the
    // driver's Execute handles them as-is.
    if (withComments) {
        // Same logic as the plain version, but with extra whitespace, a `--`
        // comment and a `/* */` comment. The normalizer must fold all of it away.
        ok &= Exec(c,
            L"CREATE FUNCTION fn_shared(a INT, b VARCHAR(50)) RETURNS INT "
            L"DETERMINISTIC RETURN   a   +   /* inline note */ LENGTH(b)  -- trailing note\n",
            "mysql fn_shared (comment variant)");
    } else {
        ok &= Exec(c,
            L"CREATE FUNCTION fn_shared(a INT, b VARCHAR(50)) RETURNS INT "
            L"DETERMINISTIC RETURN a + LENGTH(b)",
            "mysql fn_shared");
    }
    ok &= Exec(c,
        L"CREATE FUNCTION fn_srconly(x INT) RETURNS INT "
        L"DETERMINISTIC RETURN x * 2", "mysql fn_srconly");
    ok &= Exec(c,
        L"CREATE PROCEDURE sp_touch(IN n INT) SET @swiftsql_xs_probe = n",
        "mysql sp_touch");
    return ok;
}

bool CreatePgRoutines(IConnection& c)
{
    bool ok = true;
    ok &= Exec(c,
        L"CREATE FUNCTION fn_shared(a integer, b varchar) RETURNS integer "
        L"LANGUAGE sql AS $$ SELECT a + length(b) $$", "pg fn_shared");
    ok &= Exec(c,
        L"CREATE FUNCTION fn_tgtonly(x integer) RETURNS integer "
        L"LANGUAGE sql AS $$ SELECT x * 3 $$", "pg fn_tgtonly");
    return ok;
}

// ---------------------------------------------------------------------------
// 7a/7b — same-engine MySQL -> MySQL: whitespace/comment-only body differences
// must report Identical.
// ---------------------------------------------------------------------------
void SameEngineCompare(IConnection& my, const wxString& db1,
                       const wxString& host, int port,
                       const wxString& user, const wxString& pass,
                       const wxString& db2)
{
    std::printf("\n-- hazard 7a: same-engine (MySQL -> MySQL) body normalization --\n");

    auto my2 = CreateConnection(DbType::MySQL);
    wxString e;
    if (!my2->Connect(Profile(DbType::MySQL, host, port, user, pass, db2), e)) {
        std::printf("  ERR  connect second MySQL db: %s\n", (const char*)e.utf8_str());
        ExpectTrue("hazard7a second MySQL database reachable", false);
        return;
    }
    if (!CreateMySqlRoutines(*my2, /*withComments=*/true)) {
        ExpectTrue("hazard7a comment-variant routines created", false);
        return;
    }

    std::vector<RoutineDef> a, b;
    wxString da, db_;
    const RoutineReadStatus sa = mysqlroutine::ReadRoutines(my, db1, a, da);
    const RoutineReadStatus sb = mysqlroutine::ReadRoutines(*my2, db2, b, db_);
    std::printf("  OBS  read src status=%s (%d routines), tgt status=%s (%d routines)\n",
                StatusName(sa), (int)a.size(), StatusName(sb), (int)b.size());
    for (const auto& d : a)
        std::printf("  OBS    src %s bodyReadable=%d body=[%s]\n",
                    (const char*)d.Signature().utf8_str(), (int)d.bodyReadable,
                    (const char*)d.body.utf8_str());
    for (const auto& d : b)
        std::printf("  OBS    tgt %s bodyReadable=%d body=[%s]\n",
                    (const char*)d.Signature().utf8_str(), (int)d.bodyReadable,
                    (const char*)d.body.utf8_str());

    ExpectTrue("hazard7a source routine catalog readable", sa == RoutineReadStatus::Ok);
    ExpectTrue("hazard7a target routine catalog readable", sb == RoutineReadStatus::Ok);

    const RoutineDiffSet set = CompareRoutines(a, b, sa, sb,
        MakeRoutineCompareOptions(Dialect::MySQL, Dialect::MySQL));
    ShowPairs(set, "same-engine");

    // The release gate: the two fn_shared bodies differ ONLY in whitespace and
    // comments, so anything other than Identical is a false positive.
    bool found = false, identical = false;
    for (const auto& p : set.pairs)
        if (p.hasSource && p.hasTarget && p.Either().name.Lower() == L"fn_shared") {
            found = true;
            identical = (p.verdict == RoutineVerdict::Identical);
        }
    ExpectTrue("hazard7a fn_shared matched on both sides", found);
    ExpectTrue("hazard7a whitespace/comment-only difference reports Identical", identical);

    my2->Disconnect();
}

// ---------------------------------------------------------------------------
// 7c — cross-engine MySQL -> PostgreSQL: every body pair NotComparable.
// ---------------------------------------------------------------------------
void CrossEngineCompare(IConnection& my, IConnection& pg,
                        const wxString& myDb, const wxString& pgDb)
{
    std::printf("\n-- hazard 7c: cross-engine (MySQL -> PostgreSQL) --\n");

    std::vector<RoutineDef> a, b;
    wxString da, db_;
    const RoutineReadStatus sa = mysqlroutine::ReadRoutines(my, myDb, a, da);
    const RoutineReadStatus sb = pgroutine::ReadRoutines(pg, L"public", b, db_);
    std::printf("  OBS  mysql status=%s (%d routines) detail=[%s]\n",
                StatusName(sa), (int)a.size(), (const char*)da.utf8_str());
    std::printf("  OBS  pg    status=%s (%d routines) detail=[%s]\n",
                StatusName(sb), (int)b.size(), (const char*)db_.utf8_str());
    for (const auto& d : b)
        std::printf("  OBS    pg %s lang=%s bodyReadable=%d body=[%s]\n",
                    (const char*)d.Signature().utf8_str(),
                    (const char*)d.language.utf8_str(), (int)d.bodyReadable,
                    (const char*)d.body.utf8_str());

    ExpectTrue("hazard7c MySQL catalog readable", sa == RoutineReadStatus::Ok);
    ExpectTrue("hazard7c PostgreSQL catalog readable", sb == RoutineReadStatus::Ok);

    const RoutineDiffSet set = CompareRoutines(a, b, sa, sb,
        MakeRoutineCompareOptions(Dialect::MySQL, Dialect::Postgres));
    ShowPairs(set, "cross-engine");

    ExpectTrue("hazard7c set records sameEngine=false", !set.sameEngine);

    // No BODY verdict may ever be issued cross-engine, in either direction.
    int bodyVerdicts = 0, matchedPairs = 0, notComparable = 0, sourceOnly = 0;
    for (const auto& p : set.pairs) {
        if (p.verdict == RoutineVerdict::Identical ||
            p.verdict == RoutineVerdict::BodyDiffers) ++bodyVerdicts;
        if (p.hasSource && p.hasTarget) {
            ++matchedPairs;
            if (p.verdict == RoutineVerdict::NotComparable) ++notComparable;
        }
        if (p.verdict == RoutineVerdict::SourceOnly) ++sourceOnly;
    }
    ExpectEq("hazard7c no Identical/BodyDiffers verdict cross-engine", bodyVerdicts, 0);
    ExpectTrue("hazard7c at least one routine matched on both sides", matchedPairs > 0);
    ExpectEq("hazard7c every matched pair is NotComparable", notComparable, matchedPairs);
    ExpectTrue("hazard7c a source-only routine reports SourceOnly", sourceOnly > 0);
    for (const auto& p : set.pairs)
        if (p.hasSource && p.hasTarget)
            ExpectTrue("hazard7c NotComparable carries an explanation",
                       !p.reason.IsEmpty());
}

// ---------------------------------------------------------------------------
// 7d — THE QUESTION. A MySQL account that can SELECT tables but holds no
// routine privileges. Does the catalog error, or silently return nothing?
//
// Three outcomes are possible and they are NOT equivalent:
//   (A) the query ERRORS               -> MySqlRoutineRead's fix fires as designed
//   (B) rows come back, bodies NULL    -> the bodyReadable path fires (also fine)
//   (C) NO ROWS come back at all       -> the reader reports Ok + empty catalog,
//                                         i.e. "this database has no routines",
//                                         and a compare against a full side then
//                                         reports every routine as TargetOnly.
//                                         That is the silent-false-positive case
//                                         nobody has designed for.
// ---------------------------------------------------------------------------
void RestrictedAccountProbe(IConnection& myMaint, const wxString& myDb,
                            const wxString& host, int port)
{
    std::printf("\n-- hazard 7d: MySQL restricted account (the open question) --\n");

    // Throwaway account, created and dropped by this test, reachable only from
    // wherever this test runs. The password is a fixed test-local literal on
    // purpose: it grants SELECT on one disposable database that is itself
    // dropped seconds later, and is not a credential to anything real. It can be
    // overridden via SWIFTSQL_MYTEST_ROPASS.
    wxString roPass = Env("SWIFTSQL_MYTEST_ROPASS");
    if (roPass.IsEmpty()) roPass = L"Sw1ftSQL-throwaway-probe";
    const wxString roUser = L"swiftsql_xs_ro";

    wxString e;
    Exec(myMaint, L"DROP USER IF EXISTS '" + roUser + L"'@'%'", "drop probe user");
    if (!TryExec(myMaint, L"CREATE USER '" + roUser + L"'@'%' IDENTIFIED BY '" +
                          roPass + L"'", "create probe user", e)) {
        std::printf("  SKIP hazard 7d: the test account cannot CREATE USER, so the "
                    "restricted-account question cannot be answered here.\n");
        return;
    }
    // SELECT on the test database ONLY. Notably NOT: EXECUTE, ALTER ROUTINE,
    // CREATE ROUTINE — the routine privileges whose absence is under test.
    if (!TryExec(myMaint, L"GRANT SELECT ON " + QuoteIdent(myDb, Dialect::MySQL) +
                          L".* TO '" + roUser + L"'@'%'", "grant SELECT", e)) {
        std::printf("  SKIP hazard 7d: GRANT failed.\n");
        Exec(myMaint, L"DROP USER IF EXISTS '" + roUser + L"'@'%'", "drop probe user");
        return;
    }
    Exec(myMaint, L"FLUSH PRIVILEGES", "flush privileges");

    auto ro = CreateConnection(DbType::MySQL);
    if (!ro->Connect(Profile(DbType::MySQL, host, port, roUser, roPass, myDb), e)) {
        std::printf("  ERR  probe user cannot connect: %s\n", (const char*)e.utf8_str());
        ExpectTrue("hazard7d probe account connects", false);
        Exec(myMaint, L"DROP USER IF EXISTS '" + roUser + L"'@'%'", "drop probe user");
        return;
    }

    // Sanity: the account really can see the TABLES. If this fails the probe is
    // measuring the wrong thing.
    {
        QueryResult r; wxString qe;
        const bool okTables = ro->Execute(L"SELECT COUNT(*) FROM information_schema.TABLES "
                                          L"WHERE TABLE_SCHEMA=DATABASE()", r, qe);
        std::printf("  OBS  probe account sees %s tables (query ok=%d)\n",
                    (okTables && !r.rows.empty() && !r.rows[0].empty())
                        ? (const char*)r.rows[0][0].utf8_str() : "?",
                    (int)okTables);
        ExpectTrue("hazard7d probe account can still read the table catalog", okTables);
    }

    // ---- the raw catalog reads, unmediated by the reader ----
    {
        QueryResult r; wxString qe;
        const bool ok = ro->Execute(
            L"SELECT ROUTINE_NAME, ROUTINE_TYPE, "
            L"CASE WHEN ROUTINE_DEFINITION IS NULL THEN '<NULL>' ELSE "
            L"CONCAT('<len ', CHAR_LENGTH(ROUTINE_DEFINITION), '>') END "
            L"FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA=DATABASE()", r, qe);
        std::printf("  OBS  information_schema.ROUTINES: queryOk=%d rows=%d err=[%s]\n",
                    (int)ok, (int)r.rows.size(), (const char*)qe.utf8_str());
        for (const auto& row : r.rows)
            if (row.size() >= 3)
                std::printf("  OBS    ROUTINES row: name=%s type=%s definition=%s\n",
                            (const char*)row[0].utf8_str(),
                            (const char*)row[1].utf8_str(),
                            (const char*)row[2].utf8_str());

        QueryResult pr; wxString pe;
        const bool pok = ro->Execute(
            L"SELECT SPECIFIC_NAME, ORDINAL_POSITION, COALESCE(DTD_IDENTIFIER,'<NULL>') "
            L"FROM information_schema.PARAMETERS WHERE SPECIFIC_SCHEMA=DATABASE()", pr, pe);
        std::printf("  OBS  information_schema.PARAMETERS: queryOk=%d rows=%d err=[%s]\n",
                    (int)pok, (int)pr.rows.size(), (const char*)pe.utf8_str());
        for (const auto& row : pr.rows)
            if (row.size() >= 3)
                std::printf("  OBS    PARAMETERS row: routine=%s pos=%s type=%s\n",
                            (const char*)row[0].utf8_str(),
                            (const char*)row[1].utf8_str(),
                            (const char*)row[2].utf8_str());

        // The ANSWER line — the one this whole probe exists to print.
        const char* answer =
            !ok  ? "(A) ROUTINES ERRORS for a routine-privilege-less account"
          : r.rows.empty()
                 ? "(C) ROUTINES returns ZERO ROWS — silently invisible, no error"
                 : "(B) ROUTINES returns rows (see definition= above for NULL-ness)";
        std::printf("  ANSWER routines-visibility: %s\n", answer);
        const char* panswer =
            !pok ? "(A) PARAMETERS ERRORS"
          : pr.rows.empty()
                 ? "(C) PARAMETERS returns ZERO ROWS — silently empty, no error"
                 : "(B) PARAMETERS returns rows";
        std::printf("  ANSWER parameters-visibility: %s\n", panswer);
    }

    // ---- the SECOND SIGNAL the reader now uses ----
    //
    // An empty ROUTINES list is ambiguous on MySQL 8 (outcome C above), so
    // MySqlRoutineRead probes SHOW GRANTS to tell "no routines" from "no
    // privilege to see routines". Print the raw grant lines: the parser reads
    // free-form, version-dependent text, and if a future server spells this
    // differently the evidence needs to be in the log, not inferred from a
    // status code.
    {
        QueryResult g; wxString ge;
        const bool gok = ro->Execute(L"SHOW GRANTS FOR CURRENT_USER()", g, ge);
        std::printf("  OBS  SHOW GRANTS (restricted): queryOk=%d rows=%d err=[%s]\n",
                    (int)gok, (int)g.rows.size(), (const char*)ge.utf8_str());
        for (const auto& row : g.rows)
            if (!row.empty())
                std::printf("  OBS    grant: %s\n", (const char*)row[0].utf8_str());
        ExpectTrue("hazard7d SHOW GRANTS is answerable by the restricted account itself",
                   gok && !g.rows.empty());
    }

    // ---- and what the PRODUCT's reader concludes from that ----
    std::vector<RoutineDef> defs;
    wxString detail;
    const RoutineReadStatus st = mysqlroutine::ReadRoutines(*ro, myDb, defs, detail);
    std::printf("  OBS  ReadRoutines(restricted) -> status=%s routines=%d detail=[%s]\n",
                StatusName(st), (int)defs.size(), (const char*)detail.utf8_str());
    for (const auto& d : defs)
        std::printf("  OBS    %s bodyReadable=%d args=%d\n",
                    (const char*)d.Signature().utf8_str(),
                    (int)d.bodyReadable, (int)d.argTypes.size());

    // THE ASSERTION. Whatever the server does, the one outcome that must never
    // happen is the reader reporting Ok with a routine list it cannot actually
    // compare — because Ok + fewer/argument-less routines is what turns into a
    // screen of false "仅目标端存在" pairs. Either the side degrades (non-Ok), or
    // it reports the routines in full, with bodies and arguments.
    bool fullyRead = (st == RoutineReadStatus::Ok) && !defs.empty();
    if (fullyRead)
        for (const auto& d : defs)
            if (!d.bodyReadable || d.argTypes.empty()) { fullyRead = false; break; }
    const bool degraded = (st != RoutineReadStatus::Ok);
    const bool honestlyEmpty = (st == RoutineReadStatus::Ok) && defs.empty();

    if (honestlyEmpty)
        std::printf("  DEFECT hazard7d: the restricted account produced status=Ok with an "
                    "EMPTY routine list. The reader cannot distinguish that from a database "
                    "that genuinely has no routines, so a compare against a fully-privileged "
                    "side will report every routine as 仅目标端存在 — mass false positives, "
                    "silently. This is outcome (C) and no code path handles it.\n");

    ExpectTrue("hazard7d restricted account either degrades or reads in full "
               "(never Ok-with-an-empty-or-partial list)", degraded || fullyRead);

    // Sharper than the line above, which any non-Ok status would satisfy. The
    // ruling is specific: an empty list from an account whose grants
    // demonstrably contain NO routine privilege is a PRIVILEGE state, and the
    // user has to be told which privilege is missing or they cannot act on it.
    if (honestlyEmpty || st != RoutineReadStatus::Ok) {
        ExpectTrue("hazard7d empty list + no routine privilege reports PermissionDenied",
                   st == RoutineReadStatus::PermissionDenied);
        ExpectTrue("hazard7d degraded read explains itself", !detail.IsEmpty());
    }

    // ---- 7e: the SAME account, now holding EXECUTE ----
    //
    // Exercises the OTHER half of the privilege model on a server we can
    // actually reach. EXECUTE is a routine privilege, so the grant probe must
    // now answer "granted" — and whatever MySQL 8 chooses to reveal (rows with
    // bodies, rows with NULL bodies, or still nothing) the reader must remain
    // honest about it. The NULL-body path in particular is the 5.x-shaped
    // defense that has never had a real server pointed at it.
    if (TryExec(myMaint, L"GRANT EXECUTE ON " + QuoteIdent(myDb, Dialect::MySQL) +
                         L".* TO '" + roUser + L"'@'%'", "grant EXECUTE", e)) {
        Exec(myMaint, L"FLUSH PRIVILEGES", "flush privileges");

        auto ro2 = CreateConnection(DbType::MySQL);
        wxString e2;
        if (ro2->Connect(Profile(DbType::MySQL, host, port, roUser, roPass, myDb), e2)) {
            std::vector<RoutineDef> d2;
            wxString det2;
            const RoutineReadStatus s2 = mysqlroutine::ReadRoutines(*ro2, myDb, d2, det2);
            std::printf("  OBS  ReadRoutines(EXECUTE-granted) -> status=%s routines=%d "
                        "detail=[%s]\n", StatusName(s2), (int)d2.size(),
                        (const char*)det2.utf8_str());
            for (const auto& d : d2)
                std::printf("  OBS    %s bodyReadable=%d args=%d body=[%s]\n",
                            (const char*)d.Signature().utf8_str(),
                            (int)d.bodyReadable, (int)d.argTypes.size(),
                            (const char*)d.body.utf8_str());

            // Same invariant as 7d, restated for the granted case: never Ok with
            // a list the compare cannot actually use.
            bool full2 = (s2 == RoutineReadStatus::Ok) && !d2.empty();
            if (full2)
                for (const auto& d : d2)
                    if (!d.bodyReadable || d.argTypes.empty()) { full2 = false; break; }
            ExpectTrue("hazard7e EXECUTE-granted account either degrades or reads in full",
                       (s2 != RoutineReadStatus::Ok) || full2);
            ro2->Disconnect();
        } else {
            std::printf("  note hazard7e: reconnect as probe user failed: %s\n",
                        (const char*)e2.utf8_str());
        }
    }

    ro->Disconnect();
    Exec(myMaint, L"DROP USER IF EXISTS '" + roUser + L"'@'%'", "drop probe user");
}

// ---------------------------------------------------------------------------
// 7f — THE REGRESSION GUARD for the 7d fix.
//
// The fix makes an empty routine list conditional on a privilege probe, and the
// way to get that wrong is to make it too aggressive: a database that genuinely
// has no functions or procedures, read by an account that plainly could see them
// if they existed, MUST still report Ok with an empty list. If this ever starts
// reporting PermissionDenied, every empty schema on every server begins claiming
// the user's credentials are broken — a false positive of the opposite sign, and
// the reason the ruling calls this case out explicitly.
// ---------------------------------------------------------------------------
void EmptyCatalogPrivilegedProbe(IConnection& myMaint, const wxString& emptyDb)
{
    std::printf("\n-- hazard 7f: genuinely empty catalog, fully privileged account --\n");

    const wxString qDb = QuoteIdent(emptyDb, Dialect::MySQL);
    if (!Exec(myMaint, L"DROP DATABASE IF EXISTS " + qDb, "drop empty db") ||
        !Exec(myMaint, L"CREATE DATABASE " + qDb, "create empty db")) {
        ExpectTrue("hazard7f empty database created", false);
        return;
    }

    {
        QueryResult g; wxString ge;
        const bool gok = myMaint.Execute(L"SHOW GRANTS FOR CURRENT_USER()", g, ge);
        std::printf("  OBS  SHOW GRANTS (privileged): queryOk=%d rows=%d\n",
                    (int)gok, (int)g.rows.size());
        for (const auto& row : g.rows)
            if (!row.empty())
                std::printf("  OBS    grant: %s\n", (const char*)row[0].utf8_str());
    }

    std::vector<RoutineDef> defs;
    wxString detail;
    const RoutineReadStatus st = mysqlroutine::ReadRoutines(myMaint, emptyDb, defs, detail);
    std::printf("  OBS  ReadRoutines(empty db, privileged) -> status=%s routines=%d "
                "detail=[%s]\n", StatusName(st), (int)defs.size(),
                (const char*)detail.utf8_str());

    ExpectTrue("hazard7f empty catalog + routine privileges still reports Ok",
               st == RoutineReadStatus::Ok);
    ExpectEq("hazard7f empty catalog returns no routines", (long long)defs.size(), 0);

    Exec(myMaint, L"DROP DATABASE IF EXISTS " + qDb, "drop empty db");
}

// ---------------------------------------------------------------------------
// 7g — is PostgreSQL exposed to the same ambiguity?
//
// PgRoutineRead has no privilege probe and returns Ok for an empty pg_proc
// result. That is only safe if PostgreSQL does NOT filter pg_proc by privilege
// the way MySQL 8 filters information_schema.ROUTINES. pg_proc is documented as
// world-readable, but "documented" is what the MySQL 5.x behaviour was too, so
// this measures it instead: SET ROLE to a freshly created, deliberately
// privilege-starved role (no EXECUTE on anything, not even via PUBLIC) and read
// the catalog again through the product's own reader.
//
// SET ROLE rather than a second login is deliberate — it needs no PG credentials
// threaded through this harness, and it is the same privilege context a
// restricted login would run under for catalog visibility purposes.
// ---------------------------------------------------------------------------
void PgRestrictedRoleProbe(IConnection& pg, const wxString& pgSchema)
{
    std::printf("\n-- hazard 7g: PostgreSQL under a privilege-starved role --\n");

    const wxString role = L"swiftsql_xs_norole";
    wxString e;
    // A PG role is CLUSTER-wide, not per-database, so it outlives the throwaway
    // test databases and must be cleaned up explicitly. DROP ROLE alone is not
    // enough once anything has been granted TO the role — the grant is a
    // dependency and the drop fails ("cannot be dropped because some objects
    // depend on it"), leaking the role onto a shared server. DROP OWNED BY
    // removes those grants first. It errors when the role does not exist, hence
    // TryExec on the way in.
    TryExec(pg, L"DROP OWNED BY " + role, "drop pg probe role grants (pre)", e);
    TryExec(pg, L"DROP ROLE IF EXISTS " + role, "drop pg probe role (pre)", e);
    if (!TryExec(pg, L"CREATE ROLE " + role, "create pg probe role", e)) {
        std::printf("  SKIP hazard 7g: cannot CREATE ROLE here.\n");
        return;
    }
    // Strip every routine privilege this role could inherit, including the
    // EXECUTE that PUBLIC holds on new functions by default.
    TryExec(pg, L"REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA " + pgSchema +
                L" FROM PUBLIC", "revoke EXECUTE from PUBLIC", e);
    TryExec(pg, L"REVOKE ALL ON ALL FUNCTIONS IN SCHEMA " + pgSchema +
                L" FROM " + role, "revoke ALL from probe role", e);
    TryExec(pg, L"GRANT USAGE ON SCHEMA " + pgSchema + L" TO " + role,
            "grant schema USAGE", e);

    std::vector<RoutineDef> defs;
    wxString detail;
    const bool switched = TryExec(pg, L"SET ROLE " + role, "SET ROLE", e);
    RoutineReadStatus st = RoutineReadStatus::Unsupported;
    if (switched) {
        std::printf("  OBS  current_user under SET ROLE = [%s]\n",
                    (const char*)ScalarOf(pg, L"SELECT current_user").utf8_str());
        st = pgroutine::ReadRoutines(pg, pgSchema, defs, detail);
        std::printf("  OBS  ReadRoutines(pg, starved role) -> status=%s routines=%d "
                    "detail=[%s]\n", StatusName(st), (int)defs.size(),
                    (const char*)detail.utf8_str());
        for (const auto& d : defs)
            std::printf("  OBS    pg %s bodyReadable=%d body=[%s]\n",
                        (const char*)d.Signature().utf8_str(),
                        (int)d.bodyReadable, (const char*)d.body.utf8_str());
    }
    TryExec(pg, L"RESET ROLE", "RESET ROLE", e);

    if (switched) {
        // THE CLAIM PgRoutineRead.cpp RELIES ON. If this ever fails, PostgreSQL
        // has acquired MySQL 8's row filtering and PgRoutineRead needs the same
        // privilege probe MySqlRoutineRead grew.
        ExpectTrue("hazard7g pg_proc rows survive a role with no routine privileges",
                   st == RoutineReadStatus::Ok && !defs.empty());
        bool bodies = !defs.empty();
        for (const auto& d : defs)
            if (!d.bodyReadable || d.body.IsEmpty()) { bodies = false; break; }
        ExpectTrue("hazard7g prosrc bodies remain readable to an unprivileged role",
                   bodies);
    }

    // Put PUBLIC's default EXECUTE back before tearing the role down, so this
    // probe leaves the (throwaway, but shared-server) database as it found it.
    TryExec(pg, L"GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA " + pgSchema +
                L" TO PUBLIC", "restore PUBLIC EXECUTE", e);
    TryExec(pg, L"DROP OWNED BY " + role, "drop pg probe role grants", e);
    Exec(pg, L"DROP ROLE IF EXISTS " + role, "drop pg probe role");
}

} // namespace

void RunRoutineHazards(IConnection& my, IConnection& pg,
                       const wxString& myDb, const wxString& pgDb,
                       const wxString& host, int port,
                       const wxString& rootUser, const wxString& rootPass)
{
    std::printf("\n-- hazard 7: routines --\n");

    if (!CreateMySqlRoutines(my, /*withComments=*/false)) {
        ExpectTrue("hazard7 MySQL routines created", false);
        return;
    }
    if (!CreatePgRoutines(pg)) {
        ExpectTrue("hazard7 PostgreSQL routines created", false);
        return;
    }

    // A second, disposable MySQL database for the same-engine comparison.
    const wxString db2 = myDb + L"2";
    const wxString qDb2 = QuoteIdent(db2, Dialect::MySQL);
    auto maint = CreateConnection(DbType::MySQL);
    wxString e;
    if (!maint->Connect(Profile(DbType::MySQL, host, port, rootUser, rootPass, L"mysql"), e)) {
        std::printf("  ERR  connect MySQL maintenance for hazard 7: %s\n",
                    (const char*)e.utf8_str());
        ExpectTrue("hazard7 maintenance connection", false);
        return;
    }
    struct Db2Guard {
        IConnection* c; wxString q;
        ~Db2Guard() { QueryResult r; wxString e; if (c) c->Execute(L"DROP DATABASE IF EXISTS " + q, r, e); }
    } db2guard{ maint.get(), qDb2 };

    if (Exec(*maint, L"DROP DATABASE IF EXISTS " + qDb2, "drop mysql db2") &&
        Exec(*maint, L"CREATE DATABASE " + qDb2 +
                     L" DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci",
             "create mysql db2")) {
        SameEngineCompare(my, myDb, host, port, rootUser, rootPass, db2);
    } else {
        ExpectTrue("hazard7a second MySQL database created", false);
    }

    CrossEngineCompare(my, pg, myDb, pgDb);
    RestrictedAccountProbe(*maint, myDb, host, port);
    EmptyCatalogPrivilegedProbe(*maint, myDb + L"3");
    PgRestrictedRoleProbe(pg, L"public");

    // NO explicit maint->Disconnect() here. db2guard's destructor runs at the
    // end of this scope and issues its DROP DATABASE through `maint`, so
    // closing the connection first would make that drop fail silently and leak
    // the second test database onto a shared server — which is exactly what
    // happened until the startup leftover audit reported swiftsql_xsync_src2
    // surviving a clean run. `maint` is a unique_ptr declared BEFORE db2guard,
    // so the guard is destroyed first and the connection closes after it.
}

} // namespace mplive
