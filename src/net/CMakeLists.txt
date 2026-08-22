# swiftsql_net — network transport helpers. Currently: SSH local port-forward
# tunnel (libssh2). Leaf module: depends only on core; libssh2 is PRIVATE so it
# does not leak to callers.
add_library(swiftsql_net STATIC
    SshTunnel.cpp
)
add_library(swiftsql::net ALIAS swiftsql_net)

target_include_directories(swiftsql_net PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(swiftsql_net
    PUBLIC  swiftsql::core
    PRIVATE Libssh2::libssh2 ws2_32
)
