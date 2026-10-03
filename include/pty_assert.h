#ifndef PTY_ASSERT_H
#define PTY_ASSERT_H

/**
 * @file pty_assert.h
 * @brief Assertion macros for PTY-based terminal tests.
 *
 * The test program defines two counters, which every macro updates:
 *
 *     int g_tests_run = 0;
 *     int g_tests_failed = 0;
 *
 * A failed assertion prints what was expected *and what was observed*,
 * then returns from the calling (void) test function.  See
 * policies/testing-model.md, "Failure messages carry the observed value".
 *
 * Define PTY_ASSERT_OUT before including this header to send failure
 * messages somewhere other than stdout.
 */

#include "ptytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PTY_ASSERT_OUT
#define PTY_ASSERT_OUT stdout
#endif

#define PTY_ASSERT_FAIL_(...) do { \
    fprintf(PTY_ASSERT_OUT, "  [FAIL] %s:%d: ", __FILE__, __LINE__); \
    fprintf(PTY_ASSERT_OUT, __VA_ARGS__); \
    fputc('\n', PTY_ASSERT_OUT); \
    g_tests_failed++; \
} while (0)

#ifndef ASSERT
#define ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        PTY_ASSERT_FAIL_("%s", msg); \
        return; \
    } \
} while (0)
#endif

/** @brief Assert that a screen row contains the given text. */
#define ASSERT_ROW_CONTAINS(s, row, text) do { \
    g_tests_run++; \
    if (!pty_row_contains(s, row, text)) { \
        char _buf[4096]; \
        pty_row_text(s, row, _buf, sizeof(_buf)); \
        PTY_ASSERT_FAIL_("row %d should contain \"%s\" but got \"%s\"", \
                         (int)(row), text, _buf); \
        return; \
    } \
} while (0)

/** @brief Assert that a cell has (at least) the given attribute bits set. */
#define ASSERT_CELL_ATTR(s, row, col, attr_flag) do { \
    g_tests_run++; \
    int _a = pty_cell_attr(s, row, col); \
    if ((_a & (attr_flag)) != (attr_flag)) { \
        PTY_ASSERT_FAIL_("cell (%d,%d) attr=0x%x, expected 0x%x set", \
                         (int)(row), (int)(col), _a, (unsigned)(attr_flag)); \
        return; \
    } \
} while (0)

/** @brief Assert that a cell's attributes are exactly @p attrs. */
#define ASSERT_CELL_ATTR_EQ(s, row, col, attrs) do { \
    g_tests_run++; \
    int _a = pty_cell_attr(s, row, col); \
    if (_a != (attrs)) { \
        PTY_ASSERT_FAIL_("cell (%d,%d) attr=0x%x, expected exactly 0x%x", \
                         (int)(row), (int)(col), _a, (unsigned)(attrs)); \
        return; \
    } \
} while (0)

/** @brief Assert that a cell shows exactly @p text (one character). */
#define ASSERT_CELL_TEXT(s, row, col, text) do { \
    g_tests_run++; \
    const char *_t = pty_cell_text(s, row, col); \
    if (strcmp(_t, text) != 0) { \
        PTY_ASSERT_FAIL_("cell (%d,%d) should be \"%s\" but is \"%s\"", \
                         (int)(row), (int)(col), text, _t); \
        return; \
    } \
} while (0)

/** @brief Assert a cell's foreground colour (see ptytest.h, "Colours"). */
#define ASSERT_CELL_FG(s, row, col, color) do { \
    g_tests_run++; \
    int _c = pty_cell_fg(s, row, col); \
    if (_c != (color)) { \
        PTY_ASSERT_FAIL_("cell (%d,%d) fg=0x%x, expected 0x%x", \
                         (int)(row), (int)(col), _c, (unsigned)(color)); \
        return; \
    } \
} while (0)

/** @brief Assert a cell's background colour (see ptytest.h, "Colours"). */
#define ASSERT_CELL_BG(s, row, col, color) do { \
    g_tests_run++; \
    int _c = pty_cell_bg(s, row, col); \
    if (_c != (color)) { \
        PTY_ASSERT_FAIL_("cell (%d,%d) bg=0x%x, expected 0x%x", \
                         (int)(row), (int)(col), _c, (unsigned)(color)); \
        return; \
    } \
} while (0)

/** @brief Assert the cursor position (0-based). */
#define ASSERT_CURSOR(s, row, col) do { \
    g_tests_run++; \
    int _r = -1, _c = -1; \
    pty_cursor(s, &_r, &_c); \
    if (_r != (row) || _c != (col)) { \
        PTY_ASSERT_FAIL_("cursor at (%d,%d), expected (%d,%d)", \
                         _r, _c, (int)(row), (int)(col)); \
        return; \
    } \
} while (0)

/** @brief Assert that the full screen contains the given text. */
#define ASSERT_SCREEN_CONTAINS(s, text) do { \
    g_tests_run++; \
    if (!pty_screen_contains(s, text)) { \
        PTY_ASSERT_FAIL_("screen should contain \"%s\"; screen was:", text); \
        pty_screen_dump(s, PTY_ASSERT_OUT); \
        return; \
    } \
} while (0)

/**
 * @brief Assert that the full screen does NOT contain the given text.
 *
 * Fails on a blank screen: an absence assertion must not pass when there
 * was nothing to search, which is exactly what a crashed program leaves.
 * See policies/testing-model.md, "A test must be able to fail".
 */
#define ASSERT_SCREEN_NOT_CONTAINS(s, text) do { \
    g_tests_run++; \
    if (pty_screen_is_blank(s)) { \
        PTY_ASSERT_FAIL_("screen is blank; cannot assert that \"%s\" is absent", \
                         text); \
        return; \
    } \
    if (pty_screen_contains(s, text)) { \
        PTY_ASSERT_FAIL_("screen should NOT contain \"%s\"; screen was:", text); \
        pty_screen_dump(s, PTY_ASSERT_OUT); \
        return; \
    } \
} while (0)

/* Size of the report buffer of the comparison macros. */
#ifndef PTY_ASSERT_REPORT_SIZE
#define PTY_ASSERT_REPORT_SIZE 32768
#endif

/* With PTYTEST_SNAPSHOT set in the environment, a failed comparison also
 * prints the actual region as a C literal, ready to paste into the test
 * once the new screen has been judged correct. */
#define PTY_ASSERT_SNAPSHOT_(s, reg) do { \
    if (getenv("PTYTEST_SNAPSHOT")) { \
        fprintf(PTY_ASSERT_OUT, "  actual, as a C literal:\n"); \
        pty_region_dump_c(s, reg, PTY_ASSERT_OUT); \
    } \
} while (0)

/**
 * @brief Assert that a region equals @p expected, cell for cell.
 *
 * @p reg is a PtyRegion value, e.g. PTY_REGION(0, 0, 1, 80) or a variable
 * with a label and a wildcard.  See pty_region_compare(3).
 */
#define ASSERT_REGION_EQ(s, reg, expected) do { \
    g_tests_run++; \
    PtyRegion _reg = (reg); \
    char *_rep = malloc(PTY_ASSERT_REPORT_SIZE); \
    int _m = pty_region_compare(s, &_reg, expected, _rep, \
                                _rep ? PTY_ASSERT_REPORT_SIZE : 0); \
    if (_m != 0) { \
        PTY_ASSERT_FAIL_("%s", _m < 0 ? "region comparison could not be made:" \
                                     : "region differs from the expectation:"); \
        fputs(_rep ? _rep : "(no memory for the report)\n", PTY_ASSERT_OUT); \
        PTY_ASSERT_SNAPSHOT_(s, &_reg); \
        free(_rep); \
        return; \
    } \
    free(_rep); \
} while (0)

/**
 * @brief Assert that the whole screen equals @p expected, cell for cell.
 *
 * @p expected is a NULL-terminated array of row strings; rows past its end
 * must be blank.  See pty_screen_compare(3).
 */
#define ASSERT_SCREEN_EQ(s, expected) ASSERT_REGION_EQ(s, PTY_REGION_FULL, expected)

/**
 * @brief Assert the styles of a region against a style map and legend.
 *        See pty_region_compare_style(3).
 */
#define ASSERT_REGION_STYLE(s, reg, expected, legend) do { \
    g_tests_run++; \
    PtyRegion _reg = (reg); \
    char *_rep = malloc(PTY_ASSERT_REPORT_SIZE); \
    int _m = pty_region_compare_style(s, &_reg, expected, legend, _rep, \
                                      _rep ? PTY_ASSERT_REPORT_SIZE : 0); \
    if (_m != 0) { \
        PTY_ASSERT_FAIL_("%s", _m < 0 ? "style comparison could not be made:" \
                                     : "styles differ from the expectation:"); \
        fputs(_rep ? _rep : "(no memory for the report)\n", PTY_ASSERT_OUT); \
        free(_rep); \
        return; \
    } \
    free(_rep); \
} while (0)

/** @brief Assert that pty_wait_for succeeds within the timeout. */
#define ASSERT_WAIT_FOR(s, text, timeout_ms) do { \
    g_tests_run++; \
    if (pty_wait_for(s, text, timeout_ms) != 0) { \
        PTY_ASSERT_FAIL_("wait_for(\"%s\", %d ms) timed out; screen was:", \
                         text, (int)(timeout_ms)); \
        pty_screen_dump(s, PTY_ASSERT_OUT); \
        return; \
    } \
} while (0)

/** @brief Assert that the child exits by itself with @p status. */
#define ASSERT_EXIT_STATUS(s, status, timeout_ms) do { \
    g_tests_run++; \
    int _st = pty_wait_exit(s, timeout_ms); \
    if (_st != (status)) { \
        PTY_ASSERT_FAIL_("exit status %d, expected %d (-1: timeout or signal)", \
                         _st, (int)(status)); \
        return; \
    } \
} while (0)

#endif /* PTY_ASSERT_H */
