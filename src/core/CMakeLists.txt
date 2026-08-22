# swiftsql_core — domain model & persistence. No GUI logic, no DB drivers.
#   DbTypes / ConnectionProfile / Secret / ConnectionStore
# Depends only on wxWidgets base+core (wxString / wxColour / wxFileConfig).
add_library(swiftsql_core STATIC
    Secret.cpp
    ConnectionStore.cpp
    GroupStore.cpp
    FavoriteStore.cpp
    ScriptStore.cpp
    AutomationStore.cpp
    Settings.cpp
    CrashLog.cpp
    Lang.cpp
    LangPackEn.cpp
    LangPackEn2.cpp
)
add_library(swiftsql::core ALIAS swiftsql_core)

# PUBLIC so dependents resolve "core/…" / "db/…" / "ui/…" style includes.
target_include_directories(swiftsql_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# wx::core is needed only for wxColour in the engine metadata table.
target_link_libraries(swiftsql_core PUBLIC wx::base wx::core)

# Dbghelp backs CrashLog's MiniDumpWriteDump (Windows-only, post-mortem dumps).
if(WIN32)
    target_link_libraries(swiftsql_core PUBLIC Dbghelp Bcrypt)
endif()
