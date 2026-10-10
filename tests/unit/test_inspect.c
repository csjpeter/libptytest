/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Proprietary */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-03 */
/* Unit tests of the inspection and comparison API. */
/**
 * @file test_inspect.c
 * @brief Unit tests for the screen inspection API and the assertion
 *        macros, driven through pty_feed() without a child process.
 */

#include "test_helpers.h"
#include "pty_internal.h"
#include <errno.h>
#include <sys/stat.h>

int g_tests_run = 0;
int g_tests_failed = 0;

static PtySession *term(int cols, int rows) {
    PtySession *s = pty_open(cols, rows);
    if (!s) {
        printf("  [FATAL] pty_open(%d, %d) failed\n", cols, rows);
        exit(EXIT_FAILURE);
    }
    return s;
}

static void feed(PtySession *s, const char *text) {
    pty_feed(s, text, strlen(text));
}

/* Asserts that a report contains @p needle, printing the report if not:
 * a report test that fails without showing the report is undiagnosable. */
#define ASSERT_HAS(report, needle, msg) do { \
    g_tests_run++; \
    if (!strstr(report, needle)) { \
        PTY_ASSERT_FAIL_("%s: the report lacks \"%s\"; report was:\n%s", \
                         msg, needle, report); \
        return; \
    } \
} while (0)

/* ── Row and screen text ─────────────────────────────────────────────── */

static void test_row_text(void) {
    PtySession *s = term(20, 3);
    feed(s, "  Hello World   ");
    char buf[64];
    ASSERT(strcmp(pty_row_text(s, 0, buf, sizeof(buf)), "  Hello World") == 0,
           "leading blanks kept, trailing removed");
    ASSERT(strcmp(pty_row_text(s, 1, buf, sizeof(buf)), "") == 0, "blank row is empty");
    ASSERT(strcmp(pty_row_text(s, 7, buf, sizeof(buf)), "") == 0, "out of range is empty");
    ASSERT(strcmp(pty_row_text(s, -1, buf, sizeof(buf)), "") == 0, "negative row is empty");

    char small[6];
    ASSERT(strcmp(pty_row_text(s, 0, small, sizeof(small)), "  Hel") == 0,
           "truncated to the buffer, NUL-terminated");
    feed(s, "\r\n\xE6\x97\xA5\xE6\x97\xA5");
    char tiny[5];
    ASSERT(strcmp(pty_row_text(s, 1, tiny, sizeof(tiny)), "\xE6\x97\xA5") == 0,
           "never cuts a character in half");
    ASSERT(pty_row_text(s, 0, NULL, 10) == NULL, "NULL buffer is returned as is");
    ASSERT(pty_row_text(s, 0, buf, 0) == buf, "zero size is a no-op");
    pty_close(s);
}

static void test_contains(void) {
    PtySession *s = term(20, 3);
    feed(s, "alpha\r\nbeta gamma");
    ASSERT(pty_row_contains(s, 1, "gamma"), "row 1 has gamma");
    ASSERT(!pty_row_contains(s, 0, "gamma"), "row 0 has not");
    ASSERT(pty_screen_contains(s, "alpha"), "screen has alpha");
    ASSERT(!pty_screen_contains(s, "delta"), "screen has no delta");
    ASSERT(!pty_screen_contains(s, NULL), "NULL text is never found");
    ASSERT(!pty_row_contains(NULL, 0, "x"), "NULL session never contains");
    pty_close(s);
}

static void test_screen_text(void) {
    PtySession *s = term(10, 3);
    feed(s, "one\r\n\r\nthree  ");
    char buf[64];
    ASSERT(strcmp(pty_screen_text(s, buf, sizeof(buf)), "one\n\nthree") == 0,
           "rows joined with newlines, blanks trimmed");
    char small[7];
    ASSERT(strcmp(pty_screen_text(s, small, sizeof(small)), "one\n\nt") == 0,
           "truncated, NUL-terminated");
    feed(s, "\033[2J\033[H\xE6\x97\xA5\xE6\x97\xA5");
    char cut[5];
    ASSERT(strcmp(pty_screen_text(s, cut, sizeof(cut)), "\xE6\x97\xA5") == 0,
           "truncation keeps whole characters");
    pty_close(s);
}

static void test_is_blank(void) {
    PtySession *s = term(10, 3);
    ASSERT(pty_screen_is_blank(s), "new screen is blank");
    feed(s, "\033[3;10H.");
    ASSERT(!pty_screen_is_blank(s), "one character makes it non-blank");
    feed(s, "\033[2J");
    ASSERT(pty_screen_is_blank(s), "blank again after ED 2");
    ASSERT(pty_screen_is_blank(NULL), "NULL session counts as blank");
    pty_close(s);
}

/* ── Full-screen comparison ──────────────────────────────────────────── */

static void test_compare_match(void) {
    PtySession *s = term(12, 4);
    feed(s, "+--------+\r\n|  menu  |\r\n+--------+");
    const char *const expected[] = {
        "+--------+",
        "|  menu  |   ",      /* trailing blanks are not significant */
        "+--------+",
        NULL                   /* row 3 must be blank */
    };
    char report[512];
    ASSERT(pty_screen_compare(s, expected, report, sizeof(report)) == 0,
           "identical screen compares equal");
    ASSERT(report[0] == '\0', "no report when equal");
    ASSERT_SCREEN_EQ(s, expected);
    pty_close(s);
}

static void test_compare_mismatch(void) {
    PtySession *s = term(12, 4);
    feed(s, "+--------+\r\n| menu   |\r\n+--------+\r\nstray");
    const char *const expected[] = {
        "+--------+",
        "|  menu  |",
        "+--------+",
        NULL
    };
    char report[4096];
    ASSERT(pty_screen_compare(s, expected, report, sizeof(report)) == 2,
           "one shifted row and one unexpected row");
    ASSERT_HAS(report, "screen (12x4): text differs in 2 of 4 rows", "header names the area and the count");
    ASSERT_HAS(report, "        0----+----1-\n", "column ruler");
    ASSERT_HAS(report, "-    1 ||  menu  |  |\n+    1 || menu   |  |\n", "expected and actual row, framed, one under the other");
    ASSERT_HAS(report, "\n          ^^^^^\n", "a caret under every differing column");
    ASSERT_HAS(report, "     0 |+--------+  |\n", "context row shown");
    ASSERT_HAS(report, "  row 1: cols 2-6 differ\n", "ranges of columns");
    ASSERT_HAS(report, "the text in cols 2-6 is shifted left by 1 column",
               "an off-by-one inside the frame is named");
    ASSERT_HAS(report, "first difference at col 2: expected ' ' U+0020, "
                          "actual 'm' U+006D", "first cell with code points");
    ASSERT_HAS(report, "-    3 |            |\n+    3 |stray       |\n", "a stray row is a difference");
    ASSERT_HAS(report, "  row 3: cols 0-4 differ\n    expected a blank row\n", "and is called one");
    ASSERT(pty_screen_compare(s, expected, NULL, 0) == 2, "report is optional");

    char tiny[8];
    pty_screen_compare(s, expected, tiny, sizeof(tiny));
    ASSERT(strlen(tiny) == sizeof(tiny) - 1, "report truncated to its buffer");
    pty_close(s);
}

static void test_compare_leading_blanks_matter(void) {
    PtySession *s = term(10, 2);
    feed(s, " x");
    const char *const shifted[] = { "x", NULL };
    char report[2048];
    ASSERT(pty_screen_compare(s, shifted, report, sizeof(report)) == 1,
           "a column shift is a difference");
    ASSERT_HAS(report, "the text is shifted right by 1 column", "and is explained as a shift");
    pty_close(s);
}

static void test_compare_too_many_rows(void) {
    PtySession *s = term(10, 2);
    const char *const expected[] = { "a", "b", "c", NULL };
    char report[256];
    ASSERT(pty_screen_compare(s, expected, report, sizeof(report)) == -1,
           "more expected rows than the screen has");
    ASSERT_HAS(report, "the expectation has 3 rows but the screen has 2", "and the report says so");
    const char *const wide[] = { "0123456789X", NULL };
    ASSERT(pty_screen_compare(s, wide, report, sizeof(report)) == -1,
           "a row wider than the screen");
    ASSERT_HAS(report, "expected row 0 (screen row 0) is 11 columns wide, "
                          "the screen only 10", "is reported");
    const char *const blanks[] = { "          ", NULL };
    ASSERT(pty_screen_compare(s, blanks, NULL, 0) == 0,
           "trailing blanks do not count as width");
    ASSERT(pty_screen_compare(s, NULL, NULL, 0) == -1, "NULL expectation");
    ASSERT(pty_screen_compare(NULL, expected, NULL, 0) == -1, "NULL session");
    pty_close(s);
}

static void test_compare_wide(void) {
    PtySession *s = term(10, 2);
    feed(s, "\xE6\x97\xA5\xE6\x9C\xAC|");
    const char *const expected[] = { "\xE6\x97\xA5\xE6\x9C\xAC|", NULL };
    ASSERT_SCREEN_EQ(s, expected);
    ASSERT_CELL_TEXT(s, 0, 4, "|");   /* two wide characters, four columns */
    const char *const narrow[] = { "\xE6\x97\xA5|", NULL };
    char report[2048];
    ASSERT(pty_screen_compare(s, narrow, report, sizeof(report)) == 1,
           "a missing wide character is a difference");
    ASSERT_HAS(report, "+    0 |\xE6\x97\xA5\xE6\x9C\xAC|     |\n", "rows keep their width with wide characters");
    pty_close(s);
}

/* ── Regions ─────────────────────────────────────────────────────────── */

static void test_region_compare(void) {
    PtySession *s = term(20, 5);
    feed(s, "Title          12:03\r\n\r\n  \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90"
            "\r\n  \xE2\x94\x82x\xE2\x94\x82\r\n  \xE2\x94\x94\xE2\x94\x80\xE2\x94\x98 junk");
    const char *const box[] = { "┌─┐", "│x│", "└─┘", NULL };
    ASSERT_REGION_EQ(s, PTY_REGION(2, 2, 3, 3), box);   /* "junk" is outside */

    PtyRegion clock = PTY_REGION(0, 15, 1, 5);
    clock.label = "clock";
    clock.wildcard = "?";
    const char *const any_time[] = { "??:??", NULL };
    ASSERT_REGION_EQ(s, clock, any_time);
    const char *const other[] = { "1?:5?", NULL };
    char report[2048];
    ASSERT(pty_region_compare(s, &clock, other, report, sizeof(report)) == 1,
           "the wildcard matches only where it stands");
    ASSERT_HAS(report, "region \"clock\" rows 0-0, cols 15-19: text differs in 1 of 1 row", "the label and the absolute geometry are in the header");
    ASSERT_HAS(report, "        +----\n", "ruler counts screen columns");
    ASSERT_HAS(report, "  row 0: col 18 differs", "absolute column");

    const char *const two_rows[] = { "a", "b", NULL };
    ASSERT(pty_region_compare(s, &clock, two_rows, report, sizeof(report)) == -1,
           "more rows than the region");
    ASSERT_HAS(report, "the expectation has 2 rows but the region has 1", "says region, not screen");
    const char *const blank[] = { NULL };
    ASSERT(pty_region_compare(s, &PTY_REGION(1, 0, 1, 0), blank, NULL, 0) == 0,
           "size 0 extends to the screen edge");
    ASSERT(pty_region_compare(s, &PTY_REGION(4, 18, 2, 3), blank, report,
                              sizeof(report)) == -1, "region outside the screen");
    ASSERT_HAS(report, "region rows 4-5, cols 18-20 is outside the 20x5 screen", "is reported with its geometry");
    ASSERT(pty_region_compare(s, &PTY_REGION(-1, 0, 1, 1), blank, NULL, 0) == -1,
           "negative origin");
    pty_close(s);
}

static void test_report_notes(void) {
    PtySession *s = term(20, 8);
    feed(s, "\033[2;1HHello world\033[4;1Hstatus: OK\033[5;1Hab cdef"
            "\033[6;1Hnbsp\xC2\xA0x\033[7;1Hhead\033[8;1Hshort");
    const char *const expected[] = {
        "Hello world",            /* moved: it is on row 1 */
        "",
        "",
        "status: ok",             /* letter case */
        "ab  cdef",               /* shifted from the middle */
        "nbsp x",                 /* U+00A0 instead of a space */
        "headline",               /* truncated */
        "sh",                     /* extra text */
        NULL
    };
    char report[8192];
    int n = pty_screen_compare(s, expected, report, sizeof(report));
    g_tests_run++;
    if (n != 7) {
        PTY_ASSERT_FAIL_("expected 7 differing rows, got %d; report:\n%s", n, report);
        pty_close(s);
        return;
    }
    ASSERT_HAS(report, "the expected text is on row 1 (1 row lower)", "a row moved down is found");
    ASSERT_HAS(report, "nothing is shown on this row", "a blank row");
    ASSERT_HAS(report, "differs only in letter case", "letter case");
    ASSERT_HAS(report, "the text in cols 3-7 is shifted left by 1 column", "a shift in the middle of a row");
    ASSERT_HAS(report, "expected ' ' U+0020, actual '\xC2\xA0' U+00A0", "an invisible difference is spelled out in code points");
    ASSERT_HAS(report, "  row 5: col 4 differs\n    a different kind of blank",
               "and named as such");
    ASSERT_HAS(report, "  row 6: cols 4-7 differ\n    truncated", "truncation wins over "
               "blanks matching blanks");
    ASSERT_HAS(report, "truncated: matches up to col 3, the expected text "
                          "continues to col 7", "truncation");
    ASSERT_HAS(report, "extra text: matches up to col 1, then cols 2-4 should "
                          "be blank", "extra text");

    /* Only the columns that hold the extra text are named. */
    PtySession *gap = term(12, 1);
    feed(gap, "ab   cd");
    const char *const ab[] = { "ab", NULL };
    char rep2[2048];
    pty_screen_compare(gap, ab, rep2, sizeof(rep2));
    pty_close(gap);
    ASSERT_HAS(rep2, "extra text: matches up to col 1, then cols 5-6 should be blank",
               "a gap before the extra text is not counted");
    gap = term(12, 1);
    feed(gap, "ab   c");
    pty_screen_compare(gap, ab, rep2, sizeof(rep2));
    pty_close(gap);
    ASSERT_HAS(rep2, "extra text: matches up to col 1, then col 5 should be blank",
               "a single extra cell is named in the singular");
    ASSERT(strstr(report, "     ...\n") == NULL, "no gap: every row is shown");

    PtyRegion quiet = PTY_REGION_FULL;
    quiet.context = -1;
    pty_region_compare(s, &quiet, expected, report, sizeof(report));
    ASSERT(strstr(report, "     2 |") == NULL || strstr(report, "-    2 |") != NULL,
           "no context rows when context is negative");
    pty_close(s);
}

static void test_report_moved_block(void) {
    PtySession *s = term(12, 6);
    feed(s, "\r\n\r\none\r\ntwo");
    const char *const expected[] = { "", "one", "two", NULL };
    char report[4096];
    ASSERT(pty_screen_compare(s, expected, report, sizeof(report)) == 3, "three rows");
    ASSERT_HAS(report, "every missing row is 1 row lower than expected: the content "
                          "has moved as a block", "a block move is named");
    pty_close(s);
}

static void test_report_context_gap(void) {
    PtySession *s = term(6, 12);
    feed(s, "x");
    const char *const expected[] = { "y", NULL };
    char report[4096];
    pty_screen_compare(s, expected, report, sizeof(report));
    ASSERT_HAS(report, "     2 |      |\n     ...\n", "two context rows, then an ellipsis");
    ASSERT(strstr(report, "     3 |") == NULL, "rows beyond the context are left out");
    pty_close(s);
}

/* ── Styles ──────────────────────────────────────────────────────────── */

static const PtyStyleKey legend[] = {
    { '.', PTY_ATTR_NONE,    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'R', PTY_ATTR_REVERSE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'b', PTY_ATTR_BOLD,    PTY_FG_DEFAULT, PTY_BG_BLUE },
    { 0, 0, 0, 0 }
};

static void test_style_compare(void) {
    PtySession *s = term(12, 3);
    feed(s, "\033[1;44mTitle\033[0m\r\n\033[7m item \033[0m\r\n\033[31mred\033[0m");
    const char *const ok[] = { "bbbbb.", "RRRRRR..", NULL };
    ASSERT_REGION_STYLE(s, PTY_REGION_FULL, ok, legend);

    char map[256];
    pty_region_style_map(s, &PTY_REGION(0, 0, 3, 8), legend, map, sizeof(map));
    ASSERT(strcmp(map, "bbbbb...\nRRRRRR..\n###.....") == 0,
           "style map in the legend, '#' for an unknown style");

    const char *const off[] = { "bbbbb", ".RRRRRR.", NULL };
    PtyRegion reg = PTY_REGION_FULL;
    reg.label = "menu";
    char report[4096];
    ASSERT(pty_region_compare_style(s, &reg, off, legend, report, sizeof(report)) == 1,
           "highlight one column off");
    ASSERT_HAS(report, "legend: .=plain; R=reverse; b=bold, bg blue (44)", "the legend is spelled out");
    ASSERT_HAS(report, "-    1 |.RRRRRR.|\n+    1 |RRRRRR......|\n", "expected and actual maps");
    ASSERT_HAS(report, "  text | item       |\n", "the text under the map");
    ASSERT_HAS(report, "the styling is shifted left by 1 column", "the shift is named");
    ASSERT_HAS(report, "first difference at col 0 (' ' U+0020): expected . = plain, "
                          "actual R = reverse", "the first cell in words");

    const char *const unknown[] = { "", "", "...", NULL };
    ASSERT(pty_region_compare_style(s, NULL, unknown, legend, report, sizeof(report)) == 1,
           "an unknown style differs from any key");
    ASSERT_HAS(report, "actual # = fg red (31)", "and is described");

    PtyRegion wild = PTY_REGION_FULL;
    wild.wildcard = "*";
    const char *const star[] = { "*****", "R*****", NULL };
    ASSERT(pty_region_compare_style(s, &wild, star, legend, NULL, 0) == 0,
           "the wildcard matches any style");

    const char *const bad[] = { "x", NULL };
    ASSERT(pty_region_compare_style(s, NULL, bad, legend, report, sizeof(report)) == -1,
           "a key not in the legend");
    ASSERT_HAS(report, "style map row 0, col 0: 'x' is not in the legend", "is reported");
    const char *const longrow[] = { "..............", NULL };
    ASSERT(pty_region_compare_style(s, NULL, longrow, legend, NULL, 0) == -1,
           "a map row wider than the region");
    pty_close(s);
}

/* ── Waiting for a picture (no child: the screen is already final) ──── */

static void test_wait_region_without_child(void) {
    PtySession *s = term(10, 2);
    feed(s, "ready");
    const char *const ok[] = { "ready", NULL };
    char report[2048];
    ASSERT(pty_wait_region(s, NULL, ok, 100, report, sizeof(report)) == 0,
           "a matching screen satisfies the wait at once");
    ASSERT(report[0] == '\0', "no report on a match");
    const char *const never[] = { "done", NULL };
    ASSERT(pty_wait_region(s, &PTY_REGION(0, 0, 1, 10), never, 50, report,
                           sizeof(report)) == 1, "a screen that never matches");
    ASSERT_HAS(report, "-    0 |done      |\n+    0 |ready     |",
               "the report shows the screen at the timeout");
    const char *const too_tall[] = { "a", "b", "c", NULL };
    ASSERT(pty_wait_region(s, NULL, too_tall, 5000, NULL, 0) == -1,
           "an impossible expectation returns at once");
    static const PtyStyleKey plain[] = { { '.', 0, 0, 0 }, { 0, 0, 0, 0 } };
    const char *const st[] = { ".....", NULL };
    ASSERT(pty_wait_region_style(s, NULL, st, plain, 100, NULL, 0) == 0, "style wait");
    const char *const bad[] = { "R", NULL };
    ASSERT(pty_wait_region_style(s, NULL, bad, plain, 100, report, sizeof(report)) == -1,
           "a key missing from the legend");
    ASSERT(pty_wait_region_style(s, NULL, st, NULL, 100, report, sizeof(report)) == -1,
           "no legend");
    pty_close(s);
}

/* ── Evidence ────────────────────────────────────────────────────────── */

static char g_evdir[512];

static int read_file(const char *dir, const char *name, char *buf, size_t size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    size_t n = fread(buf, 1, size - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return 0;
}

/* Removes what an evidence test wrote: literal file names, no recursion. */
static void remove_evidence(const char *name) {
    static const char *const suffixes[] = { "report.txt", "screen.txt", "screen.ansi" };
    for (size_t i = 0; i < 3; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.%s", g_evdir, name, suffixes[i]);
        unlink(path);
    }
}

static void test_save_evidence(void) {
    snprintf(g_evdir, sizeof(g_evdir), "%s/evidence-%d", PTY_TEST_TMPDIR, (int)getpid());
    PtySession *s = term(8, 2);
    feed(s, "\033[31mred\033[0m");
    ASSERT(pty_save_evidence(s, g_evdir, "case1", "what went wrong") == 0,
           "evidence saved, directory created");
    char buf[1024];
    ASSERT(read_file(g_evdir, "case1.report.txt", buf, sizeof(buf)) == 0 &&
           strcmp(buf, "what went wrong\n") == 0, "the text, with a final newline");
    ASSERT(read_file(g_evdir, "case1.screen.txt", buf, sizeof(buf)) == 0 &&
           strstr(buf, "  0 |red     |") != NULL, "the framed screen");
    ASSERT(read_file(g_evdir, "case1.screen.ansi", buf, sizeof(buf)) == 0 &&
           strstr(buf, "\033[0;31mred") != NULL, "the screen with its colours");
    remove_evidence("case1");

    errno = 0;
    ASSERT(pty_save_evidence(s, g_evdir, "a/b", "x") == -1 && errno == EINVAL,
           "a name with a slash is refused");
    char missing[600];
    snprintf(missing, sizeof(missing), "%s/no-such-parent/sub", g_evdir);
    errno = 0;
    ASSERT(pty_save_evidence(s, missing, "x", "x") == -1 && errno == ENOENT,
           "only one level is created, and the error is reported");
    ASSERT(pty_save_evidence(NULL, g_evdir, "x", "x") == -1, "NULL session");
    pty_close(s);
}

static PtySession *g_ev;
static int g_region_line, g_wait_line;
static void fail_region_with_evidence(void) {
    const char *const expected[] = { "green", NULL };
    g_region_line = __LINE__ + 1;
    ASSERT_REGION_EQ(g_ev, PTY_REGION(0, 0, 1, 8), expected);
}
static void fail_wait_region_with_evidence(void) {
    const char *const expected[] = { "green", NULL };
    g_wait_line = __LINE__ + 1;
    ASSERT_WAIT_SCREEN(g_ev, expected, 20);
}

static void test_assertion_saves_evidence(void) {
    snprintf(g_evdir, sizeof(g_evdir), "%s/evidence-%d", PTY_TEST_TMPDIR, (int)getpid());
    g_ev = term(8, 2);
    feed(g_ev, "red");
    /* The runner may have set a directory of its own: keep it for later. */
    const char *outer = getenv("PTYTEST_EVIDENCE_DIR");
    char *saved = outer ? strdup(outer) : NULL;
    setenv("PTYTEST_EVIDENCE_DIR", g_evdir, 1);
    printf("  (the [FAIL] lines below are expected)\n");
    int failed = g_tests_failed, run = g_tests_run;
    fail_region_with_evidence();
    fail_wait_region_with_evidence();
    int counted = g_tests_failed - failed == 2 && g_tests_run - run == 2;
    g_tests_failed = failed;
    g_tests_run = run;
    printf("  (end of expected [FAIL] lines)\n");
    if (saved) setenv("PTYTEST_EVIDENCE_DIR", saved, 1);
    else unsetenv("PTYTEST_EVIDENCE_DIR");
    free(saved);
    ASSERT(counted, "both comparisons failed and were counted");

    const char *name = "test_inspect.c-fail_region_with_evidence-L";
    char buf[4096], file[512];
    /* The line number is part of the name: find it from the source line. */
    snprintf(file, sizeof(file), "%s%d.report.txt", name, g_region_line);
    ASSERT(read_file(g_evdir, file, buf, sizeof(buf)) == 0,
           "the macro saved its report under file-function-line");
    ASSERT(strstr(buf, "in fail_region_with_evidence: region comparison differs") != NULL,
           "the report file starts with where and what");
    ASSERT(strstr(buf, "-    0 |green   |\n+    0 |red     |") != NULL,
           "and holds the comparison report");
    char stem[256];
    snprintf(stem, sizeof(stem), "%s%d", name, g_region_line);
    remove_evidence(stem);
    snprintf(stem, sizeof(stem), "test_inspect.c-fail_wait_region_with_evidence-L%d",
             g_wait_line);
    snprintf(file, sizeof(file), "%s.screen.txt", stem);
    ASSERT(read_file(g_evdir, file, buf, sizeof(buf)) == 0, "the wait saved evidence too");
    remove_evidence(stem);
    ASSERT(rmdir(g_evdir) == 0, "nothing else was left in the directory");
    pty_close(g_ev);
}

/* ── Describing and snapshots ────────────────────────────────────────── */

static void test_cell_describe(void) {
    PtySession *s = term(10, 2);
    feed(s, "\033[1;7;38;5;196;48;2;0;128;255mA\033[0m\xE6\x97\xA5" "e\xCC\x81");
    char d[256];
    ASSERT(strcmp(pty_cell_describe(s, 0, 0, d, sizeof(d)),
                  "'A' U+0041, bold, reverse, fg palette 196, bg #0080ff") == 0,
           "character, attributes and both colours");
    ASSERT(strcmp(pty_cell_describe(s, 0, 2, d, sizeof(d)),
                  "(right half of '\xE6\x97\xA5' U+65E5), plain") == 0,
           "the tail of a wide character");
    ASSERT(strcmp(pty_cell_describe(s, 0, 3, d, sizeof(d)),
                  "'e\xCC\x81' U+0065 U+0301, plain") == 0,
           "every code point of a combined cell");
    feed(s, "\r\n\033[92;100mx");
    ASSERT(strcmp(pty_cell_describe(s, 1, 0, d, sizeof(d)),
                  "'x' U+0078, fg bright green (92), bg bright black (100)") == 0,
           "bright colours by name");
    ASSERT(strcmp(pty_cell_describe(s, 9, 9, d, sizeof(d)), "") == 0, "out of range");
    pty_close(s);
}

static void test_dump_c(void) {
    PtySession *s = term(10, 4);
    feed(s, "say \"hi\"\r\nback\\slash");
    char *text = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&text, &len);
    ASSERT(fp != NULL, "open_memstream");
    pty_region_dump_c(s, NULL, fp);
    fclose(fp);
    ASSERT(strcmp(text, "const char *const expected[] = {\n"
                        "    \"say \\\"hi\\\"\",\n"
                        "    \"back\\\\slash\",\n"
                        "    NULL\n};\n") == 0,
           "quotes and backslashes escaped, trailing blank rows left out");
    free(text);
    pty_close(s);
}

static void test_dump_ansi(void) {
    PtySession *s = term(6, 2);
    feed(s, "a\033[1;31mb\033[0;38;5;9;48;2;1;2;3mc");
    char *text = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&text, &len);
    ASSERT(fp != NULL, "open_memstream");
    pty_screen_dump_ansi(s, fp);
    fclose(fp);
    ASSERT(strcmp(text, "\033[0ma\033[0;1;31mb\033[0;38;5;9;48;2;1;2;3mc\033[0m   "
                        "\033[0m\r\n\033[0m      \033[0m") == 0,
           "styles become SGR sequences, rows end in CR LF but the last");
    /* Round trip: feeding the dump reproduces the screen. */
    PtySession *copy = term(6, 2);
    pty_feed(copy, text, len);
    ASSERT_CELL_FG(copy, 0, 1, PTY_FG_RED);
    ASSERT_CELL_ATTR_EQ(copy, 0, 1, PTY_ATTR_BOLD);
    ASSERT_CELL_FG(copy, 0, 2, PTY_COLOR_IDX(9));
    ASSERT_CELL_BG(copy, 0, 2, PTY_COLOR_RGB(1, 2, 3));
    const char *const same[] = { "abc", NULL };
    ASSERT_SCREEN_EQ(copy, same);
    free(text);
    pty_close(copy);
    pty_close(s);
}

static void test_dump(void) {
    PtySession *s = term(12, 2);
    feed(s, "hi\033[2;3H\033[?25l");
    char *text = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&text, &len);
    ASSERT(fp != NULL, "open_memstream");
    pty_screen_dump(s, fp);
    fclose(fp);
    ASSERT(strstr(text, "    +0---------1-+\n") != NULL, "ruler with column tens");
    ASSERT(strstr(text, "  0 |hi          |\n") != NULL, "row 0, untrimmed, framed");
    ASSERT(strstr(text, "  1 |            |\n") != NULL, "row 1");
    ASSERT(strstr(text, "cursor at row 1, col 2 (hidden)") != NULL, "cursor line");
    free(text);
    pty_screen_dump(NULL, stdout);   /* no crash */
    pty_close(s);
}

/* ── Cell accessors ──────────────────────────────────────────────────── */

static void test_cell_accessors_out_of_range(void) {
    PtySession *s = term(5, 2);
    ASSERT(strcmp(pty_cell_text(s, 2, 0), "") == 0, "row past the end");
    ASSERT(strcmp(pty_cell_text(s, 0, 5), "") == 0, "column past the end");
    ASSERT(strcmp(pty_cell_text(s, -1, 0), "") == 0, "negative row");
    ASSERT(pty_cell_attr(s, 9, 9) == 0, "attr out of range");
    ASSERT(pty_cell_fg(s, 9, 9) == PTY_COLOR_DEFAULT, "fg out of range");
    ASSERT(pty_cell_bg(s, 9, 9) == PTY_COLOR_DEFAULT, "bg out of range");
    ASSERT(strcmp(pty_cell_text(NULL, 0, 0), "") == 0, "NULL session");
    ASSERT(pty_cursor(NULL, NULL, NULL) == -1, "cursor of NULL session");
    int c = -1, r = -1;
    pty_get_size(NULL, &c, &r);
    ASSERT(c == 0 && r == 0, "size of NULL session");
    pty_close(s);
}

/* ── Session without a child ─────────────────────────────────────────── */

static void test_no_child(void) {
    PtySession *s = term(5, 2);
    ASSERT(pty_send_str(s, "x") == -1, "nothing to send to");
    ASSERT(pty_send_key(s, PTY_KEY_UP) == -1, "no key either");
    ASSERT(pty_wait_for(s, "x", 10) == -1, "nothing to wait for");
    ASSERT(pty_settle(s, 10) == -1, "nothing to settle");
    ASSERT(pty_wait_exit(s, 10) == -1, "no child to wait for");
    ASSERT(pty_drain(s) == 0, "nothing to drain");
    ASSERT(pty_signal(s, 0) == -1, "no child to signal");
    feed(s, "x");
    ASSERT(pty_wait_for(s, "x", 10) == 0, "but text already there is found");
    ASSERT(pty_send_str(s, NULL) == -1, "NULL string");
    ASSERT(pty_send_key(s, (PtyKey)0x1FFFF) == -1, "unknown key code");
    pty_close(s);
    pty_close(NULL);
}

static void test_setenv_validation(void) {
    PtySession *s = term(5, 2);
    ASSERT(pty_setenv(s, "A", "1") == 0, "set");
    ASSERT(pty_setenv(s, "A", "2") == 0, "overwrite");
    ASSERT(pty_setenv(s, "A", NULL) == 0, "turn into unset");
    ASSERT(pty_setenv(s, "B", NULL) == 0, "unset a new name");
    ASSERT(s->env_count == 2, "two distinct names recorded");
    ASSERT(s->env[0].value == NULL, "A is now an unset");
    ASSERT(pty_setenv(s, "", "x") == -1, "empty name");
    ASSERT(pty_setenv(s, "A=B", "x") == -1, "name with '='");
    ASSERT(pty_setenv(NULL, "A", "x") == -1, "NULL session");
    ASSERT(pty_setenv(s, NULL, "x") == -1, "NULL name");
    pty_close(s);
}

static void test_run_rejects_bad_arguments(void) {
    PtySession *s = term(5, 2);
    const char *empty[] = { NULL };
    errno = 0;
    ASSERT(pty_run(s, empty) == -1 && errno == EINVAL, "empty argv");
    ASSERT(pty_run(s, NULL) == -1, "NULL argv");
    ASSERT(pty_run(NULL, empty) == -1, "NULL session");
    pty_close(s);
}

static void test_close_grace_from_environment(void) {
    setenv("PTYTEST_CLOSE_GRACE_MS", "250", 1);
    PtySession *s = term(5, 2);
    ASSERT(s->close_grace_ms == 250, "grace read from the environment");
    pty_set_close_grace(s, -5);
    ASSERT(s->close_grace_ms == 0, "negative grace means none");
    pty_set_close_grace(s, 40);
    ASSERT(s->close_grace_ms == 40, "explicit grace");
    pty_close(s);
    setenv("PTYTEST_CLOSE_GRACE_MS", "12abc", 1);
    s = term(5, 2);
    ASSERT(s->close_grace_ms == 0, "malformed value is ignored");
    pty_close(s);
    unsetenv("PTYTEST_CLOSE_GRACE_MS");
    pty_set_close_grace(NULL, 10);
}

static void test_version(void) {
    const char *v = pty_version();
    ASSERT(v && *v, "version string present");
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%d.%d.", PTYTEST_VERSION_MAJOR, PTYTEST_VERSION_MINOR);
    ASSERT(strncmp(v, prefix, strlen(prefix)) == 0, "version agrees with the header");
}

/* ── The assertion macros can fail ───────────────────────────────────────
 *
 * Each helper runs one macro against a screen that must make it fail; the
 * test then checks that a failure was counted and takes it back.  A macro
 * that cannot fail would pass every test that uses it. */

static PtySession *g_s;

static void fail_not_contains_on_blank(void) { ASSERT_SCREEN_NOT_CONTAINS(g_s, "x"); }
static void fail_not_contains_present(void)  { ASSERT_SCREEN_NOT_CONTAINS(g_s, "abc"); }
static void fail_contains(void)              { ASSERT_SCREEN_CONTAINS(g_s, "zzz"); }
static void fail_row_contains(void)          { ASSERT_ROW_CONTAINS(g_s, 0, "zzz"); }
static void fail_cell_attr(void)             { ASSERT_CELL_ATTR(g_s, 0, 0, PTY_ATTR_BOLD); }
static void fail_cell_attr_eq(void)          { ASSERT_CELL_ATTR_EQ(g_s, 0, 0, PTY_ATTR_DIM); }
static void fail_cell_text(void)             { ASSERT_CELL_TEXT(g_s, 0, 0, "b"); }
static void fail_cell_fg(void)               { ASSERT_CELL_FG(g_s, 0, 0, PTY_FG_RED); }
static void fail_cell_bg(void)               { ASSERT_CELL_BG(g_s, 0, 0, PTY_BG_RED); }
static void fail_cursor(void)                { ASSERT_CURSOR(g_s, 1, 1); }
static void fail_wait_for(void)              { ASSERT_WAIT_FOR(g_s, "zzz", 10); }
static void fail_exit_status(void)           { ASSERT_EXIT_STATUS(g_s, 0, 10); }
static void fail_wait_screen(void) {
    const char *const expected[] = { "abd", NULL };
    ASSERT_WAIT_SCREEN(g_s, expected, 10);
}
static void fail_region_style(void) {
    static const PtyStyleKey bold[] = { { 'B', PTY_ATTR_BOLD, 0, 0 }, { 0, 0, 0, 0 } };
    const char *const expected[] = { "B", NULL };
    ASSERT_REGION_STYLE(g_s, PTY_REGION_FULL, expected, bold);
}
static void fail_screen_eq(void) {
    const char *const expected[] = { "abd", NULL };
    ASSERT_SCREEN_EQ(g_s, expected);
}

static void test_assertions_can_fail_body(void);

static int must_fail(void (*fn)(void)) {
    int failed = g_tests_failed, run = g_tests_run;
    fn();
    int ok = g_tests_failed == failed + 1 && g_tests_run == run + 1;
    g_tests_failed = failed;
    g_tests_run = run;
    return ok;
}

static void test_assertions_can_fail(void) {
    /* These failures are on purpose: they must not leave evidence that a
     * reader of the directory would take for real ones. */
    const char *outer = getenv("PTYTEST_EVIDENCE_DIR");
    char *saved = outer ? strdup(outer) : NULL;
    unsetenv("PTYTEST_EVIDENCE_DIR");
    test_assertions_can_fail_body();
    if (saved) setenv("PTYTEST_EVIDENCE_DIR", saved, 1);
    free(saved);
}

static void test_assertions_can_fail_body(void) {
    g_s = term(10, 2);
    printf("  (the [FAIL] lines below are expected)\n");
    ASSERT(must_fail(fail_not_contains_on_blank),
           "an absence assertion fails on a blank screen");
    feed(g_s, "abc");
    ASSERT(must_fail(fail_not_contains_present), "NOT_CONTAINS fails when present");
    ASSERT(must_fail(fail_contains), "SCREEN_CONTAINS fails when absent");
    ASSERT(must_fail(fail_row_contains), "ROW_CONTAINS fails when absent");
    ASSERT(must_fail(fail_cell_attr), "CELL_ATTR fails");
    ASSERT(must_fail(fail_cell_attr_eq), "CELL_ATTR_EQ fails");
    ASSERT(must_fail(fail_cell_text), "CELL_TEXT fails");
    ASSERT(must_fail(fail_cell_fg), "CELL_FG fails");
    ASSERT(must_fail(fail_cell_bg), "CELL_BG fails");
    ASSERT(must_fail(fail_cursor), "CURSOR fails");
    ASSERT(must_fail(fail_wait_for), "WAIT_FOR fails on timeout");
    ASSERT(must_fail(fail_exit_status), "EXIT_STATUS fails without a child");
    ASSERT(must_fail(fail_screen_eq), "SCREEN_EQ fails on one changed cell");
    ASSERT(must_fail(fail_wait_screen), "WAIT_SCREEN fails on a timeout");
    ASSERT(must_fail(fail_region_style), "REGION_STYLE fails on a wrong style");
    printf("  (end of expected [FAIL] lines)\n");
    pty_close(g_s);
}

int main(int argc, char **argv) {
    static const TestCase tests[] = {
        TEST_CASE(test_row_text),
        TEST_CASE(test_contains),
        TEST_CASE(test_screen_text),
        TEST_CASE(test_is_blank),
        TEST_CASE(test_compare_match),
        TEST_CASE(test_compare_mismatch),
        TEST_CASE(test_compare_leading_blanks_matter),
        TEST_CASE(test_compare_too_many_rows),
        TEST_CASE(test_compare_wide),
        TEST_CASE(test_region_compare),
        TEST_CASE(test_report_notes),
        TEST_CASE(test_report_moved_block),
        TEST_CASE(test_report_context_gap),
        TEST_CASE(test_style_compare),
        TEST_CASE(test_wait_region_without_child),
        TEST_CASE(test_save_evidence),
        TEST_CASE(test_assertion_saves_evidence),
        TEST_CASE(test_cell_describe),
        TEST_CASE(test_dump_c),
        TEST_CASE(test_dump_ansi),
        TEST_CASE(test_dump),
        TEST_CASE(test_cell_accessors_out_of_range),
        TEST_CASE(test_no_child),
        TEST_CASE(test_setenv_validation),
        TEST_CASE(test_run_rejects_bad_arguments),
        TEST_CASE(test_close_grace_from_environment),
        TEST_CASE(test_version),
        TEST_CASE(test_assertions_can_fail),
    };
    return test_run_suite("inspection unit tests", tests,
                          sizeof(tests) / sizeof(tests[0]),
                          180, 60, argc, argv);
}
