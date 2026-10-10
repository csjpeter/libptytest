# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-03
# Declaration of the unit and PTY tests and their helpers.
# The project's tests.  Every test is declared on one line of one shape,
#
#     project_test(<category> <name> SOURCES <file>... [LIBS <lib>...] [TIMEOUT <s>])
#
# and is built into build/tests/<category>/bin/<name>, which is the test: the
# integrator runs every file it finds there, with no arguments, and reads the
# names from these lines without building anything.  Programs a test drives
# but which are not tests themselves are built into
# build/tests/<category>/helpers/, never into bin/.  tools/check-test-bins.sh
# fails a build in which a declared test is missing from bin/ or bin/ holds an
# undeclared file.  See policies/testing-model.md, "How a test is declared,
# built and found".
#
# The CMake target of a test is <category>_<name>, for the target_* calls
# that follow a declaration.  The CTest name is <category>.<name> and the
# category is its label, so "ctest -L unit" runs one category.

set(PTYTEST_TEST_TIMEOUT 120)

# A command every test is run under, e.g. Valgrind (see manage.sh valgrind).
set(PTYTEST_TEST_WRAPPER "" CACHE STRING "Command prefix for every test")

# Fixtures written by the tests go under the build directory, never into a
# fixed /tmp path (policies/project-boundaries-model.md).
set(PTYTEST_TEST_TMPDIR "${CMAKE_BINARY_DIR}/tests/tmp")
file(MAKE_DIRECTORY "${PTYTEST_TEST_TMPDIR}")

function(project_test _category _name)
    cmake_parse_arguments(T "" "TIMEOUT;SCRIPT" "SOURCES;LIBS" ${ARGN})
    if(T_SCRIPT)
        # The policy allows script tests; this project has none yet, and
        # support nobody exercises is support nobody knows to work.
        message(FATAL_ERROR "project_test(${_category} ${_name}): SCRIPT tests "
                            "are not supported here yet")
    endif()
    if(NOT T_SOURCES)
        message(FATAL_ERROR "project_test(${_category} ${_name}): SOURCES missing")
    endif()
    if(NOT T_TIMEOUT)
        set(T_TIMEOUT ${PTYTEST_TEST_TIMEOUT})
    endif()
    set(_target "${_category}_${_name}")
    add_executable(${_target} ${T_SOURCES})
    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME ${_name}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/${_category}/bin")
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
    target_compile_options(${_target} PRIVATE ${PTYTEST_WARNINGS})
    target_compile_definitions(${_target} PRIVATE
        PTY_TEST_TMPDIR="${PTYTEST_TEST_TMPDIR}")
    target_link_libraries(${_target} PRIVATE ${T_LIBS})
    add_test(NAME ${_category}.${_name}
             COMMAND ${PTYTEST_TEST_WRAPPER} $<TARGET_FILE:${_target}>)
    set_tests_properties(${_category}.${_name} PROPERTIES
        LABELS ${_category}
        TIMEOUT ${T_TIMEOUT})
endfunction()

# A program a test drives; built into build/tests/<category>/helpers/.
function(project_test_helper _category _name)
    add_executable(${_category}_helper_${_name} ${ARGN})
    set_target_properties(${_category}_helper_${_name} PROPERTIES
        OUTPUT_NAME ${_name}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/${_category}/helpers")
    target_compile_options(${_category}_helper_${_name} PRIVATE ${PTYTEST_WARNINGS})
endfunction()

# --- unit: the emulator and the inspection API, in-process, no child ------
# Linked statically: the emulator tests reach the internal interface.
project_test(unit test_screen  SOURCES unit/test_screen.c  LIBS ptytest_static)
project_test(unit test_inspect SOURCES unit/test_inspect.c LIBS ptytest_static)
target_include_directories(unit_test_screen  PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_include_directories(unit_test_inspect PRIVATE "${CMAKE_SOURCE_DIR}/src")

# --- pty: real programs through a real pseudo-terminal ---------------------
# Linked against the shared library, through the public interface only:
# this is also the check that everything a consumer needs is exported.
project_test_helper(pty pty-fixture pty/pty_fixture.c)
# The example TUI of examples/, driven by its test the way a consuming
# project would drive its own program.
project_test_helper(pty tiny-menu "${CMAKE_SOURCE_DIR}/examples/tiny_menu.c")

project_test(pty test_session   SOURCES pty/test_session.c   LIBS ptytest)
project_test(pty test_tiny_menu SOURCES pty/test_tiny_menu.c LIBS ptytest TIMEOUT 60)
project_test(pty test_render    SOURCES pty/test_render.c    LIBS ptytest)
foreach(_t pty_test_session pty_test_render)
    target_compile_definitions(${_t} PRIVATE
        PTY_FIXTURE="$<TARGET_FILE:pty_helper_pty-fixture>")
    add_dependencies(${_t} pty_helper_pty-fixture)
endforeach()
target_compile_definitions(pty_test_tiny_menu PRIVATE
    PTY_TINY_MENU="$<TARGET_FILE:pty_helper_tiny-menu>")
add_dependencies(pty_test_tiny_menu pty_helper_tiny-menu)
