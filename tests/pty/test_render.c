/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Proprietary */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-06 */
/* PTY tests of the comparison reports, style maps, waiting and evidence files. */
/**
 * @file test_render.c
 * @brief PTY tests of the emulator and of the comparison reports, through
 *        a real program and a real pseudo-terminal.
 *
 * The unit tests feed the emulator directly.  Here the same sequences take
 * the way a test program's output really takes: written by a child process
 * (pty_fixture "print"), through the kernel's line discipline, read back in
 * whatever chunks the PTY delivers.  Every scenario ends in a full-screen,
 * cell-exact comparison.  The second half checks that the comparison
 * reports explain a difference found on a real program's screen.
 */

#include "test_helpers.h"
#include <errno.h>
#include <sys/stat.h>

#ifndef PTY_FIXTURE
#error "PTY_FIXTURE must name the fixture binary"
#endif

int g_tests_run = 0;
int g_tests_failed = 0;

/* Runs the fixture printing @p bytes and waits for it to exit, so the
 * screen is final.  The tty turns "\n" into "\r\n" (ONLCR); scenarios move
 * the cursor explicitly instead of relying on line feeds. */
static PtySession *show(int cols, int rows, const char *bytes) {
    PtySession *s = pty_open(cols, rows);
    if (!s) {
        printf("  [FATAL] pty_open(%d, %d) failed\n", cols, rows);
        exit(EXIT_FAILURE);
    }
    const char *argv[] = { PTY_FIXTURE, "print", bytes, NULL };
    if (pty_run(s, argv) != 0) {
        printf("  [FATAL] pty_run(%s) failed: %s\n", PTY_FIXTURE, strerror(errno));
        exit(EXIT_FAILURE);
    }
    int st = pty_wait_exit(s, 5000);
    if (st != 0) {
        printf("  [FATAL] the fixture exited with %d\n", st);
        exit(EXIT_FAILURE);
    }
    return s;
}

/* ── The emulator, sequence group by sequence group ─────────────────── */

static void test_cursor_and_erase(void) {
    PtySession *s = show(12, 5,
        "\033[2J\033[H0123456789"
        "\033[2;1H0123456789\033[2;5H\033[K"          /* EL 0 */
        "\033[3;1H0123456789\033[3;5H\033[1K"         /* EL 1 */
        "\033[4;1H0123456789\033[4;3H\033[3X"         /* ECH */
        "\033[5;1Habcdef\033[5;2H\033[2P"             /* DCH */
        "\033[5;3H\033[1@"                            /* ICH */
        "\033[1;12H\033[2D\033[A\033[B\033[C\033[3G!" /* CUB CUU CUD CUF CHA */
        "\033[5d\033[9`?");                           /* VPA HPA */
    const char *const expected[] = {
        "0123456789",
        "01!3",
        "     56789",
        "01   56789",
        "ad ef   ?",
        NULL
    };
    ASSERT_SCREEN_EQ(s, expected);
    ASSERT_CURSOR(s, 4, 9);   /* after the '?' written at HPA 9, VPA 5 */
    pty_close(s);
}

static void test_erase_display_modes(void) {
    PtySession *s = show(6, 3,
        "\033[1;1Haaaaaa\033[2;1Hbbbbbb\033[3;1Hcccccc"
        "\033[2;3H\033[J"                /* ED 0 */
        "\033[1;1Haaaaaa\033[1;3H\033[1J" /* ED 1 on row 0 */
        "\033[3;1Hzz");
    const char *const expected[] = { "   aaa", "bb", "zz", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_scrolling(void) {
    PtySession *s = show(8, 6,
        "\033[1;1Hhead\033[2;1H1\033[3;1H2\033[4;1H3\033[6;1Hfoot"
        "\033[2;4r"                   /* region rows 2-4 */
        "\033[4;1H\033Dn"             /* IND at the bottom margin scrolls */
        "\033[2;1H\033Mt"             /* RI at the top margin scrolls back */
        "\033[3;1H\033[L"             /* IL inside the region */
        "\033[S\033[T"                /* SU then SD */
        "\033[r"                      /* reset the region */
        "\033[5;1H\033Ex");           /* NEL */
    const char *const expected[] = {
        "head",
        "",                   /* "t" scrolled away by SU, SD brought a blank */
        "",
        "2",
        "",
        "xoot",
        NULL
    };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_delete_lines_and_origin(void) {
    PtySession *s = show(8, 5,
        "\033[1;1HA\033[2;1HB\033[3;1HC\033[4;1HD\033[5;1HE"
        "\033[2;1H\033[2M"            /* DL 2 at row 1 */
        "\033[2;4r\033[?6h\033[1;1H>" /* origin: row 1 is the region top */
        "\033[?6l\033[r");
    const char *const expected[] = { "A", ">", "E", "", "", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_attributes_and_colours(void) {
    PtySession *s = show(20, 2,
        "\033[1mB\033[2mD\033[0;3mI\033[4mU\033[0;5mK\033[7mR\033[0;8mH\033[9mS"
        "\033[0;31mr\033[92mg\033[44mb\033[103mc\033[0;38;5;196mx"
        "\033[48;2;1;2;3my\033[38:2::4:5:6mz\033[0m.");
    ASSERT_CELL_ATTR_EQ(s, 0, 0, PTY_ATTR_BOLD);
    ASSERT_CELL_ATTR_EQ(s, 0, 1, PTY_ATTR_BOLD | PTY_ATTR_DIM);
    ASSERT_CELL_ATTR_EQ(s, 0, 3, PTY_ATTR_ITALIC | PTY_ATTR_UNDERLINE);
    ASSERT_CELL_ATTR_EQ(s, 0, 5, PTY_ATTR_BLINK | PTY_ATTR_REVERSE);
    ASSERT_CELL_ATTR_EQ(s, 0, 7, PTY_ATTR_INVISIBLE | PTY_ATTR_STRIKE);
    ASSERT_CELL_FG(s, 0, 8, PTY_FG_RED);
    ASSERT_CELL_FG(s, 0, 9, 92);
    ASSERT_CELL_BG(s, 0, 10, PTY_BG_BLUE);
    ASSERT_CELL_BG(s, 0, 11, 103);
    ASSERT_CELL_FG(s, 0, 12, PTY_COLOR_IDX(196));
    ASSERT_CELL_BG(s, 0, 13, PTY_COLOR_RGB(1, 2, 3));
    ASSERT_CELL_FG(s, 0, 14, PTY_COLOR_RGB(4, 5, 6));
    ASSERT_CELL_FG(s, 0, 15, PTY_FG_DEFAULT);
    ASSERT_CELL_BG(s, 0, 15, PTY_BG_DEFAULT);
    const char *const expected[] = { "BDIUKRHSrgbcxyz.", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_background_colour_erase(void) {
    PtySession *s = show(6, 2, "\033[46m\033[2J\033[0m\033[1;1Hok");
    ASSERT_CELL_BG(s, 1, 5, PTY_BG_CYAN);
    ASSERT_CELL_BG(s, 0, 0, PTY_BG_DEFAULT);   /* written after the reset */
    pty_close(s);
}

static void test_wrap_and_autowrap(void) {
    PtySession *s = show(5, 3,
        "\033[1;1Habcdefg"           /* wraps onto row 1 */
        "\033[?7l\033[3;1Hvwxyz12"    /* no wrap: the last column is overwritten */
        "\033[?7h");
    const char *const expected[] = { "abcde", "fg", "vwxy2", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_alternate_screen_and_saved_cursor(void) {
    PtySession *s = show(10, 3,
        "\033[1;1Hmain\033[2;3H\0337"   /* DECSC */
        "\033[?1049hALT\033[?1049l"     /* drawn and left: main comes back */
        "\033[3;9H\0338*"               /* DECRC */
        "\033[3;1H\033[s\033[1;9H\033[u#");
    const char *const expected[] = { "main", "  *", "#", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_tabs_insert_repeat_graphics(void) {
    PtySession *s = show(24, 4,
        "\033[1;1Ha\tb\033[1;20H\033[Zc"         /* HT and CBT */
        "\033[2;1Habc\033[2;2H\033[4hXY\033[4l"   /* insert mode */
        "\033[3;1H=\033[5b|"                      /* REP */
        "\033[4;1H\033(0lqk\033(B"                /* DEC line drawing */
        "\033[4;5H\033)0\016x\017x");             /* G1 through SO/SI */
    const char *const expected[] = {
        "a       b       c",
        "aXYbc",
        "======|",
        "┌─┐ │x",
        NULL
    };
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_unicode(void) {
    PtySession *s = show(12, 2,
        "\033[1;1H\xE6\x97\xA5\xE6\x9C\xAC|e\xCC\x81|\xC3\xA1"
        "\033[2;1H\xFF" "a\xED\xA0\x80" "b");
    const char *const expected[] = {
        "\xE6\x97\xA5\xE6\x9C\xAC|e\xCC\x81|\xC3\xA1",
        "\xEF\xBF\xBD" "a\xEF\xBF\xBD" "b",
        NULL
    };
    ASSERT_SCREEN_EQ(s, expected);
    ASSERT_CELL_TEXT(s, 0, 1, "");               /* right half of a wide char */
    ASSERT_CELL_TEXT(s, 0, 5, "e\xCC\x81");
    pty_close(s);
}

static void test_strings_and_aborted_sequences(void) {
    PtySession *s = show(20, 2,
        "\033[1;1Ha\033]0;title\007b\033]8;;http://x\033\\c\033]8;;\033\\"
        "\033Pq#0;2;0;0;0\033\\d\033_apc\033\\e"
        "\033[1\030f\033[1\032g"                  /* CAN, SUB abort a CSI */
        "\033[>4;1m\033[?2004h\033[2 q\033[22;0;0th"); /* ignored forms */
    const char *const expected[] = { "abcdefgh", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    ASSERT_CELL_ATTR_EQ(s, 0, 7, PTY_ATTR_NONE);
    pty_close(s);
}

static void test_resets(void) {
    PtySession *s = show(6, 2, "\033[1;31mold\033c\033[1;1Hnew\033#8");
    const char *const expected[] = { "EEEEEE", "EEEEEE", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    ASSERT_CELL_ATTR_EQ(s, 0, 0, PTY_ATTR_NONE);
    pty_close(s);
    s = show(6, 2, "\033[1;4m\033[?25l\033[!pplain");
    ASSERT_CELL_ATTR_EQ(s, 0, 0, PTY_ATTR_NONE);
    int r, c;
    ASSERT(pty_cursor(s, &r, &c) == 1, "DECSTR shows the cursor again");
    pty_close(s);
}

static void test_answers_through_the_pty(void) {
    /* The fixture asks for the cursor position; the session answers. */
    const char *argv[] = { PTY_FIXTURE, "dsr", NULL };
    PtySession *s = pty_open(40, 10);
    ASSERT(s != NULL, "pty_open");
    ASSERT(pty_run(s, argv) == 0, "pty_run");
    ASSERT_WAIT_FOR(s, "REPLY:ESC[5;10R", 3000);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

/* ── The reports, on a real program's screen ─────────────────────────── */

#define ASSERT_HAS(report, needle, msg) do { \
    g_tests_run++; \
    if (!strstr(report, needle)) { \
        PTY_ASSERT_FAIL_("%s: the report lacks \"%s\"; report was:\n%s", \
                         msg, needle, report); \
        pty_close(s); \
        return; \
    } \
} while (0)

static PtySession *inbox(void) {
    return show(24, 6,
        "\033[1;1H\033[1;44m Inbox  3 \033[0m"
        "\033[2;1H\033[7m> Meeting moved \033[0m"
        "\033[3;1H  Invoice 2026"
        "\033[5;1HStatus: OK  12:03");
}

static void test_report_on_a_real_screen(void) {
    PtySession *s = inbox();
    const char *const expected[] = {
        " Inbox  3",
        "> Meeting moved",
        "   Invoice 2026",          /* one column to the right */
        "",
        "status: ok  ??:??",        /* case, and a clock behind a wildcard */
        NULL
    };
    PtyRegion all = PTY_REGION_FULL;
    all.wildcard = "?";
    char report[8192];
    ASSERT(pty_region_compare(s, &all, expected, report, sizeof(report)) == 2,
           "two rows differ");
    ASSERT_HAS(report, "screen (24x6): text differs in 2 of 6 rows", "header");
    ASSERT_HAS(report, "the text is shifted left by 1 column", "the shift is named");
    ASSERT_HAS(report, "differs only in letter case", "letter case is named");
    ASSERT_HAS(report, "-    2 |   Invoice 2026", "expected row");
    ASSERT_HAS(report, "+    2 |  Invoice 2026", "actual row");

    const char *const moved[] = { "", " Inbox  3", NULL };
    PtyRegion top = PTY_REGION(0, 0, 3, 0);
    top.label = "top";
    ASSERT(pty_region_compare(s, &top, moved, report, sizeof(report)) == 3,
           "the whole block one row too low");
    ASSERT_HAS(report, "region \"top\" rows 0-2, cols 0-23", "label and geometry");
    ASSERT_HAS(report, "the expected text is on row 0 (1 row higher)", "moved row");
    ASSERT_HAS(report, "  row 0: cols 1-5, 8 differ\n    expected a blank row",
               "text where a blank row was expected");

    const char *const longer[] = { " Inbox  3", "> Meeting moved today", NULL };
    ASSERT(pty_region_compare(s, &PTY_REGION(0, 0, 2, 0), longer, report,
                              sizeof(report)) == 1, "truncation");
    ASSERT_HAS(report, "truncated: matches up to col 14, the expected text continues to col 20", "is named");
    const char *const shorter[] = { " Inbox", NULL };
    ASSERT(pty_region_compare(s, &PTY_REGION(0, 0, 1, 0), shorter, report,
                              sizeof(report)) == 1, "extra text");
    ASSERT_HAS(report, "extra text: matches up to col 5, then col 8 should be blank", "is named");
    pty_close(s);
}

static void test_style_report_on_a_real_screen(void) {
    PtySession *s = inbox();
    static const PtyStyleKey legend[] = {
        { '.', PTY_ATTR_NONE,    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 'H', PTY_ATTR_BOLD,    PTY_FG_DEFAULT, PTY_BG_BLUE },
        { 'S', PTY_ATTR_REVERSE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 0, 0, 0, 0 }
    };
    const char *const right[] = { "HHHHHHHHHH.", "SSSSSSSSSSSSSSSS.", NULL };
    ASSERT_REGION_STYLE(s, PTY_REGION(0, 0, 2, 0), right, legend);

    const char *const off[] = { "HHHHHHHHHH.", ".SSSSSSSSSSSSSSSS", NULL };
    char report[8192];
    ASSERT(pty_region_compare_style(s, &PTY_REGION(0, 0, 2, 0), off, legend,
                                    report, sizeof(report)) == 1,
           "selection one column off");
    ASSERT_HAS(report, "legend: .=plain; H=bold, bg blue (44); S=reverse", "legend");
    ASSERT_HAS(report, "the styling is shifted left by 1 column", "shift");
    ASSERT_HAS(report, "  text |> Meeting moved", "the text under the map");

    char map[512];
    pty_region_style_map(s, &PTY_REGION(0, 0, 2, 18), legend, map, sizeof(map));
    ASSERT(strcmp(map, "HHHHHHHHHH........\nSSSSSSSSSSSSSSSS..") == 0,
           "the actual style map");
    char d[256];
    ASSERT(strcmp(pty_cell_describe(s, 0, 1, d, sizeof(d)),
                  "'I' U+0049, bold, bg blue (44)") == 0, "a cell in words");
    pty_close(s);
}

static void test_waiting_for_a_picture(void) {
    const char *argv[] = { PTY_FIXTURE, "frame", NULL };
    PtySession *s = pty_open(20, 8);
    ASSERT(s != NULL, "pty_open");
    ASSERT(pty_run(s, argv) == 0, "pty_run");
    const char *const status[] = { "status: ok", NULL };
    ASSERT_WAIT_REGION(s, PTY_REGION(6, 0, 1, 0), status, 3000);
    const char *const never[] = { "status: broken", NULL };
    char report[4096];
    ASSERT(pty_wait_region(s, &PTY_REGION(6, 0, 1, 0), never, 200, report,
                           sizeof(report)) == 1, "the wait times out");
    ASSERT_HAS(report, "+    6 |status: ok", "and shows the screen at the deadline");
    pty_send_str(s, "q");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

static void test_evidence_of_a_real_screen(void) {
    PtySession *s = inbox();
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/render-evidence-%d", PTY_TEST_TMPDIR, (int)getpid());
    ASSERT(pty_save_evidence(s, dir, "inbox", "report text") == 0, "saved");
    char path[700], buf[4096];
    snprintf(path, sizeof(path), "%s/inbox.screen.ansi", dir);
    FILE *fp = fopen(path, "r");
    ASSERT(fp != NULL, "the ANSI dump exists");
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    /* Loaded back, the dump is the same screen, colours included. */
    PtySession *copy = pty_open(24, 6);
    ASSERT(copy != NULL, "pty_open");
    pty_feed(copy, buf, n);
    char a[2048], b[2048];
    pty_screen_text(s, a, sizeof(a));
    pty_screen_text(copy, b, sizeof(b));
    int same = strcmp(a, b) == 0 &&
               pty_cell_bg(copy, 0, 1) == PTY_BG_BLUE &&
               pty_cell_attr(copy, 1, 0) == PTY_ATTR_REVERSE;
    pty_close(copy);
    static const char *const suffixes[] = { "report.txt", "screen.txt", "screen.ansi" };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/inbox.%s", dir, suffixes[i]);
        unlink(path);
    }
    ASSERT(rmdir(dir) == 0, "only the three files were written");
    ASSERT(same, "the saved screen loads back identical");

    char *text = NULL;
    size_t len = 0;
    FILE *ms = open_memstream(&text, &len);
    ASSERT(ms != NULL, "open_memstream");
    pty_region_dump_c(s, &PTY_REGION(0, 0, 3, 0), ms);
    fclose(ms);
    int ok = strstr(text, "    \" Inbox  3\",\n    \"> Meeting moved\",\n") != NULL;
    free(text);
    ASSERT(ok, "the snapshot is a C literal of the region");
    pty_close(s);
}

int main(int argc, char **argv) {
    static const TestCase tests[] = {
        TEST_CASE(test_cursor_and_erase),
        TEST_CASE(test_erase_display_modes),
        TEST_CASE(test_scrolling),
        TEST_CASE(test_delete_lines_and_origin),
        TEST_CASE(test_attributes_and_colours),
        TEST_CASE(test_background_colour_erase),
        TEST_CASE(test_wrap_and_autowrap),
        TEST_CASE(test_alternate_screen_and_saved_cursor),
        TEST_CASE(test_tabs_insert_repeat_graphics),
        TEST_CASE(test_unicode),
        TEST_CASE(test_strings_and_aborted_sequences),
        TEST_CASE(test_resets),
        TEST_CASE(test_answers_through_the_pty),
        TEST_CASE(test_report_on_a_real_screen),
        TEST_CASE(test_style_report_on_a_real_screen),
        TEST_CASE(test_waiting_for_a_picture),
        TEST_CASE(test_evidence_of_a_real_screen),
    };
    return test_run_suite("PTY render and report tests", tests,
                          sizeof(tests) / sizeof(tests[0]),
                          60, 100, argc, argv);
}
