# The example is documentation that runs: a small TUI and the PTY test that
# drives it, written the way a consuming project would write one.  It is
# built here against the in-tree library, and by "./manage.sh check"
# against the built package (see examples/consumer/).

add_executable(tiny-menu tiny_menu.c)
target_compile_options(tiny-menu PRIVATE ${PTYTEST_WARNINGS})

add_executable(test-tiny-menu test_tiny_menu.c)
target_compile_options(test-tiny-menu PRIVATE ${PTYTEST_WARNINGS})
target_link_libraries(test-tiny-menu PRIVATE ptytest::ptytest)
add_test(NAME pty.test-tiny-menu
         COMMAND ${PTYTEST_TEST_WRAPPER} $<TARGET_FILE:test-tiny-menu> $<TARGET_FILE:tiny-menu>)
set_tests_properties(pty.test-tiny-menu PROPERTIES LABELS pty TIMEOUT 60)
