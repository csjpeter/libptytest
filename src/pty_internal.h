/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Péter Császár <peter@csaszar.email>
 */
#ifndef PTY_INTERNAL_H
#define PTY_INTERNAL_H

/**
 * @file pty_internal.h
 * @brief Internal data structures shared between libptytest modules.
 *
 * Nothing here is part of the installed interface.  The unit tests link the
 * static library and use these directly; consumers see only ptytest.h.
 */

#include "ptytest.h"
#include <stdint.h>
#include <sys/types.h>

/* ── Screen cell ─────────────────────────────────────────────────────── */

/** Room for a base character and a few combining marks, in UTF-8. */
#define PTY_CELL_BYTES 32

typedef struct {
    char    ch[PTY_CELL_BYTES]; /**< UTF-8 text; "" for a wide char's 2nd column */
    int     attr;               /**< PTY_ATTR_* bitmask */
    int32_t fg;                 /**< Foreground colour, see ptytest.h */
    int32_t bg;                 /**< Background colour, see ptytest.h */
    uint8_t width;              /**< 1 normal, 2 wide head, 0 wide tail */
} PtyCell;

/* ── Parser state ────────────────────────────────────────────────────── */

typedef enum {
    PS_GROUND = 0,
    PS_ESC,          /**< after ESC */
    PS_ESC_INT,      /**< ESC followed by intermediate byte(s) */
    PS_CSI,          /**< collecting CSI parameters and intermediates */
    PS_CSI_IGNORE,   /**< malformed CSI: swallow until the final byte */
    PS_OSC,          /**< OSC string, ends with BEL or ST */
    PS_STR,          /**< DCS / SOS / PM / APC string, ends with ST */
} PtyParseState;

#define PTY_CSI_MAX_PARAMS 32
#define PTY_RESP_MAX       256

/* ── Virtual screen buffer ───────────────────────────────────────────── */

typedef struct PtyScreen {
    int      cols, rows;
    int      cur_row, cur_col;
    int      cur_attr;          /**< Current SGR attribute state */
    int32_t  cur_fg, cur_bg;    /**< Current colours */
    int      pending_wrap;      /**< cursor is past the last column */
    int      top, bottom;       /**< scrolling region, inclusive, 0-based */

    /* Modes */
    int      cursor_visible;    /**< DECTCEM (?25) */
    int      autowrap;          /**< DECAWM (?7) */
    int      origin_mode;       /**< DECOM (?6) */
    int      insert_mode;       /**< IRM (4) */
    int      app_cursor;        /**< DECCKM (?1) */
    int      alt_active;        /**< alternate screen in use */
    int      g0_graphics;       /**< G0 = DEC special graphics */
    int      g1_graphics;       /**< G1 = DEC special graphics */
    int      shift_out;         /**< SO: G1 invoked into GL */

    /* Saved cursor (DECSC / CSI s / ?1049) */
    int      sv_row, sv_col, sv_attr, sv_wrap, sv_g0, sv_origin;
    int32_t  sv_fg, sv_bg;

    PtyCell *cells;             /**< active buffer, rows x cols */
    PtyCell *main_cells;        /**< normal screen */
    PtyCell *alt_cells;         /**< alternate screen */
    uint8_t *tabs;              /**< 1 where a tab stop is set, per column */

    /* Last printed character, for REP (CSI b) */
    uint32_t last_cp;

    /* Parser */
    PtyParseState state;
    int      params[PTY_CSI_MAX_PARAMS];
    uint8_t  param_sub[PTY_CSI_MAX_PARAMS]; /**< 1 = introduced by ':' */
    int      nparams;
    int      param_started;     /**< digits seen for the current param */
    char     csi_private;       /**< '?', '>', '=', '<' or 0 */
    char     intermediate;      /**< last intermediate byte or 0 */
    int      str_esc;           /**< ESC seen inside a string state */

    /* UTF-8 decoder */
    uint32_t utf8_cp;
    int      utf8_need;

    /* Bytes the terminal answers with (DSR, DA), for the session to send */
    char     resp[PTY_RESP_MAX];
    size_t   resp_len;
} PtyScreen;

PtyScreen  *pty_screen_new(int cols, int rows);
void        pty_screen_free(PtyScreen *scr);
void        pty_screen_feed(PtyScreen *scr, const char *data, size_t len);
int         pty_screen_resize(PtyScreen *scr, int cols, int rows);
PtyCell    *pty_screen_cell(PtyScreen *scr, int row, int col);

/** Display width of a code point: 0 (combining), 1 or 2. */
int         pty_char_width(uint32_t cp);

/* ── PTY session ─────────────────────────────────────────────────────── */

typedef struct {
    char *name;
    char *value;   /**< NULL = unset */
} PtyEnvVar;

struct PtySession {
    int        master_fd;
    pid_t      child_pid;
    int        cols, rows;
    int        close_grace_ms;
    PtyScreen *screen;
    PtyEnvVar *env;
    size_t     env_count;
};

/** Reads once from the master and feeds the screen.
 *  @return bytes read, 0 on EOF/EIO, -2 when nothing is available. */
int  pty_read_and_feed(PtySession *s);

/** Sends whatever the emulator wants to answer to the child. */
void pty_flush_responses(PtySession *s);

/** Monotonic clock in milliseconds. */
long pty_now_ms(void);

#endif /* PTY_INTERNAL_H */
