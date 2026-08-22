# swiftsql_db — database access layer. Exposes the IConnection interface +
# CreateConnection() factory; hides the concrete drivers and their client libs.
#   DbDriver (factory + stub) / MySqlDriver (libmariadb) / PgDriver (libpq)
add_library(swiftsql_db STATIC
    DbDriver.cpp
    ConnectionClone.cpp
    MySqlDriver.cpp
    MySqlSql.cpp
    MySqlStream.cpp
    UserAdmin.cpp
    UserAdminPg.cpp
    UserAdminMssql.cpp
    UserAdminOracle.cpp
    PgDriver.cpp
    PgSql.cpp
    PgSchemaRead.cpp
    PgStream.cpp
    SqliteDriver.cpp
    SqliteStream.cpp
    SqlServerDriver.cpp
    OracleDriver.cpp
    OracleOciDriver.cpp
    OciLoader.cpp
    ProcessMonitor.cpp
    SqlKeywords.cpp
    SqlScript.cpp
    SqlLiteral.cpp
    TableExport.cpp
    TableExportBin.cpp
    TableImport.cpp
    SchemaDiff.cpp
    SyncTypeMap.cpp
    DialectProfile.cpp
    MySqlProfile.cpp
    PgProfile.cpp
    SqliteProfile.cpp
    SqlServerProfile.cpp
    OracleProfile.cpp
    DialectRegistry.cpp
    DataSync.cpp
    DataSyncExec.cpp
    SyncValueMap.cpp
    SyncCellCompare.cpp
    SyncTemporal.cpp
    RowChange.cpp
    RoutineNormalize.cpp
    RoutineCompare.cpp
    MySqlRoutineRead.cpp
    PgRoutineRead.cpp
    SyncSeqFix.cpp
    SyncEngine.cpp
)
add_library(swiftsql::db ALIAS swiftsql_db)

target_include_directories(swiftsql_db PUBLIC ${CMAKE_SOURCE_DIR}/src)

# core is part of the public interface (ConnectionProfile appears in headers).
# The vendor client libs are PRIVATE — callers of swiftsql::db never see or link
# libmariadb / libpq directly ("被调用安全": internal deps don't leak).
target_link_libraries(swiftsql_db
    PUBLIC  swiftsql::core
    PRIVATE unofficial::libmariadb PostgreSQL::PostgreSQL unofficial::sqlite3::sqlite3
)

if(WIN32)
    # odbc32 = Windows SDK system lib (DM8 / SQL Server drivers); no find_package needed.
    target_link_libraries(swiftsql_db PRIVATE ws2_32 secur32 crypt32 shlwapi bcrypt odbc32)
endif()

# Oracle native OCI (OracleOciDriver.cpp + OciLoader.cpp). We link NO OCI import
# library (no oci.lib): every OCI function is late-bound from oci.dll at runtime
# via LoadLibrary/GetProcAddress (see OciLoader.cpp), so the whole app builds and
# links even on a host with no Oracle client. Compile-time we still need only the
# Instant Client SDK headers (oci.h: types/structs/constants), added PRIVATE so
# the vendor SDK never leaks to callers of swiftsql::db.
set(SWIFTSQL_OCI_SDK ${CMAKE_SOURCE_DIR}/third_party/instantclient_23_5/sdk)
target_include_directories(swiftsql_db PRIVATE ${SWIFTSQL_OCI_SDK}/include)
