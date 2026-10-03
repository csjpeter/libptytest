/**
 * @file test_session.c
 * @brief PTY tests: real programs through a real pseudo-terminal, using
 *        only the public interface of the shared library.
 */

#include "test_helpers.h"
#include <errno.h>
#include <sys/stat.h>

#ifndef PTY_FIXTURE
#error "PTY_FIXTURE must name the fixture binary"
#endif
#ifndef PTY_TEST_TMPDIR
#error "PTY_TEST_TMPDIR must name the build-tree scratch directory"
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

/* ── Running programs ────────────────────────────────────────────────── */

static void test_echo(void) {
    const char *argv[] = { "echo", "Hello PTY", NULL };
    PtySession *s = start(40, 10, argv);
    ASSERT_WAIT_FOR(s, "Hello PTY", 3000);
    ASSERT_ROW_CONTAINS(s, 0, "Hello PTY");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

static void test_cat_interactive(void) {
    const char *argv[] = { "cat", NULL };
    PtySession *s = start(40, 10, argv);
    ASSERT(pty_send_str(s, "test123\n") == 0, "send");
    ASSERT_WAIT_FOR(s, "test123", 3000);
    /* The tty echoes the line, then cat prints it again: two rows. */
    ASSERT(pty_settle(s, 100) == 0, "settle");
    ASSERT_ROW_CONTAINS(s, 0, "test123");
    ASSERT_ROW_CONTAINS(s, 1, "test123");
    ASSERT(pty_wait_until(s, (PtyPredicate)NULL, NULL, 10) == -1, "NULL predicate");
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

static void test_missing_program_fails_at_run(void) {
    PtySession *s = pty_open(40, 10);
    ASSERT(s != NULL, "pty_open");
    const char *argv[] = { "/nonexistent/ptytest-no-such-program", NULL };
    errno = 0;
    int rc = pty_run(s, argv);
    int e = errno;
    pty_close(s);
    ASSERT(rc == -1, "pty_run reports the failed exec");
    ASSERT(e == ENOENT, "with the exec errno");
}

static void test_run_twice_is_refused(void) {
    const char *argv[] = { "cat", NULL };
    PtySession *s = start(20, 5, argv);
    errno = 0;
    ASSERT(pty_run(s, argv) == -1 && errno == EINVAL, "second pty_run refused");
    pty_close(s);
}

/* ── Environment ─────────────────────────────────────────────────────── */

static void test_default_environment(void) {
    const char *argv[] = { PTY_FIXTURE, "env", "TERM", NULL };
    PtySession *s = start(40, 5, argv);
    ASSERT_WAIT_FOR(s, "TERM=xterm-256color", 3000);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);

    setenv("COLUMNS", "999", 1);
    const char *argv2[] = { PTY_FIXTURE, "env", "COLUMNS", NULL };
    s = start(40, 5, argv2);
    ASSERT_WAIT_FOR(s, "COLUMNS unset", 3000);
    pty_close(s);
    unsetenv("COLUMNS");
}

static void test_setenv_override_and_unset(void) {
    PtySession *s = pty_open(40, 5);
    ASSERT(s != NULL, "pty_open");
    ASSERT(pty_setenv(s, "TERM", "vt100") == 0, "override TERM");
    const char *argv[] = { PTY_FIXTURE, "env", "TERM", NULL };
    ASSERT(pty_run(s, argv) == 0, "pty_run");
    ASSERT_WAIT_FOR(s, "TERM=vt100", 3000);
    pty_close(s);

    /* The test's own environment is untouched, and an unset reaches the
     * child even for a variable the parent has. */
    setenv("PTYTEST_PROBE", "parent", 1);
    s = pty_open(40, 5);
    ASSERT(s != NULL, "pty_open");
    ASSERT(pty_setenv(s, "PTYTEST_PROBE", NULL) == 0, "unset");
    const char *argv2[] = { PTY_FIXTURE, "env", "PTYTEST_PROBE", NULL };
    ASSERT(pty_run(s, argv2) == 0, "pty_run");
    ASSERT_WAIT_FOR(s, "PTYTEST_PROBE unset", 3000);
    pty_close(s);
    ASSERT(strcmp(getenv("PTYTEST_PROBE"), "parent") == 0, "parent env unchanged");
    unsetenv("PTYTEST_PROBE");
}

/* ── Keys ────────────────────────────────────────────────────────────── */

/* Sends a key and waits for the fixture to report its bytes. */
static int key_gives(PtySession *s, PtyKey key, const char *hex) {
    char want[64];
    snprintf(want, sizeof(want), "K:%s", hex);
    if (pty_send_key(s, key) != 0) return 0;
    int ok = pty_wait_for(s, want, 3000) == 0;
    if (!ok) pty_screen_dump(s, stdout);
    /* Clear the screen so the next key's answer is not confused with this
     * one's. */
    pty_feed(s, "\033[2J\033[H", 7);
    return ok;
}

static void test_keys_normal_mode(void) {
    const char *argv[] = { PTY_FIXTURE, "keys", NULL };
    PtySession *s = start(40, 10, argv);
    ASSERT_WAIT_FOR(s, "READY", 3000);
    ASSERT(key_gives(s, PTY_KEY_UP, "1b5b41"), "Up is CSI A");
    ASSERT(key_gives(s, PTY_KEY_DOWN, "1b5b42"), "Down is CSI B");
    ASSERT(key_gives(s, PTY_KEY_RIGHT, "1b5b43"), "Right is CSI C");
    ASSERT(key_gives(s, PTY_KEY_LEFT, "1b5b44"), "Left is CSI D");
    ASSERT(key_gives(s, PTY_KEY_HOME, "1b5b48"), "Home is CSI H");
    ASSERT(key_gives(s, PTY_KEY_END, "1b5b46"), "End is CSI F");
    ASSERT(key_gives(s, PTY_KEY_PGUP, "1b5b357e"), "PgUp");
    ASSERT(key_gives(s, PTY_KEY_PGDN, "1b5b367e"), "PgDn");
    ASSERT(key_gives(s, PTY_KEY_INSERT, "1b5b327e"), "Insert");
    ASSERT(key_gives(s, PTY_KEY_DELETE, "1b5b337e"), "Delete");
    ASSERT(key_gives(s, PTY_KEY_SHIFT_TAB, "1b5b5a"), "Shift-Tab");
    ASSERT(key_gives(s, PTY_KEY_F1, "1b4f50"), "F1");
    ASSERT(key_gives(s, PTY_KEY_F2, "1b4f51"), "F2");
    ASSERT(key_gives(s, PTY_KEY_F3, "1b4f52"), "F3");
    ASSERT(key_gives(s, PTY_KEY_F4, "1b4f53"), "F4");
    ASSERT(key_gives(s, PTY_KEY_F5, "1b5b31357e"), "F5");
    ASSERT(key_gives(s, PTY_KEY_F6, "1b5b31377e"), "F6");
    ASSERT(key_gives(s, PTY_KEY_F7, "1b5b31387e"), "F7");
    ASSERT(key_gives(s, PTY_KEY_F8, "1b5b31397e"), "F8");
    ASSERT(key_gives(s, PTY_KEY_F9, "1b5b32307e"), "F9");
    ASSERT(key_gives(s, PTY_KEY_F10, "1b5b32317e"), "F10");
    ASSERT(key_gives(s, PTY_KEY_F11, "1b5b32337e"), "F11");
    ASSERT(key_gives(s, PTY_KEY_F12, "1b5b32347e"), "F12");
    ASSERT(key_gives(s, PTY_KEY_ENTER, "0d"), "Enter is CR");
    ASSERT(key_gives(s, PTY_KEY_TAB, "09"), "Tab");
    ASSERT(key_gives(s, PTY_KEY_BACK, "7f"), "Backspace is DEL");
    ASSERT(key_gives(s, PTY_KEY_ESC, "1b"), "Esc");
    ASSERT(key_gives(s, PTY_KEY_CTRL('x'), "18"), "Ctrl-X");
    ASSERT(key_gives(s, PTY_KEY_CTRL_C, "03"), "Ctrl-C (raw mode: a byte)");
    ASSERT(pty_send_str(s, "q") == 0, "quit");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

static void test_keys_application_mode(void) {
    const char *argv[] = { PTY_FIXTURE, "appkeys", NULL };
    PtySession *s = start(40, 10, argv);
    ASSERT_WAIT_FOR(s, "READY", 3000);
    ASSERT(key_gives(s, PTY_KEY_UP, "1b4f41"), "Up is SS3 A under DECCKM");
    ASSERT(key_gives(s, PTY_KEY_DOWN, "1b4f42"), "Down is SS3 B");
    ASSERT(key_gives(s, PTY_KEY_RIGHT, "1b4f43"), "Right is SS3 C");
    ASSERT(key_gives(s, PTY_KEY_LEFT, "1b4f44"), "Left is SS3 D");
    ASSERT(key_gives(s, PTY_KEY_HOME, "1b4f48"), "Home is SS3 H");
    ASSERT(key_gives(s, PTY_KEY_END, "1b4f46"), "End is SS3 F");
    ASSERT(key_gives(s, PTY_KEY_PGUP, "1b5b357e"), "PgUp is unaffected");
    pty_send_str(s, "q");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

/* ── Terminal answers and window size ────────────────────────────────── */

static void test_cursor_position_report(void) {
    const char *argv[] = { PTY_FIXTURE, "dsr", NULL };
    PtySession *s = start(40, 10, argv);
    /* A program that asks where the cursor is blocks until the terminal
     * answers; the emulator has to answer like a real one. */
    ASSERT_WAIT_FOR(s, "REPLY:ESC[5;10R", 3000);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

static void test_resize_delivers_sigwinch(void) {
    const char *argv[] = { PTY_FIXTURE, "winch", NULL };
    PtySession *s = start(40, 10, argv);
    ASSERT_WAIT_FOR(s, "SIZE:40x10", 3000);
    ASSERT(pty_resize(s, 60, 15) == 0, "resize");
    ASSERT_WAIT_FOR(s, "SIZE:60x15", 3000);
    int cols, rows;
    pty_get_size(s, &cols, &rows);
    ASSERT(cols == 60 && rows == 15, "the screen follows the new size");
    pty_send_str(s, "q");
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

/* ── A full screen, cell for cell ────────────────────────────────────── */

static void test_full_screen_frame(void) {
    const char *argv[] = { PTY_FIXTURE, "frame", NULL };
    PtySession *s = start(20, 8, argv);
    ASSERT_WAIT_FOR(s, "status: ok", 3000);
    ASSERT(pty_settle(s, 100) == 0, "settle");

    const char *const expected[] = {
        "",
        "  ┌──────────┐",
        "  │ Title    │",
        "  │> item 1  │",
        "  └──────────┘",
        "",
        "status: ok",
        NULL
    };
    ASSERT_SCREEN_EQ(s, expected);

    /* What the text cannot show: attributes and colours, cell by cell. */
    ASSERT_CELL_ATTR_EQ(s, 2, 2, PTY_ATTR_NONE);              /* border */
    ASSERT_CELL_BG(s, 2, 2, PTY_BG_DEFAULT);
    ASSERT_CELL_ATTR_EQ(s, 2, 4, PTY_ATTR_BOLD);              /* title */
    ASSERT_CELL_BG(s, 2, 4, PTY_BG_BLUE);
    ASSERT_CELL_BG(s, 2, 12, PTY_BG_BLUE);                    /* padding too */
    ASSERT_CELL_BG(s, 2, 13, PTY_BG_DEFAULT);
    ASSERT_CELL_ATTR_EQ(s, 3, 3, PTY_ATTR_REVERSE);           /* selection */
    ASSERT_CELL_ATTR_EQ(s, 3, 12, PTY_ATTR_REVERSE);
    ASSERT_CELL_ATTR_EQ(s, 3, 13, PTY_ATTR_NONE);
    ASSERT_CELL_FG(s, 6, 0, PTY_FG_RED);                      /* status */
    ASSERT_CELL_FG(s, 6, 6, PTY_FG_RED);
    ASSERT_CELL_FG(s, 6, 8, PTY_FG_DEFAULT);
    int r, c;
    ASSERT(pty_cursor(s, &r, &c) == 0, "cursor hidden while drawing");

    /* The box alone, as a region, and its styles as a map. */
    PtyRegion box = PTY_REGION(1, 2, 4, 12);
    box.label = "box";
    const char *const box_text[] = {
        "┌──────────┐",
        "│ Title    │",
        "│> item 1  │",
        "└──────────┘",
        NULL
    };
    ASSERT_REGION_EQ(s, box, box_text);
    static const PtyStyleKey legend[] = {
        { '.', PTY_ATTR_NONE,    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 'T', PTY_ATTR_BOLD,    PTY_FG_DEFAULT, PTY_BG_BLUE },
        { 'R', PTY_ATTR_REVERSE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
        { 0, 0, 0, 0 }
    };
    const char *const box_style[] = {
        "............",
        ".TTTTTTTTTT.",
        ".RRRRRRRRRR.",
        "............",
        NULL
    };
    ASSERT_REGION_STYLE(s, box, box_style, legend);

    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "BYE", 3000);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Title");   /* back on the main screen */
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}

/* ── Exit, settle and close ──────────────────────────────────────────── */

static void test_wait_exit_status(void) {
    const char *argv[] = { PTY_FIXTURE, "exit", "3", NULL };
    PtySession *s = start(40, 5, argv);
    ASSERT_EXIT_STATUS(s, 3, 3000);
    ASSERT(pty_wait_exit(s, 100) == -1, "second wait finds no child");
    pty_close(s);   /* safe after the child was reaped */
}

static void test_wait_exit_drains_output(void) {
    const char *argv[] = { "sh", "-c", "echo done; exit 0", NULL };
    PtySession *s = start(40, 5, argv);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    ASSERT_SCREEN_CONTAINS(s, "done");
    pty_close(s);
}

static void test_wait_exit_timeout(void) {
    const char *argv[] = { "sleep", "30", NULL };
    PtySession *s = start(40, 5, argv);
    ASSERT(pty_wait_exit(s, 200) == -1, "gives up instead of hanging");
    pty_close(s);   /* still the owner of the child: terminates it */
}

static void test_wait_exit_killed_by_signal(void) {
    const char *argv[] = { "sleep", "30", NULL };
    PtySession *s = start(40, 5, argv);
    ASSERT(pty_signal(s, SIGTERM) == 0, "signal delivered");
    ASSERT(pty_wait_exit(s, 3000) == -1, "death by signal is not an exit status");
    ASSERT(pty_signal(s, SIGTERM) == -1, "no child left to signal");
    pty_close(s);
}

static void test_chatty_child_does_not_deadlock(void) {
    /* More output than a PTY buffer holds: without draining while waiting,
     * the child would block writing and the wait would never return. */
    const char *argv[] = { PTY_FIXTURE, "flood", "3000", NULL };
    PtySession *s = start(60, 10, argv);
    ASSERT_EXIT_STATUS(s, 0, 10000);
    ASSERT_SCREEN_CONTAINS(s, "FLOOD-END");
    ASSERT_SCREEN_CONTAINS(s, "line-02999");
    pty_close(s);
}

static void test_large_input_does_not_deadlock(void) {
    /* cat echoes while we write: the input queue and the output queue fill
     * together, and pty_send() has to read while it waits to write. */
    const char *argv[] = { "cat", NULL };
    PtySession *s = start(80, 10, argv);
    static char big[200 * 101 + 32];
    size_t pos = 0;
    for (int i = 0; i < 200; i++) {
        memset(big + pos, 'a' + i % 26, 99);
        pos += 99;
        big[pos++] = '\r';   /* ICRNL turns it into a newline */
        big[pos++] = '\n';
    }
    memcpy(big + pos, "INPUT-END\r", 10);
    pos += 10;
    ASSERT(pty_send(s, big, pos) == 0, "large input written");
    ASSERT_WAIT_FOR(s, "INPUT-END", 10000);
    pty_close(s);
}

static void test_settle(void) {
    const char *argv[] = { "cat", NULL };
    PtySession *s = start(40, 5, argv);
    pty_send_str(s, "x\n");
    ASSERT(pty_settle(s, 100) == 0, "settles once output stops");
    ASSERT_SCREEN_CONTAINS(s, "x");
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    ASSERT(pty_settle(s, 50) == -1, "cannot settle a closed PTY");
    pty_close(s);
}

static void test_wait_for_child_gone(void) {
    const char *argv[] = { "echo", "only this", NULL };
    PtySession *s = start(40, 5, argv);
    ASSERT(pty_wait_for(s, "never printed", 5000) == -1,
           "text that never comes fails once the child is gone");
    ASSERT_SCREEN_CONTAINS(s, "only this");
    pty_close(s);
}

/* Builds the fixture file name: under the build tree, with the pid. */
static void linger_path(char *buf, size_t size, const char *tag) {
    snprintf(buf, size, "%s/linger-%s-%d", PTY_TEST_TMPDIR, tag, (int)getpid());
}

static void test_close_grace_lets_child_finish(void) {
    char path[512];
    linger_path(path, sizeof(path), "grace");
    unlink(path);
    const char *argv[] = { PTY_FIXTURE, "linger", path, "300", NULL };
    PtySession *s = pty_open(40, 5);
    ASSERT(s != NULL, "pty_open");
    pty_set_close_grace(s, 3000);
    ASSERT(pty_run(s, argv) == 0, "pty_run");
    pty_close(s);
    struct stat st;
    int exists = stat(path, &st) == 0;
    unlink(path);
    ASSERT(exists, "with a grace period the child finished its write");
}

static void test_close_without_grace_terminates(void) {
    char path[512];
    linger_path(path, sizeof(path), "nograce");
    unlink(path);
    const char *argv[] = { PTY_FIXTURE, "linger", path, "2000", NULL };
    PtySession *s = start(40, 5, argv);
    pty_close(s);   /* SIGTERM right away */
    struct stat st;
    int exists = stat(path, &st) == 0;
    unlink(path);
    ASSERT(!exists, "without one the child was terminated before writing");
}

static void test_trace(void) {
    const char *argv[] = { "echo", "traced", NULL };
    PtySession *s = start(40, 5, argv);
    pty_trace_enable(1);
    ASSERT_WAIT_FOR(s, "traced", 3000);
    pty_trace_enable(0);
    pty_close(s);
}

int main(int argc, char **argv) {
    static const TestCase tests[] = {
        TEST_CASE(test_echo),
        TEST_CASE(test_cat_interactive),
        TEST_CASE(test_missing_program_fails_at_run),
        TEST_CASE(test_run_twice_is_refused),
        TEST_CASE(test_default_environment),
        TEST_CASE(test_setenv_override_and_unset),
        TEST_CASE(test_keys_normal_mode),
        TEST_CASE(test_keys_application_mode),
        TEST_CASE(test_cursor_position_report),
        TEST_CASE(test_resize_delivers_sigwinch),
        TEST_CASE(test_full_screen_frame),
        TEST_CASE(test_wait_exit_status),
        TEST_CASE(test_wait_exit_drains_output),
        TEST_CASE(test_wait_exit_timeout),
        TEST_CASE(test_wait_exit_killed_by_signal),
        TEST_CASE(test_chatty_child_does_not_deadlock),
        TEST_CASE(test_large_input_does_not_deadlock),
        TEST_CASE(test_settle),
        TEST_CASE(test_wait_for_child_gone),
        TEST_CASE(test_close_grace_lets_child_finish),
        TEST_CASE(test_close_without_grace_terminates),
        TEST_CASE(test_trace),
    };
    return test_run_suite("PTY session tests", tests,
                          sizeof(tests) / sizeof(tests[0]),
                          117, 100, argc, argv);
}
