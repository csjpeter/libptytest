/**
 * @file pty_compare.c
 * @brief Region comparison of text and style, and reports that explain a
 *        difference rather than only locating it.
 *
 * A failed screen assertion is read by someone who did not write the test,
 * often from a CI log.  The report therefore answers three questions in
 * order: where (a diff-style view with a column ruler, context rows, and a
 * caret under every differing column), what kind (notes: shifted, moved to
 * another row, truncated, extra text, blank, letter case), and what exactly
 * (the code points and style of the first differing cell, which is how a
 * no-break space or a decomposed accent becomes visible).
 */

#define _XOPEN_SOURCE 700

#include "pty_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── A growing string buffer ─────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len, cap;
    int    oom;
} Sb;

static void sb_grow(Sb *sb, size_t need) {
    if (sb->oom || sb->len + need + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 1024;
    while (cap < sb->len + need + 1) cap *= 2;
    char *n = realloc(sb->buf, cap);
    if (!n) { sb->oom = 1; return; }
    sb->buf = n;
    sb->cap = cap;
}

static void sb_puts(Sb *sb, const char *s) {
    size_t n = strlen(s);
    sb_grow(sb, n);
    if (sb->oom) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(Sb *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(Sb *sb, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_puts(sb, tmp);
}

/* Copies the buffer into the caller's, cut at a character boundary. */
static void sb_export(Sb *sb, char *out, size_t size) {
    if (!out || size == 0) return;
    size_t n = sb->buf ? sb->len : 0;
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)sb->buf[n] & 0xC0) == 0x80) n--;
    }
    if (n) memcpy(out, sb->buf, n);
    out[n] = '\0';
}

static void sb_free(Sb *sb) { free(sb->buf); }

/* ── UTF-8 ───────────────────────────────────────────────────────────── */

/* Decodes one character; invalid input yields U+FFFD for one byte. */
static size_t utf8_next(const char *p, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)p;
    int need;
    uint32_t c;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if (u[0] >= 0xC2 && u[0] <= 0xDF) { c = u[0] & 0x1F; need = 1; }
    else if (u[0] >= 0xE0 && u[0] <= 0xEF) { c = u[0] & 0x0F; need = 2; }
    else if (u[0] >= 0xF0 && u[0] <= 0xF4) { c = u[0] & 0x07; need = 3; }
    else { *cp = 0xFFFD; return 1; }
    for (int i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = (c << 6) | (u[i] & 0x3F);
    }
    *cp = c;
    return (size_t)need + 1;
}

/* Appends "'x' U+0078" (all code points of a cell) to sb. */
static void describe_text(Sb *sb, const char *text) {
    if (text[0] == '\0') { sb_puts(sb, "(nothing)"); return; }
    sb_puts(sb, "'");
    sb_puts(sb, text);
    sb_puts(sb, "'");
    for (const char *p = text; *p; ) {
        uint32_t cp;
        p += utf8_next(p, &cp);
        sb_printf(sb, " U+%04X", (unsigned)cp);
    }
}

/* ── Styles in words ─────────────────────────────────────────────────── */

static const char *const colour_names[8] = {
    "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
};

static void describe_colour(Sb *sb, const char *which, int c) {
    if (c == PTY_COLOR_DEFAULT) return;
    if (PTY_COLOR_IS_RGB(c))
        sb_printf(sb, ", %s #%06x", which, (unsigned)(c & 0xFFFFFF));
    else if (PTY_COLOR_IS_IDX(c))
        sb_printf(sb, ", %s palette %d", which, c & 0xFF);
    else if ((c >= 30 && c <= 37) || (c >= 40 && c <= 47))
        sb_printf(sb, ", %s %s (%d)", which, colour_names[c % 10], c);
    else if ((c >= 90 && c <= 97) || (c >= 100 && c <= 107))
        sb_printf(sb, ", %s bright %s (%d)", which, colour_names[c % 10], c);
    else
        sb_printf(sb, ", %s %d", which, c);
}

/* "bold, reverse, fg red (31)", or "plain". */
static void describe_style(Sb *sb, int attr, int fg, int bg) {
    static const struct { int bit; const char *name; } names[] = {
        { PTY_ATTR_BOLD, "bold" }, { PTY_ATTR_DIM, "dim" },
        { PTY_ATTR_ITALIC, "italic" }, { PTY_ATTR_UNDERLINE, "underline" },
        { PTY_ATTR_BLINK, "blink" }, { PTY_ATTR_REVERSE, "reverse" },
        { PTY_ATTR_INVISIBLE, "invisible" }, { PTY_ATTR_STRIKE, "strike" },
    };
    size_t start = sb->len;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (attr & names[i].bit) sb_printf(sb, ", %s", names[i].name);
    describe_colour(sb, "fg", fg);
    describe_colour(sb, "bg", bg);
    if (sb->oom) return;
    if (sb->len == start) {
        sb_puts(sb, "plain");
    } else {
        /* drop the leading ", " */
        memmove(sb->buf + start, sb->buf + start + 2, sb->len - start - 1);
        sb->len -= 2;
    }
}

char *pty_cell_describe(PtySession *s, int row, int col, char *buf, size_t size) {
    if (!buf || size == 0) return buf;
    buf[0] = '\0';
    PtyCell *c = s ? pty_screen_cell(s->screen, row, col) : NULL;
    if (!c) return buf;
    Sb sb = {0};
    if (c->width == 0) {
        PtyCell *head = pty_screen_cell(s->screen, row, col - 1);
        sb_puts(&sb, "(right half of ");
        describe_text(&sb, head ? head->ch : "");
        sb_puts(&sb, ")");
    } else {
        describe_text(&sb, c->ch);
    }
    sb_puts(&sb, ", ");
    describe_style(&sb, c->attr, (int)c->fg, (int)c->bg);
    sb_export(&sb, buf, size);
    sb_free(&sb);
    return buf;
}

/* ── Regions ─────────────────────────────────────────────────────────── */

typedef struct {
    int row, col, rows, cols, context;
    const char *label, *wildcard;
} Area;

static int resolve(PtySession *s, const PtyRegion *in, Area *a, Sb *sb) {
    PtyRegion r = in ? *in : PTY_REGION_FULL;
    a->row = r.row;
    a->col = r.col;
    a->rows = r.rows > 0 ? r.rows : s->rows - r.row;
    a->cols = r.cols > 0 ? r.cols : s->cols - r.col;
    a->label = r.label;
    a->wildcard = (r.wildcard && *r.wildcard) ? r.wildcard : NULL;
    a->context = r.context == 0 ? 2 : (r.context < 0 ? 0 : r.context);
    if (r.row < 0 || r.col < 0 || a->rows <= 0 || a->cols <= 0 ||
        a->row + a->rows > s->rows || a->col + a->cols > s->cols) {
        sb_printf(sb, "region rows %d-%d, cols %d-%d is outside the %dx%d screen\n",
                  r.row, r.row + a->rows - 1, r.col, r.col + a->cols - 1,
                  s->cols, s->rows);
        return -1;
    }
    return 0;
}

static int is_full(PtySession *s, const Area *a) {
    return a->row == 0 && a->col == 0 && a->rows == s->rows && a->cols == s->cols;
}

static void header(Sb *sb, PtySession *s, const Area *a, const char *what,
                   int ndiff, int nrows) {
    if (is_full(s, a) && !a->label)
        sb_printf(sb, "screen (%dx%d)", s->cols, s->rows);
    else
        sb_printf(sb, "region%s%s%s rows %d-%d, cols %d-%d",
                  a->label ? " \"" : "", a->label ? a->label : "", a->label ? "\"" : "",
                  a->row, a->row + a->rows - 1, a->col, a->col + a->cols - 1);
    sb_printf(sb, ": %s differs in %d of %d row%s\n", what, ndiff, nrows,
              nrows == 1 ? "" : "s");
}

/* Column ruler: absolute column numbers, a digit at every tenth. */
static void ruler(Sb *sb, const Area *a) {
    sb_puts(sb, "        ");
    for (int c = a->col; c < a->col + a->cols; c++) {
        char ch = c % 10 == 0 ? (char)('0' + (c / 10) % 10) : (c % 5 == 0 ? '+' : '-');
        char t[2] = { ch, '\0' };
        sb_puts(sb, t);
    }
    sb_puts(sb, "\n");
}

static void carets(Sb *sb, const unsigned char *diff, int n) {
    int last = n - 1;
    while (last >= 0 && !diff[last]) last--;
    sb_puts(sb, "        ");
    for (int c = 0; c <= last; c++) sb_puts(sb, diff[c] ? "^" : " ");
    sb_puts(sb, "\n");
}

/* "col 4 differs" / "cols 4-7, 12 differ", in absolute columns. */
static void col_ranges(Sb *sb, const unsigned char *diff, int n, int base) {
    int first = 1, count = 0;
    for (int c = 0; c < n; c++) count += diff[c] != 0;
    sb_puts(sb, count == 1 ? "col " : "cols ");
    for (int c = 0; c < n; c++) {
        if (!diff[c]) continue;
        int e = c;
        while (e + 1 < n && diff[e + 1]) e++;
        sb_printf(sb, "%s%d", first ? "" : ", ", base + c);
        if (e > c) sb_printf(sb, "-%d", base + e);
        first = 0;
        c = e;
    }
    sb_puts(sb, count == 1 ? " differs" : " differ");
}

/* ── Text comparison ─────────────────────────────────────────────────── */

typedef char CellText[PTY_CELL_BYTES];

static const char *BLANK = " ";

/* Lays one expected row out into cells.  Returns its display width, which
 * may exceed @p cols (then the cells past the end are not stored). */
static int layout_row(const char *str, CellText *out, int cols) {
    int col = 0;
    for (int c = 0; c < cols; c++) strcpy(out[c], BLANK);
    /* Trailing blanks only restate the default and do not count as width. */
    const char *end = str + strlen(str);
    while (end > str && end[-1] == ' ') end--;
    for (const char *p = str; p < end; ) {
        uint32_t cp;
        size_t n = utf8_next(p, &cp);
        int w = cp < 0x20 ? 1 : pty_char_width(cp);
        if (w == 0) {
            int t = col - 1;
            if (t >= 0 && t < cols && out[t][0] == '\0') t--;
            if (t >= 0 && t < cols) {
                size_t len = strlen(out[t]);
                if (len + n < sizeof(out[t])) {
                    memcpy(out[t] + len, p, n);
                    out[t][len + n] = '\0';
                }
            }
        } else {
            if (col + w <= cols) {
                if (cp < 0x20) strcpy(out[col], "?");
                else { memcpy(out[col], p, n); out[col][n] = '\0'; }
                if (w == 2) out[col + 1][0] = '\0';
            }
            col += w;
        }
        p += n;
    }
    return col;
}

static int cell_match(const char *exp, const char *act, const char *wildcard) {
    if (wildcard && strcmp(exp, wildcard) == 0) return 1;
    return strcmp(exp, act) == 0;
}

static int cell_blank(const char *t) { return strcmp(t, BLANK) == 0; }

/* Actual text of the region's row r (0-based in the region), as pointers. */
static void actual_row(PtySession *s, const Area *a, int abs_row, const char **out) {
    for (int c = 0; c < a->cols; c++)
        out[c] = pty_screen_cell(s->screen, abs_row, a->col + c)->ch;
}

static void render_cells(Sb *sb, const char *const *cells, int n) {
    for (int c = 0; c < n; c++) {
        /* A tail at the left edge has no head to print it: keep columns. */
        if (cells[c][0] == '\0') { if (c == 0) sb_puts(sb, " "); continue; }
        sb_puts(sb, cells[c]);
    }
}

/* Is row e (expected) equal to row act, all cells? */
static int row_equal(const char *const *e, const char *const *act, int n, const char *wc) {
    for (int c = 0; c < n; c++) if (!cell_match(e[c], act[c], wc)) return 0;
    return 1;
}

static int row_blank(const char *const *cells, int n) {
    for (int c = 0; c < n; c++) if (!cell_blank(cells[c]) && cells[c][0]) return 0;
    return 1;
}

static int last_nonblank(const char *const *cells, int n) {
    for (int c = n - 1; c >= 0; c--) if (!cell_blank(cells[c]) && cells[c][0]) return c;
    return -1;
}

/* The shift d for which act[c] == e[c-d] for every column c in [from, to),
 * or 0.  Columns of e outside the row count as blanks. */
static int horizontal_shift(const char *const *e, const char *const *act, int n,
                            int from, int to, const char *wc) {
    for (int k = 1; k <= 4; k++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            int d = k * sign, ok = 1;
            for (int c = from; c < to && ok; c++) {
                int src = c - d;
                const char *want = (src >= 0 && src < n) ? e[src] : BLANK;
                if (want[0] == '\0') want = BLANK;   /* tails line up badly; be lenient */
                const char *got = act[c][0] ? act[c] : BLANK;
                ok = cell_match(want, got, wc);
            }
            if (ok) return d;
        }
    }
    return 0;
}

static int case_only(const char *const *e, const char *const *act, int n,
                     const char *wc) {
    int any = 0;
    for (int c = 0; c < n; c++) {
        if (cell_match(e[c], act[c], wc)) continue;
        if (strlen(e[c]) != 1 || strlen(act[c]) != 1) return 0;
        if (tolower((unsigned char)e[c][0]) != tolower((unsigned char)act[c][0])) return 0;
        any = 1;
    }
    return any;
}

/* A blank of some kind: what a reader cannot tell apart on the screen. */
static int space_like(const char *t) {
    uint32_t cp;
    if (t[0] == '\0' || t[utf8_next(t, &cp)] != '\0') return 0;
    return cp == 0x20 || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200B) ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

static int only_space_kinds(const char *const *e, const char *const *act, int n,
                            const unsigned char *diff) {
    int any = 0;
    for (int c = 0; c < n; c++) {
        if (!diff[c]) continue;
        if (!space_like(e[c]) || !space_like(act[c])) return 0;
        any = 1;
    }
    return any;
}

/* Notes on one differing row: what kind of difference it is. */
static void text_notes(Sb *sb, PtySession *s, const Area *a, int r,
                       const char *const *e, const char *const *act,
                       const unsigned char *diff, int *vshift) {
    int n = a->cols, abs_row = a->row + r;
    sb_printf(sb, "  row %d: ", abs_row);
    col_ranges(sb, diff, n, a->col);
    sb_puts(sb, "\n");

    *vshift = 0;
    int e_blank = row_blank(e, n), a_blank = row_blank(act, n);
    if (a_blank) {
        sb_puts(sb, "    nothing is shown on this row; the expected text is missing\n");
    } else if (e_blank) {
        sb_puts(sb, "    expected a blank row\n");
    }

    if (!e_blank) {
        /* Moved to another row (of the whole screen, same columns)? */
        const char **other = malloc((size_t)n * sizeof(*other));
        if (other) {
            for (int k = 1; k <= 3 && !*vshift; k++) {
                for (int sign = 1; sign >= -1; sign -= 2) {
                    int rr = abs_row + k * sign;
                    if (rr < 0 || rr >= s->rows) continue;
                    actual_row(s, a, rr, other);
                    if (row_equal(e, other, n, a->wildcard)) { *vshift = k * sign; break; }
                }
            }
            free(other);
        }
        if (*vshift)
            sb_printf(sb, "    the expected text is on row %d (%d row%s %s)%s\n",
                      abs_row + *vshift, abs(*vshift), abs(*vshift) == 1 ? "" : "s",
                      *vshift > 0 ? "lower" : "higher",
                      (abs_row + *vshift < a->row || abs_row + *vshift >= a->row + a->rows)
                          ? ", outside the region" : "");
    }

    if (!e_blank && !a_blank) {
        int first = 0, last = n - 1;
        while (first < n && !diff[first]) first++;
        while (last > first && !diff[last]) last--;
        int le = last_nonblank(e, n), la = last_nonblank(act, n);
        /* The whole row moved, or only the differing stretch of it (a
         * centred word one column off, a blank inserted or lost). */
        int h = horizontal_shift(e, act, n, 0, n, a->wildcard);
        /* The stretch counts as shifted only if it holds text: blanks
         * "shifted" onto blanks explain nothing. */
        int span_text = 0;
        for (int c = first; c <= last; c++)
            if (!cell_blank(act[c]) && act[c][0]) span_text = 1;
        int hs = (h || !span_text || last <= first)
                     ? 0 : horizontal_shift(e, act, n, first, last + 1, a->wildcard);
        if (h) {
            sb_printf(sb, "    the text is shifted %s by %d column%s\n",
                      h > 0 ? "right" : "left", abs(h), abs(h) == 1 ? "" : "s");
        } else if (only_space_kinds(e, act, n, diff)) {
            sb_puts(sb, "    a different kind of blank: the cells look alike, "
                        "the code points differ\n");
        } else if (case_only(e, act, n, a->wildcard)) {
            sb_puts(sb, "    differs only in letter case\n");
        } else if (la < le && row_equal(e, act, la + 1, a->wildcard)) {
            sb_printf(sb, "    truncated: matches up to col %d, the expected text "
                          "continues to col %d\n", a->col + la, a->col + le);
        } else if (le < la && row_equal(e, act, le + 1, a->wildcard)) {
            sb_printf(sb, "    extra text: matches up to col %d, then cols %d-%d "
                          "should be blank\n", a->col + le, a->col + le + 1, a->col + la);
        } else if (hs) {
            sb_printf(sb, "    the text in cols %d-%d is shifted %s by %d column%s\n",
                      a->col + first, a->col + last, hs > 0 ? "right" : "left",
                      abs(hs), abs(hs) == 1 ? "" : "s");
        }
    }

    for (int c = 0; c < n; c++) {
        if (!diff[c]) continue;
        sb_printf(sb, "    first difference at col %d: expected ", a->col + c);
        describe_text(sb, e[c]);
        sb_puts(sb, ", actual ");
        describe_text(sb, act[c]);
        sb_puts(sb, "\n");
        break;
    }
}

int pty_region_compare(PtySession *s, const PtyRegion *reg,
                       const char *const expected[],
                       char *report, size_t report_size) {
    if (report && report_size) report[0] = '\0';
    if (!s || !s->screen || !expected) return -1;

    Sb sb = {0};
    Area a;
    if (resolve(s, reg, &a, &sb) < 0) {
        sb_export(&sb, report, report_size);
        sb_free(&sb);
        return -1;
    }

    int nexp = 0;
    while (expected[nexp]) nexp++;
    if (nexp > a.rows) {
        sb_printf(&sb, "the expectation has %d rows but the %s has %d\n", nexp,
                  is_full(s, &a) ? "screen" : "region", a.rows);
        sb_export(&sb, report, report_size);
        sb_free(&sb);
        return -1;
    }

    size_t cells = (size_t)a.rows * (size_t)a.cols;
    CellText *exp = malloc(cells * sizeof(CellText));
    const char **ep = malloc(cells * sizeof(*ep));
    const char **ap = malloc(cells * sizeof(*ap));
    unsigned char *diff = calloc(cells, 1);
    unsigned char *row_diff = calloc((size_t)a.rows, 1);
    unsigned char *show = calloc((size_t)a.rows, 1);
    int result = -1;
    if (!exp || !ep || !ap || !diff || !row_diff || !show) goto out;

    for (int r = 0; r < a.rows; r++) {
        CellText *er = &exp[(size_t)r * (size_t)a.cols];
        int w = layout_row(r < nexp ? expected[r] : "", er, a.cols);
        if (w > a.cols) {
            sb_printf(&sb, "expected row %d (screen row %d) is %d columns wide, "
                           "the %s only %d: |%s|\n", r, a.row + r, w,
                      is_full(s, &a) ? "screen" : "region", a.cols, expected[r]);
            goto out;
        }
        for (int c = 0; c < a.cols; c++) ep[(size_t)r * (size_t)a.cols + (size_t)c] = er[c];
        actual_row(s, &a, a.row + r, &ap[(size_t)r * (size_t)a.cols]);
    }

    int ndiff = 0;
    for (int r = 0; r < a.rows; r++) {
        size_t base = (size_t)r * (size_t)a.cols;
        for (int c = 0; c < a.cols; c++) {
            if (!cell_match(ep[base + (size_t)c], ap[base + (size_t)c], a.wildcard)) {
                diff[base + (size_t)c] = 1;
                row_diff[r] = 1;
            }
        }
        if (row_diff[r]) {
            ndiff++;
            for (int k = r - a.context; k <= r + a.context; k++)
                if (k >= 0 && k < a.rows) show[k] = 1;
        }
    }
    result = ndiff;
    if (ndiff == 0) goto out;

    header(&sb, s, &a, "text", ndiff, a.rows);
    sb_puts(&sb, "  (- expected, + actual, ^ differing column; ruler = screen column)\n");
    for (int r = 0; r < a.rows; r++) {
        if (!show[r]) continue;
        if (r == 0 || !show[r - 1]) {
            if (r > 0) sb_puts(&sb, "     ...\n");
            ruler(&sb, &a);
        }
        size_t base = (size_t)r * (size_t)a.cols;
        if (row_diff[r]) {
            sb_printf(&sb, "- %4d |", a.row + r);
            render_cells(&sb, &ep[base], a.cols);
            sb_printf(&sb, "|\n+ %4d |", a.row + r);
            render_cells(&sb, &ap[base], a.cols);
            sb_puts(&sb, "|\n");
            carets(&sb, &diff[base], a.cols);
        } else {
            sb_printf(&sb, "  %4d |", a.row + r);
            render_cells(&sb, &ap[base], a.cols);
            sb_puts(&sb, "|\n");
        }
    }
    if (a.rows > 0 && !show[a.rows - 1]) sb_puts(&sb, "     ...\n");

    sb_puts(&sb, "notes:\n");
    int common_v = 0, consistent = 1, moved = 0;
    for (int r = 0; r < a.rows; r++) {
        if (!row_diff[r]) continue;
        size_t base = (size_t)r * (size_t)a.cols;
        int v;
        text_notes(&sb, s, &a, r, &ep[base], &ap[base], &diff[base], &v);
        if (!row_blank(&ep[base], a.cols)) {
            if (!v || (common_v && v != common_v)) consistent = 0;
            common_v = v;
            moved++;
        }
    }
    if (consistent && common_v && moved > 1)
        sb_printf(&sb, "  every missing row is %d row%s %s than expected: the content "
                       "has moved as a block\n",
                  abs(common_v), abs(common_v) == 1 ? "" : "s",
                  common_v > 0 ? "lower" : "higher");

out:
    if (sb.oom && report && report_size) {
        snprintf(report, report_size, "(out of memory writing the report)\n");
    } else {
        sb_export(&sb, report, report_size);
    }
    sb_free(&sb);
    free(exp);
    free(ep);
    free(ap);
    free(diff);
    free(row_diff);
    free(show);
    return result;
}

int pty_screen_compare(PtySession *s, const char *const expected[],
                       char *report, size_t report_size) {
    return pty_region_compare(s, NULL, expected, report, report_size);
}

/* ── Style comparison ────────────────────────────────────────────────── */

static const PtyStyleKey *legend_find(const PtyStyleKey *legend, char key) {
    for (const PtyStyleKey *k = legend; k->key; k++) if (k->key == key) return k;
    return NULL;
}

static char style_key(const PtyStyleKey *legend, const PtyCell *c) {
    for (const PtyStyleKey *k = legend; k->key; k++)
        if (k->attr == c->attr && k->fg == c->fg && k->bg == c->bg) return k->key;
    return '#';
}

static int style_wildcard(const Area *a) {
    return (a->wildcard && a->wildcard[0] && !a->wildcard[1]) ? a->wildcard[0] : -1;
}

char *pty_region_style_map(PtySession *s, const PtyRegion *reg,
                           const PtyStyleKey legend[], char *buf, size_t size) {
    if (!buf || size == 0) return buf;
    buf[0] = '\0';
    if (!s || !s->screen || !legend) return buf;
    Sb sb = {0};
    Area a;
    if (resolve(s, reg, &a, &sb) == 0) {
        sb.len = 0;
        if (sb.buf) sb.buf[0] = '\0';
        for (int r = 0; r < a.rows; r++) {
            char line[4097];
            int n = a.cols < 4096 ? a.cols : 4096;
            for (int c = 0; c < n; c++)
                line[c] = style_key(legend, pty_screen_cell(s->screen, a.row + r, a.col + c));
            line[n] = '\0';
            sb_puts(&sb, line);
            if (r + 1 < a.rows) sb_puts(&sb, "\n");
        }
    } else {
        sb.len = 0;
        if (sb.buf) sb.buf[0] = '\0';
    }
    sb_export(&sb, buf, size);
    sb_free(&sb);
    return buf;
}

int pty_region_compare_style(PtySession *s, const PtyRegion *reg,
                             const char *const expected[],
                             const PtyStyleKey legend[],
                             char *report, size_t report_size) {
    if (report && report_size) report[0] = '\0';
    if (!s || !s->screen || !expected || !legend) return -1;

    Sb sb = {0};
    Area a;
    int result = -1;
    unsigned char *diff = NULL, *row_diff = NULL, *show = NULL;
    char *amap = NULL;
    if (resolve(s, reg, &a, &sb) < 0) goto out;

    int nexp = 0;
    while (expected[nexp]) nexp++;
    if (nexp > a.rows) {
        sb_printf(&sb, "the style map has %d rows but the region has %d\n", nexp, a.rows);
        goto out;
    }
    int wc = style_wildcard(&a);
    for (int r = 0; r < nexp; r++) {
        int len = (int)strlen(expected[r]);
        if (len > a.cols) {
            sb_printf(&sb, "style map row %d is %d characters, the region only %d wide\n",
                      r, len, a.cols);
            goto out;
        }
        for (int c = 0; c < len; c++) {
            char k = expected[r][c];
            if (k != wc && !legend_find(legend, k)) {
                sb_printf(&sb, "style map row %d, col %d: '%c' is not in the legend\n",
                          r, c, k);
                goto out;
            }
        }
    }

    size_t cells = (size_t)a.rows * (size_t)a.cols;
    diff = calloc(cells, 1);
    row_diff = calloc((size_t)a.rows, 1);
    show = calloc((size_t)a.rows, 1);
    amap = malloc((size_t)a.cols + 1);
    if (!diff || !row_diff || !show || !amap) goto out;

    int ndiff = 0;
    for (int r = 0; r < nexp; r++) {
        int len = (int)strlen(expected[r]);
        for (int c = 0; c < len; c++) {
            char k = expected[r][c];
            if (k == wc) continue;
            const PtyStyleKey *want = legend_find(legend, k);
            PtyCell *cell = pty_screen_cell(s->screen, a.row + r, a.col + c);
            if (want->attr != cell->attr || want->fg != cell->fg || want->bg != cell->bg) {
                diff[(size_t)r * (size_t)a.cols + (size_t)c] = 1;
                row_diff[r] = 1;
            }
        }
        if (row_diff[r]) {
            ndiff++;
            for (int k = r - a.context; k <= r + a.context; k++)
                if (k >= 0 && k < a.rows) show[k] = 1;
        }
    }
    result = ndiff;
    if (ndiff == 0) goto out;

    header(&sb, s, &a, "style", ndiff, nexp);
    sb_puts(&sb, "  (- expected map, + actual map, ^ differing column, # style not in the legend)\n"
                 "  legend:");
    for (const PtyStyleKey *k = legend; k->key; k++) {
        sb_printf(&sb, " %c=", k->key);
        describe_style(&sb, k->attr, k->fg, k->bg);
        sb_puts(&sb, k[1].key ? ";" : "\n");
    }
    const char **text = malloc((size_t)a.cols * sizeof(*text));
    if (!text) { result = -1; goto out; }
    for (int r = 0; r < a.rows; r++) {
        if (!show[r]) continue;
        if (r == 0 || !show[r - 1]) {
            if (r > 0) sb_puts(&sb, "     ...\n");
            ruler(&sb, &a);
        }
        for (int c = 0; c < a.cols; c++)
            amap[c] = style_key(legend, pty_screen_cell(s->screen, a.row + r, a.col + c));
        amap[a.cols] = '\0';
        actual_row(s, &a, a.row + r, text);
        if (row_diff[r]) {
            sb_printf(&sb, "- %4d |%s|\n", a.row + r, expected[r]);
            sb_printf(&sb, "+ %4d |%s|\n", a.row + r, amap);
            carets(&sb, &diff[(size_t)r * (size_t)a.cols], a.cols);
            sb_puts(&sb, "  text |");
            render_cells(&sb, text, a.cols);
            sb_puts(&sb, "|\n");
        } else {
            sb_printf(&sb, "  %4d |%s|\n", a.row + r, amap);
        }
    }

    sb_puts(&sb, "notes:\n");
    for (int r = 0; r < nexp; r++) {
        if (!row_diff[r]) continue;
        unsigned char *d = &diff[(size_t)r * (size_t)a.cols];
        int len = (int)strlen(expected[r]);
        sb_printf(&sb, "  row %d: ", a.row + r);
        col_ranges(&sb, d, a.cols, a.col);
        sb_puts(&sb, "\n");

        /* A highlight one column off is the commonest style bug. */
        for (int k = 1; k <= 3; k++) {
            int found = 0;
            for (int sign = 1; sign >= -1 && !found; sign -= 2) {
                int sh = k * sign, ok = 1;
                for (int c = 0; c < len && ok; c++) {
                    int src = c - sh;
                    if (src < 0 || src >= len || expected[r][src] == wc) continue;
                    const PtyStyleKey *want = legend_find(legend, expected[r][src]);
                    PtyCell *cell = pty_screen_cell(s->screen, a.row + r, a.col + c);
                    ok = want->attr == cell->attr && want->fg == cell->fg && want->bg == cell->bg;
                }
                if (ok) {
                    sb_printf(&sb, "    the styling is shifted %s by %d column%s\n",
                              sh > 0 ? "right" : "left", k, k == 1 ? "" : "s");
                    found = 1;
                }
            }
            if (found) break;
        }

        for (int c = 0; c < len; c++) {
            if (!d[c]) continue;
            const PtyStyleKey *want = legend_find(legend, expected[r][c]);
            PtyCell *cell = pty_screen_cell(s->screen, a.row + r, a.col + c);
            sb_printf(&sb, "    first difference at col %d (", a.col + c);
            describe_text(&sb, cell->ch);
            sb_printf(&sb, "): expected %c = ", want->key);
            describe_style(&sb, want->attr, want->fg, want->bg);
            sb_printf(&sb, ", actual %c = ", style_key(legend, cell));
            describe_style(&sb, cell->attr, (int)cell->fg, (int)cell->bg);
            sb_puts(&sb, "\n");
            break;
        }
    }
    free(text);

out:
    if (sb.oom && report && report_size)
        snprintf(report, report_size, "(out of memory writing the report)\n");
    else
        sb_export(&sb, report, report_size);
    sb_free(&sb);
    free(diff);
    free(row_diff);
    free(show);
    free(amap);
    return result;
}

/* ── Snapshots ───────────────────────────────────────────────────────── */

void pty_region_dump_c(PtySession *s, const PtyRegion *reg, FILE *fp) {
    if (!s || !s->screen || !fp) return;
    Sb sb = {0};
    Area a;
    if (resolve(s, reg, &a, &sb) < 0) {
        if (sb.buf) fputs(sb.buf, fp);
        sb_free(&sb);
        return;
    }
    sb_free(&sb);

    const char **row = malloc((size_t)a.cols * sizeof(*row));
    if (!row) return;
    /* Trailing blank rows are left out: rows past the array must be blank. */
    int last = a.rows - 1;
    for (; last >= 0; last--) {
        actual_row(s, &a, a.row + last, row);
        if (!row_blank(row, a.cols)) break;
    }
    fputs("const char *const expected[] = {\n", fp);
    for (int r = 0; r <= last; r++) {
        actual_row(s, &a, a.row + r, row);
        int end = last_nonblank(row, a.cols);
        fputs("    \"", fp);
        for (int c = 0; c <= end; c++) {
            const char *t = row[c];
            if (t[0] == '\0') { if (c == 0) fputc(' ', fp); continue; }
            for (; *t; t++) {
                if (*t == '"' || *t == '\\') fputc('\\', fp);
                fputc(*t, fp);
            }
        }
        fputs("\",\n", fp);
    }
    fputs("    NULL\n};\n", fp);
    free(row);
}

static void sgr_colour(FILE *fp, int c, int fg) {
    if (c == PTY_COLOR_DEFAULT) return;
    if (PTY_COLOR_IS_RGB(c))
        fprintf(fp, ";%d;2;%d;%d;%d", fg ? 38 : 48, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    else if (PTY_COLOR_IS_IDX(c))
        fprintf(fp, ";%d;5;%d", fg ? 38 : 48, c & 0xFF);
    else
        fprintf(fp, ";%d", c);
}

void pty_screen_dump_ansi(PtySession *s, FILE *fp) {
    if (!s || !s->screen || !fp) return;
    static const struct { int bit, sgr; } map[] = {
        { PTY_ATTR_BOLD, 1 }, { PTY_ATTR_DIM, 2 }, { PTY_ATTR_ITALIC, 3 },
        { PTY_ATTR_UNDERLINE, 4 }, { PTY_ATTR_BLINK, 5 }, { PTY_ATTR_REVERSE, 7 },
        { PTY_ATTR_INVISIBLE, 8 }, { PTY_ATTR_STRIKE, 9 },
    };
    for (int r = 0; r < s->rows; r++) {
        int attr = -1;
        int32_t fg = -1, bg = -1;
        for (int c = 0; c < s->cols; c++) {
            PtyCell *cell = pty_screen_cell(s->screen, r, c);
            if (cell->width == 0) continue;
            if (cell->attr != attr || cell->fg != fg || cell->bg != bg) {
                fputs("\033[0", fp);
                for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                    if (cell->attr & map[i].bit) fprintf(fp, ";%d", map[i].sgr);
                sgr_colour(fp, (int)cell->fg, 1);
                sgr_colour(fp, (int)cell->bg, 0);
                fputc('m', fp);
                attr = cell->attr;
                fg = cell->fg;
                bg = cell->bg;
            }
            fputs(cell->ch, fp);
        }
        /* CR LF between rows and none after the last: fed back into a
         * terminal of the same size, the dump reproduces the screen
         * without scrolling it. */
        fputs(r + 1 < s->rows ? "\033[0m\r\n" : "\033[0m", fp);
    }
}
