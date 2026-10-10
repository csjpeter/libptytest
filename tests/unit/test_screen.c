/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Proprietary */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-03 */
/* Unit tests of the terminal emulator. */
/**
 * @file test_screen.c
 * @brief Unit tests for the terminal emulator: every sequence the library
 *        claims to understand in libptytest(7) has a test here that fails
 *        if it is mis-handled.
 */

#include "test_helpers.h"
#include "pty_internal.h"

int g_tests_run = 0;
int g_tests_failed = 0;

/* A session with no child: pty_feed() drives the emulator directly. */
static PtySession *term(int cols, int rows) {
    PtySession *s = pty_open(cols, rows);
    if (!s) {
        printf("  [FATAL] pty_open(%d, %d) failed\n", cols, rows);
        exit(EXIT_FAILURE);   /* a fixture that cannot be created fails */
    }
    return s;
}

static void feed(PtySession *s, const char *text) {
    pty_feed(s, text, strlen(text));
}

static const char *row(PtySession *s, int r) {
    static char buf[1024];
    return pty_row_text(s, r, buf, sizeof(buf));
}

#define ASSERT_ROW_EQ(s, r, text) do { \
    g_tests_run++; \
    const char *_got = row(s, r); \
    if (strcmp(_got, text) != 0) { \
        PTY_ASSERT_FAIL_("row %d should be \"%s\" but is \"%s\"", r, text, _got); \
        pty_close(s); \
        return; \
    } \
} while (0)

/* ── Text and cursor ─────────────────────────────────────────────────── */

static void test_basic_write(void) {
    PtySession *s = term(20, 5);
    feed(s, "Hello");
    ASSERT_ROW_EQ(s, 0, "Hello");
    ASSERT_CELL_TEXT(s, 0, 0, "H");
    ASSERT_CELL_TEXT(s, 0, 4, "o");
    ASSERT_CURSOR(s, 0, 5);
    pty_close(s);
}

static void test_newline_keeps_column(void) {
    PtySession *s = term(20, 5);
    feed(s, "AB\nCD");
    /* LF moves down only; ONLCR is the tty's business, not the terminal's */
    ASSERT_ROW_EQ(s, 0, "AB");
    ASSERT_ROW_EQ(s, 1, "  CD");
    pty_close(s);
}

static void test_cr_lf(void) {
    PtySession *s = term(20, 5);
    feed(s, "AB\r\nCD");
    ASSERT_ROW_EQ(s, 1, "CD");
    ASSERT_CURSOR(s, 1, 2);
    pty_close(s);
}

static void test_backspace(void) {
    PtySession *s = term(20, 5);
    feed(s, "ABC\b\bx");
    ASSERT_ROW_EQ(s, 0, "AxC");
    feed(s, "\r\b\bZ");   /* backspace stops at column 0 */
    ASSERT_ROW_EQ(s, 0, "ZxC");
    pty_close(s);
}

static void test_cup(void) {
    PtySession *s = term(20, 5);
    feed(s, "\033[4;6H*");
    ASSERT_CELL_TEXT(s, 3, 5, "*");
    feed(s, "\033[H#");                 /* defaults to 1;1 */
    ASSERT_CELL_TEXT(s, 0, 0, "#");
    feed(s, "\033[;3H$");               /* missing row defaults to 1 */
    ASSERT_CELL_TEXT(s, 0, 2, "$");
    feed(s, "\033[99;99H%");            /* clamped to the screen */
    ASSERT_CELL_TEXT(s, 4, 19, "%");
    feed(s, "\033[2;2f&");              /* HVP is CUP */
    ASSERT_CELL_TEXT(s, 1, 1, "&");
    pty_close(s);
}

static void test_relative_moves(void) {
    PtySession *s = term(20, 10);
    feed(s, "\033[5;5H");
    feed(s, "\033[2A");  ASSERT_CURSOR(s, 2, 4);
    feed(s, "\033[3B");  ASSERT_CURSOR(s, 5, 4);
    feed(s, "\033[4C");  ASSERT_CURSOR(s, 5, 8);
    feed(s, "\033[2D");  ASSERT_CURSOR(s, 5, 6);
    feed(s, "\033[A");   ASSERT_CURSOR(s, 4, 6);   /* default count 1 */
    feed(s, "\033[0B");  ASSERT_CURSOR(s, 5, 6);   /* 0 means 1 */
    feed(s, "\033[99A"); ASSERT_CURSOR(s, 0, 6);
    feed(s, "\033[99D"); ASSERT_CURSOR(s, 0, 0);
    feed(s, "\033[99C"); ASSERT_CURSOR(s, 0, 19);
    feed(s, "\033[99B"); ASSERT_CURSOR(s, 9, 19);
    feed(s, "\033[3a");  ASSERT_CURSOR(s, 9, 19);  /* HPR, clamped */
    feed(s, "\033[1;1H\033[2e"); ASSERT_CURSOR(s, 2, 0); /* VPR */
    pty_close(s);
}

static void test_absolute_moves(void) {
    PtySession *s = term(20, 10);
    feed(s, "\033[3;3H\033[7G");  ASSERT_CURSOR(s, 2, 6);   /* CHA */
    feed(s, "\033[5d");           ASSERT_CURSOR(s, 4, 6);   /* VPA */
    feed(s, "\033[2`");           ASSERT_CURSOR(s, 4, 1);   /* HPA */
    feed(s, "\033[2E");           ASSERT_CURSOR(s, 6, 0);   /* CNL */
    feed(s, "\033[5;5H\033[3F");  ASSERT_CURSOR(s, 1, 0);   /* CPL */
    pty_close(s);
}

/* ── Erasing ─────────────────────────────────────────────────────────── */

static void fill(PtySession *s, int rows) {
    for (int r = 0; r < rows; r++) {
        char line[48];
        snprintf(line, sizeof(line), "\033[%d;1H%d123456789", r + 1, r);
        feed(s, line);
    }
}

static void test_erase_display(void) {
    PtySession *s = term(10, 3);
    fill(s, 3);
    feed(s, "\033[2;5H\033[J");          /* ED 0: cursor to end */
    ASSERT_ROW_EQ(s, 0, "0123456789");
    ASSERT_ROW_EQ(s, 1, "1123");
    ASSERT_ROW_EQ(s, 2, "");

    fill(s, 3);
    feed(s, "\033[2;5H\033[1J");         /* ED 1: start to cursor, inclusive */
    ASSERT_ROW_EQ(s, 0, "");
    ASSERT_ROW_EQ(s, 1, "     56789");
    ASSERT_ROW_EQ(s, 2, "2123456789");

    fill(s, 3);
    feed(s, "\033[2J");
    ASSERT(pty_screen_is_blank(s), "ED 2 clears everything");
    ASSERT_CURSOR(s, 2, 9);              /* ED does not move the cursor */
    fill(s, 3);
    feed(s, "\033[3J");
    ASSERT(pty_screen_is_blank(s), "ED 3 clears the screen too");
    pty_close(s);
}

static void test_erase_line(void) {
    PtySession *s = term(10, 3);
    feed(s, "0123456789\033[1;6H\033[K");
    ASSERT_ROW_EQ(s, 0, "01234");
    feed(s, "\r0123456789\033[1;6H\033[1K");
    ASSERT_ROW_EQ(s, 0, "      6789");
    feed(s, "\033[2K");
    ASSERT_ROW_EQ(s, 0, "");
    pty_close(s);
}

static void test_erase_insert_delete_chars(void) {
    PtySession *s = term(10, 3);
    feed(s, "0123456789\033[1;3H\033[3X");   /* ECH */
    ASSERT_ROW_EQ(s, 0, "01   56789");
    ASSERT_CURSOR(s, 0, 2);
    feed(s, "\r0123456789\033[1;3H\033[2@");  /* ICH */
    ASSERT_ROW_EQ(s, 0, "01  234567");
    feed(s, "\r0123456789\033[1;3H\033[3P");  /* DCH */
    ASSERT_ROW_EQ(s, 0, "0156789");
    feed(s, "\r0123456789\033[1;3H\033[99P"); /* DCH beyond the end */
    ASSERT_ROW_EQ(s, 0, "01");
    pty_close(s);
}

static void test_background_colour_erase(void) {
    PtySession *s = term(10, 3);
    feed(s, "\033[44m\033[2J\033[0m");
    ASSERT_CELL_BG(s, 0, 0, PTY_BG_BLUE);
    ASSERT_CELL_BG(s, 2, 9, PTY_BG_BLUE);
    feed(s, "\033[1;1H\033[41m\033[K");
    ASSERT_CELL_BG(s, 0, 5, PTY_BG_RED);
    ASSERT_CELL_BG(s, 1, 5, PTY_BG_BLUE);
    pty_close(s);
}

/* ── Scrolling ───────────────────────────────────────────────────────── */

static void test_scroll_at_bottom(void) {
    PtySession *s = term(10, 3);
    feed(s, "AAA\r\nBBB\r\nCCC\r\nDDD");
    ASSERT_ROW_EQ(s, 0, "BBB");
    ASSERT_ROW_EQ(s, 1, "CCC");
    ASSERT_ROW_EQ(s, 2, "DDD");
    pty_close(s);
}

static void test_scroll_region(void) {
    PtySession *s = term(10, 5);
    feed(s, "head\r\n1\r\n2\r\n3\r\nfoot");
    feed(s, "\033[2;4r");                 /* region rows 2..4, cursor homes */
    ASSERT_CURSOR(s, 0, 0);
    feed(s, "\033[4;1H\nnew");            /* LF at the bottom margin */
    ASSERT_ROW_EQ(s, 0, "head");
    ASSERT_ROW_EQ(s, 1, "2");
    ASSERT_ROW_EQ(s, 2, "3");
    ASSERT_ROW_EQ(s, 3, "new");
    ASSERT_ROW_EQ(s, 4, "foot");
    feed(s, "\033[2;1H\033M");            /* RI at the top margin */
    ASSERT_ROW_EQ(s, 1, "");
    ASSERT_ROW_EQ(s, 2, "2");
    ASSERT_ROW_EQ(s, 3, "3");
    ASSERT_ROW_EQ(s, 4, "foot");
    feed(s, "\033[S");                    /* SU inside the region */
    ASSERT_ROW_EQ(s, 1, "2");
    ASSERT_ROW_EQ(s, 3, "");
    feed(s, "\033[2T");                   /* SD inside the region */
    ASSERT_ROW_EQ(s, 1, "");
    ASSERT_ROW_EQ(s, 3, "2");
    ASSERT_ROW_EQ(s, 0, "head");
    ASSERT_ROW_EQ(s, 4, "foot");
    feed(s, "\033[1;5;1;1;5T");           /* mouse tracking, not SD */
    ASSERT_ROW_EQ(s, 3, "2");
    feed(s, "\033[5;9r");                 /* invalid: ignored */
    feed(s, "\033[r");                    /* reset to the full screen */
    feed(s, "\033[5;1H\nX");
    ASSERT_ROW_EQ(s, 0, "");              /* "head" scrolled away */
    pty_close(s);
}

static void test_index_and_next_line(void) {
    PtySession *s = term(10, 3);
    feed(s, "ab\033Dc");                  /* IND: down, same column */
    ASSERT_ROW_EQ(s, 1, "  c");
    feed(s, "\033Ed");                    /* NEL: down to column 0 */
    ASSERT_ROW_EQ(s, 2, "d");
    feed(s, "\033[1;1H\033Mtop");         /* RI at row 0 scrolls down */
    ASSERT_ROW_EQ(s, 0, "top");
    ASSERT_ROW_EQ(s, 1, "ab");
    pty_close(s);
}

static void test_insert_delete_lines(void) {
    PtySession *s = term(10, 4);
    feed(s, "A\r\nB\r\nC\r\nD");
    feed(s, "\033[2;3H\033[L");           /* IL at row 1, cursor to col 0 */
    ASSERT_CURSOR(s, 1, 0);
    ASSERT_ROW_EQ(s, 0, "A");
    ASSERT_ROW_EQ(s, 1, "");
    ASSERT_ROW_EQ(s, 2, "B");
    ASSERT_ROW_EQ(s, 3, "C");
    feed(s, "\033[2M");                   /* DL 2 */
    ASSERT_ROW_EQ(s, 1, "C");
    ASSERT_ROW_EQ(s, 2, "");
    ASSERT_ROW_EQ(s, 3, "");
    feed(s, "\033[2;3r\033[4;1H\033[L");  /* outside the region: no effect */
    ASSERT_ROW_EQ(s, 1, "C");
    pty_close(s);
}

/* ── Attributes and colours ──────────────────────────────────────────── */

static void test_sgr_attributes(void) {
    PtySession *s = term(30, 3);
    feed(s, "\033[1mB\033[2mD\033[3mI\033[4mU\033[5mK\033[7mR\033[8mH\033[9mS\033[0mN");
    ASSERT_CELL_ATTR_EQ(s, 0, 0, PTY_ATTR_BOLD);
    ASSERT_CELL_ATTR_EQ(s, 0, 1, PTY_ATTR_BOLD | PTY_ATTR_DIM);
    ASSERT_CELL_ATTR(s, 0, 2, PTY_ATTR_ITALIC);
    ASSERT_CELL_ATTR(s, 0, 3, PTY_ATTR_UNDERLINE);
    ASSERT_CELL_ATTR(s, 0, 4, PTY_ATTR_BLINK);
    ASSERT_CELL_ATTR(s, 0, 5, PTY_ATTR_REVERSE);
    ASSERT_CELL_ATTR(s, 0, 6, PTY_ATTR_INVISIBLE);
    ASSERT_CELL_ATTR(s, 0, 7, PTY_ATTR_STRIKE);
    ASSERT_CELL_ATTR_EQ(s, 0, 8, PTY_ATTR_NONE);

    feed(s, "\r\n\033[1;2;3;4;5;7;8;9m\033[22ma\033[23mb\033[24mc\033[25md"
            "\033[27me\033[28mf\033[29mg");
    ASSERT_CELL_ATTR_EQ(s, 1, 0, PTY_ATTR_ITALIC | PTY_ATTR_UNDERLINE | PTY_ATTR_BLINK |
                                 PTY_ATTR_REVERSE | PTY_ATTR_INVISIBLE | PTY_ATTR_STRIKE);
    ASSERT_CELL_ATTR_EQ(s, 1, 6, PTY_ATTR_NONE);

    feed(s, "\033[m\033[4:3mx\033[4:0my\033[21mz\033[6mw\033[m");
    ASSERT_CELL_ATTR_EQ(s, 1, 7, PTY_ATTR_UNDERLINE);
    ASSERT_CELL_ATTR_EQ(s, 1, 8, PTY_ATTR_NONE);
    ASSERT_CELL_ATTR_EQ(s, 1, 9, PTY_ATTR_UNDERLINE);
    ASSERT_CELL_ATTR_EQ(s, 1, 10, PTY_ATTR_UNDERLINE | PTY_ATTR_BLINK);
    pty_close(s);
}

static void test_sgr_colours(void) {
    PtySession *s = term(30, 3);
    feed(s, "\033[31ma\033[92mb\033[39mc\033[44md\033[103me\033[49mf");
    ASSERT_CELL_FG(s, 0, 0, PTY_FG_RED);
    ASSERT_CELL_FG(s, 0, 1, 92);
    ASSERT_CELL_FG(s, 0, 2, PTY_FG_DEFAULT);
    ASSERT_CELL_BG(s, 0, 3, PTY_BG_BLUE);
    ASSERT_CELL_FG(s, 0, 3, PTY_FG_DEFAULT);
    ASSERT_CELL_BG(s, 0, 4, 103);
    ASSERT_CELL_BG(s, 0, 5, PTY_BG_DEFAULT);

    feed(s, "\033[38;5;196mg\033[48;5;21mh\033[0m");
    ASSERT_CELL_FG(s, 0, 6, PTY_COLOR_IDX(196));
    ASSERT_CELL_BG(s, 0, 7, PTY_COLOR_IDX(21));
    ASSERT_CELL_FG(s, 0, 7, PTY_COLOR_IDX(196));

    feed(s, "\033[38;2;255;128;0;48;2;0;0;255mi\033[0mj");
    ASSERT_CELL_FG(s, 0, 8, PTY_COLOR_RGB(255, 128, 0));
    ASSERT_CELL_BG(s, 0, 8, PTY_COLOR_RGB(0, 0, 255));
    ASSERT_CELL_FG(s, 0, 9, PTY_FG_DEFAULT);
    ASSERT_CELL_BG(s, 0, 9, PTY_BG_DEFAULT);

    /* colon forms, with and without the colour space id */
    feed(s, "\033[38:5:9mk\033[38:2::1:2:3ml\033[48:2:4:5:6mm\033[0m");
    ASSERT_CELL_FG(s, 0, 10, PTY_COLOR_IDX(9));
    ASSERT_CELL_FG(s, 0, 11, PTY_COLOR_RGB(1, 2, 3));
    ASSERT_CELL_BG(s, 0, 12, PTY_COLOR_RGB(4, 5, 6));

    /* a parameter after an extended colour still applies */
    feed(s, "\033[38;5;1;1mn\033[58;5;3;4mo\033[0m");
    ASSERT_CELL_FG(s, 0, 13, PTY_COLOR_IDX(1));
    ASSERT_CELL_ATTR_EQ(s, 0, 13, PTY_ATTR_BOLD);
    ASSERT_CELL_FG(s, 0, 14, PTY_COLOR_IDX(1));   /* 58 is underline colour */
    ASSERT_CELL_ATTR_EQ(s, 0, 14, PTY_ATTR_BOLD | PTY_ATTR_UNDERLINE);

    /* truncated extended colour: no crash, nothing after it is invented */
    feed(s, "\033[0m\033[38;2;1mp\033[38;5mq\033[38mr");
    ASSERT_CELL_TEXT(s, 0, 15, "p");
    ASSERT_CELL_TEXT(s, 0, 17, "r");
    ASSERT(PTY_COLOR_IS_IDX(PTY_COLOR_IDX(5)) && !PTY_COLOR_IS_RGB(PTY_COLOR_IDX(5)),
           "colour classification macros");
    pty_close(s);
}

/* ── Wrapping ────────────────────────────────────────────────────────── */

static void test_pending_wrap(void) {
    PtySession *s = term(5, 3);
    feed(s, "abcde");
    ASSERT_CURSOR(s, 0, 4);               /* stays in the last column */
    ASSERT_ROW_EQ(s, 1, "");
    feed(s, "\033[31mf");                 /* SGR does not cancel the wrap */
    ASSERT_ROW_EQ(s, 0, "abcde");
    ASSERT_ROW_EQ(s, 1, "f");
    ASSERT_CELL_FG(s, 1, 0, PTY_FG_RED);

    feed(s, "\033[0m\r\nvwxyz\rA");       /* CR cancels the wrap */
    ASSERT_ROW_EQ(s, 2, "Awxyz");
    feed(s, "\033[3;5HZ\033[3;5HY");       /* so does a cursor move */
    ASSERT_ROW_EQ(s, 2, "AwxyY");
    feed(s, "\033[3;5HQ\nR");             /* LF at the bottom with wrap pending */
    ASSERT_ROW_EQ(s, 1, "AwxyQ");
    pty_close(s);
}

static void test_autowrap_off(void) {
    PtySession *s = term(5, 3);
    feed(s, "\033[?7labcdefg");
    ASSERT_ROW_EQ(s, 0, "abcdg");
    ASSERT_ROW_EQ(s, 1, "");
    feed(s, "\033[?7h\r\nabcdefg");
    ASSERT_ROW_EQ(s, 1, "abcde");
    ASSERT_ROW_EQ(s, 2, "fg");
    pty_close(s);
}

/* ── Parser robustness ───────────────────────────────────────────────── */

static void test_sequence_split_across_reads(void) {
    PtySession *s = term(20, 3);
    /* Every possible split point of a sequence must give the same screen:
     * a PTY read can end anywhere. */
    const char *seq = "\033[1;31mR\033]0;title\007\xC3\xA9\033[2;3HZ";
    size_t len = strlen(seq);
    for (size_t cut = 1; cut < len; cut++) {
        feed(s, "\033[2J\033[H\033[0m");
        pty_feed(s, seq, cut);
        pty_feed(s, seq + cut, len - cut);
        g_tests_run++;
        if (strcmp(pty_cell_text(s, 0, 0), "R") != 0 ||
            strcmp(pty_cell_text(s, 0, 1), "\xC3\xA9") != 0 ||
            pty_cell_fg(s, 0, 0) != PTY_FG_RED ||
            strcmp(pty_cell_text(s, 1, 2), "Z") != 0 ||
            strcmp(row(s, 0), "R\xC3\xA9") != 0) {
            PTY_ASSERT_FAIL_("split at byte %zu: row 0 \"%s\", row 1 \"%s\"",
                             cut, row(s, 0), row(s, 1));
            pty_close(s);
            return;
        }
    }
    pty_close(s);
}

static void test_byte_by_byte(void) {
    PtySession *s = term(20, 3);
    const char *seq = "\033[38;2;10;20;30mX\033[0m\xE6\x97\xA5";
    for (const char *p = seq; *p; p++) pty_feed(s, p, 1);
    ASSERT_CELL_TEXT(s, 0, 0, "X");
    ASSERT_CELL_FG(s, 0, 0, PTY_COLOR_RGB(10, 20, 30));
    ASSERT_CELL_TEXT(s, 0, 1, "\xE6\x97\xA5");
    ASSERT_CURSOR(s, 0, 3);
    pty_close(s);
}

static void test_strings_are_skipped(void) {
    PtySession *s = term(30, 3);
    feed(s, "a\033]0;window title\007b");             /* OSC, BEL */
    feed(s, "\033]2;other\033\\c");                     /* OSC, ST */
    feed(s, "\033Pq#0;2;0;0;0#1~~@@\033\\d");           /* DCS (sixel) */
    feed(s, "\033_apc\033\\e\033^pm\033\\f\033Xsos\033\\g");
    feed(s, "\033]8;;http://x\033\\h\033]8;;\033\\");   /* hyperlink */
    ASSERT_ROW_EQ(s, 0, "abcdefgh");
    feed(s, "\033]0;cut\033[1mB");   /* ESC that is not ST ends the string */
    ASSERT_CELL_TEXT(s, 0, 8, "B");
    ASSERT_CELL_ATTR(s, 0, 8, PTY_ATTR_BOLD);
    feed(s, "\033[0m\033]0;x\x18y");                    /* CAN aborts */
    ASSERT_CELL_TEXT(s, 0, 9, "y");
    pty_close(s);
}

static void test_unhandled_sequences_print_nothing(void) {
    PtySession *s = term(30, 3);
    feed(s, "\033[>4;1m"          /* modifyOtherKeys */
            "\033[?2004h"         /* bracketed paste */
            "\033[?1000;1006h"    /* mouse */
            "\033[22;0;0t"        /* window ops */
            "\033[2 q"            /* cursor style */
            "\033[=5u"            /* kitty keyboard */
            "\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;"
            "26;27;28;29;30;31;32;33;34;35;36x"   /* overlong parameter list */
            "\033[99999999999999m" /* huge number */
            "\033[1$}"             /* intermediate before final */
            "\033[1?m"             /* private marker after a digit */
            "\033[ 1m"             /* digit after an intermediate */
            "\033=\033>"           /* keypad modes */
            "\033%G\033 F"         /* charset / conformance */
            "ok");
    ASSERT_ROW_EQ(s, 0, "ok");
    ASSERT_CELL_ATTR_EQ(s, 0, 0, PTY_ATTR_NONE);
    feed(s, "\033[1\030m");       /* CAN aborts the CSI; 'm' is printed */
    ASSERT_ROW_EQ(s, 0, "okm");
    feed(s, "\033[1\032n");       /* so does SUB */
    ASSERT_ROW_EQ(s, 0, "okmn");
    feed(s, "\033[2\nCx");         /* C0 inside a CSI is executed, */
    ASSERT_ROW_EQ(s, 1, "      x");   /* and the CSI (CUF 2) still completes */
    feed(s, "\x07\x01\x7F");      /* BEL, SOH, DEL: invisible */
    ASSERT_ROW_EQ(s, 1, "      x");
    pty_close(s);
}

/* ── Unicode ─────────────────────────────────────────────────────────── */

static void test_utf8(void) {
    PtySession *s = term(20, 3);
    feed(s, "\xC3\xA1"            /* á, 2 bytes */
            "\xE2\x82\xAC"        /* €, 3 bytes */
            "\xF0\x9D\x84\x9E");  /* 𝄞, 4 bytes, narrow */
    ASSERT_CELL_TEXT(s, 0, 0, "\xC3\xA1");
    ASSERT_CELL_TEXT(s, 0, 1, "\xE2\x82\xAC");
    ASSERT_CELL_TEXT(s, 0, 2, "\xF0\x9D\x84\x9E");
    ASSERT_CURSOR(s, 0, 3);

    feed(s, "\r\n\xFF" "a" "\xC3" "b" "\xE2\x82" "\033[1mc\033[0m" "\xED\xA0\x80");
    ASSERT_CELL_TEXT(s, 1, 0, "\xEF\xBF\xBD");   /* invalid lead byte */
    ASSERT_CELL_TEXT(s, 1, 1, "a");
    ASSERT_CELL_TEXT(s, 1, 2, "\xEF\xBF\xBD");   /* truncated, then 'b' */
    ASSERT_CELL_TEXT(s, 1, 3, "b");
    ASSERT_CELL_TEXT(s, 1, 4, "\xEF\xBF\xBD");   /* truncated by ESC */
    ASSERT_CELL_TEXT(s, 1, 5, "c");
    ASSERT_CELL_TEXT(s, 1, 6, "\xEF\xBF\xBD");   /* surrogate */

    feed(s, "\r\n\xC2\x9B" "x");                 /* C1 CSI: not printed */
    ASSERT_ROW_EQ(s, 2, "x");
    pty_close(s);
}

static void test_wide_characters(void) {
    PtySession *s = term(6, 3);
    feed(s, "\xE6\x97\xA5\xE6\x9C\xAC!");        /* 日本! */
    ASSERT_CELL_TEXT(s, 0, 0, "\xE6\x97\xA5");
    ASSERT_CELL_TEXT(s, 0, 1, "");               /* tail of the wide char */
    ASSERT_CELL_TEXT(s, 0, 2, "\xE6\x9C\xAC");
    ASSERT_CELL_TEXT(s, 0, 4, "!");
    ASSERT_CURSOR(s, 0, 5);
    ASSERT_ROW_EQ(s, 0, "\xE6\x97\xA5\xE6\x9C\xAC!");

    feed(s, "\033[1;2Hx");                       /* overwrite the tail */
    ASSERT_ROW_EQ(s, 0, " x\xE6\x9C\xAC!");
    feed(s, "\033[1;3Hy");                       /* overwrite a head */
    ASSERT_ROW_EQ(s, 0, " xy !");

    feed(s, "\033[2;6H\xE6\x97\xA5");            /* no room: wraps */
    ASSERT_ROW_EQ(s, 1, "");
    ASSERT_CELL_TEXT(s, 2, 0, "\xE6\x97\xA5");

    feed(s, "\033[3;1H\033[K\xE6\x97\xA5\xE6\x9C\xAC\033[3;2H\033[K");
    ASSERT_ROW_EQ(s, 2, "");                     /* erase from a tail */
    feed(s, "\033[3;1H\xE6\x97\xA5\033[3;1H\033[P");
    ASSERT_ROW_EQ(s, 2, "");                     /* delete a head */
    feed(s, "\033[3;1Hab\xE6\x97\xA5\033[3;1H\033[@");
    ASSERT_ROW_EQ(s, 2, " ab\xE6\x97\xA5");      /* insert before it */
    feed(s, "\033[2@");
    ASSERT_ROW_EQ(s, 2, "   ab");                /* half pushed off: no stray head */
    pty_close(s);
}

static void test_combining_characters(void) {
    PtySession *s = term(10, 3);
    feed(s, "e\xCC\x81x");                       /* e + U+0301 */
    ASSERT_CELL_TEXT(s, 0, 0, "e\xCC\x81");
    ASSERT_CELL_TEXT(s, 0, 1, "x");
    ASSERT_CURSOR(s, 0, 2);
    feed(s, "\r\n\xCC\x81");                     /* nothing to combine with */
    ASSERT_ROW_EQ(s, 1, "");
    pty_close(s);
}

static void test_char_width(void) {
    ASSERT(pty_char_width('A') == 1, "ASCII is narrow");
    ASSERT(pty_char_width(0x65E5) == 2, "CJK is wide");
    ASSERT(pty_char_width(0x0301) == 0, "combining acute is zero width");
    ASSERT(pty_char_width(0x00E9) == 1, "Latin-1 is narrow");
}

/* ── Modes and state ─────────────────────────────────────────────────── */

static void test_tabs(void) {
    PtySession *s = term(30, 3);
    feed(s, "a\tb\tc");
    ASSERT_CELL_TEXT(s, 0, 8, "b");
    ASSERT_CELL_TEXT(s, 0, 16, "c");
    feed(s, "\033[1;4H\033H\033[1;1H\tX");       /* HTS at column 3 */
    ASSERT_CELL_TEXT(s, 0, 3, "X");
    feed(s, "\033[1;4H\033[g\033[1;1H\tY");      /* TBC 0 clears it */
    ASSERT_CELL_TEXT(s, 0, 8, "Y");
    feed(s, "\033[1;1H\033[2IZ");                /* CHT 2 */
    ASSERT_CELL_TEXT(s, 0, 16, "Z");
    feed(s, "\033[1;20H\033[ZW");                /* CBT */
    ASSERT_CELL_TEXT(s, 0, 16, "W");
    feed(s, "\033[3g\033[1;1H\tV");              /* TBC 3: none left */
    ASSERT_CELL_TEXT(s, 0, 29, "V");
    feed(s, "\033[2;10H\033[ZU");                /* CBT with none: column 0 */
    ASSERT_CELL_TEXT(s, 1, 0, "U");
    pty_close(s);
}

static void test_save_restore_cursor(void) {
    PtySession *s = term(20, 5);
    feed(s, "\033[2;3H\033[1;32m\0337\033[0m\033[5;5Hx\0338y");
    ASSERT_CELL_TEXT(s, 1, 2, "y");
    ASSERT_CELL_ATTR(s, 1, 2, PTY_ATTR_BOLD);
    ASSERT_CELL_FG(s, 1, 2, PTY_FG_GREEN);
    feed(s, "\033[0m\033[3;4H\033[s\033[1;1H\033[uz");
    ASSERT_CELL_TEXT(s, 2, 3, "z");
    feed(s, "\033[4;4H\033[?1048h\033[1;1H\033[?1048lw");
    ASSERT_CELL_TEXT(s, 3, 3, "w");
    pty_close(s);
}

static void test_alternate_screen(void) {
    PtySession *s = term(10, 3);
    feed(s, "main\033[2;2H");
    feed(s, "\033[?1049h");
    ASSERT(pty_screen_is_blank(s), "alternate screen starts blank");
    feed(s, "\033[1;1Halt");
    ASSERT_ROW_EQ(s, 0, "alt");
    feed(s, "\033[?1049l");
    ASSERT_ROW_EQ(s, 0, "main");
    ASSERT_CURSOR(s, 1, 1);                      /* cursor restored */
    feed(s, "\033[?1049h");
    ASSERT(pty_screen_is_blank(s), "alternate screen is cleared on re-entry");
    feed(s, "\033[?1049l\033[?47hX\033[?47l");   /* 47: no clear, no cursor */
    ASSERT_ROW_EQ(s, 0, "main");
    feed(s, "\033[?47h");
    ASSERT_CELL_TEXT(s, 1, 1, "X");
    feed(s, "\033[?1047l\033[?1047h");           /* 1047 clears on leaving */
    ASSERT(pty_screen_is_blank(s), "1047 cleared the alternate screen");
    feed(s, "\033[?1047l");
    ASSERT_ROW_EQ(s, 0, "main");
    pty_close(s);
}

static void test_dec_graphics(void) {
    PtySession *s = term(10, 3);
    feed(s, "\033(0lqqk\033(Bq");
    ASSERT_ROW_EQ(s, 0, "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90q");
    feed(s, "\r\n\033)0a\016x\017x");            /* G1 via SO / SI */
    ASSERT_CELL_TEXT(s, 1, 0, "a");
    ASSERT_CELL_TEXT(s, 1, 1, "\xE2\x94\x82");
    ASSERT_CELL_TEXT(s, 1, 2, "x");
    pty_close(s);
}

static void test_insert_mode(void) {
    PtySession *s = term(10, 3);
    feed(s, "abcd\033[1;2H\033[4hXY\033[4lZ");
    ASSERT_ROW_EQ(s, 0, "aXYZcd");
    feed(s, "\033[2;1H123456789\033[2;1H\033[4h\xE6\x97\xA5\033[4l");
    ASSERT_ROW_EQ(s, 1, "\xE6\x97\xA5" "12345678");
    pty_close(s);
}

static void test_repeat(void) {
    PtySession *s = term(10, 3);
    feed(s, "-\033[4b|");
    ASSERT_ROW_EQ(s, 0, "-----|");
    pty_close(s);
}

static void test_cursor_visibility(void) {
    PtySession *s = term(10, 3);
    int r, c;
    ASSERT(pty_cursor(s, &r, &c) == 1, "cursor visible by default");
    feed(s, "\033[?25l");
    ASSERT(pty_cursor(s, &r, &c) == 0, "?25l hides the cursor");
    feed(s, "\033[?25h");
    ASSERT(pty_cursor(s, &r, &c) == 1, "?25h shows it");
    pty_close(s);
}

static void test_origin_mode(void) {
    PtySession *s = term(10, 6);
    feed(s, "\033[3;5r\033[?6h");
    ASSERT_CURSOR(s, 2, 0);                      /* home is the region top */
    feed(s, "\033[2;2Hx");
    ASSERT_CELL_TEXT(s, 3, 1, "x");
    feed(s, "\033[9;1H");
    ASSERT_CURSOR(s, 4, 0);                      /* clamped to the region */
    feed(s, "\033[?6l");
    ASSERT_CURSOR(s, 0, 0);
    pty_close(s);
}

static void test_resets(void) {
    PtySession *s = term(10, 3);
    feed(s, "\033[1;31m\033[?25l\033[2;3rtext\033c");
    ASSERT(pty_screen_is_blank(s), "RIS clears the screen");
    ASSERT_CURSOR(s, 0, 0);
    int r, c;
    ASSERT(pty_cursor(s, &r, &c) == 1, "RIS shows the cursor");
    feed(s, "x");
    ASSERT_CELL_ATTR_EQ(s, 0, 0, PTY_ATTR_NONE);
    ASSERT_CELL_FG(s, 0, 0, PTY_FG_DEFAULT);

    feed(s, "\033[1;4m\033[?25l\033[!py");       /* DECSTR keeps the screen */
    ASSERT_CELL_TEXT(s, 0, 0, "x");
    ASSERT_CELL_ATTR_EQ(s, 0, 1, PTY_ATTR_NONE);
    ASSERT(pty_cursor(s, &r, &c) == 1, "DECSTR shows the cursor");

    feed(s, "\033#8");                           /* DECALN */
    ASSERT_ROW_EQ(s, 0, "EEEEEEEEEE");
    ASSERT_ROW_EQ(s, 2, "EEEEEEEEEE");
    ASSERT_CURSOR(s, 0, 0);
    pty_close(s);
}

static void test_terminal_responses(void) {
    PtyScreen *scr = pty_screen_new(20, 10);
    ASSERT(scr != NULL, "screen allocated");
    const char *q = "\033[4;7H\033[6n";
    pty_screen_feed(scr, q, strlen(q));
    ASSERT(scr->resp_len == 6 && memcmp(scr->resp, "\033[4;7R", 6) == 0,
           "DSR 6 reports the cursor position, 1-based");
    scr->resp_len = 0;
    q = "\033[5n\033[c\033[>c";
    pty_screen_feed(scr, q, strlen(q));
    const char *want = "\033[0n\033[?62;22c\033[>1;10;0c";
    g_tests_run++;
    if (scr->resp_len != strlen(want) || memcmp(scr->resp, want, scr->resp_len) != 0) {
        PTY_ASSERT_FAIL_("DSR 5 / DA1 / DA2 answers: got %zu bytes", scr->resp_len);
        pty_screen_free(scr);
        return;
    }
    scr->resp_len = 0;
    q = "\033[3;6r\033[?6h\033[2;2H\033[6n";
    pty_screen_feed(scr, q, strlen(q));
    ASSERT(scr->resp_len == 6 && memcmp(scr->resp, "\033[2;2R", 6) == 0,
           "DSR 6 is relative to the region in origin mode");
    /* A reader that never reads: the buffer is bounded, not overrun. */
    for (int i = 0; i < 200; i++) pty_screen_feed(scr, "\033[6n", 4);
    ASSERT(scr->resp_len <= sizeof(scr->resp), "response buffer is bounded");
    pty_screen_free(scr);
}

static void test_resize(void) {
    PtySession *s = term(10, 4);
    feed(s, "0123456789\r\nabc\033[4;10H");
    ASSERT(pty_resize(s, 5, 2) == 0, "shrink");
    int cols, rows;
    pty_get_size(s, &cols, &rows);
    ASSERT(cols == 5 && rows == 2, "size reported after shrinking");
    ASSERT_ROW_EQ(s, 0, "01234");
    ASSERT_ROW_EQ(s, 1, "abc");
    ASSERT_CURSOR(s, 1, 4);                      /* clamped */
    ASSERT(pty_resize(s, 8, 3) == 0, "grow");
    ASSERT_ROW_EQ(s, 0, "01234");
    ASSERT_ROW_EQ(s, 2, "");
    feed(s, "\033[3;1H\033[?1049h");
    ASSERT(pty_resize(s, 6, 3) == 0, "resize on the alternate screen");
    feed(s, "alt");
    feed(s, "\033[?1049l");
    ASSERT_ROW_EQ(s, 0, "01234");
    ASSERT(pty_resize(s, 0, 3) == -1, "zero width rejected");
    ASSERT(pty_resize(s, 5000, 3) == -1, "absurd width rejected");
    feed(s, "\033[1;1H\xE6\x97\xA5\xE6\x97\xA5\xE6\x97\xA5");
    ASSERT(pty_resize(s, 5, 3) == 0, "clip through a wide character");
    ASSERT_ROW_EQ(s, 0, "\xE6\x97\xA5\xE6\x97\xA5");
    pty_close(s);
}

static void test_screen_new_rejects_bad_sizes(void) {
    ASSERT(pty_screen_new(0, 5) == NULL, "zero columns");
    ASSERT(pty_screen_new(5, -1) == NULL, "negative rows");
    ASSERT(pty_open(0, 0) == NULL, "pty_open with no size");
    pty_screen_free(NULL);
    pty_screen_feed(NULL, "x", 1);
    ASSERT(pty_screen_cell(NULL, 0, 0) == NULL, "no cell without a screen");
}

int main(int argc, char **argv) {
    static const TestCase tests[] = {
        TEST_CASE(test_basic_write),
        TEST_CASE(test_newline_keeps_column),
        TEST_CASE(test_cr_lf),
        TEST_CASE(test_backspace),
        TEST_CASE(test_cup),
        TEST_CASE(test_relative_moves),
        TEST_CASE(test_absolute_moves),
        TEST_CASE(test_erase_display),
        TEST_CASE(test_erase_line),
        TEST_CASE(test_erase_insert_delete_chars),
        TEST_CASE(test_background_colour_erase),
        TEST_CASE(test_scroll_at_bottom),
        TEST_CASE(test_scroll_region),
        TEST_CASE(test_index_and_next_line),
        TEST_CASE(test_insert_delete_lines),
        TEST_CASE(test_sgr_attributes),
        TEST_CASE(test_sgr_colours),
        TEST_CASE(test_pending_wrap),
        TEST_CASE(test_autowrap_off),
        TEST_CASE(test_sequence_split_across_reads),
        TEST_CASE(test_byte_by_byte),
        TEST_CASE(test_strings_are_skipped),
        TEST_CASE(test_unhandled_sequences_print_nothing),
        TEST_CASE(test_utf8),
        TEST_CASE(test_wide_characters),
        TEST_CASE(test_combining_characters),
        TEST_CASE(test_char_width),
        TEST_CASE(test_tabs),
        TEST_CASE(test_save_restore_cursor),
        TEST_CASE(test_alternate_screen),
        TEST_CASE(test_dec_graphics),
        TEST_CASE(test_insert_mode),
        TEST_CASE(test_repeat),
        TEST_CASE(test_cursor_visibility),
        TEST_CASE(test_origin_mode),
        TEST_CASE(test_resets),
        TEST_CASE(test_terminal_responses),
        TEST_CASE(test_resize),
        TEST_CASE(test_screen_new_rejects_bad_sizes),
    };
    return test_run_suite("emulator unit tests", tests,
                          sizeof(tests) / sizeof(tests[0]),
                          280, 60, argc, argv);
}
