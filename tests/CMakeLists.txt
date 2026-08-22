# swiftsql tests — offline, self-contained unit tests for pure logic.
#
# SplitSqlScript is a pure, dialect-aware splitter (no I/O, no drivers, no GUI),
# so we compile its single translation unit directly into the test binary and
# link only swiftsql::core — which supplies wxString, the src/ include root, and
# the db::Dialect enum. No vendor DB client libs (libmariadb/libpq/…) are pulled
# in, keeping the test target tiny and fast to build/run.
add_executable(sqlscript_test
    sqlscript_test.cpp
    ${CMAKE_SOURCE_DIR}/src/db/SqlScript.cpp
)
target_link_libraries(sqlscript_test PRIVATE swiftsql::core)

add_test(NAME sqlscript_test COMMAND sqlscript_test)

# group_store_test covers core::GroupStore — the ordered group list + member→group
# map behind the sidebar's 分组管理. It links swiftsql::core (wxFileConfig lives
# behind it) and exercises REAL file persistence, redirected into the temp dir via
# GroupStore::UseFile so the developer's own groups.ini is never touched.
add_executable(group_store_test group_store_test.cpp)
target_link_libraries(group_store_test PRIVATE swiftsql::core)

add_test(NAME group_store_test COMMAND group_store_test)

# ghost_suggest_test covers ui::ghost::LocalSkeleton — the LOCAL tier of the SQL
# editor's grey inline suggestion. Pure text-in/text-out (no wxSTC, no
# connection), so its single TU is compiled straight in and only swiftsql::core
# is linked, exactly like sqlscript_test. The widget half (painting, Tab accept)
# is not covered here — this is the decision layer.
add_executable(ghost_suggest_test
    ghost_suggest_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/GhostSuggest.cpp
)
target_link_libraries(ghost_suggest_test PRIVATE swiftsql::core)

add_test(NAME ghost_suggest_test COMMAND ghost_suggest_test)

# tableio_test covers db::TableExport (five P0 format writers) + db::TableImport
# (four P0 parsers) + export→import round-trips. Unlike sqlscript_test it links
# the whole swiftsql::db library rather than compiling one TU, because the SQL
# writer reuses db::RenderLiteral (SqlLiteral.cpp) and the parsers live inside
# swiftsql::db. Pure logic still — no driver/connection is exercised.
add_executable(tableio_test tableio_test.cpp)
target_link_libraries(tableio_test PRIVATE swiftsql::db)

add_test(NAME tableio_test COMMAND tableio_test)

# swiftsql_tests covers the BINARY export writers (db::XlsxWriter → OOXML/ZIP,
# db::DbfWriter → dBASE III) that the text-only tableio_test does not touch,
# plus a self-contained set of CSV/JSON/XML export→import round-trips. It links
# swiftsql::db (writers live there) and reads bytes back through wxZipInputStream
# / wxFileInputStream — no wxApp, no driver, headless-safe.
add_executable(swiftsql_tests swiftsql_tests.cpp)
target_link_libraries(swiftsql_tests PRIVATE swiftsql::db)

add_test(NAME swiftsql_tests COMMAND swiftsql_tests)

# dialect_profile_test is a golden-DDL fitness function over the five ADR-014
# DialectProfile implementations (MySql/Pg/Sqlite/SqlServer/Oracle). It pulls each
# profile from db::GetDialectProfile(DbType), builds db::TableEdit scenarios, and
# asserts RenderAlter's emitted statements byte-for-byte against independently
# hand-authored "correct DDL" for each dialect (ADD/MODIFY/rename/DROP, precision,
# dialect attrs, SQLite honest-skip). Pure rendering — RenderAlter never touches a
# connection — so it links only swiftsql::db (profiles + wxString live there) and
# runs headless in CI.
add_executable(dialect_profile_test dialect_profile_test.cpp dialect_profile_test_ext.cpp)
target_link_libraries(dialect_profile_test PRIVATE swiftsql::db)

add_test(NAME dialect_profile_test COMMAND dialect_profile_test)

# sync_schema_test / sync_data_test / sync_exec_test / sync_plan_test cover the
# cross-database synchronization suite offline. They were ONE target, sync_test,
# whose single TU reached the charter's 1000-line ceiling at 991 lines — the same
# preventive split as ResultGridSql/ResultGridModel and EditorSqlFormat/
# EditorCompletion, done before someone was blocked rather than after.
#
# The seam is SUBJECT, not line count, and deliberately not the mechanical
# halving that produced ResultGridPanelOps.cpp (which grew back over the limit
# because neither half was about anything). Each target answers "what is this
# file for?" in one phrase, so a new test has an obvious home:
#
#   sync_schema_test  db::sync::DiffSchema — the pure structural diff over
#                     TableSchema values. No connection is constructed at all.
#   sync_data_test    the COMPARE pass over rows: DiffAndEmitData (statement
#                     text) and DiffRowChanges (structured RowChanges). READS
#                     ONLY.
#   sync_exec_test    the WRITE pass: ExecuteDataSync and the delete gate. Its
#                     assertions are about what was SENT — several of them
#                     NEGATIVE ("no DELETE reached the target"), which is why it
#                     is worth its own red line in ctest output.
#   sync_plan_test    SyncEngine::BuildPlan — FK topological order, the MySQL
#                     FK-check preamble/postamble, the cross-engine structure
#                     gate, and each unit's DataVerdict.
#
# That compare/write split is the same vocabulary the live suites already use
# (mysql_pg_live_test proves the compare pass and never writes;
# mysql_pg_live_exec_test is the write half). One assertion moved during the
# split: the ColumnChange "current definition" snapshot, which lived among the
# plan verdicts but calls DiffSchema directly and builds no plan, is now in
# sync_schema_test. The assertion total across the four is no longer claimed
# here: this comment asserted "unchanged at 119" and had silently drifted to 126
# by the time the assertion-census lint was built, which is precisely the defect
# that motivated the lint. The per-assertion census in tests/lint/census/ owns
# that claim now, because it is checked on every run instead of being remembered.
#
# Shared harness — the ExpectTrue/ExpectStr/ExpectEq assert loop, the TableSchema
# builders, and the canned-row StubConn / statement-recording RecordingConn —
# lives in sync_stub.h so a stub cannot drift between the suite that reads it and
# the suite that writes through it (same pattern as mysql_pg_live.h). All four
# link swiftsql::db (SchemaDiff/DataSync/SyncEngine live there); no driver or
# connection is exercised — the stub supplies canned schema + rows.
add_executable(sync_schema_test sync_schema_test.cpp)
target_link_libraries(sync_schema_test PRIVATE swiftsql::db)

add_test(NAME sync_schema_test COMMAND sync_schema_test)

add_executable(sync_data_test sync_data_test.cpp)
target_link_libraries(sync_data_test PRIVATE swiftsql::db)

add_test(NAME sync_data_test COMMAND sync_data_test)

add_executable(sync_exec_test sync_exec_test.cpp)
target_link_libraries(sync_exec_test PRIVATE swiftsql::db)

add_test(NAME sync_exec_test COMMAND sync_exec_test)

add_executable(sync_plan_test sync_plan_test.cpp)
target_link_libraries(sync_plan_test PRIVATE swiftsql::db)

add_test(NAME sync_plan_test COMMAND sync_plan_test)

# ddl_test covers the *real* DDL text generation that sync_test deliberately
# stubs out: MySqlConnection/PgConnection::RenderSchemaChange (driving the
# db::detail helpers in MySqlSql.cpp / PgSql.cpp) exercised through *unconnected*
# driver instances — RenderSchemaChange is pure rendering and never touches the
# wire handle — plus db::RenderLiteral (SqlLiteral.cpp) across every dialect ×
# CellKind and db::LimitOffsetClause. Links swiftsql::db (drivers + helpers live
# there); no driver/connection is exercised.
add_executable(ddl_test ddl_test.cpp)
target_link_libraries(ddl_test PRIVATE swiftsql::db)

add_test(NAME ddl_test COMMAND ddl_test)

# SyncTypeMapTests is A1 of the cross-engine sync feature: standalone unit tests
# for db::SyncTypeMap (CanonicalType/TypeVerdict, MySQL<->PostgreSQL Interpret/
# Render, CompareCanonical), exercised BEFORE it is wired into DialectProfile
# (A3) or SchemaDelta/SyncEngine (A4). Pure value types + pure functions — links
# swiftsql::db, no driver/connection is exercised.
add_executable(SyncTypeMapTests SyncTypeMapTests.cpp)
target_link_libraries(SyncTypeMapTests PRIVATE swiftsql::db)

add_test(NAME SyncTypeMapTests COMMAND SyncTypeMapTests)

# SyncValueMapTests covers ADR-015 T1 (db::sync::SyncValueMap — ValueVerdict/
# MayEmit, BuildBridges' once-per-table conversion plan, ConvertCell's per-value
# rulings: MySQL zero-date, tinyint(1)->boolean, per-VALUE unsigned overflow,
# binary/bytea reframing, NULL-vs-empty-string preservation, invalid UTF-8) and
# T3 (db::sync::RowChange/RowChangeBuilder/RowChangeSet + RenderRowDml —
# including the structural proof that an Unrepresentable cell cannot produce a
# RowChange at all). Same standalone pattern as SyncTypeMapTests: pure value
# types + free functions, links swiftsql::db, no driver/connection/GUI.
add_executable(SyncValueMapTests SyncValueMapTests.cpp)
target_link_libraries(SyncValueMapTests PRIVATE swiftsql::db)

add_test(NAME SyncValueMapTests COMMAND SyncValueMapTests)

# SchemaDiffGoldenTests is WP0 for the cross-engine sync feature: the safety
# net that MUST exist and pass before anyone touches Postgres schema
# introspection or type mapping. It pins two things from CURRENT behavior: (1)
# PG->PG rendered CREATE TABLE/ALTER COLUMN DDL byte-for-byte for the column
# types the upcoming type-mapping fix touches (bigint/smallint/timestamptz/
# numeric(10,2)/varchar(80) via PgTypeText/PgColumnDef in PgSql.cpp), using
# hand-built offline NormColumn fixtures (kind=ColKind::Other, matching what
# PgConnection::GetTableSchema actually produces today) driven through an
# *unconnected* Postgres IConnection — same pure-rendering trick ddl_test.cpp
# already relies on; and (2) today's db::sync::DiffSchema cross-engine
# skip-and-warn shape (exact warning string) when a MySQL-known-kind column is
# diffed against a PG column whose kind is Other. Deliberately does NOT use
# pg_live_test's live-connection path, which is env-gated and silently skips
# without SWIFTSQL_PGTEST_* env vars — that would make this "safety net" fake.
# Links swiftsql::db (drivers + SchemaDiff live there); no driver/connection
# is exercised.
add_executable(SchemaDiffGoldenTests SchemaDiffGoldenTests.cpp)
target_link_libraries(SchemaDiffGoldenTests PRIVATE swiftsql::db)

add_test(NAME SchemaDiffGoldenTests COMMAND SchemaDiffGoldenTests)

# SyncDiffModelTests is T6 of the diff-review UI: standalone unit tests for
# ui::BuildDiffTree (src/ui/SyncDiffModel.{h,cpp}), the pure/non-wx tree model
# the compare grid (T8, SyncCompareGrid) renders. Covers a
# SyncPlan with 2-3 mixed-change tables (create/structure/data), confirms
# stable Table-node ids (== SyncPlan::TableUnit::table) survive the build, and
# confirms a table-missing warning (a Finding with no TableUnit — the bug a
# concurrent track just fixed upstream) surfaces as its own tree node rather
# than vanishing. Compiles SyncDiffModel.cpp as a standalone TU (mirroring
# mermaid_test.cpp's pattern) rather than linking the whole swiftsql_ui
# library, since the model itself pulls in nothing beyond swiftsql::db
# (SyncEngine.h/SchemaModel.h) + wxString — no wxWindow, no GUI.
add_executable(SyncDiffModelTests
    SyncDiffModelTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncDiffModel.cpp
)
target_link_libraries(SyncDiffModelTests PRIVATE swiftsql::db)

add_test(NAME SyncDiffModelTests COMMAND SyncDiffModelTests)

# SyncSelectionTests is T6 of ADR-015: standalone unit tests for
# ui::SyncSelection (src/ui/SyncSelection.{h,cpp}), the pure/non-wx model of
# the user's check-state on the data-sync compare screen (the grid itself is a
# separate track). The headline case is reorder-invariance: an asymmetric
# selection must survive reversing/rotating/permuting the underlying compare
# collection, which is the regression test for Round 1's position-keyed
# selection bug (`plan_.units[i]` by list-row index -> wrong table's DDL once a
# categorized tree interleaved header rows). Also pins the delete double-gate
# (deletes are unrepresentable in the execution spec while the global "allow
# deleting extra rows" master switch is off, no matter what is checked),
# structural exclusion of non-executable RowChangeSets, and row-level selection
# being offered only for non-truncated diffs. Compiles SyncSelection.cpp as a
# standalone TU (mirroring mermaid_test.cpp) and links only swiftsql::core —
# the model depends on nothing but wxString: no db types, no wxWindow/GUI.
#
# That core-only link is itself a FITNESS FUNCTION, not an optimization: if
# anyone ever pulls a db or wx-widget type into SyncSelection.{h,cpp}, this
# target stops linking. Keep it on swiftsql::core. Data-layer integration is
# tested separately by SyncSelectionAdapterTests below.
# QualifiedNameTests pins db::QualifiedName, the schema-qualified table identity
# that replaced a bare table-name string across the sync suite. Three properties
# matter and all three are load-bearing elsewhere: Key()/Parse() must be exact
# inverses (a key produced in the plan is resolved back by the PG DDL renderer);
# a MySQL key must stay byte-identical to the bare name it replaced; and two
# same-named tables in different PostgreSQL schemas must never compare equal.
# Header-only type, so this links nothing but wxWidgets via swiftsql::core.
# mysql_pg_live_schema_test is the LIVE proof that schema-qualified table
# identity actually works against a real PostgreSQL server -- the round this
# defect was finally closable, because it needs two same-named tables in two
# schemas and that had never been possible to set up before.
#
# It was written as a CHARACTERIZATION probe first. Against the pre-fix build,
# on PostgreSQL 15, it observed: ListTables returning the string "orders" twice
# with nothing to tell them apart; GetTableSchema("orders") silently answering
# with public's columns; and -- the dangerous one -- GetTableSchema on a table
# living only in a non-public schema returning TRUE WITH ZERO COLUMNS, which is
# DiffSchema's convention for "this table does not exist". A table the picker
# had just listed was introspected as absent. That was never latent.
#
# The five probes now assert the fixed behaviour end to end: the catalog listing
# distinguishes the two, introspection returns each table's own columns, row
# streaming reads the schema it was asked for, a single SyncPlan carries TWO
# units with distinct keys, and a DIFFERENTLY-named source/target pair really
# reads the target it was pointed at (the read/write asymmetry).
#
# Same safety contract as the other live targets: nothing hard-coded, skips
# cleanly without SWIFTSQL_PGTEST_*, and only ever touches the throwaway
#  database, dropped on the way in AND out.
add_executable(mysql_pg_live_schema_test mysql_pg_live_schema.cpp)
target_link_libraries(mysql_pg_live_schema_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_schema_test COMMAND mysql_pg_live_schema_test)

add_executable(QualifiedNameTests QualifiedNameTests.cpp)
target_link_libraries(QualifiedNameTests PRIVATE swiftsql::core)

add_test(NAME QualifiedNameTests COMMAND QualifiedNameTests)

add_executable(SyncSelectionTests
    SyncSelectionTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncSelection.cpp
)
target_link_libraries(SyncSelectionTests PRIVATE swiftsql::core)

add_test(NAME SyncSelectionTests COMMAND SyncSelectionTests)

# SyncSelectionAdapterTests is the integration half of T6: ui::MakeTableDiff
# (src/ui/SyncSelectionAdapter.h, header-only) mapping REAL db::sync::
# RowChangeSet values (src/db/RowChange.h, T3) into the selection model, then
# driving SyncSelection end-to-end. Unlike SyncSelectionTests it links
# swiftsql::db, because the adapter is the one file in this component that is
# allowed to know the data layer exists. Pins, on real data-layer objects
# rather than hand-set booleans: RowChangeSet::Executable() (derived, no path
# back to true) making a blocked table unplannable even with every box
# force-checked, RowChangeSet::Truncated() suppressing row-level selection, and
# the delete double-gate on a real delete-bearing set. Rows are built through
# the real RowChangeBuilder gate with passthrough ColumnBridges — value
# conversion itself is SyncValueMapTests' subject, not this one's. No
# driver/connection/GUI is exercised.
add_executable(SyncSelectionAdapterTests
    SyncSelectionAdapterTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncSelection.cpp
)
target_link_libraries(SyncSelectionAdapterTests PRIVATE swiftsql::db)

add_test(NAME SyncSelectionAdapterTests COMMAND SyncSelectionAdapterTests)

# SyncCrossEngineGateTests is a table-driven regression test for
# ui::CrossEngineSupported (src/ui/SyncCrossEngineGate.{h,cpp}) -- the
# wizard-entry cross-engine dialect-pair gate, extracted from
# SyncWizardDialog.cpp's former anonymous-namespace definition (QA-scope
# exception, see that header's comment) specifically so it is directly
# linkable/testable without pulling in the rest of the wizard dialog's wx
# GUI dependency chain. Links only swiftsql::db (db::Dialect lives in
# db/DbDriver.h); compiles SyncCrossEngineGate.cpp as a standalone TU,
# mirroring SyncDiffModelTests' pattern above. No wxWindow/GUI is exercised.
add_executable(SyncCrossEngineGateTests
    SyncCrossEngineGateTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncCrossEngineGate.cpp
)
target_link_libraries(SyncCrossEngineGateTests PRIVATE swiftsql::db)

add_test(NAME SyncCrossEngineGateTests COMMAND SyncCrossEngineGateTests)

# SyncCompareRowTests is T8 of ADR-015: the pure half of the single-screen
# compare page (src/ui/SyncCompareRow.h, header-only) -- row classification
# (类型 / 状态), the SyncPlan -> CompareResult adapter, and above all
# FilterPlanBySpec, which turns a ui::ExecutionSpec back into the
# db::sync::SyncPlan that SyncEngine::Execute runs. That last one is the final
# gate before statements reach a live target -- notably "the delete master
# switch is off" must mean "no DELETE is in the plan" -- so it is asserted
# headlessly here rather than only exercised by clicking through the wizard.
# Compiles SyncSelection.cpp directly (same standalone pattern as
# SyncSelectionTests) and links swiftsql::db, which PUBLIC-links swiftsql::core
# for the core::Lang lookup behind ui::tr. No wxWindow/GUI is exercised:
# SyncCompareRow.h is deliberately widget-free so this test can exist at all --
# the wxDataViewCtrl half lives in SyncCompareGrid.cpp and is not linked here.
add_executable(SyncCompareRowTests
    SyncCompareRowTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncSelection.cpp
)
target_link_libraries(SyncCompareRowTests PRIVATE swiftsql::db)

add_test(NAME SyncCompareRowTests COMMAND SyncCompareRowTests)

# SyncRowSelectionTests covers the two functional gaps closed on the compare
# screen: an UPDATE's real before/after rendering (db::sync::RowChange::
# PriorValues reaching ui::AppendSampleRows) and per-ROW selection actually
# reaching a db::sync::DataExecPlan.
#
# It compiles BOTH pure models -- SyncDiffModel.cpp and SyncSelection.cpp --
# because the behaviour under test is the CHAIN between them: a row identity
# produced by the data layer, carried on a DiffNode, stored in SyncSelection,
# and emitted as a TableDataSpec exclusion. SyncDiffModelTests links only the
# first and SyncCompareRowTests only the second, so neither can assert that the
# key survives end to end -- and "the key silently changed on the way through"
# is exactly the failure mode (a re-encoded cross-engine key misses, and a row
# the user unchecked syncs anyway). Links swiftsql::db for DiffRowChanges and
# the row carrier; no wxWindow/GUI is exercised.
add_executable(SyncRowSelectionTests
    SyncRowSelectionTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncDiffModel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncSelection.cpp
)
target_link_libraries(SyncRowSelectionTests PRIVATE swiftsql::db)

add_test(NAME SyncRowSelectionTests COMMAND SyncRowSelectionTests)

# SyncWizardSpecTests closes the last layer of the sync feature that was
# verified only by argument: the compare SCREEN's own construction of what will
# execute (dialog state -> ui::ExecutionSpec -> db::sync::DataExecPlan).
#
# Its three neighbours above all hand-build their inputs and none of them
# touches the object the running dialog owns, so all three would stay green if
# SyncComparePage stopped seeding its selection from the plan, stopped
# mirroring the delete master switch, or started answering 执行 from a second
# copy of the state. This target drives ui::SyncCompareSession -- the model half
# SyncComparePage now delegates to -- through ui::syncspec::RunSpecScript, which
# is the SAME entry point the SWIFTSQL_SYNCSPEC hook in SyncWizardDialog.cpp
# calls on the live page. No re-implementation sits between the assertions and
# the code the wizard runs.
#
# SCOPE: this is the BACKEND CONTRACT REACHABLE FROM THE GUI SEAT. It creates no
# window and asserts nothing about rendering -- a green run is not evidence the
# compare screen draws. See src/ui/SyncSpecScript.h.
#
# Offline: a canned-row IConnection stub feeds the REAL DiffRowChanges merge, so
# the RowChangeSets under test are the data layer's product rather than hand-set
# structs. Links swiftsql::db; no wxApp, no driver, no server.
add_executable(SyncWizardSpecTests
    SyncWizardSpecTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncCompareSession.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncDiffModel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncSelection.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncSpecScript.cpp
)
target_link_libraries(SyncWizardSpecTests PRIVATE swiftsql::db)

add_test(NAME SyncWizardSpecTests COMMAND SyncWizardSpecTests)

# SyncCompareReentryTest is the REGRESSION test for the P0 compare->display
# crash. ui::SyncCompareGrid populates its wxDataViewCtrl by calling
# wxDataViewModel::ItemChanged() to repaint each top-level row; on the generic
# (MSW) control that SYNCHRONOUSLY emits wxEVT_DATAVIEW_ITEM_VALUE_CHANGED -- the
# same event a user's checkbox edit produces -- which the grid handled by calling
# RefreshFromSelection again, which fired ItemChanged again: unbounded recursion
# that stack-overflowed the instant the grid was first populated (the "对比之后
# 崩溃" the user hit). It could not be caught by any headless model test because
# it lives entirely in the widget's event loop.
#
# This test constructs the REAL ui::SyncComparePage and calls SetPlan() with a
# canned plan (the recursion is data-independent -- any non-empty plan populates
# a root and starts the loop). Before the fix SetPlan never returns and the
# process dies; after the fix it exits 0. No server and no visible window (the
# frame is never Show()n -- the model notification that starts the loop fires
# regardless of visibility). Links swiftsql::ui; needs a wxApp window station.
add_executable(SyncCompareReentryTest SyncCompareReentryTest.cpp)
target_link_libraries(SyncCompareReentryTest PRIVATE swiftsql::ui)

add_test(NAME SyncCompareReentryTest COMMAND SyncCompareReentryTest)

# sync_compare_widget_live is the env-gated LIVE GUI verification driver used to
# OBSERVE the crash and prove the fix against a real server: it builds a plan
# from live MySQL (SyncEngine::BuildPlan + CompareRoutines), creates the REAL
# wxDataViewCtrl (ui::SyncComparePage), calls SetPlan, paints, re-compares and
# destroys -- the "full compare against the live server with no human" render.
# Not a ctest (needs live env + a display); build + run manually. Env-gated and
# self-cleaning (two throwaway swiftsql_p0w_* databases dropped in and out).
add_executable(sync_compare_widget_live sync_compare_widget_live.cpp)
target_link_libraries(sync_compare_widget_live PRIVATE swiftsql::ui)

# pg_live_test is an env-gated live PostgreSQL integration test. Without the
# SWIFTSQL_PGTEST_HOST/USER/PASS env vars it prints "SKIP" and exits 0, so it is
# harmless in CI. When configured it drives real PgConnection introspection
# (Connect/Execute/GetTableSchema/GetPrimaryKey) against two disposable test
# databases and asserts DiffSchema results. Links swiftsql::db (pulls in libpq
# transitively via the driver).
add_executable(pg_live_test pg_live_test.cpp)
target_link_libraries(pg_live_test PRIVATE swiftsql::db)

add_test(NAME pg_live_test COMMAND pg_live_test)

# sqlite_live_test is a self-contained, zero-config live SQLite integration test.
# SQLite is embedded/file-based (no server, no network, no env), so unlike
# pg_live_test it runs unconditionally in CI against a throwaway db file in the
# system temp dir. It drives real SqliteConnection introspection + query paths
# (Connect/Execute/GetColumns/GetPrimaryKey/GetTableSchema/GetIndexes). Because
# SqliteConnection::Connect opens READWRITE-only (no CREATE), the test bootstraps
# a valid empty database file with the raw sqlite3 C API first — so this target
# links unofficial::sqlite3::sqlite3 directly (mirroring src/db's PRIVATE link),
# in addition to swiftsql::db which supplies the driver + sync suite.
add_executable(sqlite_live_test sqlite_live_test.cpp)
target_link_libraries(sqlite_live_test PRIVATE swiftsql::db unofficial::sqlite3::sqlite3)

add_test(NAME sqlite_live_test COMMAND sqlite_live_test)

# mysql_pg_live_test is the env-gated LIVE CROSS-ENGINE integration test: a real
# MySQL server as source, a real PostgreSQL server as target. Without the
# SWIFTSQL_MYTEST_* AND SWIFTSQL_PGTEST_* env vars it prints "SKIP" and exits 0,
# exactly like pg_live_test, so it is harmless in CI.
#
# It exists because the cross-database sync suite shipped three rounds of work
# with ZERO observed runtime behaviour: six silent data-corrupting defects were
# found by construction-time reasoning alone, and every fix rests on an
# assumption about what a real server returns that had never been checked. The
# offline suites (DataSyncOrderingTests, SyncCrossEngineCompareTests,
# SyncValueMapTests, RoutineCompareTests) pin the LOGIC against simulated
# servers; this target is the only place the logic meets a real MySQL collation,
# a real tinyint(1), a real information_schema privilege boundary.
#
# One hazard per defect, each asserting the expected result AND printing what
# the servers actually returned (OBS lines): text-PK collation ordering (the
# 10-spurious-INSERT desync), tinyint(1) vs boolean coercion, temporal text
# forms + a refused TIMESTAMP primary key, MySQL zero-dates, per-VALUE unsigned
# overflow, no-PK exclusion, and the routine compare. Split across two TUs
# (hazards 1-6 / hazard 7) with a shared harness header, to stay under the
# charter's 1000-line-per-file ceiling. Links swiftsql::db, which pulls in both
# libmariadb and libpq transitively via the two drivers.
add_executable(mysql_pg_live_test
    mysql_pg_live_test.cpp
    mysql_pg_live_routines.cpp
)
target_link_libraries(mysql_pg_live_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_test COMMAND mysql_pg_live_test)

# mysql_pg_live_exec_test is the WRITE half of the same story, and the reason
# data sync was held at Beta. mysql_pg_live_test above proves the COMPARE pass
# against real servers; it never writes. db::sync::SyncEngine::Execute,
# db::sync::ExecuteDataSync and IConnection::ExecuteBatch -- the only components
# in this program that put bytes into a customer's database -- had never run
# against a real server at all, so every claim about them ("inserts land",
# "500 rows per batch", "a failing table rolls back", "the delete master switch
# means no DELETE reaches the target") was an argument about source code rather
# than an observation.
#
# Its standing rule, and the reason it is worth its runtime: every assertion
# READS THE TARGET BACK and compares actual contents. "Execute() returned true"
# is never accepted as evidence -- wrote-nothing, wrote-once-correctly,
# wrote-twice and wrote-the-wrong-values all return true, and only the target's
# own rows tell them apart.
#
# Items A-H: the basic write's correctness cross-engine AND same-engine
# (MySQL->PG, MySQL->MySQL, PG->PG); ExecuteBatch across the 500-row batch
# boundary with coerced values (TINYINT(1)->boolean, binary round-trip,
# NULL-vs-empty-string); per-table transactions and partial failure incl. the
# MySQL DDL auto-commit interaction; the two-pass consistency window; selection
# filtering asserted at the DATABASE rather than in the plan; unconvertible
# values refused at the target; identity/sequence repair proved by performing
# the next plain INSERT; and a 3000-row run where the materialization cap,
# Truncated() and intra-table progress all engage.
#
# Same env gating as the two live targets above (SKIP + exit 0 when unset) and
# the same self-provisioning discipline, under its own `swiftsql_xexec_` prefix
# so it can never collide with the compare suite's `swiftsql_xsync_` databases.
# Split across two TUs to stay under the charter's 1000-line ceiling. Links
# swiftsql::db, which pulls in libmariadb and libpq via the two drivers.
add_executable(mysql_pg_live_exec_test
    mysql_pg_live_exec.cpp
    mysql_pg_live_exec2.cpp
    mysql_pg_live_exec3.cpp
)
target_link_libraries(mysql_pg_live_exec_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_exec_test COMMAND mysql_pg_live_exec_test)

# mysql_pg_live_exec4_test closes the four gaps the engineers who ran the suite
# above named as the highest-value things still unobserved. Each needs a fixture
# mysql_pg_live_exec_test structurally cannot express, which is why it is a
# separate target rather than four more items there:
#
#   1. WRITING into a table outside `public`. The qualified-name work proved the
#      READ half (compare/plan/stream tell public.orders from archive.orders);
#      no test had ever WRITTEN into archive.orders, and a write landing in the
#      wrong schema is the exact failure that change exists to prevent. Needs
#      two same-named tables in two schemas; asserts on BOTH after every write,
#      because "archive got the rows" and "public was untouched" are two claims.
#   2. CANCELLING mid-write. The stop token had never been tripped during an
#      actual write. Needs a table spanning several 500-row batches plus a
#      second, already-committed table to prove the per-table transaction
#      boundary, and it checks the ROLLBACK at the database rather than trusting
#      the report.
#   3. LOSING THE CONNECTION mid-batch (pg_terminate_backend against the
#      worker's own backend, fired from the executor's progress callback so it
#      lands at a known point in the stream, with no sleep anywhere).
#   4. FK ORDERING at execute time. Every fixture in the suite above is FK-FREE,
#      so the two sweeps' ordering had never been validated against a real
#      foreign key at all — not even same-schema. Both halves are covered, and
#      each runs a CONTROL in deliberately wrong order that must FAIL, because a
#      green result against a fixture that would also pass wrong-ordered is not
#      evidence of anything.
#
# PostgreSQL target throughout: items 1 and 4b need a schema layer, and item 4a
# deliberately avoids a MySQL target because BuildPlan emits
# SET FOREIGN_KEY_CHECKS=0 for one, which would make both orders pass.
#
# Same env gating as the live targets above (SKIP + exit 0 when unset), under
# its own `swiftsql_xexec4_` prefix so it can never collide with them. Split
# across two TUs to stay under the charter's 1000-line ceiling.
add_executable(mysql_pg_live_exec4_test
    mysql_pg_live_exec4.cpp
    mysql_pg_live_exec5.cpp
)
target_link_libraries(mysql_pg_live_exec4_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_exec4_test COMMAND mysql_pg_live_exec4_test)

# mysql_pg_live_scale_test MEASURES the claims the two-pass streaming design was
# built on, at a size that actually engages them. Every suite above runs on
# 2-3000 row tables (item H, the largest, is 3000), and the design's three
# load-bearing claims — compare materializes a bounded 500-row sample plus EXACT
# counts, execute streams into 500-row batches, reads are memory-flat via the
# bounded-queue merge — are all indistinguishable from a quadratic
# implementation at that size. This target seeds 500k rows and reads real
# process memory (GetProcessMemoryInfo, sampled on a polling thread so each
# phase gets its own peak rather than the process-lifetime high-water mark).
#
# The measurement that establishes flatness is a PAIR: a small diff and a large
# diff over the SAME table, small first, so that only the diff size varies and
# no earlier phase's retained allocator arena can absorb a later one. It also
# pins cancel LATENCY on a large write, the merge's intra-table progress cadence,
# and what one per-table transaction costs each engine (PostgreSQL WAL bytes;
# MySQL undo, via trx_rows_modified watched from a second connection, in the one
# phase that writes INTO MySQL).
#
# GATED TWICE, DELIBERATELY. Its siblings skip when credentials are unset, which
# is enough for a suite that takes seconds. This one takes MINUTES and puts a
# real load on a shared server, so credentials alone must not trigger it: anyone
# working on sync has them set, and every ctest run would be dominated by this.
# It additionally requires SWIFTSQL_SCALETEST=1 and skips (exit 0) otherwise, so
# a normal `ctest` is unaffected. SWIFTSQL_SCALE_ROWS overrides the row count for
# smoke runs. Own `swiftsql_xscale_` prefix; split across three TUs for the
# charter's 1000-line ceiling. psapi is for GetProcessMemoryInfo.
#
# IT CURRENTLY FAILS ITS LAST PHASE, ON PURPOSE. Phase D found a real defect —
# the execute pass streams the target side of the diff over the same connection
# it writes through, so an UPDATE/DELETE emitted before that stream reaches
# end-of-data collides with it ("Commands out of sync" on MySQL; a dead
# connection or a segfault on PostgreSQL). Fixing it is a design decision (the
# target read needs its own connection), so the assertions are left stating what
# DataSyncExec.h promises rather than relaxed to match current behavior. The
# opt-in gate is what keeps a normal ctest run green.
add_executable(mysql_pg_live_scale_test
    mysql_pg_live_scale.cpp
    mysql_pg_live_scale2.cpp
    mysql_pg_live_scale3.cpp
)
target_link_libraries(mysql_pg_live_scale_test PRIVATE swiftsql::db psapi)

add_test(NAME mysql_pg_live_scale_test COMMAND mysql_pg_live_scale_test)

# mysql_pg_live_fkrestore_test pins ONE defect the suites above could not have
# caught, because it is invisible in the target's DATA: for a MySQL target,
# BuildPlan emits `SET FOREIGN_KEY_CHECKS=0` as a preamble and `=1` as a
# postamble, and Execute() never collected the postamble at all — nothing in
# src/ executed it. Every sync against MySQL therefore turned foreign-key
# enforcement OFF on the target connection and left it off. On the automation
# path that connection is the ConnectionTree's own long-lived entry, so the
# user's subsequent ad-hoc SQL ran unconstrained.
#
# Session state is exactly what a mock cannot model: it is per-connection, it is
# NOT transactional (a ROLLBACK does not restore it), and its only observable
# effect is on the server's own constraint checking. So every assertion reads
# `SELECT @@SESSION.foreign_key_checks` back from the server on the connection
# the engine ran on, and the decisive check is behavioural — an FK-violating
# INSERT must actually be REFUSED afterwards.
#
# Three cases, because Execute() reaches them by three different returns and the
# old trailing-statement shape would have skipped two: SUCCESS, FAILURE (a
# rejected statement, after a ROLLBACK) and CANCELLATION (the stop token,
# likewise after a ROLLBACK). The cancellation case also reads the variable from
# inside the progress callback while the run is in flight, so the suite cannot
# be satisfied by a build that "restores" correctly by never relaxing at all;
# the success case proves the same thing functionally by writing child rows
# whose parent does not exist.
#
# MySQL on BOTH sides — the preamble/postamble pair only exists for a MySQL
# target. Same env gating as the live targets above (SKIP + exit 0 when unset),
# under its own `swiftsql_xfkr_` prefix so it can never collide with them.
add_executable(mysql_pg_live_fkrestore_test mysql_pg_live_fkrestore.cpp)
target_link_libraries(mysql_pg_live_fkrestore_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_fkrestore_test COMMAND mysql_pg_live_fkrestore_test)

# mysql_pg_live_autocreate_test is the LIVE proof for cross-engine auto-CREATE-
# TABLE (A5): a table present on the SOURCE and entirely MISSING on the target is
# now created on the target in the target's OWN dialect, then populated. The
# translation reuses the same per-column Interpret(src)->Render(tgt) path the
# column-level Add/Modify uses (SchemaDiff.cpp), so a green result here proves the
# whole-table path emits target-native DDL — but only if it is checked against a
# real catalog, which is exactly what no offline test can do. Every assertion
# reads the TARGET CATALOG back (information_schema / pg_indexes): the columns,
# their translated types, the carried index, the identity attribute and the row
# count. Proof C confirms the honest-refusal path by asserting the Unmappable
# table is ABSENT from the catalog, not by trusting the plan. Same env gating and
# `swiftsql_xcreate_` prefix discipline as the live targets above.
add_executable(mysql_pg_live_autocreate_test mysql_pg_live_autocreate.cpp)
target_link_libraries(mysql_pg_live_autocreate_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_autocreate_test COMMAND mysql_pg_live_autocreate_test)

# connection_clone_test covers db::CloneConnection + IConnection::EffectiveProfile()
# (src/db/ConnectionClone.*, src/db/DbDriver.h): the single chokepoint a worker
# thread uses to get its own independent connection instead of sharing the UI's
# (see docs/devlog on the AiKnowledgeBase heap-corruption incident). Same
# self-contained pattern as sqlite_live_test (throwaway file, no server/network),
# plus a small fake IConnection for the tunnel-rewrite contract that would
# otherwise need a live SSH tunnel to exercise.
add_executable(connection_clone_test connection_clone_test.cpp)
target_link_libraries(connection_clone_test PRIVATE swiftsql::db unofficial::sqlite3::sqlite3)

add_test(NAME connection_clone_test COMMAND connection_clone_test)

# sqlite_clone_sweep_test pins what the target-read clone in RunTableSweep
# (src/db/DataSyncExec.cpp) does on SQLITE specifically. The clone is
# load-bearing on MySQL/PostgreSQL, where sharing one client-library connection
# between the RowCursor's producer thread and the write path is undefined
# behavior. SQLite has no wire state to corrupt, but it does have a file-locking
# ladder, and the driver opens with no WAL and no busy timeout — so a lock
# conflict fails instantly rather than waiting. This target drives a real
# SQLite->SQLite ExecuteDataSync against a target that already holds rows, with
# the DEFAULT (production, db::CloneConnection) opener, at two scales: one that
# fits in the default page cache and one that forces a mid-transaction spill.
# Self-contained and zero-config like sqlite_live_test, so it runs in CI.
add_executable(sqlite_clone_sweep_test sqlite_clone_sweep_test.cpp)
target_link_libraries(sqlite_clone_sweep_test PRIVATE swiftsql::db unofficial::sqlite3::sqlite3)

add_test(NAME sqlite_clone_sweep_test COMMAND sqlite_clone_sweep_test)

# sqlite_batch_sync_test pins that SQLite can be an INSERT TARGET at all. Until
# src/db/SqliteStream.cpp landed, SqliteConnection had no ExecuteBatch override
# and inherited IConnection's refusal ("该驱动不支持批量执行"), so the first
# buffered insert aborted the whole sweep -- SQLite could be a sync source and
# could take UPDATE/DELETE, but a SQLite target could never be populated. This
# target drives real SQLite->SQLite ExecuteDataSync runs into EMPTY targets and
# reads the results back: 1200 rows to cross the 500-row batch boundary twice
# (boundary rows verified individually); NULL vs '' vs zero-length blob vs a blob
# with an embedded 0x00, which is where the equivalent MySQL bind bug lived; and
# a 71-column table whose 500-row batch would need 35500 parameters, forcing the
# SQLITE_LIMIT_VARIABLE_NUMBER lowering path that would otherwise be dead code.
# Zero-config and self-contained like the two SQLite targets above, so CI runs it.
add_executable(sqlite_batch_sync_test sqlite_batch_sync_test.cpp)
target_link_libraries(sqlite_batch_sync_test PRIVATE swiftsql::db unofficial::sqlite3::sqlite3)

add_test(NAME sqlite_batch_sync_test COMMAND sqlite_batch_sync_test)

# AI layer unit tests — Json reader/writer, SSE/NDJSON framing, prompt templates.
# All pure logic; link swiftsql::ai (which contains Json/SseParser/PromptBuilder and
# PUBLIC-links swiftsql::db for the db::Dialect enum used by prompts).
add_executable(json_test json_test.cpp)
target_link_libraries(json_test PRIVATE swiftsql::ai)
add_test(NAME json_test COMMAND json_test)

add_executable(sse_parser_test sse_parser_test.cpp)
target_link_libraries(sse_parser_test PRIVATE swiftsql::ai)
add_test(NAME sse_parser_test COMMAND sse_parser_test)

add_executable(prompt_builder_test prompt_builder_test.cpp)
target_link_libraries(prompt_builder_test PRIVATE swiftsql::ai)
add_test(NAME prompt_builder_test COMMAND prompt_builder_test)

# Every provider must report an error that arrives INSIDE a 200 stream, and must
# NOT mistake an ordinary chunk for one. Guards the fix for the silent
# "(No response)" turn — see the header of ai_stream_error_test.cpp.
add_executable(ai_stream_error_test ai_stream_error_test.cpp)
target_link_libraries(ai_stream_error_test PRIVATE swiftsql::ai)
add_test(NAME ai_stream_error_test COMMAND ai_stream_error_test)

# The provider SELECTED in Preferences must be the one that answers — never a
# silent fall-through to whichever sibling happens to hold an API key — and
# ai::ProviderKey must change whenever a cached AiClient goes stale. See the
# header of ai_provider_selection_test.cpp.
add_executable(ai_provider_selection_test ai_provider_selection_test.cpp)
target_link_libraries(ai_provider_selection_test PRIVATE swiftsql::ai)
add_test(NAME ai_provider_selection_test COMMAND ai_provider_selection_test)

# mermaid_test covers the *pure* half of the Mermaid flowchart engine — the
# text parser (ui::mmd::ParseMermaid: nodes/shapes/edges/direction) and the
# Sugiyama-style layering (ui::mmd::AssignRanks: longest-path with DFS back-edge
# removal). Like sqlscript_test it compiles the single logic TU directly
# (ui/MermaidDiagram.cpp is GUI-free, wxString-only) and links swiftsql::core,
# which supplies wxString + the src/ include root. The wxGraphicsContext drawing
# (MermaidRender.cpp) is a thin GUI layer and is deliberately not exercised.
add_executable(mermaid_test
    mermaid_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MermaidDiagram.cpp
)
target_link_libraries(mermaid_test PRIVATE swiftsql::core)

add_test(NAME mermaid_test COMMAND mermaid_test)

# DataSyncOrderingTests covers ADR-015 T2/T4: the three PK-ordering defenses of
# db::sync::DiffAndEmitData (query-level byte ordering, the CheckDataDiffOrdering
# refusal gate, the in-merge backwards-key abort) plus intra-table progress,
# prompt cancellation, and IConnection::ExecuteBatch's degradation contract.
#
# Offline by construction: two FakeServer IConnections stand in for a MySQL _ci
# server and a PG default-collation server by sorting a canonical row set with
# collation-simulating comparators, so the desync this feature's silent
# data-corruption bug depends on is reproduced without a live server. It also
# asserts the ORDER BY text mysqlstream/pgstream emit, which is why it links the
# vendor client libs PRIVATE �� purely for MySqlStream.h/PgStream.h's include
# dirs (those headers take the raw MYSQL*/PGconn*); no handle is ever opened.
add_executable(DataSyncOrderingTests DataSyncOrderingTests.cpp)
target_link_libraries(DataSyncOrderingTests
    PRIVATE swiftsql::db unofficial::libmariadb PostgreSQL::PostgreSQL)

add_test(NAME DataSyncOrderingTests COMMAND DataSyncOrderingTests)

# SyncRowIdentityTests pins the two things a db::sync::RowChange must carry
# besides the values it writes: the target's BEFORE values for an UPDATE (so the
# drill-in can render a real per-column before/after instead of "not sent"), and
# the row's own EncodeRowKey identity. The headline case is a genuinely coerced
# cross-engine key (integer -> PostgreSQL boolean, which rewrites 1 into TRUE):
# it proves the RETAINED key still equals the one the RowChangeSink emitted over
# the raw pre-bridge cells, while the key a consumer would re-derive from
# RowChange::Key() does not — the silent failure that would make a user's
# row-level exclusion miss and sync the row anyway. Also pins the memory
# contract: prior values are materialized for the <=500-row sample only, never
# by the streaming execute pass. A separate TU from sync_test.cpp purely because
# that file is at the project's 1000-line ceiling. Canned-row stub IConnection;
# no driver/connection/GUI is exercised.
add_executable(SyncRowIdentityTests SyncRowIdentityTests.cpp)
target_link_libraries(SyncRowIdentityTests PRIVATE swiftsql::db)

add_test(NAME SyncRowIdentityTests COMMAND SyncRowIdentityTests)

# SyncCrossEngineCompareTests pins the fix for the cross-engine comparison defect
# found during the ADR-015 integration: DataSync's sorted merge compared source
# and target cells RAW, before any ColumnBridge, so a MySQL tinyint(1) holding 1
# and a PostgreSQL boolean holding 't' �� the same logical value �� compared
# unequal. In a non-key column that was a false-positive UPDATE storm (breaking
# the release-blocking zero-false-positive bar); in a key column it was an
# identity failure that turned an unchanged row into a DELETE + INSERT pair.
#
# Covers: the zero-false-positive bar on a coerced non-key column, a GENUINE
# difference in the same column still surfacing as exactly one UPDATE, a coerced
# composite KEY matching instead of churning, the same-engine path staying raw
# and zero-cost (CompareRules::allRaw), the order-preserving proofs each
# CompareOp rests on (the sorted merge's ordering assumption is what makes this
# dangerous), and the fail-closed key gate. Canned-row stub IConnection; no
# driver/connection/GUI is exercised.
add_executable(SyncCrossEngineCompareTests SyncCrossEngineCompareTests.cpp)
target_link_libraries(SyncCrossEngineCompareTests PRIVATE swiftsql::db)

add_test(NAME SyncCrossEngineCompareTests COMMAND SyncCrossEngineCompareTests)

# RoutineNormalizeTests is the adversarial corpus for db::sync::
# NormalizeRoutineBody (src/db/RoutineNormalize.{h,cpp}), the highest-risk piece
# of the compare-only functions & procedures diff (ADR-013 Q3). ADR-013 required
# this file to exist BEFORE the catalog readers, because every other component of
# that feature fails loudly (PermissionDenied / NotComparable) while a subtly
# wrong normalizer fails silently in the worst direction: telling the user two
# identical routines differ, against a zero-false-positive release bar.
#
# The corpus is hostile by construction: `--` and `/* */` inside PG `$$` (which
# are literal text, not comments), nested `$outer$…$inner$…$outer$`, `$1`
# parameter refs that must NOT open a dollar quote, MySQL's `x--1` arithmetic
# (`--` needs a following space there), `#` comments, backtick identifiers,
# doubled/backslash-escaped quotes, mixed CRLF/LF, MySQL executable `/*! … */`
# comments that must be PRESERVED, and unterminated quotes — which must yield
# NormalizeStatus::Unresolved with the ORIGINAL text handed back, never a wrong
# answer. Pure logic; links swiftsql::db for wxString + db::Dialect. No
# connection, no GUI.
add_executable(RoutineNormalizeTests RoutineNormalizeTests.cpp)
target_link_libraries(RoutineNormalizeTests PRIVATE swiftsql::db)

add_test(NAME RoutineNormalizeTests COMMAND RoutineNormalizeTests)

# RoutineCompareTests covers the pure decision layer of the same feature:
# db::sync::CompareRoutines + the lexical matching key (src/db/RoutineCompare.
# {h,cpp}). Because the standing ruling is that this feature may only ever
# ANSWER — never generate or apply routine DDL — the verdict IS the product, so
# the verdicts are asserted here rather than eyeballed in the wizard.
#
# Headline cases: cross-engine bodies are ALWAYS NotComparable (even when the
# two bodies are byte-identical — PL/pgSQL and MySQL's routine dialect are
# different languages, so neither "same" nor "differs" is a claim we can make);
# an unparseable body degrades to NotComparable and never to BodyDiffers; an
# ambiguous overload key degrades rather than guessing a pairing; and a
# PermissionDenied/Unsupported side yields ZERO pairs instead of a fabricated
# "the target is missing 40 functions" (ADR-013 Q4). It also pins the key's
# order-independence — the property ADR-013 rejected db::CompareCanonical's
# graded TypeVerdict for lacking — and, in TestNoDdlSurface, the structural
# absence of any DDL-yielding member. Pure logic; links swiftsql::db. No
# connection, no GUI.
add_executable(RoutineCompareTests RoutineCompareTests.cpp)
target_link_libraries(RoutineCompareTests PRIVATE swiftsql::db)

add_test(NAME RoutineCompareTests COMMAND RoutineCompareTests)

# MySqlRoutinePrivilegeTests owns DEF-R3-02, found by OBSERVATION on a real
# MySQL 8.0.36 rather than by review: an account with `GRANT SELECT ON db.*` and
# no routine privileges gets ZERO ROWS from information_schema.ROUTINES -- the
# query succeeds, the bodies are not NULL, the rows are simply filtered out by
# the 8.0 data dictionary. Neither of MySqlRoutineRead's existing defenses can
# fire on that (the blank-body heuristic has nothing to list; the PARAMETERS
# degradation needs a FAILED query), so the reader answered Ok with an empty
# vector and a restricted source reported every target routine as "仅目标端存在".
#
# The fix resolves the ambiguity with a second signal (SHOW GRANTS), and this TU
# pins all three of its outcomes -- including, critically, that a legitimately
# empty catalog read by a PRIVILEGED account still reports Ok, and that no
# unparsable/role/wildcard grant output ever resolves TOWARDS Ok.
#
# Its own TU rather than more of RoutineCompareTests.cpp: that file is at 960
# lines against the charter's hard 1000-line ceiling and this coverage does not
# fit. The live counterpart is mysql_pg_live_routines.cpp hazards 7d/7e/7f;
# this one runs in CI, where no MySQL is reachable. Links swiftsql::db, no
# connection, no GUI.
add_executable(MySqlRoutinePrivilegeTests MySqlRoutinePrivilegeTests.cpp)
target_link_libraries(MySqlRoutinePrivilegeTests PRIVATE swiftsql::db)

add_test(NAME MySqlRoutinePrivilegeTests COMMAND MySqlRoutinePrivilegeTests)

# SyncRoutineTreeTests is the UI half of that same feature: ui::
# MakeRoutineCategoryNode / BuildDiffTree's 函数 & 存储过程 categories
# (src/ui/SyncDiffModel.{h,cpp}). RoutineCompareTests pins what may be CLAIMED;
# this pins what is SHOWN, and above all the two things a reviewer of the CEO
# ruling would check:
#
#   * no node under a routine category carries an `id` or `checkable`, which
#     are exactly what ui::SyncSelection needs to hold check state — so a
#     routine cannot enter the ExecutionSpec, structurally rather than by
#     convention;
#   * a degraded read (PermissionDenied / Unsupported) renders the category
#     with its 不可见 label and ZERO children, never a half list.
#
# Plus the presentation rule that carries the honesty of the verdict into
# pixels: the body row's highlight flag is ON for a same-engine BodyDiffers and
# OFF for a cross-engine NotComparable. Compiles SyncDiffModel.cpp directly
# (same pattern as SyncDiffModelTests) and links swiftsql::db for
# CompareRoutines. No wxWindow, no GUI.
add_executable(SyncRoutineTreeTests
    SyncRoutineTreeTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/SyncDiffModel.cpp
)
target_link_libraries(SyncRoutineTreeTests PRIVATE swiftsql::db)

add_test(NAME SyncRoutineTreeTests COMMAND SyncRoutineTreeTests)

# ResultGridSqlTests + ResultGridModelTests cover the two PURE modules carved
# out of the result grid / data browser (src/ui/ResultGridSql.{h,cpp} and
# src/ui/ResultGridModel.{h,cpp}) when ResultGridPanel.cpp / ResultGridPanelOps
# .cpp were brought back under the charter's 1000-line ceiling.
#
# The split was chosen for testability rather than line count: the SQL the data
# browser emits -- including the edit->DML diff that 保存 applies and the 复制为
# UPDATE/DELETE statements -- used to be inline among wxGrid reads and was
# therefore reachable ONLY by clicking through a running app against a live
# server. Those are among the few code paths in this program that WRITE to a
# customer's database, and a wrong WHERE clause in either is silent data
# corruption. Now they are plain functions over plain values.
#
# Both began as CHARACTERIZATION suites: they pinned behaviour byte for byte so
# the structural split could be reviewed as behaviour-preserving. Two assertions
# in ResultGridSqlTests deliberately pinned KNOWN DEFECTS rather than fixing them
# mid-refactor -- a partially-present composite PK still emitting an
# under-constrained WHERE (silently rewriting rows the user never edited), and
# the DML/clipboard/filter paths not doubling an embedded quote character where
# the pager path did. Both are now FIXED, and those two assertions have been
# INVERTED to demand the fix: they are kept, not deleted, and now serve as
# regression tests. See that file's header.
#
# ResultGridSqlTests links swiftsql::db (db::Dialect / QuoteIdent /
# LimitOffsetClause live in db/DbDriver.h). ResultGridModelTests links only
# swiftsql::core, and that is a FITNESS FUNCTION, not an optimization (same
# rule as SyncSelectionTests): the model must depend on nothing but wxString,
# so if anyone pulls a db type or a wx widget into it, this target stops
# linking. Both compile their one pure TU directly, mirroring
# SyncDiffModelTests. No wxWindow, no wxGrid, no connection.
add_executable(ResultGridSqlTests
    ResultGridSqlTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ResultGridSql.cpp
)
target_link_libraries(ResultGridSqlTests PRIVATE swiftsql::db)

add_test(NAME ResultGridSqlTests COMMAND ResultGridSqlTests)

# QuerySourceTests covers src/ui/QuerySource.{h,cpp} — the third pure module
# carved out of the result-grid path, and the one that decides WHICH TABLE the
# 保存 button writes to.
#
# It was extracted for exactly the reason the two above were: the derivation was
# a substring search inline in MainFrame_Query.cpp, a wx TU reachable only by
# clicking through a running app against a live server, and it was WRONG in three
# independent ways -- deleting quote characters instead of unquoting (a table
# named `we`ird` bound to a different table called `weird`), splitting on a dot
# inside a quoted name, and dropping the db/schema qualifier so a statement was
# READ from one table and WRITTEN to another. The write path's move onto
# db::QuoteIdent could not fix any of them: quoting a name that is already lossy
# cannot recover it.
#
# Links swiftsql::db for db::Dialect/db::QuoteIdent AND for db::sync::
# NormalizeRoutineBody -- the dialect quote-state machine QuerySource reuses to
# strip comments rather than reimplementing one. That reuse is deliberate and the
# tests pin it: MySQL `#` and `x--1`, PostgreSQL `$$…$$`, unterminated quoting.
add_executable(QuerySourceTests
    QuerySourceTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/QuerySource.cpp
)
target_link_libraries(QuerySourceTests PRIVATE swiftsql::db)

add_test(NAME QuerySourceTests COMMAND QuerySourceTests)

add_executable(ResultGridModelTests
    ResultGridModelTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ResultGridModel.cpp
)
target_link_libraries(ResultGridModelTests PRIVATE swiftsql::core)

add_test(NAME ResultGridModelTests COMMAND ResultGridModelTests)

# mysql_pg_live_mysqltx_test �� transaction integrity against a **MySQL** target.
#
# mysql_pg_live_exec4_test proved cancellation and connection loss against a
# PostgreSQL target. Every one of those observations rests on a property MySQL
# does not have: PostgreSQL rolls DDL back, MySQL commits it implicitly and
# mid-transaction. So "the transaction rolled back" and "the target is back the
# way it was" are two different claims on MySQL, and the per-table transaction
# story has to be re-observed rather than assumed to carry over.
#
#   M1  Cancellation mid-write on InnoDB, read back through a SEPARATE
#       connection �� the worker's own session can see its uncommitted rows and
#       therefore cannot distinguish "rolled back" from "still open".
#   M2  Connection loss via `KILL <id>`, where the id comes from
#       `SELECT CONNECTION_ID()` ON THE VICTIM. Never from a PROCESSLIST scan,
#       which on a shared server is how a test kills a colleague's session.
#   M3  THE MySQL-SPECIFIC QUESTION: whether the implicit DDL commit defeats the
#       phase-1 rollback, leaving a table that EXISTS with ZERO rows after a run
#       that reported failure. The product cannot prevent that �� the server
#       decides �� so the assertion is on whether it is REPORTED honestly, plus
#       whether a retry converges the half-state.
#   M4  SyncEngine::Execute's phase-1 COMMIT-failure path, staged by killing the
#       connection from the progress callback after the last DDL statement so
#       the very next thing the engine does is the COMMIT.
#   M5  What the engines ACTUALLY do on ROLLBACK after a FAILED COMMIT, probed
#       at the raw connection level on both engines �� PostgreSQL via a
#       DEFERRABLE INITIALLY DEFERRED constraint (a COMMIT failure on a healthy
#       socket), MySQL via the killed connection. This is the prerequisite for
#       M4's error handling: a rollback that errors and one that is a harmless
#       no-op call for different handling.
#
# Same env gating as the live targets above (SKIP + exit 0 when unset), under
# its own `swiftsql_xmtx_` prefix so it can never collide with them. M5's
# PostgreSQL half gates independently on SWIFTSQL_PGTEST_*.
#   M6  Whether the COMMIT-failure path leaves an OPEN TRANSACTION behind. This
#       is the item that proves the fix rather than the engines' behaviour, and
#       it is PostgreSQL-targeted for the same reason M3 is MySQL-targeted: it
#       is the engine that can express the question. M4 reaches the same code
#       path by killing the connection, which is the realistic MySQL route, but
#       on a dead connection "was a transaction left open?" is meaningless —
#       the server tore the session down. A DEFERRABLE INITIALLY DEFERRED
#       constraint fails the COMMIT with the socket still healthy, and
#       pg_stat_activity.state read FROM A SECOND CONNECTION then distinguishes
#       'idle' from 'idle in transaction'.
#
# Split across two TUs to stay under the charter's 1000-line ceiling.
add_executable(mysql_pg_live_mysqltx_test
    mysql_pg_live_mysqltx.cpp
    mysql_pg_live_mysqltx2.cpp
)
target_link_libraries(mysql_pg_live_mysqltx_test PRIVATE swiftsql::db)

add_test(NAME mysql_pg_live_mysqltx_test COMMAND mysql_pg_live_mysqltx_test)

# EditorSqlFormatTests + EditorCompletionTests cover the two PURE modules carved
# out of the SQL editor tab (src/ui/EditorSqlFormat.{h,cpp} and
# src/ui/EditorCompletion.{h,cpp}) when EditorPage.cpp was brought back under the
# charter's 1000-line ceiling. It was at 991 -- nine lines of headroom.
#
# The seam was chosen for testability rather than line count, following
# ResultGridSql/ResultGridModel and deliberately NOT following the mechanical cut
# that produced ResultGridPanelOps.cpp. Two things came out as plain functions
# over plain values:
#
#   * the SQL beautifier -- the one code path in this program that REWRITES THE
#     USER'S TYPED TEXT in place. It was a ~100-line loop reading its keyword set
#     off a connection callback and writing into a wxStyledTextCtrl, so it could
#     only be run by launching the app, connecting to a server, and clicking a
#     toolbar button. If it drops a character or eats an unterminated comment,
#     the user's work is gone.
#   * the completion decision layer -- which words complete, and which schema
#     source answers a dot-qualified prefix. This one had a worse problem than
#     being untested: the SWIFTSQL_AUTOEDIT ScriptedComplete hook, the mechanism
#     that was supposed to VERIFY completion, contained a SECOND independently
#     written copy of the qualifier-chain parse. A verification hook that
#     re-implements what it verifies cannot find a bug in either copy. Both call
#     sites now share one module and this suite checks it.
#
# Both suites are CHARACTERIZATION suites: they pin current behaviour byte for
# byte so the split is reviewable as behaviour-preserving, and equivalence was
# additionally established with a temporary differential-fuzz harness that ran
# the transcribed git-HEAD originals against the extracted modules over 140,000
# randomized inputs (0 divergences; the harness was mutation-checked first, then
# deleted). Three pins are on KNOWN COSMETIC/BENIGN DEFECTS asserted as CURRENT
# behaviour rather than fixed mid-refactor -- a missing space in `WHERE(`, a
# blank line after a line comment, and an alias scan that reads any adjacent
# "<table> <word>" pair as a binding. Each is marked DEFECT in its test file with
# what fixing it would entail.
#
# Both link ONLY swiftsql::core and compile their one pure TU directly. That is a
# FITNESS FUNCTION, not an optimization (same rule as ResultGridModelTests):
# these modules must depend on nothing but wxString, so if anyone pulls a
# db::Dialect or a wx widget back into them, these targets stop linking. No
# wxWindow, no wxStyledTextCtrl, no connection.
add_executable(EditorSqlFormatTests
    EditorSqlFormatTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/EditorSqlFormat.cpp
)
target_link_libraries(EditorSqlFormatTests PRIVATE swiftsql::core)

add_test(NAME EditorSqlFormatTests COMMAND EditorSqlFormatTests)

add_executable(EditorCompletionTests
    EditorCompletionTests.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/EditorCompletion.cpp
)
target_link_libraries(EditorCompletionTests PRIVATE swiftsql::core)

add_test(NAME EditorCompletionTests COMMAND EditorCompletionTests)

# The assertion-census gate (docs/CHARTER.md 第七節). Must stay LAST: it
# enumerates every target above via ctest and diffs each one's executed
# assertion labels against tests/lint/census/<target>.txt.
add_subdirectory(lint)
