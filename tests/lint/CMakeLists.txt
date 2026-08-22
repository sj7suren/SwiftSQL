# The assertion-census gate. Mechanism and rationale live in the header of
# assertion_census.py; the charter rule it enforces is docs/CHARTER.md 第七節.
#
# This is its own CMakeLists rather than 30 more lines in tests/CMakeLists.txt
# because that file is at 978 of the charter's 1000-line ceiling.
#
# The lint discovers its targets from ctest itself, so a NEW TEST TARGET IS
# ENROLLED THE MOMENT IT IS REGISTERED — there is no second list to forget to
# update, which is the failure mode the lint exists to catch.
#
# Python is a soft dependency: if no interpreter is present the gate is skipped
# with a warning rather than breaking the build for everyone. CI must have it.
find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_FOUND)
    message(WARNING
        "Python3 not found: the assertion-census lint will NOT run. "
        "Vacuous-assertion drift is unguarded in this build tree.")
    return()
endif()

add_test(NAME assertion_census
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/assertion_census.py
                 --build-dir  ${CMAKE_BINARY_DIR}
                 --ctest      ${CMAKE_CTEST_COMMAND}
                 --census-dir ${CMAKE_CURRENT_SOURCE_DIR}/census)

# The census runs every other test binary itself. RUN_SERIAL keeps it from
# doing that concurrently with ctest's own copy of the same binaries, which
# would have two processes touching one live database under `ctest -j`.
set_tests_properties(assertion_census PROPERTIES RUN_SERIAL TRUE)
