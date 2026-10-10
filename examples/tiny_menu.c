/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Péter Császár <peter@csaszar.email>
 */
/**
 * @file tiny_menu.c
 * @brief A minimal full-screen menu: the program under test in the example.
 *
 * Up/Down move the selection, Enter chooses, q quits.  Like a curses
 * program it switches to the alternate screen and to application cursor
 * key mode, and draws only with escape sequences.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static const char *items[] = { "Open", "Save", "Quit" };
#define NITEMS 3

static void draw(int sel) {
    printf("\033[H\033[2J\033[1m Tiny menu \033[0m\r\n\r\n");
    for (int i = 0; i < NITEMS; i++) {
        if (i == sel) printf(" \033[7m> %-6s\033[0m\r\n", items[i]);
        else          printf("   %-6s\r\n", items[i]);
    }
    printf("\r\n\033[2mUp/Down, Enter, q\033[0m");
    fflush(stdout);
}

int main(void) {
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) < 0) {
        fprintf(stderr, "tiny-menu: not a terminal\n");
        return 2;
    }
    raw = saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[?1049h\033[?1h\033[?25l");

    int sel = 0, chosen = -1;
    char buf[8];
    draw(sel);
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        if (n == 1 && buf[0] == 'q') break;
        if (n == 1 && buf[0] == '\r') { chosen = sel; break; }
        if (n == 3 && (buf[1] == 'O' || buf[1] == '[')) {
            if (buf[2] == 'A' && sel > 0) sel--;
            if (buf[2] == 'B' && sel < NITEMS - 1) sel++;
        }
        draw(sel);
    }

    printf("\033[?25h\033[?1l\033[?1049l");
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (chosen < 0) return 1;
    printf("Selected: %s\n", items[chosen]);
    return 0;
}
