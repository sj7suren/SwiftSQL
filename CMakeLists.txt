cmake_minimum_required(VERSION 3.24)

project(SwiftSQL
    VERSION 1.1.21
    DESCRIPTION "Lightweight cross-platform SQL client"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===============================================================
# Third-party dependencies
# ===============================================================
# wxWidgets: fetched from source, built as STATIC libs so that
# Windows/macOS can ship a single self-contained executable.
include(FetchContent)

set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxUSE_STC ON CACHE BOOL "" FORCE)          # wxStyledTextCtrl (SQL editor)
set(wxUSE_WEBREQUEST ON CACHE BOOL "" FORCE)   # wxWebRequest (AI provider HTTP, WinHTTP backend)
if(MSVC)
    set(wxBUILD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)  # /MT single-file dist
endif()

FetchContent_Declare(
    wxWidgets
    URL https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.9/wxWidgets-3.2.9.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(wxWidgets)

# Database client libraries (via vcpkg toolchain):
#   MySQL / OceanBase / KingBase → libmariadb / libpq
find_package(unofficial-libmariadb CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)   # SQLite (embedded, file-based)
find_package(Libssh2 CONFIG REQUIRED)   # SSH tunnel (swiftsql::net)

# ===============================================================
# Compile settings applied to OUR modules (not the fetched wx tree)
# ===============================================================
if(MSVC)
    add_compile_options(/utf-8)                # sources carry Chinese UI strings
    # static CRT to match wxWidgets → single-file distribution
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # ---- SYMBOLS IN RELEASE, ON PURPOSE ----------------------------------
    # This program installs a crash handler and writes a MINIDUMP on a hard
    # fault (core/CrashLog.cpp). Without symbols that dump is a dead end: a
    # real 0xC0000005 was diagnosable only down to "somewhere in SwiftSQL.exe
    # at RVA 0xFC3D88", which is not enough to fix anything. A crash reporter
    # whose output cannot be read is not a crash reporter.
    #
    # /Zi + /DEBUG emit a PDB; /OPT:REF,ICF is re-stated because /DEBUG turns
    # them OFF by default, and without them the optimised layout would change.
    # The PDB does NOT ship with the exe and does NOT slow it down — the code
    # generated is byte-for-byte the same optimised code as before.
    #
    # /MAP is the fallback path: it produces a plain-text function->address
    # table that can resolve a dump's RVAs on a machine with no debugger
    # installed, which is exactly the situation this was diagnosed in.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>
                     $<$<CONFIG:Release>:/OPT:REF>
                     $<$<CONFIG:Release>:/OPT:ICF>
                     $<$<CONFIG:Release>:/MAP>)
endif()

# ===============================================================
# Modular layers — each is its own static library ("工程"):
#   swiftsql::core  (model/persistence, no GUI, no drivers)
#     ▲
#   swiftsql::db    (IConnection + drivers; vendor libs PRIVATE)
#     ▲
#   swiftsql::ui    (wxWidgets presentation)
#     ▲
#   swiftsql        (executable = main.cpp only → tiny, doesn't grow with features)
# Dependencies point one way only (no cycles). See docs/ARCHITECTURE.md.
# ===============================================================
add_subdirectory(src/core)
add_subdirectory(src/db)
add_subdirectory(src/net)
add_subdirectory(src/ai)
add_subdirectory(src/ui)

# ===============================================================
# Unit tests for pure logic (opt out with -DSWIFTSQL_BUILD_TESTS=OFF).
# The first target covers db::SplitSqlScript (the dialect-aware SQL splitter),
# a pure function that is the highest-ROI thing to pin down before the editor's
# statement splitting is consolidated onto it. Run with: ctest --test-dir build
# ===============================================================
option(SWIFTSQL_BUILD_TESTS "Build unit tests" ON)
if(SWIFTSQL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set(SWIFTSQL_APP_SOURCES src/main.cpp)
if(WIN32)
    list(APPEND SWIFTSQL_APP_SOURCES src/win/SwiftSQL.rc)
endif()

add_executable(swiftsql WIN32 MACOSX_BUNDLE ${SWIFTSQL_APP_SOURCES})
target_link_libraries(swiftsql PRIVATE swiftsql::ui)

# Ship the external language packs next to the executable so they load at runtime.
add_custom_command(TARGET swiftsql POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/lang $<TARGET_FILE_DIR:swiftsql>/lang
    COMMENT "Copying language packs → lang/")

set_target_properties(swiftsql PROPERTIES
    OUTPUT_NAME "SwiftSQL"
    MACOSX_BUNDLE_BUNDLE_NAME "SwiftSQL"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.swiftsql.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
