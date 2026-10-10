/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-03 */
/* The virtual screen and the terminal emulator that fills it. */
/**
 * @file pty_screen.c
 * @brief Virtual screen buffer and the terminal emulator that fills it.
 *
 * The parser is a byte-driven state machine (ground, ESC, CSI, OSC and
 * string states, plus a UTF-8 decoder) whose state survives between calls,
 * so a sequence split across two reads of the PTY is assembled rather than
 * dropped.  It emulates the subset of xterm that TERM=xterm-256color
 * programs use; the complete list of what is understood, and what is
 * deliberately ignored, is in man/man7/libptytest.7.  Keep the two in step:
 * the emulator's limits are the test suite's limits.
 */

#define _XOPEN_SOURCE 700

#include "pty_internal.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── Character width ─────────────────────────────────────────────────── */

/* The program under test measures its strings with the C library's
 * wcwidth() in a UTF-8 locale; using the same function here is what makes
 * the emulator agree with it column for column.  A locale object is used
 * instead of setlocale() so the test process's own locale is not touched. */
static locale_t g_utf8_loc = (locale_t)0;
static int      g_loc_tried = 0;

int pty_char_width(uint32_t cp) {
    if (cp < 0x7F) return 1;
    if (!g_loc_tried) {
        g_loc_tried = 1;
        g_utf8_loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
        if (!g_utf8_loc)
            g_utf8_loc = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
    }
    if (g_utf8_loc) {
        locale_t old = uselocale(g_utf8_loc);
        int w = wcwidth((wchar_t)cp);
        uselocale(old);
        if (w < 0) return 1;   /* unassigned or control: occupy one cell */
        return w > 2 ? 2 : w;
    }
    /* No UTF-8 locale on this machine: a minimal fallback. */
    if (cp >= 0x0300 && cp <= 0x036F) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* DEC special graphics (ESC ( 0), 0x5F–0x7E, as Unicode. */
static const uint16_t dec_graphics[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, /* _`abcdef */
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, /* ghijklmn */
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, /* opqrstuv */
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, /* wxyz{|}~ */
};

/* ── Cells ───────────────────────────────────────────────────────────── */

PtyCell *pty_screen_cell(PtyScreen *scr, int row, int col) {
    if (!scr || row < 0 || row >= scr->rows || col < 0 || col >= scr->cols)
        return NULL;
    return &scr->cells[row * scr->cols + col];
}

/* An erased cell takes the current background colour, as xterm does
 * ("background colour erase"); TERM=xterm-256color advertises bce, so
 * programs rely on it to paint areas with a single erase. */
static void blank(PtyScreen *scr, PtyCell *c) {
    c->ch[0] = ' ';
    c->ch[1] = '\0';
    c->attr  = PTY_ATTR_NONE;
    c->fg    = PTY_COLOR_DEFAULT;
    c->bg    = scr->cur_bg;
    c->width = 1;
}

static void blank_plain(PtyCell *c) {
    c->ch[0] = ' ';
    c->ch[1] = '\0';
    c->attr  = PTY_ATTR_NONE;
    c->fg    = PTY_COLOR_DEFAULT;
    c->bg    = PTY_COLOR_DEFAULT;
    c->width = 1;
}

/* Overwriting or erasing one half of a wide character destroys the other
 * half too; otherwise a stray tail would sit in the grid. */
static void break_wide(PtyScreen *scr, int row, int col) {
    PtyCell *c = pty_screen_cell(scr, row, col);
    if (!c) return;
    if (c->width == 0) {
        PtyCell *head = pty_screen_cell(scr, row, col - 1);
        if (head) blank(scr, head);
        blank(scr, c);
    } else if (c->width == 2) {
        PtyCell *tail = pty_screen_cell(scr, row, col + 1);
        if (tail) blank(scr, tail);
        blank(scr, c);
    }
}

static void erase_range(PtyScreen *scr, int row, int from, int to) {
    if (from < 0) from = 0;
    if (to > scr->cols) to = scr->cols;
    if (from >= to) return;
    break_wide(scr, row, from);
    break_wide(scr, row, to - 1);
    for (int c = from; c < to; c++) blank(scr, pty_screen_cell(scr, row, c));
}

static void erase_rows(PtyScreen *scr, int from, int to) {
    for (int r = from; r < to; r++) erase_range(scr, r, 0, scr->cols);
}

/* ── Scrolling ───────────────────────────────────────────────────────── */

static void scroll_up(PtyScreen *scr, int top, int bottom, int n) {
    int height = bottom - top + 1;
    if (n <= 0 || height <= 0) return;
    if (n > height) n = height;
    size_t row_bytes = (size_t)scr->cols * sizeof(PtyCell);
    memmove(&scr->cells[top * scr->cols], &scr->cells[(top + n) * scr->cols],
            (size_t)(height - n) * row_bytes);
    erase_rows(scr, bottom - n + 1, bottom + 1);
}

static void scroll_down(PtyScreen *scr, int top, int bottom, int n) {
    int height = bottom - top + 1;
    if (n <= 0 || height <= 0) return;
    if (n > height) n = height;
    size_t row_bytes = (size_t)scr->cols * sizeof(PtyCell);
    memmove(&scr->cells[(top + n) * scr->cols], &scr->cells[top * scr->cols],
            (size_t)(height - n) * row_bytes);
    erase_rows(scr, top, top + n);
}

static void linefeed(PtyScreen *scr) {
    scr->pending_wrap = 0;
    if (scr->cur_row == scr->bottom)
        scroll_up(scr, scr->top, scr->bottom, 1);
    else if (scr->cur_row < scr->rows - 1)
        scr->cur_row++;
}

static void reverse_index(PtyScreen *scr) {
    scr->pending_wrap = 0;
    if (scr->cur_row == scr->top)
        scroll_down(scr, scr->top, scr->bottom, 1);
    else if (scr->cur_row > 0)
        scr->cur_row--;
}

/* ── Cursor ──────────────────────────────────────────────────────────── */

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void move_to(PtyScreen *scr, int row, int col) {
    scr->pending_wrap = 0;
    scr->cur_row = clampi(row, 0, scr->rows - 1);
    scr->cur_col = clampi(col, 0, scr->cols - 1);
}

/* CUP / VPA address rows relative to the scrolling region in origin mode. */
static void move_to_origin(PtyScreen *scr, int row, int col) {
    if (scr->origin_mode)
        move_to(scr, clampi(scr->top + row, scr->top, scr->bottom), col);
    else
        move_to(scr, row, col);
}

static void save_cursor(PtyScreen *scr) {
    scr->sv_row = scr->cur_row;
    scr->sv_col = scr->cur_col;
    scr->sv_attr = scr->cur_attr;
    scr->sv_fg = scr->cur_fg;
    scr->sv_bg = scr->cur_bg;
    scr->sv_wrap = scr->pending_wrap;
    scr->sv_g0 = scr->g0_graphics;
    scr->sv_origin = scr->origin_mode;
}

static void restore_cursor(PtyScreen *scr) {
    scr->cur_row = clampi(scr->sv_row, 0, scr->rows - 1);
    scr->cur_col = clampi(scr->sv_col, 0, scr->cols - 1);
    scr->cur_attr = scr->sv_attr;
    scr->cur_fg = scr->sv_fg;
    scr->cur_bg = scr->sv_bg;
    scr->pending_wrap = scr->sv_wrap;
    scr->g0_graphics = scr->sv_g0;
    scr->origin_mode = scr->sv_origin;
}

static void tab_forward(PtyScreen *scr, int n) {
    scr->pending_wrap = 0;
    while (n-- > 0) {
        int c = scr->cur_col + 1;
        while (c < scr->cols - 1 && !scr->tabs[c]) c++;
        scr->cur_col = c < scr->cols ? c : scr->cols - 1;
    }
}

static void tab_backward(PtyScreen *scr, int n) {
    scr->pending_wrap = 0;
    while (n-- > 0) {
        int c = scr->cur_col - 1;
        while (c > 0 && !scr->tabs[c]) c--;
        scr->cur_col = c > 0 ? c : 0;
    }
}

static void reset_tabs(PtyScreen *scr) {
    for (int c = 0; c < scr->cols; c++) scr->tabs[c] = (c % 8 == 0 && c > 0);
}

/* ── Responses ───────────────────────────────────────────────────────── */

static void respond(PtyScreen *scr, const char *text) {
    size_t len = strlen(text);
    if (scr->resp_len + len > sizeof(scr->resp)) return; /* reader is not reading */
    memcpy(scr->resp + scr->resp_len, text, len);
    scr->resp_len += len;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void soft_reset(PtyScreen *scr) {
    scr->cur_attr = PTY_ATTR_NONE;
    scr->cur_fg = PTY_COLOR_DEFAULT;
    scr->cur_bg = PTY_COLOR_DEFAULT;
    scr->pending_wrap = 0;
    scr->top = 0;
    scr->bottom = scr->rows - 1;
    scr->cursor_visible = 1;
    scr->autowrap = 1;
    scr->origin_mode = 0;
    scr->insert_mode = 0;
    scr->app_cursor = 0;
    scr->g0_graphics = 0;
    scr->g1_graphics = 0;
    scr->shift_out = 0;
    save_cursor(scr);
    scr->sv_row = 0;
    scr->sv_col = 0;
}

static void full_reset(PtyScreen *scr) {
    soft_reset(scr);
    scr->cells = scr->main_cells;
    scr->alt_active = 0;
    for (int i = 0; i < scr->cols * scr->rows; i++) {
        blank_plain(&scr->main_cells[i]);
        blank_plain(&scr->alt_cells[i]);
    }
    reset_tabs(scr);
    scr->cur_row = 0;
    scr->cur_col = 0;
    scr->last_cp = ' ';
}

PtyScreen *pty_screen_new(int cols, int rows) {
    if (cols <= 0 || rows <= 0 || cols > 4096 || rows > 4096) return NULL;
    PtyScreen *scr = calloc(1, sizeof(*scr));
    if (!scr) return NULL;
    scr->cols = cols;
    scr->rows = rows;
    scr->main_cells = calloc((size_t)cols * (size_t)rows, sizeof(PtyCell));
    scr->alt_cells  = calloc((size_t)cols * (size_t)rows, sizeof(PtyCell));
    scr->tabs       = calloc((size_t)cols, 1);
    if (!scr->main_cells || !scr->alt_cells || !scr->tabs) {
        pty_screen_free(scr);
        return NULL;
    }
    full_reset(scr);
    return scr;
}

void pty_screen_free(PtyScreen *scr) {
    if (!scr) return;
    free(scr->main_cells);
    free(scr->alt_cells);
    free(scr->tabs);
    free(scr);
}

int pty_screen_resize(PtyScreen *scr, int cols, int rows) {
    if (!scr || cols <= 0 || rows <= 0 || cols > 4096 || rows > 4096) return -1;
    PtyCell *nm = calloc((size_t)cols * (size_t)rows, sizeof(PtyCell));
    PtyCell *na = calloc((size_t)cols * (size_t)rows, sizeof(PtyCell));
    uint8_t *nt = calloc((size_t)cols, 1);
    if (!nm || !na || !nt) { free(nm); free(na); free(nt); return -1; }

    /* No reflow: content is kept at its top-left position and clipped, which
     * is what xterm does with the alternate screen a TUI draws on. */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            PtyCell *dm = &nm[r * cols + c], *da = &na[r * cols + c];
            if (r < scr->rows && c < scr->cols) {
                *dm = scr->main_cells[r * scr->cols + c];
                *da = scr->alt_cells[r * scr->cols + c];
                /* A wide head whose tail was clipped off becomes blank. */
                if (c == cols - 1 && dm->width == 2) blank_plain(dm);
                if (c == cols - 1 && da->width == 2) blank_plain(da);
            } else {
                blank_plain(dm);
                blank_plain(da);
            }
        }
    }
    int alt = scr->alt_active;
    free(scr->main_cells);
    free(scr->alt_cells);
    free(scr->tabs);
    scr->main_cells = nm;
    scr->alt_cells = na;
    scr->tabs = nt;
    scr->cells = alt ? na : nm;
    scr->cols = cols;
    scr->rows = rows;
    scr->top = 0;
    scr->bottom = rows - 1;
    reset_tabs(scr);
    move_to(scr, scr->cur_row, scr->cur_col);
    scr->sv_row = clampi(scr->sv_row, 0, rows - 1);
    scr->sv_col = clampi(scr->sv_col, 0, cols - 1);
    return 0;
}

/* ── Printing ────────────────────────────────────────────────────────── */

static void put_char(PtyScreen *scr, uint32_t cp) {
    int graphics = scr->shift_out ? scr->g1_graphics : scr->g0_graphics;
    if (graphics && cp >= 0x5F && cp <= 0x7E) cp = dec_graphics[cp - 0x5F];

    int w = pty_char_width(cp);

    if (w == 0) {
        /* Combining mark: belongs to the character before the cursor. */
        int col = scr->pending_wrap ? scr->cur_col : scr->cur_col - 1;
        PtyCell *c = pty_screen_cell(scr, scr->cur_row, col);
        if (c && c->width == 0) c = pty_screen_cell(scr, scr->cur_row, col - 1);
        if (!c) return;
        char enc[4];
        size_t n = utf8_encode(cp, enc), len = strlen(c->ch);
        if (len + n < sizeof(c->ch)) {
            memcpy(c->ch + len, enc, n);
            c->ch[len + n] = '\0';
        }
        return;
    }
    if (w == 2 && scr->cols < 2) w = 1;

    if (scr->pending_wrap && scr->autowrap) {
        scr->cur_col = 0;
        linefeed(scr);
    }
    scr->pending_wrap = 0;

    if (w == 2 && scr->cur_col == scr->cols - 1) {
        if (scr->autowrap) {
            erase_range(scr, scr->cur_row, scr->cur_col, scr->cols);
            scr->cur_col = 0;
            linefeed(scr);
        } else {
            scr->cur_col = scr->cols - 2;
        }
    }

    int row = scr->cur_row, col = scr->cur_col;
    PtyCell *line = &scr->cells[row * scr->cols];

    if (scr->insert_mode) {
        break_wide(scr, row, col);
        int keep = scr->cols - col - w;
        if (keep > 0) memmove(&line[col + w], &line[col], (size_t)keep * sizeof(PtyCell));
        /* A wide head pushed into the last column has lost its tail. */
        if (line[scr->cols - 1].width == 2) blank(scr, &line[scr->cols - 1]);
    }

    break_wide(scr, row, col);
    if (w == 2) break_wide(scr, row, col + 1);

    PtyCell *c = &line[col];
    size_t n = utf8_encode(cp, c->ch);
    c->ch[n] = '\0';
    c->attr = scr->cur_attr;
    c->fg = scr->cur_fg;
    c->bg = scr->cur_bg;
    c->width = (uint8_t)w;
    if (w == 2) {
        PtyCell *t = &line[col + 1];
        t->ch[0] = '\0';
        t->attr = scr->cur_attr;
        t->fg = scr->cur_fg;
        t->bg = scr->cur_bg;
        t->width = 0;
    }
    scr->last_cp = cp;

    scr->cur_col += w;
    if (scr->cur_col >= scr->cols) {
        /* Deferred wrap: stay in the last column until the next character,
         * as a real terminal does. */
        scr->cur_col = scr->cols - 1;
        if (scr->autowrap) scr->pending_wrap = 1;
    }
}

/* ── Editing ─────────────────────────────────────────────────────────── */

static void insert_chars(PtyScreen *scr, int n) {
    int row = scr->cur_row, col = scr->cur_col;
    PtyCell *line = &scr->cells[row * scr->cols];
    scr->pending_wrap = 0;
    if (n > scr->cols - col) n = scr->cols - col;
    break_wide(scr, row, col);
    memmove(&line[col + n], &line[col], (size_t)(scr->cols - col - n) * sizeof(PtyCell));
    for (int c = col; c < col + n; c++) blank(scr, &line[c]);
    if (line[scr->cols - 1].width == 2) blank(scr, &line[scr->cols - 1]);
}

static void delete_chars(PtyScreen *scr, int n) {
    int row = scr->cur_row, col = scr->cur_col;
    PtyCell *line = &scr->cells[row * scr->cols];
    scr->pending_wrap = 0;
    if (n > scr->cols - col) n = scr->cols - col;
    break_wide(scr, row, col);
    break_wide(scr, row, col + n - 1);
    memmove(&line[col], &line[col + n], (size_t)(scr->cols - col - n) * sizeof(PtyCell));
    for (int c = scr->cols - n; c < scr->cols; c++) blank(scr, &line[c]);
}

static void erase_display(PtyScreen *scr, int mode) {
    scr->pending_wrap = 0;
    switch (mode) {
    case 0:
        erase_range(scr, scr->cur_row, scr->cur_col, scr->cols);
        erase_rows(scr, scr->cur_row + 1, scr->rows);
        break;
    case 1:
        erase_rows(scr, 0, scr->cur_row);
        erase_range(scr, scr->cur_row, 0, scr->cur_col + 1);
        break;
    case 2:
    case 3:   /* 3 also clears scrollback, which this emulator does not keep */
        erase_rows(scr, 0, scr->rows);
        break;
    default:
        break;
    }
}

static void erase_line(PtyScreen *scr, int mode) {
    scr->pending_wrap = 0;
    switch (mode) {
    case 0: erase_range(scr, scr->cur_row, scr->cur_col, scr->cols); break;
    case 1: erase_range(scr, scr->cur_row, 0, scr->cur_col + 1); break;
    case 2: erase_range(scr, scr->cur_row, 0, scr->cols); break;
    default: break;
    }
}

static void switch_screen(PtyScreen *scr, int alt, int clear_alt) {
    if (alt == scr->alt_active) return;
    if (!alt && clear_alt) {
        PtyCell *keep = scr->cells;
        for (int i = 0; i < scr->cols * scr->rows; i++) blank(scr, &keep[i]);
    }
    scr->alt_active = alt;
    scr->cells = alt ? scr->alt_cells : scr->main_cells;
    if (alt && clear_alt)
        for (int i = 0; i < scr->cols * scr->rows; i++) blank(scr, &scr->cells[i]);
}

/* ── CSI dispatch ────────────────────────────────────────────────────── */

/* Parameter i, or @p def when it is missing or zero. */
static int param(const PtyScreen *scr, int i, int def) {
    return (i < scr->nparams && scr->params[i] > 0) ? scr->params[i] : def;
}

/* Parses an extended colour (38 / 48 / 58) starting at index i, in either
 * the ';' form (38;5;n, 38;2;r;g;b) or the ':' form (38:5:n, 38:2::r:g:b).
 * Stores the colour in *out (if not NULL) and returns the last index used. */
static int parse_ext_color(const PtyScreen *scr, int i, int32_t *out) {
    const int *p = scr->params;
    int n = scr->nparams;
    if (i + 1 < n && scr->param_sub[i + 1]) {
        int j = i + 1;
        while (j < n && scr->param_sub[j]) j++;
        int subs = j - (i + 1);
        int mode = p[i + 1];
        if (mode == 5 && subs >= 2) {
            if (out) *out = PTY_COLOR_IDX(p[i + 2]);
        } else if (mode == 2 && subs >= 5) {      /* 2:colourspace:r:g:b */
            if (out) *out = PTY_COLOR_RGB(p[i + 3], p[i + 4], p[i + 5]);
        } else if (mode == 2 && subs == 4) {      /* 2:r:g:b */
            if (out) *out = PTY_COLOR_RGB(p[i + 2], p[i + 3], p[i + 4]);
        }
        return j - 1;
    }
    if (i + 1 >= n) return i;
    if (p[i + 1] == 5) {
        if (i + 2 < n && out) *out = PTY_COLOR_IDX(p[i + 2]);
        return i + 2 < n ? i + 2 : n - 1;
    }
    if (p[i + 1] == 2) {
        if (i + 4 < n && out) *out = PTY_COLOR_RGB(p[i + 2], p[i + 3], p[i + 4]);
        return i + 4 < n ? i + 4 : n - 1;
    }
    return i + 1;
}

static void apply_sgr(PtyScreen *scr) {
    if (scr->nparams == 0) {
        scr->cur_attr = PTY_ATTR_NONE;
        scr->cur_fg = PTY_COLOR_DEFAULT;
        scr->cur_bg = PTY_COLOR_DEFAULT;
        return;
    }
    for (int i = 0; i < scr->nparams; i++) {
        int v = scr->params[i];
        switch (v) {
        case 0:
            scr->cur_attr = PTY_ATTR_NONE;
            scr->cur_fg = PTY_COLOR_DEFAULT;
            scr->cur_bg = PTY_COLOR_DEFAULT;
            break;
        case 1: scr->cur_attr |= PTY_ATTR_BOLD; break;
        case 2: scr->cur_attr |= PTY_ATTR_DIM; break;
        case 3: scr->cur_attr |= PTY_ATTR_ITALIC; break;
        case 4:
            /* 4:0 turns underline off; 4:1..4:5 are underline styles. */
            if (i + 1 < scr->nparams && scr->param_sub[i + 1]) {
                if (scr->params[i + 1] == 0) scr->cur_attr &= ~PTY_ATTR_UNDERLINE;
                else scr->cur_attr |= PTY_ATTR_UNDERLINE;
                while (i + 1 < scr->nparams && scr->param_sub[i + 1]) i++;
            } else {
                scr->cur_attr |= PTY_ATTR_UNDERLINE;
            }
            break;
        case 5: case 6: scr->cur_attr |= PTY_ATTR_BLINK; break;
        case 7: scr->cur_attr |= PTY_ATTR_REVERSE; break;
        case 8: scr->cur_attr |= PTY_ATTR_INVISIBLE; break;
        case 9: scr->cur_attr |= PTY_ATTR_STRIKE; break;
        case 21: scr->cur_attr |= PTY_ATTR_UNDERLINE; break;
        case 22: scr->cur_attr &= ~(PTY_ATTR_BOLD | PTY_ATTR_DIM); break;
        case 23: scr->cur_attr &= ~PTY_ATTR_ITALIC; break;
        case 24: scr->cur_attr &= ~PTY_ATTR_UNDERLINE; break;
        case 25: scr->cur_attr &= ~PTY_ATTR_BLINK; break;
        case 27: scr->cur_attr &= ~PTY_ATTR_REVERSE; break;
        case 28: scr->cur_attr &= ~PTY_ATTR_INVISIBLE; break;
        case 29: scr->cur_attr &= ~PTY_ATTR_STRIKE; break;
        case 38: i = parse_ext_color(scr, i, &scr->cur_fg); break;
        case 39: scr->cur_fg = PTY_COLOR_DEFAULT; break;
        case 48: i = parse_ext_color(scr, i, &scr->cur_bg); break;
        case 49: scr->cur_bg = PTY_COLOR_DEFAULT; break;
        case 58: i = parse_ext_color(scr, i, NULL); break; /* underline colour */
        default:
            if ((v >= 30 && v <= 37) || (v >= 90 && v <= 97)) scr->cur_fg = v;
            else if ((v >= 40 && v <= 47) || (v >= 100 && v <= 107)) scr->cur_bg = v;
            /* anything else (overline, fonts, ...) is not represented */
            break;
        }
    }
}

static void set_private_mode(PtyScreen *scr, int mode, int on) {
    switch (mode) {
    case 1: scr->app_cursor = on; break;
    case 6:
        scr->origin_mode = on;
        move_to_origin(scr, 0, 0);
        break;
    case 7: scr->autowrap = on; if (!on) scr->pending_wrap = 0; break;
    case 25: scr->cursor_visible = on; break;
    case 47: switch_screen(scr, on, 0); break;
    case 1047: switch_screen(scr, on, !on); break;
    case 1048: if (on) save_cursor(scr); else restore_cursor(scr); break;
    case 1049:
        if (on) {
            save_cursor(scr);
            switch_screen(scr, 1, 1);
        } else {
            switch_screen(scr, 0, 0);
            restore_cursor(scr);
        }
        break;
    default: break;   /* mouse, focus, bracketed paste, ...: not represented */
    }
}

static void dispatch_csi(PtyScreen *scr, char final) {
    char buf[64];

    if (scr->csi_private == '?') {
        if (scr->intermediate) return;
        if (final == 'h' || final == 'l')
            for (int i = 0; i < scr->nparams; i++)
                set_private_mode(scr, scr->params[i], final == 'h');
        return;
    }
    if (scr->csi_private == '>') {
        if (final == 'c' && !scr->intermediate && param(scr, 0, 0) == 0)
            respond(scr, "\033[>1;10;0c");   /* DA2: a VT220 */
        return;   /* modifyOtherKeys (CSI > ... m) and the rest: ignored */
    }
    if (scr->csi_private) return;

    if (scr->intermediate) {
        if (scr->intermediate == '!' && final == 'p') soft_reset(scr); /* DECSTR */
        return;   /* DECSCUSR (CSI n SP q) and the rest: ignored */
    }

    switch (final) {
    case '@': insert_chars(scr, param(scr, 0, 1)); break;
    case 'A': { /* CUU: stops at the top margin when inside the region */
        int lim = scr->cur_row >= scr->top ? scr->top : 0;
        move_to(scr, clampi(scr->cur_row - param(scr, 0, 1), lim, scr->rows - 1), scr->cur_col);
        break;
    }
    case 'B': case 'e': { /* CUD, VPR */
        int lim = scr->cur_row <= scr->bottom ? scr->bottom : scr->rows - 1;
        move_to(scr, clampi(scr->cur_row + param(scr, 0, 1), 0, lim), scr->cur_col);
        break;
    }
    case 'C': case 'a': move_to(scr, scr->cur_row, scr->cur_col + param(scr, 0, 1)); break;
    case 'D': move_to(scr, scr->cur_row, scr->cur_col - param(scr, 0, 1)); break;
    case 'E': move_to(scr, scr->cur_row + param(scr, 0, 1), 0); break;
    case 'F': move_to(scr, scr->cur_row - param(scr, 0, 1), 0); break;
    case 'G': case '`': move_to(scr, scr->cur_row, param(scr, 0, 1) - 1); break;
    case 'H': case 'f':
        move_to_origin(scr, param(scr, 0, 1) - 1, param(scr, 1, 1) - 1);
        break;
    case 'I': tab_forward(scr, param(scr, 0, 1)); break;
    case 'J': erase_display(scr, scr->nparams ? scr->params[0] : 0); break;
    case 'K': erase_line(scr, scr->nparams ? scr->params[0] : 0); break;
    case 'L': /* IL */
        if (scr->cur_row >= scr->top && scr->cur_row <= scr->bottom) {
            scroll_down(scr, scr->cur_row, scr->bottom, param(scr, 0, 1));
            move_to(scr, scr->cur_row, 0);
        }
        break;
    case 'M': /* DL */
        if (scr->cur_row >= scr->top && scr->cur_row <= scr->bottom) {
            scroll_up(scr, scr->cur_row, scr->bottom, param(scr, 0, 1));
            move_to(scr, scr->cur_row, 0);
        }
        break;
    case 'P': delete_chars(scr, param(scr, 0, 1)); break;
    case 'S': scroll_up(scr, scr->top, scr->bottom, param(scr, 0, 1)); break;
    case 'T':
        if (scr->nparams <= 1)   /* with 5 params it is mouse highlight tracking */
            scroll_down(scr, scr->top, scr->bottom, param(scr, 0, 1));
        break;
    case 'X': { /* ECH */
        scr->pending_wrap = 0;
        erase_range(scr, scr->cur_row, scr->cur_col, scr->cur_col + param(scr, 0, 1));
        break;
    }
    case 'Z': tab_backward(scr, param(scr, 0, 1)); break;
    case 'b': { /* REP */
        int n = param(scr, 0, 1);
        if (n > scr->cols * scr->rows) n = scr->cols * scr->rows;
        uint32_t cp = scr->last_cp;
        for (int i = 0; i < n; i++) put_char(scr, cp);
        break;
    }
    case 'c':
        if (param(scr, 0, 0) == 0) respond(scr, "\033[?62;22c"); /* DA1 */
        break;
    case 'd': move_to_origin(scr, param(scr, 0, 1) - 1, scr->cur_col); break;
    case 'g':
        if (param(scr, 0, 0) == 0) scr->tabs[scr->cur_col] = 0;
        else if (scr->params[0] == 3) memset(scr->tabs, 0, (size_t)scr->cols);
        break;
    case 'h': case 'l':
        for (int i = 0; i < scr->nparams; i++)
            if (scr->params[i] == 4) scr->insert_mode = (final == 'h');
        break;
    case 'm': apply_sgr(scr); break;
    case 'n':
        if (param(scr, 0, 0) == 5) {
            respond(scr, "\033[0n");
        } else if (param(scr, 0, 0) == 6) {
            int row = scr->cur_row - (scr->origin_mode ? scr->top : 0);
            snprintf(buf, sizeof(buf), "\033[%d;%dR", row + 1, scr->cur_col + 1);
            respond(scr, buf);
        }
        break;
    case 'r': { /* DECSTBM */
        int top = param(scr, 0, 1) - 1;
        int bottom = param(scr, 1, scr->rows) - 1;
        if (bottom >= scr->rows) bottom = scr->rows - 1;
        if (top < bottom) {
            scr->top = top;
            scr->bottom = bottom;
            move_to_origin(scr, 0, 0);
        }
        break;
    }
    case 's': save_cursor(scr); break;
    case 'u': restore_cursor(scr); break;
    default: break;   /* window ops (t) and the rest: ignored */
    }
}

/* ── ESC dispatch ────────────────────────────────────────────────────── */

static void dispatch_esc(PtyScreen *scr, unsigned char final) {
    if (scr->intermediate) {
        switch (scr->intermediate) {
        case '(': scr->g0_graphics = (final == '0'); break;
        case ')': scr->g1_graphics = (final == '0'); break;
        case '#':
            if (final == '8') { /* DECALN: fill the screen with 'E' */
                scr->top = 0;
                scr->bottom = scr->rows - 1;
                for (int i = 0; i < scr->cols * scr->rows; i++) {
                    blank_plain(&scr->cells[i]);
                    scr->cells[i].ch[0] = 'E';
                }
                move_to(scr, 0, 0);
            }
            break;
        default: break;   /* ESC %, ESC SP, other charsets: ignored */
        }
        return;
    }
    switch (final) {
    case '7': save_cursor(scr); break;
    case '8': restore_cursor(scr); break;
    case 'D': linefeed(scr); break;
    case 'E': scr->cur_col = 0; linefeed(scr); break;
    case 'M': reverse_index(scr); break;
    case 'H': scr->tabs[scr->cur_col] = 1; break;
    case 'c': full_reset(scr); break;
    default: break;   /* keypad modes (= >), ST, ...: ignored */
    }
}

/* ── Byte feed ───────────────────────────────────────────────────────── */

static void execute_c0(PtyScreen *scr, unsigned char ch) {
    switch (ch) {
    case '\n': case '\v': case '\f': linefeed(scr); break;
    case '\r': scr->pending_wrap = 0; scr->cur_col = 0; break;
    case '\b':
        scr->pending_wrap = 0;
        if (scr->cur_col > 0) scr->cur_col--;
        break;
    case '\t': tab_forward(scr, 1); break;
    case 0x0E: scr->shift_out = 1; break;
    case 0x0F: scr->shift_out = 0; break;
    default: break;   /* BEL, NUL, ...: no visible effect */
    }
}

static void begin_csi(PtyScreen *scr) {
    scr->state = PS_CSI;
    scr->nparams = 0;
    scr->param_started = 0;
    scr->params[0] = 0;
    scr->param_sub[0] = 0;
    scr->csi_private = 0;
    scr->intermediate = 0;
}

static void ground_byte(PtyScreen *scr, unsigned char ch) {
    if (scr->utf8_need > 0) {
        if ((ch & 0xC0) == 0x80) {
            scr->utf8_cp = (scr->utf8_cp << 6) | (ch & 0x3F);
            if (--scr->utf8_need == 0) {
                uint32_t cp = scr->utf8_cp;
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                if (cp >= 0x80 && cp < 0xA0) return;   /* C1 controls: not printed */
                put_char(scr, cp);
            }
            return;
        }
        /* Truncated sequence: show it as such, then handle this byte. */
        scr->utf8_need = 0;
        put_char(scr, 0xFFFD);
    }
    if (ch < 0x20) { execute_c0(scr, ch); return; }
    if (ch < 0x7F) { put_char(scr, ch); return; }
    if (ch == 0x7F) return;
    if (ch >= 0xC2 && ch <= 0xDF)      { scr->utf8_cp = ch & 0x1F; scr->utf8_need = 1; }
    else if (ch >= 0xE0 && ch <= 0xEF) { scr->utf8_cp = ch & 0x0F; scr->utf8_need = 2; }
    else if (ch >= 0xF0 && ch <= 0xF4) { scr->utf8_cp = ch & 0x07; scr->utf8_need = 3; }
    else put_char(scr, 0xFFFD);
}

void pty_screen_feed(PtyScreen *scr, const char *data, size_t len) {
    if (!scr || !data) return;
    for (size_t k = 0; k < len; k++) {
        unsigned char ch = (unsigned char)data[k];

        /* ESC, CAN and SUB interrupt any sequence, as on a real terminal. */
        if (scr->state != PS_OSC && scr->state != PS_STR) {
            if (ch == 0x1B) {
                if (scr->utf8_need) { scr->utf8_need = 0; put_char(scr, 0xFFFD); }
                scr->state = PS_ESC;
                scr->intermediate = 0;
                continue;
            }
            if (ch == 0x18 || ch == 0x1A) { scr->state = PS_GROUND; continue; }
        }

        switch (scr->state) {
        case PS_GROUND:
            ground_byte(scr, ch);
            break;

        case PS_ESC:
            if (ch < 0x20) { execute_c0(scr, ch); break; }
            if (ch <= 0x2F) { scr->intermediate = (char)ch; scr->state = PS_ESC_INT; break; }
            switch (ch) {
            case '[': begin_csi(scr); break;
            case ']': scr->state = PS_OSC; scr->str_esc = 0; break;
            case 'P': case 'X': case '^': case '_':
                scr->state = PS_STR; scr->str_esc = 0; break;
            default:
                scr->state = PS_GROUND;
                dispatch_esc(scr, ch);
                break;
            }
            break;

        case PS_ESC_INT:
            if (ch < 0x20) { execute_c0(scr, ch); break; }
            if (ch <= 0x2F) { scr->intermediate = (char)ch; break; }
            scr->state = PS_GROUND;
            dispatch_esc(scr, ch);
            break;

        case PS_CSI:
            if (ch < 0x20) { execute_c0(scr, ch); break; }
            if (ch >= '0' && ch <= '9') {
                if (scr->intermediate) { scr->state = PS_CSI_IGNORE; break; }
                int *p = &scr->params[scr->nparams];
                if (*p < 100000) *p = *p * 10 + (ch - '0');
                scr->param_started = 1;
            } else if (ch == ';' || ch == ':') {
                if (scr->intermediate) { scr->state = PS_CSI_IGNORE; break; }
                if (scr->nparams < PTY_CSI_MAX_PARAMS - 1) {
                    scr->nparams++;
                    scr->params[scr->nparams] = 0;
                    scr->param_sub[scr->nparams] = (ch == ':');
                }
                scr->param_started = 1;
            } else if (ch >= 0x3C && ch <= 0x3F) {
                if (scr->param_started || scr->csi_private || scr->intermediate)
                    scr->state = PS_CSI_IGNORE;
                else
                    scr->csi_private = (char)ch;
            } else if (ch >= 0x20 && ch <= 0x2F) {
                scr->intermediate = (char)ch;
            } else if (ch >= 0x40 && ch <= 0x7E) {
                /* nparams counts parameters; "CSI m" has none. */
                if (scr->param_started) scr->nparams++;
                scr->state = PS_GROUND;
                dispatch_csi(scr, (char)ch);
            }
            break;

        case PS_CSI_IGNORE:
            if (ch < 0x20) execute_c0(scr, ch);
            else if (ch >= 0x40 && ch <= 0x7E) scr->state = PS_GROUND;
            break;

        case PS_OSC:
        case PS_STR:
            /* Contents are not interpreted (titles, colour queries, DCS). */
            if (scr->str_esc) {
                scr->str_esc = 0;
                if (ch == '\\') { scr->state = PS_GROUND; break; }
                if (ch == 0x1B) { scr->str_esc = 1; break; }
                /* ESC followed by something else: the string ended there. */
                scr->state = PS_ESC;
                scr->intermediate = 0;
                k--;   /* reprocess this byte in the ESC state */
                break;
            }
            if (ch == 0x1B) scr->str_esc = 1;
            else if (ch == 0x07 && scr->state == PS_OSC) scr->state = PS_GROUND;
            else if (ch == 0x18 || ch == 0x1A) scr->state = PS_GROUND;
            break;
        }
    }
}
