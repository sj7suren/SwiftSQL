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

# ---- Oracle native OCI driver (OPTIONAL) -----------------------------------
# We link NO OCI import library (no oci.lib): every OCI function is late-bound
# from oci.dll at runtime via LoadLibrary/GetProcAddress (see OciLoader.cpp), so
# the app runs fine on a host with no Oracle client installed. What we DO need is
# the Instant Client SDK header <oci.h> at COMPILE time, for the OCI types,
# structs and constants.
#
# Oracle's licence does not let us redistribute that header, so it cannot live in
# this repository. Making it a hard requirement meant nobody could build SwiftSQL
# without first downloading the Oracle SDK -- including everyone who never uses
# Oracle. So the two OCI translation units are built only when the SDK is
# actually present; otherwise OciStub.cpp supplies the same symbols and Oracle
# reports itself unavailable. Every other engine is unaffected either way.
#
# Point at your own SDK with -DSWIFTSQL_OCI_SDK=<dir containing include/oci.h>,
# or just unpack it under third_party/ and let the glob below find it.
set(SWIFTSQL_OCI_SDK "" CACHE PATH
    "Oracle Instant Client SDK directory (the one containing include/oci.h). Empty = auto-detect under third_party/.")

if(NOT SWIFTSQL_OCI_SDK)
    file(GLOB _swiftsql_oci_candidates "${CMAKE_SOURCE_DIR}/third_party/instantclient_*/sdk")
    foreach(_candidate IN LISTS _swiftsql_oci_candidates)
        if(EXISTS "${_candidate}/include/oci.h")
            set(SWIFTSQL_OCI_SDK "${_candidate}")
            break()
        endif()
    endforeach()
    unset(_swiftsql_oci_candidates)
    unset(_candidate)
endif()

if(SWIFTSQL_OCI_SDK AND EXISTS "${SWIFTSQL_OCI_SDK}/include/oci.h")
    message(STATUS "SwiftSQL: Oracle OCI driver ENABLED (SDK: ${SWIFTSQL_OCI_SDK})")
    target_sources(swiftsql_db PRIVATE OracleOciDriver.cpp OciLoader.cpp)
    # PRIVATE so the vendor SDK never leaks to callers of swiftsql::db.
    target_include_directories(swiftsql_db PRIVATE "${SWIFTSQL_OCI_SDK}/include")
else()
    message(STATUS
        "SwiftSQL: Oracle OCI driver DISABLED - no Instant Client SDK found. "
        "Every other engine still builds; Oracle will report itself unavailable. "
        "To enable it, unpack the SDK under third_party/ or pass -DSWIFTSQL_OCI_SDK=<dir>.")
    target_sources(swiftsql_db PRIVATE OciStub.cpp)
endif()
