#ifndef PTYTEST_H
#define PTYTEST_H

/**
 * @file ptytest.h
 * @brief PTY-based terminal test library for TUI programs.
 *
 * Opens a pseudo-terminal, forks and execs the program under test, sends
 * keystrokes, and turns everything the program writes into a grid of cells
 * through a built-in terminal emulator.  A test can then ask, cell by cell,
 * which character, attribute and colour is shown, or compare the whole
 * screen against an expected picture.
 *
 * The emulator is a test instrument, not a mock of the product: nothing of
 * the program under test is replaced.  What it understands, and what it
 * drops, is documented in libptytest(7).
 *
 * Depends only on POSIX (forkpty, poll) and the C library.
 *
 * See policies/testing-model.md in the workspace, "A TUI is tested through
 * a real terminal, cell by cell".
 */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define PTYTEST_API __attribute__((visibility("default")))
#else
#define PTYTEST_API
#endif

/* ── Version ─────────────────────────────────────────────────────────── */

#define PTYTEST_VERSION_MAJOR 0
#define PTYTEST_VERSION_MINOR 1

/** @brief Returns the library version string, e.g. "0.1.0.12". */
PTYTEST_API const char *pty_version(void);

/* ── Cell attributes (bitmask) ───────────────────────────────────────── */

#define PTY_ATTR_NONE      0
#define PTY_ATTR_BOLD      (1 << 0)
#define PTY_ATTR_DIM       (1 << 1)
#define PTY_ATTR_REVERSE   (1 << 2)
#define PTY_ATTR_STRIKE    (1 << 3)
#define PTY_ATTR_UNDERLINE (1 << 4)
#define PTY_ATTR_ITALIC    (1 << 5)
#define PTY_ATTR_BLINK     (1 << 6)
#define PTY_ATTR_INVISIBLE (1 << 7)

/* ── Colours ─────────────────────────────────────────────────────────────
 *
 * A colour is reported the way the program selected it, so a test can
 * tell "SGR 31" from "SGR 38;5;1" even though both are usually red:
 *
 *   0                      default colour
 *   30–37, 90–97           foreground SGR value (basic, bright)
 *   40–47, 100–107         background SGR value (basic, bright)
 *   PTY_COLOR_IDX(n)       256-colour palette entry n (SGR 38;5;n / 48;5;n)
 *   PTY_COLOR_RGB(r,g,b)   direct colour (SGR 38;2;r;g;b / 48;2;r;g;b)
 */

#define PTY_COLOR_DEFAULT  0
#define PTY_COLOR_IDX(n)       (0x100 | ((n) & 0xFF))
#define PTY_COLOR_RGB(r, g, b) (0x1000000 | (((r) & 0xFF) << 16) | \
                                (((g) & 0xFF) << 8) | ((b) & 0xFF))
#define PTY_COLOR_IS_IDX(c)    (((c) & ~0xFF) == 0x100)
#define PTY_COLOR_IS_RGB(c)    (((c) & 0x1000000) != 0)

#define PTY_FG_DEFAULT  0
#define PTY_FG_BLACK    30
#define PTY_FG_RED      31
#define PTY_FG_GREEN    32
#define PTY_FG_YELLOW   33
#define PTY_FG_BLUE     34
#define PTY_FG_MAGENTA  35
#define PTY_FG_CYAN     36
#define PTY_FG_WHITE    37

#define PTY_BG_DEFAULT  0
#define PTY_BG_BLACK    40
#define PTY_BG_RED      41
#define PTY_BG_GREEN    42
#define PTY_BG_YELLOW   43
#define PTY_BG_BLUE     44
#define PTY_BG_MAGENTA  45
#define PTY_BG_CYAN     46
#define PTY_BG_WHITE    47

/* ── Special keys ────────────────────────────────────────────────────── */

typedef enum {
    PTY_KEY_ENTER = 0x0D,
    PTY_KEY_ESC   = 0x1B,
    PTY_KEY_TAB   = 0x09,
    PTY_KEY_BACK  = 0x7F,
    PTY_KEY_UP    = 0x100,
    PTY_KEY_DOWN,
    PTY_KEY_RIGHT,
    PTY_KEY_LEFT,
    PTY_KEY_PGUP,
    PTY_KEY_PGDN,
    PTY_KEY_HOME,
    PTY_KEY_END,
    PTY_KEY_INSERT,
    PTY_KEY_DELETE,
    PTY_KEY_SHIFT_TAB,
    PTY_KEY_F1, PTY_KEY_F2, PTY_KEY_F3,  PTY_KEY_F4,
    PTY_KEY_F5, PTY_KEY_F6, PTY_KEY_F7,  PTY_KEY_F8,
    PTY_KEY_F9, PTY_KEY_F10, PTY_KEY_F11, PTY_KEY_F12,
    PTY_KEY_CTRL_C = 0x03,
    PTY_KEY_CTRL_D = 0x04,
} PtyKey;

/** @brief The key code of Ctrl+@p c, e.g. PTY_KEY_CTRL('x') == 0x18. */
#define PTY_KEY_CTRL(c) ((PtyKey)((c) & 0x1F))

/* ── Session opaque handle ───────────────────────────────────────────── */

typedef struct PtySession PtySession;

/* ── Session lifecycle ───────────────────────────────────────────────── */

/**
 * @brief Creates a session with an empty screen of the given size.
 * @return Session handle, or NULL on failure. Caller must pty_close().
 */
PTYTEST_API PtySession *pty_open(int cols, int rows);

/**
 * @brief Sets (or with @p value NULL, unsets) a variable in the environment
 *        the child will be started with.
 *
 * Applied after the defaults pty_run() sets, so it can override TERM and
 * LC_ALL.  Only affects a later pty_run(); the test's own environment is
 * not touched.
 * @return 0 on success, -1 on invalid arguments or allocation failure.
 */
PTYTEST_API int pty_setenv(PtySession *s, const char *name, const char *value);

/**
 * @brief Forks and execs a program inside the PTY.
 *
 * The child gets TERM=xterm-256color and LC_ALL=en_US.UTF-8, has COLUMNS
 * and LINES removed, and then gets every pty_setenv() override.
 * @param argv NULL-terminated argument vector; argv[0] is looked up in PATH.
 * @return 0 on success; -1 if the PTY could not be created or the program
 *         could not be executed (errno is set to the reason).
 */
PTYTEST_API int pty_run(PtySession *s, const char *argv[]);

/**
 * @brief Closes the session and frees resources.
 *
 * A child still running gets the close grace period (pty_set_close_grace())
 * to exit by itself, then SIGTERM, then SIGKILL after two seconds.
 */
PTYTEST_API void pty_close(PtySession *s);

/**
 * @brief Sets how long pty_close() waits for the child to exit by itself
 *        before signalling it.  Default: $PTYTEST_CLOSE_GRACE_MS, else 0.
 */
PTYTEST_API void pty_set_close_grace(PtySession *s, int ms);

/**
 * @brief Resizes the terminal; the kernel delivers SIGWINCH to the child.
 * @return 0 on success, -1 on failure.
 */
PTYTEST_API int pty_resize(PtySession *s, int cols, int rows);

/**
 * @brief Sends a signal to the child.
 * @return 0 on success, -1 if there is no child or kill() failed.
 */
PTYTEST_API int pty_signal(PtySession *s, int sig);

/** @brief Returns the terminal dimensions. */
PTYTEST_API void pty_get_size(PtySession *s, int *cols, int *rows);

/* ── Input ───────────────────────────────────────────────────────────── */

/**
 * @brief Sends raw bytes to the PTY master (the child's stdin).
 *
 * Output is drained while waiting for room, so a large input cannot
 * deadlock against a child blocked on writing.
 * @return 0 when everything was written, -1 on error or after 5 s.
 */
PTYTEST_API int pty_send(PtySession *s, const char *bytes, size_t len);

/**
 * @brief Sends a single key.  Cursor keys follow the child's DECCKM mode
 *        (CSI A in normal mode, SS3 A in application mode), as xterm does.
 */
PTYTEST_API int pty_send_key(PtySession *s, PtyKey key);

/** @brief Sends a NUL-terminated string. */
PTYTEST_API int pty_send_str(PtySession *s, const char *str);

/* ── Synchronisation ─────────────────────────────────────────────────── */

/**
 * @brief Reads output until @p text appears on the screen.
 * @return 0 if found, -1 on timeout or when the child closed the PTY.
 */
PTYTEST_API int pty_wait_for(PtySession *s, const char *text, int timeout_ms);

/** @brief Predicate for pty_wait_until(): non-zero when the wait is over. */
typedef int (*PtyPredicate)(PtySession *s, void *ctx);

/**
 * @brief Reads output until @p pred returns non-zero.
 * @return 0 when the predicate was satisfied, -1 on timeout.
 */
PTYTEST_API int pty_wait_until(PtySession *s, PtyPredicate pred, void *ctx,
                               int timeout_ms);

/**
 * @brief Waits until no new output arrives for @p quiet_ms milliseconds.
 * @return 0 on success, -1 if the child closed the PTY or its output did
 *         not pause within 30 seconds.
 */
PTYTEST_API int pty_settle(PtySession *s, int quiet_ms);

/**
 * @brief Waits for the child to exit on its own, reading output meanwhile.
 *
 * pty_close() terminates the child with SIGTERM and then SIGKILL, which can
 * cut it off mid-write: the last screen line saying the work is done is not
 * a promise that the process has flushed its files and exited.
 * On success the child is reaped and a later pty_close() will not signal it.
 * @return Child exit status (0 on clean success), or -1 on timeout, on a
 *         child killed by a signal, or when there is no child to wait for.
 */
PTYTEST_API int pty_wait_exit(PtySession *s, int timeout_ms);

/**
 * @brief Reads and processes any pending PTY output (non-blocking).
 * @return Number of bytes read, or 0 if nothing was available.
 */
PTYTEST_API int pty_drain(PtySession *s);

/**
 * @brief Feeds bytes straight into the emulator, as if the child had
 *        written them.  Works without pty_run(): a renderer can be tested
 *        by feeding it the bytes it produced.
 */
PTYTEST_API void pty_feed(PtySession *s, const char *data, size_t len);

/* ── Screen inspection ───────────────────────────────────────────────── */

/**
 * @brief Returns the character at (row, col) as a UTF-8 string.
 *
 * A wide character is reported at its first column; its second column
 * returns "".  Combining marks are part of the string of their base.
 * Valid until the screen next changes.  Out of bounds returns "".
 */
PTYTEST_API const char *pty_cell_text(PtySession *s, int row, int col);

/** @brief Returns the PTY_ATTR_* bitmask at (row, col). */
PTYTEST_API int pty_cell_attr(PtySession *s, int row, int col);

/** @brief Returns the foreground colour at (row, col) (see "Colours"). */
PTYTEST_API int pty_cell_fg(PtySession *s, int row, int col);

/** @brief Returns the background colour at (row, col) (see "Colours"). */
PTYTEST_API int pty_cell_bg(PtySession *s, int row, int col);

/**
 * @brief Returns the cursor position (0-based).
 * @return 1 if the cursor is visible (DECTCEM), 0 if hidden, -1 on error.
 */
PTYTEST_API int pty_cursor(PtySession *s, int *row, int *col);

/**
 * @brief Extracts the text of a row, trailing blanks removed.
 * @return @p buf, always NUL-terminated.
 */
PTYTEST_API char *pty_row_text(PtySession *s, int row, char *buf, size_t size);

/** @brief Returns 1 if @p row contains @p text, 0 otherwise. */
PTYTEST_API int pty_row_contains(PtySession *s, int row, const char *text);

/** @brief Returns 1 if any row contains @p text, 0 otherwise. */
PTYTEST_API int pty_screen_contains(PtySession *s, const char *text);

/** @brief Returns 1 if every cell of the screen is blank, 0 otherwise. */
PTYTEST_API int pty_screen_is_blank(PtySession *s);

/**
 * @brief Copies the whole screen into @p buf: one line per row, trailing
 *        blanks removed, rows separated by '\n'.
 * @return @p buf, always NUL-terminated (truncated if too small).
 */
PTYTEST_API char *pty_screen_text(PtySession *s, char *buf, size_t size);

/* ── Comparison ──────────────────────────────────────────────────────────
 *
 * Expectations are written as pictures: an array of UTF-8 row strings,
 * NULL-terminated, one per row from the top of the compared area.  A
 * failed comparison produces a report meant to say *what kind* of
 * difference it is, not only where: a diff-style view with context rows
 * and a column ruler, a caret under every differing column, and notes
 * such as "shifted right by 1 column", "truncated after col 12",
 * "expected row 3 appears at row 4", and the code points of the first
 * differing cell (so that U+00A0 versus a space is visible).
 * See pty_region_compare(3).
 */

/** @brief A rectangle of the screen to compare, and how to report on it. */
typedef struct {
    int row, col;          /**< top-left cell, 0-based */
    int rows, cols;        /**< size; 0 or less: up to the screen edge */
    const char *label;     /**< name used in reports, e.g. "status bar"; may be NULL */
    const char *wildcard;  /**< a character that matches any one cell, e.g. "?"; NULL: none */
    int context;           /**< unchanged rows shown around a difference:
                                0 = default (2), negative = none */
} PtyRegion;

/** @brief A region of @p h rows and @p w columns at (@p r, @p c). */
#define PTY_REGION(r, c, h, w) \
    ((PtyRegion){ .row = (r), .col = (c), .rows = (h), .cols = (w) })

/** @brief The whole screen. */
#define PTY_REGION_FULL PTY_REGION(0, 0, 0, 0)

/**
 * @brief Compares a rectangle of the screen with @p expected, cell by cell.
 *
 * Row i of @p expected is compared with row (region.row + i); rows past the
 * end of the array must be blank.  Within a row every cell of the region
 * counts: missing characters at the end of an expected row stand for
 * blanks.  Cells outside the region are not looked at.  A wide character is
 * written once and covers two cells.  @p reg may be NULL for the whole
 * screen.  When @p report is not NULL it receives the report (truncated to
 * @p report_size, always NUL-terminated); it is empty on a match.
 * @return 0 on a match, the number of differing rows otherwise, or -1 if
 *         the region is outside the screen or an expected row is wider or
 *         taller than the region (the report says which).
 */
PTYTEST_API int pty_region_compare(PtySession *s, const PtyRegion *reg,
                                   const char *const expected[],
                                   char *report, size_t report_size);

/**
 * @brief Compares the whole screen with @p expected:
 *        pty_region_compare() with the region NULL.
 */
PTYTEST_API int pty_screen_compare(PtySession *s, const char *const expected[],
                                   char *report, size_t report_size);

/** @brief One entry of a style legend: a map character and the style it
 *         stands for.  A legend is an array ended by an entry with key 0. */
typedef struct {
    char key;   /**< printable ASCII character used in style maps */
    int  attr;  /**< exact PTY_ATTR_* bitmask */
    int  fg;    /**< exact foreground colour */
    int  bg;    /**< exact background colour */
} PtyStyleKey;

/**
 * @brief Compares the styles (attributes and colours) of a region with a
 *        style map: one legend character per cell.
 *
 * Only the cells the map covers are checked; a map row may be shorter than
 * the region, and rows past the end of the map are not checked.  The
 * region's wildcard, if it is one ASCII character, matches any style.  The
 * report shows the actual map in the same legend ('#' marks a style the
 * legend has no key for) and spells out the first differing cell.
 * @return 0 on a match, the number of differing rows, or -1 on a bad
 *         region, a map larger than the region, or a map character missing
 *         from the legend.
 */
PTYTEST_API int pty_region_compare_style(PtySession *s, const PtyRegion *reg,
                                         const char *const expected[],
                                         const PtyStyleKey legend[],
                                         char *report, size_t report_size);

/**
 * @brief Writes the style map of a region in the given legend, rows
 *        separated by '\n', '#' for styles without a key.  For writing the
 *        expectation of a new test.
 * @return @p buf, always NUL-terminated.
 */
PTYTEST_API char *pty_region_style_map(PtySession *s, const PtyRegion *reg,
                                       const PtyStyleKey legend[],
                                       char *buf, size_t size);

/**
 * @brief Describes one cell in words: its character with code points, and
 *        its style, e.g. "'T' U+0054, bold, fg red (31), bg default".
 * @return @p buf, always NUL-terminated.
 */
PTYTEST_API char *pty_cell_describe(PtySession *s, int row, int col,
                                    char *buf, size_t size);

/**
 * @brief Writes a region as a C array literal ready to paste into a test
 *        as its expectation (a snapshot of the current screen).
 */
PTYTEST_API void pty_region_dump_c(PtySession *s, const PtyRegion *reg, FILE *fp);

/**
 * @brief Writes the screen with its attributes and colours as escape
 *        sequences, so that "cat file" in a terminal shows what the program
 *        showed.  Rows are separated by a reset and CR LF; the last row has
 *        no line end, so feeding the dump to a screen of the same size
 *        reproduces it exactly (pty_feed()).
 */
PTYTEST_API void pty_screen_dump_ansi(PtySession *s, FILE *fp);

/**
 * @brief Reads output until a region equals @p expected, then compares it
 *        once more with a report.
 *
 * The wait for a view that is drawn in several writes, or that replaces a
 * view with the same words in it: unlike pty_wait_for(), it is satisfied
 * only by the exact picture.  On a timeout the report describes how the
 * screen differs at that moment, which says why the wait failed.
 * @return 0 when the region matched within @p timeout_ms, otherwise what
 *         pty_region_compare() returns for the final screen (the number of
 *         differing rows, or -1 on a bad region or expectation).
 */
PTYTEST_API int pty_wait_region(PtySession *s, const PtyRegion *reg,
                                const char *const expected[], int timeout_ms,
                                char *report, size_t report_size);

/** @brief pty_wait_region() for a style map: see pty_region_compare_style(). */
PTYTEST_API int pty_wait_region_style(PtySession *s, const PtyRegion *reg,
                                      const char *const expected[],
                                      const PtyStyleKey legend[], int timeout_ms,
                                      char *report, size_t report_size);

/**
 * @brief Saves the evidence of a failure in directory @p dir (created if it
 *        does not exist, one level): NAME.report.txt with @p text,
 *        NAME.screen.txt with pty_screen_dump(), and NAME.screen.ansi with
 *        pty_screen_dump_ansi().  Existing files of the same name are
 *        replaced.  The assertion macros call it when PTYTEST_EVIDENCE_DIR
 *        is set.
 * @return 0 on success, -1 with errno set.
 */
PTYTEST_API int pty_save_evidence(PtySession *s, const char *dir, const char *name,
                                  const char *text);

/**
 * @brief Writes the screen to @p fp, framed and with row numbers, followed
 *        by the cursor position.  For failure messages and debugging.
 */
PTYTEST_API void pty_screen_dump(PtySession *s, FILE *fp);

/**
 * @brief Enables (1) or disables (0) tracing of every byte read from the
 *        child to stdout.  Process-wide.
 */
PTYTEST_API void pty_trace_enable(int on);

#ifdef __cplusplus
}
#endif

#endif /* PTYTEST_H */
