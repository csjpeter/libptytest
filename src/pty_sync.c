/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Péter Császár <peter@csaszar.email>
 */
/**
 * @file pty_sync.c
 * @brief PTY output reading and synchronisation (poll + deadline).
 *
 * Every wait in this file is a wait for a condition with a deadline, never
 * a fixed sleep: see policies/testing-model.md, "Wait for a condition, not
 * for a clock".
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "pty_internal.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** Upper bound on pty_settle(), whatever the quiet period. */
#define PTY_SETTLE_LIMIT_MS 30000

/* ── Time helpers ────────────────────────────────────────────────────── */

long pty_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── Read and feed ───────────────────────────────────────────────────── */

static int g_trace_active = 0;
void pty_trace_enable(int on) { g_trace_active = on; }

static void trace(const char *buf, size_t n) {
    printf("  [TRACE %zu bytes]:", n);
    for (size_t i = 0; i < n && i < 120; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x1B) printf(" ESC");
        else if (c < 0x20 || c > 0x7E) printf(" %02X", c);
        else printf(" %c", c);
    }
    printf("\n");
    fflush(stdout);
}

void pty_feed(PtySession *s, const char *data, size_t len) {
    if (!s || !s->screen || !data) return;
    pty_screen_feed(s->screen, data, len);
    pty_flush_responses(s);
}

int pty_read_and_feed(PtySession *s) {
    char buf[8192];
    ssize_t n;
    do {
        n = read(s->master_fd, buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        if (g_trace_active) trace(buf, (size_t)n);
        pty_feed(s, buf, (size_t)n);
        return (int)n;
    }
    /* -2 distinguishes "nothing yet" from EIO/EOF (the slave side closed). */
    if (n < 0 && errno == EAGAIN) return -2;
    return 0;
}

int pty_drain(PtySession *s) {
    if (!s || s->master_fd < 0) return 0;
    int total = 0;
    /* Retry on EAGAIN — the kernel may need a moment to deliver buffered PTY
     * data after the slave side closes (POLLHUP race on a non-blocking
     * master). */
    for (int attempt = 0; attempt < 3; attempt++) {
        for (;;) {
            int n = pty_read_and_feed(s);
            if (n > 0) { total += n; continue; }
            if (n == -2) break; /* nothing right now: retry after a pause */
            return total;       /* EIO/EOF: slave closed and buffer empty */
        }
        if (total > 0) break;
        usleep(5000);
    }
    return total;
}

/* Waits up to @p timeout_ms for output and processes it.
 * @return 1 when there was something to process (or the wait was
 *         interrupted), 0 on timeout, -1 when the child closed the PTY. */
static int pump(PtySession *s, int timeout_ms) {
    struct pollfd pfd = { .fd = s->master_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret == 0) return 0;
    if (ret < 0) return errno == EINTR ? 1 : -1;
    if (pfd.revents & POLLIN) {
        int n = pty_read_and_feed(s);
        if (n > 0 || n == -2) return 1;
        return -1;
    }
    /* POLLHUP/POLLERR without POLLIN: pick up anything still buffered. */
    return pty_drain(s) > 0 ? 1 : -1;
}

/* ── Synchronisation ─────────────────────────────────────────────────── */

int pty_wait_until(PtySession *s, PtyPredicate pred, void *ctx, int timeout_ms) {
    if (!s || !pred) return -1;
    if (pred(s, ctx)) return 0;
    if (s->master_fd < 0) return -1;

    long deadline = pty_now_ms() + timeout_ms;
    for (;;) {
        long remaining = deadline - pty_now_ms();
        if (remaining <= 0) return -1;
        int r = pump(s, (int)remaining);
        if (pred(s, ctx)) return 0;
        if (r < 0) {
            /* The child is gone; one more drain in case the kernel delivered
             * data after the hangup. */
            usleep(10000);
            pty_drain(s);
            return pred(s, ctx) ? 0 : -1;
        }
    }
}

static int contains_pred(PtySession *s, void *ctx) {
    return pty_screen_contains(s, (const char *)ctx);
}

int pty_wait_for(PtySession *s, const char *text, int timeout_ms) {
    if (!text) return -1;
    return pty_wait_until(s, contains_pred, (void *)text, timeout_ms);
}

int pty_wait_exit(PtySession *s, int timeout_ms) {
    if (!s || s->child_pid <= 0) return -1;

    long deadline = pty_now_ms() + timeout_ms;
    int hung_up = s->master_fd < 0;

    for (;;) {
        int status = 0;
        pid_t r = waitpid(s->child_pid, &status, WNOHANG);
        if (r == s->child_pid) {
            /* Clear the pid now that it is reaped: pty_close() must never
             * signal a pid the kernel may have already reassigned. */
            s->child_pid = -1;
            pty_drain(s);   /* pick up whatever it printed on the way out */
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0 && errno != EINTR) { s->child_pid = -1; return -1; }

        long remaining = deadline - pty_now_ms();
        if (remaining <= 0) return -1;
        /* Wait in slices: the child exiting does not wake poll() on the
         * master in every case, so waitpid has to be re-checked. */
        int slice = remaining > 20 ? 20 : (int)remaining;
        if (hung_up) {
            usleep((useconds_t)slice * 1000);
        } else if (pump(s, slice) < 0) {
            hung_up = 1;
        }
    }
}

int pty_settle(PtySession *s, int quiet_ms) {
    if (!s || s->master_fd < 0) return -1;
    /* A child that never stops writing must fail the wait, not hang it. */
    long deadline = pty_now_ms() + PTY_SETTLE_LIMIT_MS;
    for (;;) {
        if (pty_now_ms() > deadline) return -1;
        int r = pump(s, quiet_ms);
        if (r == 0) return 0;   /* quiet period elapsed: settled */
        if (r < 0) return -1;   /* the child closed the PTY */
    }
}
