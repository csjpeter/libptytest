/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-03 */
/* Minimal test runner shared by the test programs: cases, filter, alarm, assertion floor. */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

/**
 * @file test_helpers.h
 * @brief The project's own test runner: no external test library.
 *
 * One executable per suite, so a crash in one suite cannot take another's
 * results with it.  Each suite:
 *   - runs under an alarm, so a hang fails instead of waiting for CI;
 *   - takes an optional filter argument to run only the tests whose name
 *     contains it, and "-l" to list them;
 *   - reports its assertion count and fails if a full run made fewer
 *     assertions than its floor, so a deleted registration line or an
 *     early return cannot pass unnoticed.
 *
 * See policies/testing-model.md.
 */

/* alarm(), setenv(), open_memstream(): POSIX, beyond plain C11. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "pty_assert.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int g_tests_run;
extern int g_tests_failed;

typedef struct {
    const char *name;
    void (*fn)(void);
} TestCase;

#define TEST_CASE(fn) { #fn, fn }

static inline int test_run_suite(const char *suite, const TestCase *tests,
                                  size_t count, int min_assertions,
                                  unsigned alarm_seconds, int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;

    if (filter && strcmp(filter, "-l") == 0) {
        for (size_t i = 0; i < count; i++) printf("%s\n", tests[i].name);
        return EXIT_SUCCESS;
    }

    /* SIGALRM's default action terminates the process abnormally, which
     * CTest counts as a failure. */
    alarm(alarm_seconds);
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("--- %s ---\n", suite);
    int selected = 0;
    for (size_t i = 0; i < count; i++) {
        if (filter && !strstr(tests[i].name, filter)) continue;
        selected++;
        int failed_before = g_tests_failed;
        printf("  %s...\n", tests[i].name);
        tests[i].fn();
        if (g_tests_failed != failed_before) printf("  -> %s FAILED\n", tests[i].name);
    }

    printf("\n--- Results: %s ---\n", suite);
    printf("Tests:            %d\n", selected);
    printf("Assertions run:   %d\n", g_tests_run);
    printf("Assertions failed:%d\n", g_tests_failed);

    if (filter && selected == 0) {
        printf("FAIL: no test matches \"%s\"\n", filter);
        return EXIT_FAILURE;
    }
    if (!filter && g_tests_run < min_assertions) {
        printf("FAIL: %d assertions, fewer than the floor of %d\n",
               g_tests_run, min_assertions);
        return EXIT_FAILURE;
    }
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

#endif /* TEST_HELPERS_H */
