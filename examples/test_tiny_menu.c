/**
 * @file test_tiny_menu.c
 * @brief Example PTY test, written as a consuming project would write one.
 *
 *   cc -o test_tiny_menu test_tiny_menu.c $(pkg-config --cflags --libs libptytest)
 *   ./test_tiny_menu ./tiny-menu
 */

#include "ptytest.h"
#include "pty_assert.h"
#include <stdlib.h>

int g_tests_run = 0;
int g_tests_failed = 0;

static const char *g_menu;

static void test_menu(void) {
    PtySession *s = pty_open(30, 8);
    ASSERT(s != NULL, "pty_open");
    const char *argv[] = { g_menu, NULL };
    ASSERT(pty_run(s, argv) == 0, "start the menu");

    /* Wait for a condition, never for a clock. */
    ASSERT_WAIT_FOR(s, "Up/Down", 3000);

    /* The whole screen, cell for cell: a shifted column fails this. */
    const char *const first[] = {
        " Tiny menu",
        "",
        " > Open",
        "   Save",
        "   Quit",
        "",
        "Up/Down, Enter, q",
        NULL
    };
    ASSERT_SCREEN_EQ(s, first);
    ASSERT_CELL_ATTR_EQ(s, 0, 1, PTY_ATTR_BOLD);
    ASSERT_CELL_ATTR_EQ(s, 2, 1, PTY_ATTR_REVERSE);
    ASSERT_CELL_ATTR_EQ(s, 2, 8, PTY_ATTR_REVERSE);   /* padded selection */
    ASSERT_CELL_ATTR_EQ(s, 3, 1, PTY_ATTR_NONE);

    /* The menu enabled application cursor mode; pty_send_key follows it. */
    pty_send_key(s, PTY_KEY_DOWN);
    ASSERT_WAIT_FOR(s, "> Save", 3000);
    ASSERT_CELL_ATTR_EQ(s, 2, 1, PTY_ATTR_NONE);
    ASSERT_CELL_ATTR_EQ(s, 3, 1, PTY_ATTR_REVERSE);

    pty_send_key(s, PTY_KEY_ENTER);
    /* Exit status first, then what it left on the main screen. */
    ASSERT_EXIT_STATUS(s, 0, 3000);
    ASSERT_SCREEN_CONTAINS(s, "Selected: Save");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Tiny menu");
    pty_close(s);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s PATH-TO-TINY-MENU\n", argv[0]);
        return 2;
    }
    g_menu = argv[1];
    test_menu();
    printf("%d assertions, %d failed\n", g_tests_run, g_tests_failed);
    /* A floor: an early return must not look like a pass. */
    if (g_tests_run < 14) {
        printf("FAIL: only %d assertions ran\n", g_tests_run);
        return 1;
    }
    return g_tests_failed ? 1 : 0;
}
