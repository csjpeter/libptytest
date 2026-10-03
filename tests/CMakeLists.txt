# Test suites.  One executable per suite; the kind is the CTest label, so
# "ctest -L unit" and "ctest -L pty" run one kind each.
# See policies/testing-model.md.

set(PTYTEST_TEST_TIMEOUT 120)

# A command every test is run under, e.g. Valgrind (see manage.sh valgrind).
set(PTYTEST_TEST_WRAPPER "" CACHE STRING "Command prefix for every test")

# Fixtures written by the PTY suites go under the build directory, never
# into a fixed /tmp path (policies/project-boundaries-model.md).
set(PTYTEST_TEST_TMPDIR "${CMAKE_CURRENT_BINARY_DIR}/tmp")
file(MAKE_DIRECTORY "${PTYTEST_TEST_TMPDIR}")

function(ptytest_add_suite _name _kind _src _lib)
    add_executable(${_name} ${_src})
    target_include_directories(${_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/common")
    target_compile_options(${_name} PRIVATE ${PTYTEST_WARNINGS})
    target_link_libraries(${_name} PRIVATE ${_lib})
    add_test(NAME ${_kind}.${_name} COMMAND ${PTYTEST_TEST_WRAPPER} $<TARGET_FILE:${_name}>)
    set_tests_properties(${_kind}.${_name} PROPERTIES
        LABELS ${_kind}
        TIMEOUT ${PTYTEST_TEST_TIMEOUT})
endfunction()

# --- unit: the emulator and the inspection API, in-process, no child ------
# Linked statically: the emulator tests reach the internal interface.
ptytest_add_suite(test-screen  unit unit/test_screen.c  ptytest_static)
ptytest_add_suite(test-inspect unit unit/test_inspect.c ptytest_static)
target_include_directories(test-screen  PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_include_directories(test-inspect PRIVATE "${CMAKE_SOURCE_DIR}/src")

# --- pty: real programs through a real pseudo-terminal ---------------------
# Linked against the shared library, through the public interface only:
# this is also the check that everything a consumer needs is exported.
add_executable(pty-fixture pty/pty_fixture.c)
target_compile_options(pty-fixture PRIVATE ${PTYTEST_WARNINGS})

ptytest_add_suite(test-session pty pty/test_session.c ptytest)
target_compile_definitions(test-session PRIVATE
    PTY_FIXTURE="$<TARGET_FILE:pty-fixture>"
    PTY_TEST_TMPDIR="${PTYTEST_TEST_TMPDIR}")
add_dependencies(test-session pty-fixture)
