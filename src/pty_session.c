/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-03 */
/* PTY session lifecycle, input and screen inspection. */
/**
 * @file pty_session.c
 * @brief PTY session lifecycle, input, and screen inspection.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "pty_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <pty.h>

#ifndef PTYTEST_VERSION_STRING
#define PTYTEST_VERSION_STRING "unknown"
#endif

const char *pty_version(void) { return PTYTEST_VERSION_STRING; }

/* ── Lifecycle ───────────────────────────────────────────────────────── */

PtySession *pty_open(int cols, int rows) {
    PtySession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->master_fd = -1;
    s->child_pid = -1;
    s->cols = cols;
    s->rows = rows;
    s->screen = pty_screen_new(cols, rows);
    if (!s->screen) { free(s); return NULL; }

    /* A coverage build of the program under test writes its .gcda files
     * during exit, and a SIGTERM landing in the middle leaves a truncated
     * file that fails the whole coverage report.  The caller's build system
     * knows whether that is the case; an environment variable lets it say
     * so without a code change in every test. */
    const char *grace = getenv("PTYTEST_CLOSE_GRACE_MS");
    if (grace && *grace) {
        char *end = NULL;
        long ms = strtol(grace, &end, 10);
        if (end && *end == '\0' && ms > 0 && ms < 600000) s->close_grace_ms = (int)ms;
    }
    return s;
}

int pty_setenv(PtySession *s, const char *name, const char *value) {
    if (!s || !name || !*name || strchr(name, '=')) return -1;
    for (size_t i = 0; i < s->env_count; i++) {
        if (strcmp(s->env[i].name, name) == 0) {
            char *v = value ? strdup(value) : NULL;
            if (value && !v) return -1;
            free(s->env[i].value);
            s->env[i].value = v;
            return 0;
        }
    }
    PtyEnvVar *grown = realloc(s->env, (s->env_count + 1) * sizeof(*grown));
    if (!grown) return -1;
    s->env = grown;
    char *n = strdup(name);
    char *v = value ? strdup(value) : NULL;
    if (!n || (value && !v)) { free(n); free(v); return -1; }
    s->env[s->env_count].name = n;
    s->env[s->env_count].value = v;
    s->env_count++;
    return 0;
}

/* Runs in the child between fork and exec. */
static void child_setup(PtySession *s) {
    /* A test runner may have ignored or blocked signals; the program under
     * test must start with the dispositions a shell would give it. */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    setenv("TERM", "xterm-256color", 1);
    setenv("LC_ALL", "en_US.UTF-8", 1);
    /* Remove COLUMNS/LINES so the program queries the PTY */
    unsetenv("COLUMNS");
    unsetenv("LINES");
    for (size_t i = 0; i < s->env_count; i++) {
        if (s->env[i].value) setenv(s->env[i].name, s->env[i].value, 1);
        else unsetenv(s->env[i].name);
    }
}

int pty_run(PtySession *s, const char *argv[]) {
    if (!s || !argv || !argv[0] || s->child_pid > 0 || s->master_fd >= 0) {
        errno = EINVAL;
        return -1;
    }

    struct winsize ws = {
        .ws_row = (unsigned short)s->rows,
        .ws_col = (unsigned short)s->cols
    };

    /* A close-on-exec pipe tells the parent whether exec succeeded: it is
     * closed by a successful exec, and carries errno when exec fails.
     * Without it a missing binary is indistinguishable from a program that
     * printed nothing, and the test fails later with a misleading timeout. */
    int errpipe[2];
    if (pipe(errpipe) < 0) return -1;
    if (fcntl(errpipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        int e = errno;
        close(errpipe[0]);
        close(errpipe[1]);
        errno = e;
        return -1;
    }

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        int e = errno;
        close(errpipe[0]);
        close(errpipe[1]);
        errno = e;
        return -1;
    }

    if (pid == 0) {
        close(errpipe[0]);
        child_setup(s);
        execvp(argv[0], (char *const *)argv);
        int e = errno;
        ssize_t w = write(errpipe[1], &e, sizeof(e));
        (void)w;
        _exit(127);
    }

    close(errpipe[1]);
    int child_errno = 0;
    ssize_t n;
    do {
        n = read(errpipe[0], &child_errno, sizeof(child_errno));
    } while (n < 0 && errno == EINTR);
    close(errpipe[0]);

    if (n == (ssize_t)sizeof(child_errno)) {
        int status;
        waitpid(pid, &status, 0);
        close(master_fd);
        errno = child_errno;
        return -1;
    }

    s->master_fd = master_fd;
    s->child_pid = pid;

    /* Non-blocking master: every read is driven by poll() with a deadline. */
    int flags = fcntl(master_fd, F_GETFL);
    if (flags < 0 || fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int e = errno;
        pty_signal(s, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        s->child_pid = -1;
        close(master_fd);
        s->master_fd = -1;
        errno = e;
        return -1;
    }
    return 0;
}

void pty_set_close_grace(PtySession *s, int ms) {
    if (s) s->close_grace_ms = ms > 0 ? ms : 0;
}

/* Polls waitpid for up to @p ms; returns 1 once the child is reaped. */
static int reap_within(PtySession *s, int ms) {
    int status;
    for (int waited = 0; ; waited += 20) {
        pid_t r = waitpid(s->child_pid, &status, WNOHANG);
        if (r == s->child_pid || (r < 0 && errno == ECHILD)) {
            s->child_pid = -1;
            return 1;
        }
        if (waited >= ms) return 0;
        /* Keep the PTY drained: a child blocked on a full output buffer
         * cannot reach its own exit. */
        if (s->master_fd >= 0) pty_drain(s);
        usleep(20000);
    }
}

void pty_close(PtySession *s) {
    if (!s) return;

    if (s->child_pid > 0) {
        /* Let a child that is on its way out finish before signalling it:
         * see the comment on PTYTEST_CLOSE_GRACE_MS in pty_open(). */
        if (s->close_grace_ms > 0) reap_within(s, s->close_grace_ms);
        if (s->child_pid > 0) {
            kill(s->child_pid, SIGTERM);
            /* Poll rather than assume a fixed delay is enough. */
            reap_within(s, 2000);
        }
        if (s->child_pid > 0) {   /* ignored SIGTERM, or wedged */
            int status;
            kill(s->child_pid, SIGKILL);
            waitpid(s->child_pid, &status, 0);
            s->child_pid = -1;
        }
    }

    if (s->master_fd >= 0) close(s->master_fd);
    for (size_t i = 0; i < s->env_count; i++) {
        free(s->env[i].name);
        free(s->env[i].value);
    }
    free(s->env);
    pty_screen_free(s->screen);
    free(s);
}

int pty_resize(PtySession *s, int cols, int rows) {
    if (!s || cols <= 0 || rows <= 0) return -1;
    if (s->master_fd >= 0) {
        /* Bring the screen up to date first, so output produced for the old
         * size is laid out at the old size. */
        pty_drain(s);
        struct winsize ws = { .ws_row = (unsigned short)rows,
                              .ws_col = (unsigned short)cols };
        if (ioctl(s->master_fd, TIOCSWINSZ, &ws) < 0) return -1;
    }
    if (pty_screen_resize(s->screen, cols, rows) < 0) return -1;
    s->cols = cols;
    s->rows = rows;
    return 0;
}

int pty_signal(PtySession *s, int sig) {
    if (!s || s->child_pid <= 0) return -1;
    return kill(s->child_pid, sig) == 0 ? 0 : -1;
}

void pty_get_size(PtySession *s, int *cols, int *rows) {
    if (cols) *cols = s ? s->cols : 0;
    if (rows) *rows = s ? s->rows : 0;
}

/* ── Input ───────────────────────────────────────────────────────────── */

static int write_all(PtySession *s, const char *bytes, size_t len, int drain) {
    if (!s || s->master_fd < 0 || (!bytes && len)) return -1;
    long deadline = pty_now_ms() + 5000;
    while (len > 0) {
        ssize_t n = write(s->master_fd, bytes, len);
        if (n > 0) {
            bytes += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN) return -1;

        /* The input queue is full.  The child may itself be blocked writing
         * output nobody reads, so read while waiting for room. */
        long remaining = deadline - pty_now_ms();
        if (remaining <= 0) return -1;
        struct pollfd pfd = { .fd = s->master_fd,
                              .events = (short)(POLLOUT | (drain ? POLLIN : 0)) };
        int r = poll(&pfd, 1, remaining > 50 ? 50 : (int)remaining);
        if (r > 0 && drain && (pfd.revents & POLLIN)) pty_read_and_feed(s);
        else if (r > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                 && !(pfd.revents & POLLOUT)) return -1;
    }
    return 0;
}

int pty_send(PtySession *s, const char *bytes, size_t len) {
    return write_all(s, bytes, len, 1);
}

void pty_flush_responses(PtySession *s) {
    if (!s || !s->screen || s->screen->resp_len == 0) return;
    if (s->master_fd >= 0)
        write_all(s, s->screen->resp, s->screen->resp_len, 0);
    s->screen->resp_len = 0;
}

int pty_send_key(PtySession *s, PtyKey key) {
    /* DECCKM: in application cursor mode xterm sends SS3 instead of CSI for
     * the cursor keys and Home/End, and a program that enabled it (every
     * curses program in keypad mode) recognises only that form. */
    int app = s && s->screen && s->screen->app_cursor;
    const char *seq = NULL;
    switch (key) {
    case PTY_KEY_UP:        seq = app ? "\033OA" : "\033[A"; break;
    case PTY_KEY_DOWN:      seq = app ? "\033OB" : "\033[B"; break;
    case PTY_KEY_RIGHT:     seq = app ? "\033OC" : "\033[C"; break;
    case PTY_KEY_LEFT:      seq = app ? "\033OD" : "\033[D"; break;
    case PTY_KEY_HOME:      seq = app ? "\033OH" : "\033[H"; break;
    case PTY_KEY_END:       seq = app ? "\033OF" : "\033[F"; break;
    case PTY_KEY_PGUP:      seq = "\033[5~"; break;
    case PTY_KEY_PGDN:      seq = "\033[6~"; break;
    case PTY_KEY_INSERT:    seq = "\033[2~"; break;
    case PTY_KEY_DELETE:    seq = "\033[3~"; break;
    case PTY_KEY_SHIFT_TAB: seq = "\033[Z"; break;
    case PTY_KEY_F1:        seq = "\033OP"; break;
    case PTY_KEY_F2:        seq = "\033OQ"; break;
    case PTY_KEY_F3:        seq = "\033OR"; break;
    case PTY_KEY_F4:        seq = "\033OS"; break;
    case PTY_KEY_F5:        seq = "\033[15~"; break;
    case PTY_KEY_F6:        seq = "\033[17~"; break;
    case PTY_KEY_F7:        seq = "\033[18~"; break;
    case PTY_KEY_F8:        seq = "\033[19~"; break;
    case PTY_KEY_F9:        seq = "\033[20~"; break;
    case PTY_KEY_F10:       seq = "\033[21~"; break;
    case PTY_KEY_F11:       seq = "\033[23~"; break;
    case PTY_KEY_F12:       seq = "\033[24~"; break;
    default:
        if ((int)key < 0 || (int)key > 0xFF) return -1;
        {
            char c = (char)key;
            return pty_send(s, &c, 1);
        }
    }
    return pty_send(s, seq, strlen(seq));
}

int pty_send_str(PtySession *s, const char *str) {
    if (!str) return -1;
    return pty_send(s, str, strlen(str));
}

/* ── Screen inspection ───────────────────────────────────────────────── */

static PtyCell *cell(PtySession *s, int row, int col) {
    return s ? pty_screen_cell(s->screen, row, col) : NULL;
}

const char *pty_cell_text(PtySession *s, int row, int col) {
    PtyCell *c = cell(s, row, col);
    return c ? c->ch : "";
}

int pty_cell_attr(PtySession *s, int row, int col) {
    PtyCell *c = cell(s, row, col);
    return c ? c->attr : 0;
}

int pty_cell_fg(PtySession *s, int row, int col) {
    PtyCell *c = cell(s, row, col);
    return c ? (int)c->fg : PTY_COLOR_DEFAULT;
}

int pty_cell_bg(PtySession *s, int row, int col) {
    PtyCell *c = cell(s, row, col);
    return c ? (int)c->bg : PTY_COLOR_DEFAULT;
}

int pty_cursor(PtySession *s, int *row, int *col) {
    if (!s || !s->screen) return -1;
    if (row) *row = s->screen->cur_row;
    if (col) *col = s->screen->cur_col;
    return s->screen->cursor_visible ? 1 : 0;
}

char *pty_row_text(PtySession *s, int row, char *buf, size_t size) {
    if (!buf || size == 0) return buf;
    buf[0] = '\0';
    if (!s || !s->screen || row < 0 || row >= s->rows) return buf;

    size_t pos = 0;
    for (int c = 0; c < s->cols; c++) {
        const char *ch = s->screen->cells[row * s->cols + c].ch;
        size_t len = strlen(ch);   /* 0 for the tail of a wide character */
        if (pos + len >= size) break;
        memcpy(buf + pos, ch, len);
        pos += len;
    }
    buf[pos] = '\0';

    /* Trim trailing spaces */
    while (pos > 0 && buf[pos - 1] == ' ') buf[--pos] = '\0';

    return buf;
}

static size_t row_buf_size(PtySession *s) {
    return (size_t)s->cols * PTY_CELL_BYTES + 1;
}

int pty_row_contains(PtySession *s, int row, const char *text) {
    if (!s || !s->screen || !text) return 0;
    size_t size = row_buf_size(s);
    char *buf = malloc(size);
    if (!buf) return 0;
    pty_row_text(s, row, buf, size);
    int found = strstr(buf, text) != NULL;
    free(buf);
    return found;
}

int pty_screen_contains(PtySession *s, const char *text) {
    if (!s || !s->screen || !text) return 0;
    for (int r = 0; r < s->rows; r++) {
        if (pty_row_contains(s, r, text)) return 1;
    }
    return 0;
}

int pty_screen_is_blank(PtySession *s) {
    if (!s || !s->screen) return 1;
    for (int i = 0; i < s->cols * s->rows; i++) {
        const char *ch = s->screen->cells[i].ch;
        if (ch[0] != ' ' || ch[1] != '\0') return 0;
    }
    return 1;
}

char *pty_screen_text(PtySession *s, char *buf, size_t size) {
    if (!buf || size == 0) return buf;
    buf[0] = '\0';
    if (!s || !s->screen) return buf;
    size_t rsize = row_buf_size(s);
    char *row = malloc(rsize);
    if (!row) return buf;
    size_t pos = 0;
    for (int r = 0; r < s->rows; r++) {
        pty_row_text(s, r, row, rsize);
        size_t len = strlen(row);
        size_t need = len + (r + 1 < s->rows ? 1 : 0);
        if (pos + need >= size) {
            /* Truncate at a character boundary rather than mid-sequence. */
            size_t room = size - 1 - pos;
            while (room > 0 && ((unsigned char)row[room] & 0xC0) == 0x80) room--;
            memcpy(buf + pos, row, room);
            pos += room;
            break;
        }
        memcpy(buf + pos, row, len);
        pos += len;
        if (r + 1 < s->rows) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    free(row);
    return buf;
}

void pty_screen_dump(PtySession *s, FILE *fp) {
    if (!s || !s->screen || !fp) return;
    size_t rsize = row_buf_size(s);
    char *row = malloc(rsize);
    if (!row) return;
    fprintf(fp, "    +");
    for (int c = 0; c < s->cols; c++) fputc(c % 10 == 0 ? '0' + (c / 10) % 10 : '-', fp);
    fprintf(fp, "+\n");
    for (int r = 0; r < s->rows; r++) {
        /* Untrimmed, so the frame shows where the row really ends. */
        size_t p = 0;
        for (int c = 0; c < s->cols; c++) {
            const char *ch = s->screen->cells[r * s->cols + c].ch;
            size_t len = strlen(ch);
            if (p + len >= rsize) break;
            memcpy(row + p, ch, len);
            p += len;
        }
        row[p] = '\0';
        fprintf(fp, "%3d |%s|\n", r, row);
    }
    fprintf(fp, "    +");
    for (int c = 0; c < s->cols; c++) fputc('-', fp);
    fprintf(fp, "+\n    cursor at row %d, col %d (%s)\n",
            s->screen->cur_row, s->screen->cur_col,
            s->screen->cursor_visible ? "visible" : "hidden");
    free(row);
}
