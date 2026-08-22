// ResultGridSqlTests.cpp — headless unit tests for ui::gridsql
// (src/ui/ResultGridSql.{h,cpp}), the pure SQL-text generator behind the result
// grid / data browser.
//
// WHY THIS FILE EXISTS. Everything asserted here used to be inline inside
// ResultGridPanelOps.cpp, tangled with wxGrid reads, and therefore reachable
// ONLY by clicking through a running app against a live server. That includes
// the statements that WRITE to a customer's database: the edit→DML diff that 保存
// applies, and the 复制为 UPDATE/DELETE the user pastes into a query window. A
// wrong WHERE clause in either is a silent data-corruption bug, and until this
// extraction there was no way to see one without a database in the loop.
//
// The suite began as a CHARACTERIZATION suite: it pinned what the code did, byte
// for byte, so the structural split that produced it could be reviewed as
// behaviour-preserving. Two of those pins were pins on KNOWN DEFECTS, asserted
// as current behaviour so that fixing them would be a deliberate, visible change
// rather than a silent one:
//
//   * the composite-PK gate on the edit→DML path was `pkIdx is non-empty` rather
//     than "every PK column was found", so a partially-present composite key
//     emitted an UPDATE keyed on the found column(s) alone — silently rewriting
//     rows the user never edited;
//   * identifiers on the DML / clipboard / filter paths were wrapped WITHOUT
//     doubling an embedded quote char, while the pager path quoted correctly.
//
// Both are now FIXED, and those two pins have been inverted: they assert the
// fixed behaviour and stand as REGRESSION tests. They are kept, not deleted —
// each was a bug report, and its job now is to keep the bug from coming back.
//
// Same dependency-free harness as the rest of tests/: an ExpectEq loop, non-zero
// exit on failure, no doctest/Catch2. Compiles ResultGridSql.cpp directly
// (mirroring SyncDiffModelTests / SyncCrossEngineGateTests) and links
// swiftsql::db for db::Dialect / db::QuoteIdent / db::LimitOffsetClause. No
// wxWindow, no wxGrid, no connection.
#include "ui/ResultGridSql.h"

#include <cstdio>
#include <iterator>   // std::size

using db::Dialect;
using ui::SortKey;
using ui::gridsql::CellGrid;
using ui::gridsql::CellRow;
using ui::gridsql::DmlContext;
namespace gs = ui::gridsql;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectEq(const char* what, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n        want: [%s]\n        got : [%s]\n",
                    what, (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", what);
    }
}

static void ExpectTrue(const char* what, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", what); }
    else       { std::printf("  ok   %s\n", what); }
}

// Most edit-path assertions care only about the statements, so unwrap .sql.
// The tests that exercise the PK GATE call gs::BuildEditDml directly and inspect
// .gate / .dirty / .missingPk — .sql alone cannot distinguish "clean" from
// "refused", which is exactly the confusion that made the defect silent.
static wxString EditSql(const DmlContext& ctx, const CellGrid& origRows,
                        const std::vector<int>& rowOrig, const CellGrid& deleted,
                        const CellGrid& current)
{
    return gs::BuildEditDml(ctx, origRows, rowOrig, deleted, current).sql;
}

// A plain editable single-table MySQL result: users(id PK, name, age).
static DmlContext Users(Dialect d = Dialect::MySQL)
{
    DmlContext c;
    c.table     = L"users";
    c.dialect   = d;
    c.columns   = { L"id", L"name", L"age" };
    c.pkColumns = { L"id" };
    return c;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
static void TestPrimitives()
{
    std::printf("-- primitives --\n");
    // QuoteVerbatim is GONE — db::QuoteIdent is the module's only quoter now.
    // A plain name looks the same through either; the difference shows up on the
    // hostile names asserted in TestQuotingIsUnified below.
    ExpectEq("identifiers quote through db::QuoteIdent (MySQL)",
             db::QuoteIdent(L"name", Dialect::MySQL), L"`name`");
    ExpectEq("identifiers quote through db::QuoteIdent (PG)",
             db::QuoteIdent(L"name", Dialect::Postgres), L"\"name\"");

    // The driver's null marker is a bare keyword; every other value is quoted
    // with ' doubled. Getting this wrong is either a syntax error or an
    // injection, so it is pinned on both arms plus the adversarial value.
    ExpectEq("RenderValue NULL marker", gs::RenderValue(L"NULL"), L"NULL");
    ExpectEq("RenderValue plain",       gs::RenderValue(L"alice"), L"'alice'");
    ExpectEq("RenderValue empty",       gs::RenderValue(wxString()), L"''");
    ExpectEq("RenderValue apostrophe",  gs::RenderValue(L"O'Brien"), L"'O''Brien'");
    ExpectEq("RenderValue injection",   gs::RenderValue(L"x'; DROP TABLE t; --"),
             L"'x''; DROP TABLE t; --'");
    // ⚠ STILL A PIN, DELIBERATELY NOT FIXED. The literal four characters N,U,L,L
    // are indistinguishable from the null marker, so a user cannot store the
    // string "NULL" through the data browser. Left alone alongside the two fixes
    // in this change because it is NOT small: the marker is a wxString sentinel
    // shared by the driver layer (QueryResult.rows renders NULL as L"NULL"), the
    // grid's null recolouring, the 设为 NULL cell action and the cell filter's
    // IS NULL mapping. Fixing it properly means a real per-cell null flag
    // threaded through all of those — a redesign of the grid's value encoding,
    // not a patch here, and far wider than the write-path defects this change
    // exists to fix. Pinned so the ambiguity stays visible and costed.
    ExpectEq("PINNED (not fixed): cannot store the literal string NULL",
             gs::RenderValue(L"NULL"), L"NULL");

    DmlContext c = Users();
    ExpectEq("QualifyForDml no db", gs::QualifyForDml(c), L"`users`");
    c.db = L"shop";
    ExpectEq("QualifyForDml MySQL db-qualified", gs::QualifyForDml(c), L"`shop`.`users`");
    // A db qualifier is a MySQL-only concern (PG cannot cross databases in an
    // identifier), so it must be dropped rather than emitted for other dialects.
    c.dialect = Dialect::Postgres;
    ExpectEq("QualifyForDml PG ignores db", gs::QualifyForDml(c), L"\"users\"");

    DmlContext k = Users();
    k.columns   = { L"tenant", L"name", L"id" };
    k.pkColumns = { L"id", L"tenant" };
    const std::vector<int> pk = gs::MapPkIndices(k);
    ExpectTrue("MapPkIndices maps in pkColumns order, not column order",
               pk.size() == 2 && pk[0] == 2 && pk[1] == 0);
    ExpectEq("BuildPkWhere composite",
             gs::BuildPkWhere(k, pk, { L"acme", L"alice", L"7" }),
             L"`id`='7' AND `tenant`='acme'");

    // A pk column absent from the result yields a SHORT index vector — the
    // signal callers must check before emitting an under-constrained WHERE.
    DmlContext miss = Users();
    miss.pkColumns = { L"id", L"tenant" };            // `tenant` is not in columns
    ExpectTrue("MapPkIndices reports a partial key as a short vector",
               gs::MapPkIndices(miss).size() == 1);

    // MissingPkColumns NAMES what is absent — this is the text the refusal
    // message shows the user, so it must be the missing column, not the found one.
    const std::vector<wxString> gone = gs::MissingPkColumns(miss);
    ExpectTrue("MissingPkColumns names exactly the absent key column",
               gone.size() == 1 && gone[0] == L"tenant");
    ExpectTrue("MissingPkColumns is empty when the whole key is present",
               gs::MissingPkColumns(k).empty());
    ExpectTrue("MissingPkColumns reports every absent column, in declaration order",
               [] {
                   DmlContext m = Users();
                   m.pkColumns = { L"tenant", L"id", L"region" };
                   const std::vector<wxString> g = gs::MissingPkColumns(m);
                   return g.size() == 2 && g[0] == L"tenant" && g[1] == L"region";
               }());

    // BuildPkWhere refuses a partial key outright rather than emitting a WHERE
    // that names only the found columns. "" makes a caller that ignores its gate
    // produce `WHERE ;` — a syntax error the server rejects — instead of an
    // UPDATE that quietly matches rows the user never touched.
    ExpectEq("BuildPkWhere refuses a partial key",
             gs::BuildPkWhere(miss, gs::MapPkIndices(miss), { L"1", L"alice", L"30" }),
             wxString());
}

// ---------------------------------------------------------------------------
// The edit → DML diff (保存). The highest-stakes function in the file.
// ---------------------------------------------------------------------------
static void TestEditDml()
{
    std::printf("-- edit -> DML diff --\n");
    const DmlContext c = Users();
    const CellGrid orig = { { L"1", L"alice", L"30" },
                            { L"2", L"bob",   L"41" } };

    // Nothing touched → no statements at all. (This is also what drives the 保存
    // button's enabled state, so a false positive here means a permanently-green
    // Save on a clean grid.)
    ExpectEq("clean grid emits nothing",
             EditSql(c, orig, { 0, 1 }, {}, orig), wxString());

    // UPDATE names ONLY the changed column, and keys off the ORIGINAL row —
    // keying off the edited row would lose the record when the PK is edited.
    ExpectEq("one edited cell -> one-column UPDATE",
             EditSql(c, orig, { 0, 1 }, {},
                              { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } }),
             L"UPDATE `users` SET `age`='31' WHERE `id`='1';\n");

    ExpectEq("two edited cells in one row -> one UPDATE, two SETs",
             EditSql(c, orig, { 0, 1 }, {},
                              { { L"1", L"alicia", L"31" }, { L"2", L"bob", L"41" } }),
             L"UPDATE `users` SET `name`='alicia', `age`='31' WHERE `id`='1';\n");

    // Editing the PK itself: the WHERE must still carry the OLD key.
    ExpectEq("edited PK keys the UPDATE off the ORIGINAL value",
             EditSql(c, orig, { 0, 1 }, {},
                              { { L"99", L"alice", L"30" }, { L"2", L"bob", L"41" } }),
             L"UPDATE `users` SET `id`='99' WHERE `id`='1';\n");

    // A new row (rowOrig -1) inserts only its non-empty cells, so server-side
    // defaults / auto-increment still apply to the rest.
    ExpectEq("new row -> INSERT of non-empty cells only",
             EditSql(c, orig, { 0, 1, -1 }, {},
                              { { L"1", L"alice", L"30" }, { L"2", L"bob", L"41" },
                                { wxString(), L"carol", wxString() } }),
             L"INSERT INTO `users` (`name`) VALUES ('carol');\n");

    // A wholly empty new row is not an INSERT of nothing — it is skipped.
    ExpectEq("entirely empty new row emits nothing",
             EditSql(c, orig, { 0, 1, -1 }, {},
                              { { L"1", L"alice", L"30" }, { L"2", L"bob", L"41" },
                                { wxString(), wxString(), wxString() } }),
             wxString());

    // DELETEs come FIRST. Ordering is load-bearing: an INSERT reusing a deleted
    // row's unique key would collide if the DELETE ran after it.
    ExpectEq("DELETEs are emitted before INSERTs",
             EditSql(c, orig, { 0, -1 }, { { L"2", L"bob", L"41" } },
                              { { L"1", L"alice", L"30" }, { L"2", L"carol", L"22" } }),
             L"DELETE FROM `users` WHERE `id`='2';\n"
             L"INSERT INTO `users` (`id`, `name`, `age`) VALUES ('2', 'carol', '22');\n");

    // NULL round-trips as a keyword on both sides of the diff.
    const CellGrid nullOrig = { { L"1", L"NULL", L"30" } };
    ExpectEq("value -> NULL becomes SET col=NULL",
             EditSql(c, nullOrig, { 0 }, {}, { { L"1", L"NULL", L"31" } }),
             L"UPDATE `users` SET `age`='31' WHERE `id`='1';\n");
    ExpectEq("NULL is not confused with empty string",
             EditSql(c, { { L"1", L"alice", L"30" } }, { 0 }, {},
                              { { L"1", L"NULL", L"30" } }),
             L"UPDATE `users` SET `name`=NULL WHERE `id`='1';\n");

    // No PK column present in the result → refuse entirely. A keyless UPDATE
    // would rewrite the whole table. The refusal must also be DISTINGUISHABLE
    // from a clean grid: dirty stays true so the caller can say why.
    DmlContext nopk = Users();
    nopk.pkColumns = { L"rowid" };                    // not among the columns
    const gs::EditDml noneFound =
        gs::BuildEditDml(nopk, orig, { 0, 1 }, {},
                         { { L"1", L"zzz", L"30" }, { L"2", L"bob", L"41" } });
    ExpectEq("no PK in the result -> no statements at all", noneFound.sql, wxString());
    ExpectTrue("...but the edit is still reported as pending, not as clean",
               noneFound.dirty);
    ExpectTrue("...and the gate says the key is incomplete",
               noneFound.gate == gs::PkGate::IncompleteKey &&
               noneFound.missingPk.size() == 1 && noneFound.missingPk[0] == L"rowid");

    // A table with NO declared PK is a different refusal from a query that just
    // dropped the key columns, and the user-facing message differs accordingly.
    DmlContext keyless = Users();
    keyless.pkColumns.clear();
    const gs::EditDml none =
        gs::BuildEditDml(keyless, orig, { 0, 1 }, {},
                         { { L"1", L"zzz", L"30" }, { L"2", L"bob", L"41" } });
    ExpectEq("table with no PK at all -> no statements", none.sql, wxString());
    ExpectTrue("table with no PK at all -> NoPrimaryKey, not IncompleteKey",
               none.gate == gs::PkGate::NoPrimaryKey && none.missingPk.empty());

    // Postgres uses double quotes throughout.
    ExpectEq("Postgres quoting",
             EditSql(Users(Dialect::Postgres), orig, { 0, 1 }, {},
                              { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } }),
             L"UPDATE \"users\" SET \"age\"='31' WHERE \"id\"='1';\n");

    // Browse mode qualifies with the browsed database so DML works with no
    // default schema selected ("No database selected").
    DmlContext qualified = Users();
    qualified.db = L"shop";
    ExpectEq("browse-mode DML is db-qualified",
             EditSql(qualified, orig, { 0, 1 }, {},
                              { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } }),
             L"UPDATE `shop`.`users` SET `age`='31' WHERE `id`='1';\n");

    // ── REGRESSION TEST (was a pin on a KNOWN DEFECT) ──────────────────────
    // This assertion used to demand the BUG: a COMPOSITE key of which only some
    // columns were in the result still emitted statements, keyed on the found
    // column(s) alone — `UPDATE users SET age='31' WHERE id='1'`, which matches
    // EVERY tenant's id=1, not just the row the user edited. The gate is now the
    // same all-or-nothing rule the 复制为 path always had, and this assertion is
    // inverted to demand the fix. It stays because it is the bug report.
    DmlContext partial = Users();
    partial.pkColumns = { L"id", L"tenant" };         // `tenant` is not a result column
    const gs::EditDml part =
        gs::BuildEditDml(partial, orig, { 0, 1 }, {},
                         { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } });
    ExpectEq("FIXED: partial composite PK emits NO under-constrained UPDATE",
             part.sql, wxString());
    ExpectTrue("FIXED: the refusal is reported, not silent — gate + missing column",
               part.gate == gs::PkGate::IncompleteKey &&
               part.missingPk.size() == 1 && part.missingPk[0] == L"tenant");
    ExpectTrue("FIXED: the pending edit is still visible to the caller",
               part.dirty);

    // The gate keys off PRESENCE of the column, not its VALUE. A PK column that
    // is present but empty (or NULL) is a located key — refusing there would
    // block legitimate edits, which is the opposite failure.
    DmlContext bothPresent;
    bothPresent.table     = L"orders";
    bothPresent.dialect   = Dialect::MySQL;
    bothPresent.columns   = { L"tenant", L"id", L"total" };
    bothPresent.pkColumns = { L"tenant", L"id" };
    ExpectEq("PK column present but EMPTY still passes the gate (presence != value)",
             EditSql(bothPresent, { { wxString(), L"7", L"99" } }, { 0 }, {},
                     { { wxString(), L"7", L"100" } }),
             L"UPDATE `orders` SET `total`='100' WHERE `tenant`='' AND `id`='7';\n");
    // (A PK column holding the NULL marker also passes the gate — presence is
    // presence — but the resulting `key=NULL` never matches. Unreachable in
    // practice: a PK column is NOT NULL by definition. Not asserted here, so as
    // not to bless a never-matching WHERE as intended behaviour.)

    // A fully-present composite key is unaffected by the tightened gate.
    ExpectEq("fully-present composite PK still emits both key columns",
             EditSql(bothPresent, { { L"acme", L"7", L"99" } }, { 0 }, {},
                     { { L"acme", L"7", L"100" } }),
             L"UPDATE `orders` SET `total`='100' WHERE `tenant`='acme' AND `id`='7';\n");

    // ...and neither is the ordinary single-column-PK edit, which is the case
    // essentially every user hits. Gate verdict Ok, statements byte-identical.
    const gs::EditDml plain =
        gs::BuildEditDml(c, orig, { 0, 1 }, {},
                         { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } });
    ExpectEq("single-column PK edit is completely unaffected by the new gate",
             plain.sql, L"UPDATE `users` SET `age`='31' WHERE `id`='1';\n");
    ExpectTrue("single-column PK edit passes the gate cleanly",
               plain.gate == gs::PkGate::Ok && plain.missingPk.empty() && plain.dirty);
    const gs::EditDml clean = gs::BuildEditDml(c, orig, { 0, 1 }, {}, orig);
    ExpectTrue("a clean grid is Ok-but-not-dirty (distinct from a refusal)",
               clean.gate == gs::PkGate::Ok && !clean.dirty && clean.sql.IsEmpty());
}

// ---------------------------------------------------------------------------
// HasPendingEdits — "is there anything to save?" answered WITHOUT the PK.
// ---------------------------------------------------------------------------
// This has to stay answerable when the gate refuses. Deriving it from the
// generated SQL (the old `!BuildDml().IsEmpty()`) reported a refused edit as a
// CLEAN grid, which greyed out 保存 and let the unsaved-changes guard wave a page
// turn through — the refused edits were then silently discarded.
static void TestPendingEditsAreVisibleWithoutAKey()
{
    std::printf("-- pending edits without a usable key --\n");
    const DmlContext c = Users();
    const CellGrid orig = { { L"1", L"alice", L"30" },
                            { L"2", L"bob",   L"41" } };

    ExpectTrue("clean grid is not dirty",
               !gs::HasPendingEdits(c, orig, { 0, 1 }, {}, orig));
    ExpectTrue("an edited cell is dirty",
               gs::HasPendingEdits(c, orig, { 0, 1 }, {},
                                   { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } }));
    ExpectTrue("a deleted row is dirty",
               gs::HasPendingEdits(c, orig, { 0 }, { { L"2", L"bob", L"41" } },
                                   { { L"1", L"alice", L"30" } }));
    ExpectTrue("a new row with content is dirty",
               gs::HasPendingEdits(c, orig, { 0, 1, -1 }, {},
                                   { { L"1", L"alice", L"30" }, { L"2", L"bob", L"41" },
                                     { wxString(), L"carol", wxString() } }));
    ExpectTrue("an entirely empty new row is NOT dirty",
               !gs::HasPendingEdits(c, orig, { 0, 1, -1 }, {},
                                    { { L"1", L"alice", L"30" }, { L"2", L"bob", L"41" },
                                      { wxString(), wxString(), wxString() } }));

    // The load-bearing case: no usable key, but the user HAS edited.
    DmlContext partial = Users();
    partial.pkColumns = { L"id", L"tenant" };
    ExpectTrue("dirty is independent of the PK gate",
               gs::HasPendingEdits(partial, orig, { 0, 1 }, {},
                                   { { L"1", L"alice", L"31" }, { L"2", L"bob", L"41" } }));
}

// ---------------------------------------------------------------------------
// 复制为 SQL — clipboard statements
// ---------------------------------------------------------------------------
static void TestClipboardStatements()
{
    std::printf("-- clipboard statements --\n");
    const DmlContext c = Users();
    const CellGrid rows = { { L"1", L"alice", L"30" }, { L"2", L"bob", L"41" } };

    ExpectEq("INSERT statements name every column",
             gs::BuildInsertStatements(c, rows),
             L"INSERT INTO `users` (`id`, `name`, `age`) VALUES ('1', 'alice', '30');\n"
             L"INSERT INTO `users` (`id`, `name`, `age`) VALUES ('2', 'bob', '41');\n");

    // An ad-hoc (non-single-table) result has no table name; the snippet must
    // still be paste-able, so a placeholder is used rather than empty SQL.
    DmlContext anon = c;
    anon.table.clear();
    ExpectEq("INSERT falls back to the table_name placeholder",
             gs::BuildInsertStatements(anon, { { L"1", L"alice", L"30" } }),
             L"INSERT INTO `table_name` (`id`, `name`, `age`) VALUES ('1', 'alice', '30');\n");

    ExpectEq("no columns -> no INSERTs", gs::BuildInsertStatements(DmlContext{}, rows),
             wxString());

    // UPDATE sets every NON-PK column and keys on the PK.
    ExpectEq("UPDATE statements exclude the PK from SET",
             gs::BuildRowDmlStatements(c, rows, /*asUpdate*/ true),
             L"UPDATE `users` SET `name`='alice', `age`='30' WHERE `id`='1';\n"
             L"UPDATE `users` SET `name`='bob', `age`='41' WHERE `id`='2';\n");

    ExpectEq("DELETE statements key on the PK",
             gs::BuildRowDmlStatements(c, rows, /*asUpdate*/ false),
             L"DELETE FROM `users` WHERE `id`='1';\n"
             L"DELETE FROM `users` WHERE `id`='2';\n");

    // THE all-or-nothing gate: unlike the edit diff above, a partially-present
    // composite key yields NOTHING here. This is the correct behaviour of the
    // two, and the reason the defect pinned above is worth reporting.
    DmlContext partial = Users();
    partial.pkColumns = { L"id", L"tenant" };
    ExpectEq("partial composite PK refuses to generate UPDATEs",
             gs::BuildRowDmlStatements(partial, rows, true), wxString());
    ExpectEq("partial composite PK refuses to generate DELETEs",
             gs::BuildRowDmlStatements(partial, rows, false), wxString());

    DmlContext nopk = Users();
    nopk.pkColumns.clear();
    ExpectEq("no PK at all refuses to generate row DML",
             gs::BuildRowDmlStatements(nopk, rows, false), wxString());

    // Composite key, fully present → both columns in the WHERE, in key order.
    DmlContext comp;
    comp.table     = L"orders";
    comp.dialect   = Dialect::MySQL;
    comp.columns   = { L"tenant", L"id", L"total" };
    comp.pkColumns = { L"tenant", L"id" };
    ExpectEq("composite PK WHERE carries every key column",
             gs::BuildRowDmlStatements(comp, { { L"acme", L"7", L"99" } }, false),
             L"DELETE FROM `orders` WHERE `tenant`='acme' AND `id`='7';\n");
    ExpectEq("composite PK UPDATE sets only the non-key column",
             gs::BuildRowDmlStatements(comp, { { L"acme", L"7", L"99" } }, true),
             L"UPDATE `orders` SET `total`='99' WHERE `tenant`='acme' AND `id`='7';\n");

    // Values are escaped on the clipboard path too.
    ExpectEq("clipboard DML escapes apostrophes",
             gs::BuildRowDmlStatements(c, { { L"1", L"O'Brien", L"30" } }, true),
             L"UPDATE `users` SET `name`='O''Brien', `age`='30' WHERE `id`='1';\n");
}

// ---------------------------------------------------------------------------
// Pager / filter (the db::QuoteIdent regime)
// ---------------------------------------------------------------------------
static void TestPagingAndFilter()
{
    std::printf("-- paging + filter --\n");
    ExpectEq("QualifyForPaging MySQL db-qualified",
             gs::QualifyForPaging(Dialect::MySQL, L"shop", L"users"), L"`shop`.`users`");
    ExpectEq("QualifyForPaging MySQL no db",
             gs::QualifyForPaging(Dialect::MySQL, wxString(), L"users"), L"`users`");
    ExpectEq("QualifyForPaging PG ignores db",
             gs::QualifyForPaging(Dialect::Postgres, L"shop", L"users"), L"\"users\"");

    const std::vector<wxString> cols = { L"id", L"name", L"created_at" };

    ExpectEq("no keys -> empty ORDER BY body",
             gs::BuildOrderByBody(cols, {}, Dialect::MySQL), wxString());
    ExpectEq("single key",
             gs::BuildOrderByBody(cols, { { 1, true } }, Dialect::MySQL), L"`name` ASC");
    ExpectEq("multi-key keeps priority order",
             gs::BuildOrderByBody(cols, { { 2, false }, { 1, true } }, Dialect::MySQL),
             L"`created_at` DESC, `name` ASC");
    // An out-of-range key (a stale sort after the column set shrank) must be
    // skipped, not spliced in as garbage or an empty identifier.
    ExpectEq("out-of-range keys are skipped",
             gs::BuildOrderByBody(cols, { { 9, true }, { -1, false }, { 0, true } },
                                  Dialect::MySQL),
             L"`id` ASC");

    ExpectEq("page SELECT, first page, no filter/sort",
             gs::BuildPageSelect(Dialect::MySQL, L"shop", L"users", wxString(),
                                 cols, {}, 100, 0),
             L"SELECT * FROM `shop`.`users`\nLIMIT 100 OFFSET 0;");
    ExpectEq("page SELECT offsets by page*size",
             gs::BuildPageSelect(Dialect::MySQL, L"shop", L"users", wxString(),
                                 cols, {}, 100, 3),
             L"SELECT * FROM `shop`.`users`\nLIMIT 100 OFFSET 300;");
    ExpectEq("page SELECT carries WHERE and ORDER BY in that order",
             gs::BuildPageSelect(Dialect::MySQL, L"shop", L"users", L"`age` > 30",
                                 cols, { { 2, false } }, 50, 1),
             L"SELECT * FROM `shop`.`users`\nWHERE `age` > 30\n"
             L"ORDER BY `created_at` DESC\nLIMIT 50 OFFSET 50;");
    // Oracle pages with OFFSET…FETCH rather than LIMIT — routed through
    // db::LimitOffsetClause, pinned here so a dialect regression is visible.
    ExpectEq("Oracle page SELECT uses OFFSET/FETCH",
             gs::BuildPageSelect(Dialect::Oracle, wxString(), L"users", wxString(),
                                 cols, {}, 25, 2),
             L"SELECT * FROM \"users\"\nOFFSET 50 ROWS FETCH NEXT 25 ROWS ONLY;");

    // The cell filter. NULL must become IS [NOT] NULL — `col = 'NULL'` would
    // silently match nothing, which reads to the user as "the filter is broken".
    ExpectEq("filter by value",
             gs::BuildCellFilterClause(Dialect::MySQL, L"name", L"alice", false),
             L"`name` = 'alice'");
    ExpectEq("exclude value",
             gs::BuildCellFilterClause(Dialect::MySQL, L"name", L"alice", true),
             L"`name` <> 'alice'");
    ExpectEq("filter by NULL uses IS NULL",
             gs::BuildCellFilterClause(Dialect::MySQL, L"name", L"NULL", false),
             L"`name` IS NULL");
    ExpectEq("exclude NULL uses IS NOT NULL",
             gs::BuildCellFilterClause(Dialect::MySQL, L"name", L"NULL", true),
             L"`name` IS NOT NULL");
    ExpectEq("filter escapes apostrophes",
             gs::BuildCellFilterClause(Dialect::MySQL, L"name", L"O'Brien", false),
             L"`name` = 'O''Brien'");
    ExpectEq("filter PG quoting",
             gs::BuildCellFilterClause(Dialect::Postgres, L"name", L"alice", false),
             L"\"name\" = 'alice'");
}

// ---------------------------------------------------------------------------
// REGRESSION TEST (was a pin on a KNOWN DEFECT) — one quoting regime.
// ---------------------------------------------------------------------------
// The DML/clipboard/filter paths used to wrap identifiers WITHOUT doubling an
// embedded quote character, while the pager path (db::QuoteIdent) doubled
// correctly. A column or table whose name contains a backtick (legal in MySQL,
// as is a double quote in PG) therefore produced malformed — and in principle
// injectable — SQL on the first set of paths and correct SQL on the second.
// These assertions used to demand that disagreement. They now demand that EVERY
// path agrees, on both dialects, on the table name, the column names and the
// filter clause. Kept, not deleted: this was the bug report.
static void TestQuotingIsUnified()
{
    std::printf("-- FIXED: one quoting regime (db::QuoteIdent) --\n");
    const wxString hostile   = L"we`ird";     // backtick — the MySQL delimiter
    const wxString hostilePg = L"we\"ird";    // double quote — the PG delimiter

    ExpectEq("db::QuoteIdent doubles an embedded backtick",
             db::QuoteIdent(hostile, Dialect::MySQL), L"`we``ird`");
    ExpectEq("db::QuoteIdent doubles an embedded double quote (PG)",
             db::QuoteIdent(hostilePg, Dialect::Postgres), L"\"we\"\"ird\"");

    // The assertion that WAS the bug report: same identifier, same dialect, the
    // DML path and the pager path must now agree.
    DmlContext c;
    c.table     = hostile;
    c.dialect   = Dialect::MySQL;
    c.columns   = { L"id" };
    c.pkColumns = { L"id" };
    ExpectEq("FIXED: DML path escapes the table name",
             gs::QualifyForDml(c), L"`we``ird`");
    ExpectEq("pager path escapes the same table name identically",
             gs::QualifyForPaging(Dialect::MySQL, wxString(), hostile), L"`we``ird`");
    ExpectTrue("FIXED: the two paths now agree (the defect is gone)",
               gs::QualifyForDml(c) ==
               gs::QualifyForPaging(Dialect::MySQL, wxString(), hostile));

    // The db qualifier is quoted as a SEPARATE identifier — a hostile db name
    // must not leak a delimiter into the table half of `db`.`table`.
    DmlContext q = c;
    q.db = L"sh`op";
    ExpectEq("both halves of db.table are escaped independently",
             gs::QualifyForDml(q), L"`sh``op`.`we``ird`");

    // Column names on the WRITE path: SET list, WHERE key, and INSERT column
    // list all had the same hole.
    DmlContext w;
    w.table     = L"t";
    w.dialect   = Dialect::MySQL;
    w.columns   = { L"i`d", L"na`me" };
    w.pkColumns = { L"i`d" };
    ExpectEq("FIXED: UPDATE escapes both the SET column and the WHERE key",
             EditSql(w, { { L"1", L"alice" } }, { 0 }, {}, { { L"1", L"bob" } }),
             L"UPDATE `t` SET `na``me`='bob' WHERE `i``d`='1';\n");
    ExpectEq("FIXED: INSERT escapes its column list",
             EditSql(w, {}, { -1 }, {}, { { L"9", L"carol" } }),
             L"INSERT INTO `t` (`i``d`, `na``me`) VALUES ('9', 'carol');\n");
    ExpectEq("FIXED: DELETE escapes the WHERE key",
             EditSql(w, { { L"1", L"alice" } }, {}, { { L"1", L"alice" } }, {}),
             L"DELETE FROM `t` WHERE `i``d`='1';\n");

    // Clipboard statements share the quoter.
    ExpectEq("FIXED: 复制为 INSERT escapes table and columns",
             gs::BuildInsertStatements(w, { { L"1", L"alice" } }),
             L"INSERT INTO `t` (`i``d`, `na``me`) VALUES ('1', 'alice');\n");
    ExpectEq("FIXED: 复制为 UPDATE escapes table, SET column and WHERE key",
             gs::BuildRowDmlStatements(w, { { L"1", L"alice" } }, true),
             L"UPDATE `t` SET `na``me`='alice' WHERE `i``d`='1';\n");
    ExpectEq("FIXED: 复制为 DELETE escapes table and WHERE key",
             gs::BuildRowDmlStatements(w, { { L"1", L"alice" } }, false),
             L"DELETE FROM `t` WHERE `i``d`='1';\n");

    // The cell filter was the third path on the wrong regime.
    ExpectEq("FIXED: cell filter escapes the column name",
             gs::BuildCellFilterClause(Dialect::MySQL, hostile, L"x", false),
             L"`we``ird` = 'x'");
    ExpectEq("FIXED: cell filter escapes the column name on IS NULL too",
             gs::BuildCellFilterClause(Dialect::MySQL, hostile, L"NULL", true),
             L"`we``ird` IS NOT NULL");

    // Same story on Postgres, where the delimiter is the double quote.
    DmlContext p;
    p.table     = hostilePg;
    p.dialect   = Dialect::Postgres;
    p.columns   = { L"i\"d", L"na\"me" };
    p.pkColumns = { L"i\"d" };
    ExpectEq("FIXED: PG DML escapes the table name",
             gs::QualifyForDml(p), L"\"we\"\"ird\"");
    ExpectEq("FIXED: PG UPDATE escapes the SET column and the WHERE key",
             EditSql(p, { { L"1", L"alice" } }, { 0 }, {}, { { L"1", L"bob" } }),
             L"UPDATE \"we\"\"ird\" SET \"na\"\"me\"='bob' WHERE \"i\"\"d\"='1';\n");
    ExpectEq("FIXED: PG cell filter escapes the column name",
             gs::BuildCellFilterClause(Dialect::Postgres, hostilePg, L"x", false),
             L"\"we\"\"ird\" = 'x'");

    // A quote char belonging to the OTHER dialect is data, not a delimiter, and
    // must be left alone rather than doubled.
    ExpectEq("a double quote is not special to MySQL and is not doubled",
             db::QuoteIdent(hostilePg, Dialect::MySQL), L"`we\"ird`");
    ExpectEq("a backtick is not special to PG and is not doubled",
             db::QuoteIdent(hostile, Dialect::Postgres), L"\"we`ird\"");

    // Nothing arrives pre-quoted (see the provenance note in ResultGridSql.h), so
    // an ordinary name must come out quoted EXACTLY ONCE — no double-wrapping.
    ExpectEq("an ordinary name is wrapped exactly once",
             gs::QualifyForDml(Users()), L"`users`");
}

// ---------------------------------------------------------------------------
// The scripted-edit (SWIFTSQL_AUTOEDIT) verdict vocabulary.
//
// This hook is how a live verification run drives the app, so its output is
// EVIDENCE. It used to print `ERR:` whenever the diff came out empty — which is
// what "the value was already equal", "the write was REFUSED because the result
// is not bound to a table", and "nothing was edited" all look like. A refusal
// that reads as a generic error is a refusal nobody can prove happened, and the
// cross-database read-only case was verified through exactly that hole.
// One token per outcome, and no benign outcome shaped like a failure.
// ---------------------------------------------------------------------------
static void TestScriptedEditVerdicts()
{
    auto tok = [](gs::ScriptedEditStatus s, const wxString& detail = wxString()) {
        gs::ScriptedEditResult r; r.status = s; r.detail = detail;
        return gs::ScriptedEditToken(r);
    };
    using S = gs::ScriptedEditStatus;

    ExpectEq("Ok reports OK", tok(S::Ok), L"OK");

    // The three ex-`ERR:` cases, now told apart.
    ExpectEq("an already-equal cell is UNCHANGED, not an error",
             tok(S::NoChange), L"UNCHANGED");
    ExpectEq("an empty result is NO_ROWS, not an error",
             tok(S::NoRows), L"NO_ROWS");
    ExpectEq("a refused bind is REFUSED:NOT_EDITABLE and carries the reason",
             tok(S::NotEditable, L"跨库限定名"), L"REFUSED:NOT_EDITABLE:跨库限定名");
    ExpectEq("a refused bind with no stated reason still reports the refusal",
             tok(S::NotEditable), L"REFUSED:NOT_EDITABLE");

    // The PK gate's two verdicts stay distinguishable from each other AND from
    // the bind refusal — they mean different things to whoever reads the run.
    ExpectEq("no primary key at all", tok(S::PkRefused), L"REFUSED:NO_PRIMARY_KEY");
    ExpectEq("an incomplete composite key names the missing columns",
             tok(S::PkRefused, L"tenant_id,id"), L"REFUSED:INCOMPLETE_KEY:tenant_id,id");

    // The property the whole vocabulary exists to guarantee: no two outcomes
    // share a token, so the file never has to be guessed at.
    const wxString all[] = {
        tok(S::Ok), tok(S::NoChange), tok(S::NoRows), tok(S::NoSuchColumn, L"name"),
        tok(S::NotEditable, L"r"), tok(S::PkRefused), tok(S::PkRefused, L"id"),
    };
    bool distinct = true;
    for (size_t i = 0; i < std::size(all); ++i)
        for (size_t j = i + 1; j < std::size(all); ++j)
            if (all[i] == all[j]) distinct = false;
    ExpectEq("every outcome has its own token", distinct ? L"yes" : L"no", L"yes");

    // A refusal must never be reported with a leading OK, however it is parsed:
    // graders grep the first token.
    for (const wxString& t : all)
        if (t != L"OK")
            ExpectEq("no non-Ok outcome starts with OK",
                     t.StartsWith(L"OK") ? L"bad" : L"good", L"good");
}

int main()
{
    TestPrimitives();
    TestEditDml();
    TestPendingEditsAreVisibleWithoutAKey();
    TestClipboardStatements();
    TestPagingAndFilter();
    TestQuotingIsUnified();
    TestScriptedEditVerdicts();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
