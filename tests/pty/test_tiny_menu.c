/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Péter Császár <peter@csaszar.email>
 */
/**
 * @file test_tiny_menu.c
 * @brief Example PTY test, written as a consuming project would write one:
 *        it drives examples/tiny_menu.c.
 *
 *   cc -o test_tiny_menu test_tiny_menu.c $(pkg-config --cflags --libs libptytest)
 *   ./test_tiny_menu ./tiny-menu
 *
 * Built by this project's tests/CMakeLists.txt, it knows where the menu is
 * (PTY_TINY_MENU) and runs with no arguments, as every test in
 * build/tests/<category>/bin/ must.
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

    /* The whole screen, cell for cell: a shifted column fails this.  Wait
     * for the exact picture, never for a clock, and not for one word of it:
     * a view is drawn in several writes. */
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
    ASSERT_WAIT_SCREEN(s, first, 3000);

    /* Styles as a picture too: one legend character per cell. */
    static const PtyStyleKey legend[] = {
        { '.', PTY_ATTR_NONE,    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 'B', PTY_ATTR_BOLD,    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 'R', PTY_ATTR_REVERSE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 'd', PTY_ATTR_DIM,     PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 0, 0, 0, 0 }
    };
    const char *const first_style[] = {
        "BBBBBBBBBBB.",
        "",
        ".RRRRRRRR...",
        "............",
        "............",
        "",
        "ddddddddddddddddd.",
        NULL
    };
    ASSERT_REGION_STYLE(s, PTY_REGION_FULL, first_style, legend);

    /* The menu enabled application cursor mode; pty_send_key follows it. */
    pty_send_key(s, PTY_KEY_DOWN);

    /* Only the list matters now: a region, with a label for the report.
     * Waiting for "> Save" alone would return while the menu is still being
     * redrawn, before "Quit" is back on the screen. */
    PtyRegion list = PTY_REGION(2, 0, 3, 12);
    list.label = "menu items";
    const char *const second[] = { "   Open", " > Save", "   Quit", NULL };
    ASSERT_WAIT_REGION(s, list, second, 3000);
    const char *const second_style[] = { "............", ".RRRRRRRR...", NULL };
    ASSERT_REGION_STYLE(s, list, second_style, legend);

    pty_send_key(s, PTY_KEY_ENTER);
    /* Exit status first, then what it left on the main screen. */
    ASSERT_EXIT_STATUS(s, 0, 3000);
    ASSERT_SCREEN_CONTAINS(s, "Selected: Save");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Tiny menu");
    pty_close(s);
}

int main(int argc, char **argv) {
#ifdef PTY_TINY_MENU
    g_menu = argc == 2 ? argv[1] : PTY_TINY_MENU;
    if (argc > 2) {
#else
    g_menu = argv[1];
    if (argc != 2) {
#endif
        fprintf(stderr, "usage: %s PATH-TO-TINY-MENU\n", argv[0]);
        return 2;
    }
    test_menu();
    printf("%d assertions, %d failed\n", g_tests_run, g_tests_failed);
    /* A floor: an early return must not look like a pass. */
    if (g_tests_run < 9) {
        printf("FAIL: only %d assertions ran\n", g_tests_run);
        return 1;
    }
    return g_tests_failed ? 1 : 0;
}
