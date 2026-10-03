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
 * With PTYTEST_EVIDENCE_DIR set in the environment, every failed assertion
 * about a session also saves its evidence there (pty_save_evidence(3)):
 * the message or comparison report, the screen as text, and the screen
 * with its colours.  CI can keep the directory as an artefact.
 *
 * Define PTY_ASSERT_OUT before including this header to send failure
 * messages somewhere other than stdout.
 */

#include "ptytest.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PTY_ASSERT_OUT
#define PTY_ASSERT_OUT stdout
#endif

/* Size of the report buffer of the comparison macros. */
#ifndef PTY_ASSERT_REPORT_SIZE
#define PTY_ASSERT_REPORT_SIZE 32768
#endif

/* Saves the evidence of a failure when PTYTEST_EVIDENCE_DIR is set, under
 * a name made of the source file, the function and the line, so that a
 * file in the directory leads straight back to the assertion. */
static inline void pty_assert_evidence_(PtySession *s, const char *file, int line,
                                        const char *func, const char *message,
                                        const char *report) {
    const char *dir = getenv("PTYTEST_EVIDENCE_DIR");
    if (!dir || !*dir || !s) return;
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    char name[256];
    snprintf(name, sizeof(name), "%s-%s-L%d", base, func, line);
    size_t len = strlen(file) + strlen(func) + strlen(message) +
                 (report ? strlen(report) : 0) + 64;
    char *text = (char *)malloc(len);
    if (!text) {
        fprintf(PTY_ASSERT_OUT, "  evidence not saved: out of memory\n");
        return;
    }
    snprintf(text, len, "%s:%d: in %s: %s\n%s", file, line, func, message,
             report ? report : "");
    if (pty_save_evidence(s, dir, name, text) == 0)
        fprintf(PTY_ASSERT_OUT, "  evidence: %s/%s.{report.txt,screen.txt,screen.ansi}\n",
                dir, name);
    else
        fprintf(PTY_ASSERT_OUT, "  evidence not saved in %s: %s\n", dir, strerror(errno));
    free(text);
}

/* Prints a comparison report, if there is one. */
static inline void pty_assert_print_report_(const char *report) {
    if (report) fputs(report, PTY_ASSERT_OUT);
}

#define PTY_ASSERT_FAIL_(...) do { \
    fprintf(PTY_ASSERT_OUT, "  [FAIL] %s:%d: ", __FILE__, __LINE__); \
    fprintf(PTY_ASSERT_OUT, __VA_ARGS__); \
    fputc('\n', PTY_ASSERT_OUT); \
    g_tests_failed++; \
} while (0)

/* A failure about session s: printed, counted, and its evidence saved.
 * report (may be NULL) is printed after the message. */
#define PTY_ASSERT_FAIL_S_(s, report, ...) do { \
    char _msg[8192]; \
    snprintf(_msg, sizeof(_msg), __VA_ARGS__); \
    fprintf(PTY_ASSERT_OUT, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, _msg); \
    pty_assert_print_report_(report); \
    g_tests_failed++; \
    pty_assert_evidence_(s, __FILE__, __LINE__, __func__, _msg, report); \
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
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "row %d should contain \"%s\" but got \"%s\"", \
                           (int)(row), text, _buf); \
        return; \
    } \
} while (0)

/** @brief Assert that a cell has (at least) the given attribute bits set. */
#define ASSERT_CELL_ATTR(s, row, col, attr_flag) do { \
    g_tests_run++; \
    int _a = pty_cell_attr(s, row, col); \
    if ((_a & (attr_flag)) != (attr_flag)) { \
        char _d[256]; \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "cell (%d,%d) attr=0x%x, expected 0x%x set; the cell is %s", \
                           (int)(row), (int)(col), _a, (unsigned)(attr_flag), \
                           pty_cell_describe(s, row, col, _d, sizeof(_d))); \
        return; \
    } \
} while (0)

/** @brief Assert that a cell's attributes are exactly @p attrs. */
#define ASSERT_CELL_ATTR_EQ(s, row, col, attrs) do { \
    g_tests_run++; \
    int _a = pty_cell_attr(s, row, col); \
    if (_a != (attrs)) { \
        char _d[256]; \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "cell (%d,%d) attr=0x%x, expected exactly 0x%x; the cell is %s", \
                           (int)(row), (int)(col), _a, (unsigned)(attrs), \
                           pty_cell_describe(s, row, col, _d, sizeof(_d))); \
        return; \
    } \
} while (0)

/** @brief Assert that a cell shows exactly @p text (one character). */
#define ASSERT_CELL_TEXT(s, row, col, text) do { \
    g_tests_run++; \
    const char *_t = pty_cell_text(s, row, col); \
    if (strcmp(_t, text) != 0) { \
        char _d[256]; \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "cell (%d,%d) should be \"%s\" but is %s", \
                           (int)(row), (int)(col), text, \
                           pty_cell_describe(s, row, col, _d, sizeof(_d))); \
        return; \
    } \
} while (0)

/** @brief Assert a cell's foreground colour (see ptytest.h, "Colours"). */
#define ASSERT_CELL_FG(s, row, col, color) do { \
    g_tests_run++; \
    int _c = pty_cell_fg(s, row, col); \
    if (_c != (color)) { \
        char _d[256]; \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "cell (%d,%d) fg=0x%x, expected 0x%x; the cell is %s", \
                           (int)(row), (int)(col), _c, (unsigned)(color), \
                           pty_cell_describe(s, row, col, _d, sizeof(_d))); \
        return; \
    } \
} while (0)

/** @brief Assert a cell's background colour (see ptytest.h, "Colours"). */
#define ASSERT_CELL_BG(s, row, col, color) do { \
    g_tests_run++; \
    int _c = pty_cell_bg(s, row, col); \
    if (_c != (color)) { \
        char _d[256]; \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "cell (%d,%d) bg=0x%x, expected 0x%x; the cell is %s", \
                           (int)(row), (int)(col), _c, (unsigned)(color), \
                           pty_cell_describe(s, row, col, _d, sizeof(_d))); \
        return; \
    } \
} while (0)

/** @brief Assert the cursor position (0-based). */
#define ASSERT_CURSOR(s, row, col) do { \
    g_tests_run++; \
    int _r = -1, _c = -1; \
    pty_cursor(s, &_r, &_c); \
    if (_r != (row) || _c != (col)) { \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, "cursor at (%d,%d), expected (%d,%d)", \
                           _r, _c, (int)(row), (int)(col)); \
        return; \
    } \
} while (0)

/** @brief Assert that the full screen contains the given text. */
#define ASSERT_SCREEN_CONTAINS(s, text) do { \
    g_tests_run++; \
    if (!pty_screen_contains(s, text)) { \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "screen should contain \"%s\"; screen was:", text); \
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
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "screen is blank; cannot assert that \"%s\" is absent", text); \
        return; \
    } \
    if (pty_screen_contains(s, text)) { \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "screen should NOT contain \"%s\"; screen was:", text); \
        pty_screen_dump(s, PTY_ASSERT_OUT); \
        return; \
    } \
} while (0)

/* With PTYTEST_SNAPSHOT set in the environment, a failed comparison also
 * prints the actual region as a C literal, ready to paste into the test
 * once the new screen has been judged correct. */
#define PTY_ASSERT_SNAPSHOT_(s, reg) do { \
    if (getenv("PTYTEST_SNAPSHOT")) { \
        fprintf(PTY_ASSERT_OUT, "  actual, as a C literal:\n"); \
        pty_region_dump_c(s, reg, PTY_ASSERT_OUT); \
    } \
} while (0)

/* Shared body of the comparison macros: _m is the comparison's result,
 * _rep its report. */
#define PTY_ASSERT_COMPARED_(s, reg_ptr, what) do { \
    if (_m != 0) { \
        PTY_ASSERT_FAIL_S_(s, _rep ? _rep : "(no memory for the report)\n", "%s", \
                           _m < 0 ? what " could not be made:" \
                                  : what " differs from the expectation:"); \
        PTY_ASSERT_SNAPSHOT_(s, reg_ptr); \
        free(_rep); \
        return; \
    } \
    free(_rep); \
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
    char *_rep = (char *)malloc(PTY_ASSERT_REPORT_SIZE); \
    int _m = pty_region_compare(s, &_reg, expected, _rep, \
                                _rep ? PTY_ASSERT_REPORT_SIZE : 0); \
    PTY_ASSERT_COMPARED_(s, &_reg, "region comparison"); \
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
    char *_rep = (char *)malloc(PTY_ASSERT_REPORT_SIZE); \
    int _m = pty_region_compare_style(s, &_reg, expected, legend, _rep, \
                                      _rep ? PTY_ASSERT_REPORT_SIZE : 0); \
    PTY_ASSERT_COMPARED_(s, &_reg, "style comparison"); \
} while (0)

/**
 * @brief Wait until a region equals @p expected; on a timeout, fail with
 *        the report of the screen as it was then.  See pty_wait_region(3).
 */
#define ASSERT_WAIT_REGION(s, reg, expected, timeout_ms) do { \
    g_tests_run++; \
    PtyRegion _reg = (reg); \
    char *_rep = (char *)malloc(PTY_ASSERT_REPORT_SIZE); \
    int _m = pty_wait_region(s, &_reg, expected, timeout_ms, _rep, \
                             _rep ? PTY_ASSERT_REPORT_SIZE : 0); \
    PTY_ASSERT_COMPARED_(s, &_reg, "awaited region"); \
} while (0)

/** @brief ASSERT_WAIT_REGION for the whole screen. */
#define ASSERT_WAIT_SCREEN(s, expected, timeout_ms) \
    ASSERT_WAIT_REGION(s, PTY_REGION_FULL, expected, timeout_ms)

/** @brief Wait until a region's styles match the map; see pty_wait_region(3). */
#define ASSERT_WAIT_REGION_STYLE(s, reg, expected, legend, timeout_ms) do { \
    g_tests_run++; \
    PtyRegion _reg = (reg); \
    char *_rep = (char *)malloc(PTY_ASSERT_REPORT_SIZE); \
    int _m = pty_wait_region_style(s, &_reg, expected, legend, timeout_ms, _rep, \
                                   _rep ? PTY_ASSERT_REPORT_SIZE : 0); \
    PTY_ASSERT_COMPARED_(s, &_reg, "awaited style"); \
} while (0)

/** @brief Assert that pty_wait_for succeeds within the timeout. */
#define ASSERT_WAIT_FOR(s, text, timeout_ms) do { \
    g_tests_run++; \
    if (pty_wait_for(s, text, timeout_ms) != 0) { \
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "wait_for(\"%s\", %d ms) timed out; screen was:", \
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
        PTY_ASSERT_FAIL_S_(s, (const char *)NULL, \
                           "exit status %d, expected %d (-1: timeout or signal)", \
                           _st, (int)(status)); \
        return; \
    } \
} while (0)

#endif /* PTY_ASSERT_H */
