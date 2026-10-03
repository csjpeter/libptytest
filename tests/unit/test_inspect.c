/**
 * @file test_inspect.c
 * @brief Unit tests for the screen inspection API and the assertion
 *        macros, driven through pty_feed() without a child process.
 */

#include "test_helpers.h"
#include "pty_internal.h"
#include <errno.h>

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
    char report[512];
    ASSERT(pty_screen_compare(s, expected, report, sizeof(report)) == 2,
           "one shifted row and one unexpected row");
    ASSERT(strstr(report, "row  1 differs at col 2") != NULL,
           "report names the row and the first differing column");
    ASSERT(strstr(report, "expected ||  menu  ||") != NULL, "report has the expectation");
    ASSERT(strstr(report, "actual   || menu   ||") != NULL, "report has the observation");
    ASSERT(strstr(report, "row  3 differs") != NULL, "a stray row is a mismatch");
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
    ASSERT(pty_screen_compare(s, shifted, NULL, 0) == 1,
           "a column shift is a difference");
    pty_close(s);
}

static void test_compare_too_many_rows(void) {
    PtySession *s = term(10, 2);
    const char *const expected[] = { "a", "b", "c", NULL };
    char report[128];
    ASSERT(pty_screen_compare(s, expected, report, sizeof(report)) == -1,
           "more expected rows than the screen has");
    ASSERT(strstr(report, "expected 3 rows but the screen has 2") != NULL,
           "and the report says so");
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
static void fail_screen_eq(void) {
    const char *const expected[] = { "abd", NULL };
    ASSERT_SCREEN_EQ(g_s, expected);
}

static int must_fail(void (*fn)(void)) {
    int failed = g_tests_failed, run = g_tests_run;
    fn();
    int ok = g_tests_failed == failed + 1 && g_tests_run == run + 1;
    g_tests_failed = failed;
    g_tests_run = run;
    return ok;
}

static void test_assertions_can_fail(void) {
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
                          90, 60, argc, argv);
}
