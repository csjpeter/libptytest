/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Functional tests of the inspection and waiting API against a real child on a real PTY. */
/**
 * @file test_api.c
 * @brief Functional tests: the capabilities that were only exercised by the
 *        in-process unit tests are driven here through the shared library,
 *        a real child process and a real pseudo-terminal.
 *
 * Each test names the capability it covers in docs/functional-coverage.md.
 * See policies/testing-model.md, "Every capability has a functional test".
 */

#include "test_helpers.h"
#include <errno.h>
#include <unistd.h>

#ifndef PTY_FIXTURE
#error "PTY_FIXTURE must name the fixture binary"
#endif

int g_tests_run = 0;
int g_tests_failed = 0;

static PtySession *start(int cols, int rows, const char *argv[]) {
    PtySession *s = pty_open(cols, rows);
    if (!s) {
        printf("  [FATAL] pty_open(%d, %d) failed\n", cols, rows);
        exit(EXIT_FAILURE);
    }
    if (pty_run(s, argv) != 0) {
        printf("  [FATAL] pty_run(%s) failed: %s\n", argv[0], strerror(errno));
        exit(EXIT_FAILURE);
    }
    return s;
}

static PtySession *print_and_finish(int cols, int rows, const char *bytes) {
    const char *argv[] = { PTY_FIXTURE, "print", bytes, NULL };
    PtySession *s = start(cols, rows, argv);
    if (pty_wait_exit(s, 3000) != 0) {
        printf("  [FATAL] fixture print did not exit cleanly\n");
        exit(EXIT_FAILURE);
    }
    return s;
}

static const PtyStyleKey legend[] = {
    { '.', PTY_ATTR_NONE,    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'T', PTY_ATTR_BOLD,    PTY_FG_DEFAULT, PTY_BG_BLUE },
    { 'R', PTY_ATTR_REVERSE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 0, 0, 0, 0 }
};

/* pty_version */
static void test_version(void) {
    const char *v = pty_version();
    ASSERT(v != NULL && v[0] >= '0' && v[0] <= '9', "version starts with a digit");
    ASSERT(strncmp(v, "0.1", 3) == 0, "version carries the major.minor of the header");
}

/* pty_drain: non-blocking read of what the child has written */
static void test_drain(void) {
    const char *argv[] = { PTY_FIXTURE, "print", "drained", NULL };
    PtySession *s = start(20, 3, argv);
    int total = 0;
    for (int i = 0; i < 300 && !pty_screen_contains(s, "drained"); i++) {
        int n = pty_drain(s);
        ASSERT(n >= 0, "drain does not fail");
        total += n;
        if (n == 0) usleep(10000);
    }
    ASSERT(total >= 7, "drain returned the bytes it read");
    ASSERT_SCREEN_CONTAINS(s, "drained");
    pty_close(s);
}

/* pty_cell_text, pty_row_text, pty_row_contains, pty_screen_contains */
static void test_text_inspection(void) {
    PtySession *s = print_and_finish(20, 3,
        "\033[1;1Hab\xE6\x97\xA5" "z\033[3;1Hlast");
    ASSERT(strcmp(pty_cell_text(s, 0, 0), "a") == 0, "cell (0,0)");
    ASSERT(strcmp(pty_cell_text(s, 0, 2), "\xE6\x97\xA5") == 0, "wide char at first column");
    ASSERT(strcmp(pty_cell_text(s, 0, 3), "") == 0, "wide char second column is empty");
    ASSERT(strcmp(pty_cell_text(s, 99, 99), "") == 0, "out of bounds is empty");
    char buf[64];
    ASSERT(pty_row_text(s, 0, buf, sizeof(buf)) == buf, "row_text returns buf");
    ASSERT(strcmp(buf, "ab\xE6\x97\xA5" "z") == 0, "row 0 text without trailing blanks");
    pty_row_text(s, 1, buf, sizeof(buf));
    ASSERT(buf[0] == '\0', "blank row is empty");
    pty_row_text(s, 0, buf, 3);
    ASSERT(strcmp(buf, "ab") == 0, "row_text truncates and terminates");
    ASSERT(pty_row_contains(s, 2, "last") == 1, "row 2 contains last");
    ASSERT(pty_row_contains(s, 0, "last") == 0, "row 0 does not contain last");
    ASSERT(pty_screen_contains(s, "last") == 1, "screen contains last");
    ASSERT(pty_screen_contains(s, "nope") == 0, "screen does not contain nope");
    pty_close(s);
}

/* pty_screen_compare: a match, and a mismatch with its report */
static void test_screen_compare(void) {
    PtySession *s = print_and_finish(10, 3, "\033[1;1Hone\033[2;1Htwo");
    const char *const same[] = { "one", "two", NULL };
    char report[2048];
    ASSERT(pty_screen_compare(s, same, report, sizeof(report)) == 0, "match");
    ASSERT(report[0] == '\0', "report is empty on a match");
    const char *const diff[] = { "one", "twx", NULL };
    ASSERT(pty_screen_compare(s, diff, report, sizeof(report)) == 1, "one differing row");
    ASSERT(strstr(report, "two") != NULL && strstr(report, "twx") != NULL,
           "report shows both rows");
    pty_close(s);
}

/* pty_screen_dump_ansi: replaying the dump reproduces text and styles */
static void test_dump_ansi_roundtrip(void) {
    const char *argv[] = { PTY_FIXTURE, "frame", NULL };
    PtySession *s = start(20, 8, argv);
    ASSERT_WAIT_FOR(s, "status: ok", 3000);
    char *dump = NULL;
    size_t dump_len = 0;
    FILE *fp = open_memstream(&dump, &dump_len);
    ASSERT(fp != NULL, "open_memstream");
    pty_screen_dump_ansi(s, fp);
    fclose(fp);
    ASSERT(dump_len > 0, "dump is not empty");

    PtySession *copy = pty_open(20, 8);
    ASSERT(copy != NULL, "second session");
    pty_feed(copy, dump, dump_len);
    char a[1024], b[1024];
    pty_screen_text(s, a, sizeof(a));
    pty_screen_text(copy, b, sizeof(b));
    ASSERT(strcmp(a, b) == 0, "replayed text equals the original");
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 20; c++) {
            if (pty_cell_attr(s, r, c) != pty_cell_attr(copy, r, c) ||
                pty_cell_fg(s, r, c) != pty_cell_fg(copy, r, c) ||
                pty_cell_bg(s, r, c) != pty_cell_bg(copy, r, c)) {
                ASSERT(0, "replayed style equals the original");
                r = 8;
                break;
            }
        }
    free(dump);
    pty_close(copy);
    pty_send_str(s, "q");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

/* pty_wait_region_style: satisfied by the drawn styles, times out with a report */
static void test_wait_region_style(void) {
    const char *argv[] = { PTY_FIXTURE, "frame", NULL };
    PtySession *s = start(20, 8, argv);
    PtyRegion box = PTY_REGION(1, 2, 4, 12);
    const char *const good[] = {
        "............",
        ".TTTTTTTTTT.",
        ".RRRRRRRRRR.",
        "............",
        NULL
    };
    char report[2048];
    ASSERT(pty_wait_region_style(s, &box, good, legend, 3000,
                                 report, sizeof(report)) == 0, "styles match");
    const char *const bad[] = {
        "............",
        ".TTTTTTTTTT.",
        ".TTTTTTTTTT.",
        "............",
        NULL
    };
    ASSERT(pty_wait_region_style(s, &box, bad, legend, 200,
                                 report, sizeof(report)) == 1, "one differing row");
    ASSERT(report[0] != '\0', "timeout leaves a report saying why");
    pty_send_str(s, "q");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

/* pty_screen_is_blank */
static void test_screen_is_blank(void) {
    PtySession *s = pty_open(10, 3);
    ASSERT(s != NULL, "pty_open");
    ASSERT(pty_screen_is_blank(s) == 1, "a new screen is blank");
    pty_close(s);
    s = print_and_finish(10, 3, "x");
    ASSERT(pty_screen_is_blank(s) == 0, "a screen with text is not blank");
    pty_close(s);
    s = print_and_finish(10, 3, "x\033[2J");
    ASSERT(pty_screen_is_blank(s) == 1, "erased by the child, blank again");
    pty_close(s);
}

/* ASSERT_CELL_ATTR: a flag is set, others are not asserted */
static void test_assert_cell_attr_flag(void) {
    PtySession *s = print_and_finish(10, 2, "\033[1;4mB\033[0mn");
    ASSERT_CELL_ATTR(s, 0, 0, PTY_ATTR_BOLD);
    ASSERT_CELL_ATTR(s, 0, 0, PTY_ATTR_UNDERLINE);
    ASSERT(pty_cell_attr(s, 0, 1) == PTY_ATTR_NONE, "plain cell has no flag");
    pty_close(s);
}

int main(int argc, char **argv) {
    static const TestCase tests[] = {
        TEST_CASE(test_version),
        TEST_CASE(test_drain),
        TEST_CASE(test_text_inspection),
        TEST_CASE(test_screen_compare),
        TEST_CASE(test_dump_ansi_roundtrip),
        TEST_CASE(test_wait_region_style),
        TEST_CASE(test_screen_is_blank),
        TEST_CASE(test_assert_cell_attr_flag),
    };
    return test_run_suite("Functional API tests", tests,
                          sizeof(tests) / sizeof(tests[0]),
                          30, 100, argc, argv);
}
