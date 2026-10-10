# libptytest

**Class: Library** — ships `libptytest0` (shared library) and `libptytest-dev`
(headers, static library, pkg-config and CMake files, man pages).
See `policies/packaging-and-release-model.md` in the workspace.

A C library for testing interactive terminal programs **character by
character**. It runs the real program inside a pseudo-terminal, sends it real
keystrokes, and turns everything it writes into a grid of cells through a
built-in terminal emulator. A test then asks which character, attribute and
colour sits in a given cell — or compares the whole screen against an expected
picture, so that a column shifted by one fails the test.

It grew out of the PTY test harness of `email-cli` (`libs/libptytest`), and is
now a separate library for every project here that has a TUI. `email-cli`
keeps its own copy and is not changed by this project.

## What changed against the email-cli original

The public API is a superset of the original: existing tests compile unchanged
(`pty_send*` now return `int` instead of `void`).

- **Cell-exact comparison of the screen or any rectangle of it**:
  `pty_region_compare()` / `ASSERT_REGION_EQ`, `ASSERT_SCREEN_EQ`, with a
  label for the report and a wildcard for cells a test cannot know (clocks).
- **Reports that explain the difference**, not only locate it: a diff view
  (expected `-`, actual `+`) under a ruler of screen columns, carets under
  every differing cell, context rows, and notes — *moved to row 4*, *moved as
  a block*, *shifted left by 1 column*, *truncated*, *extra text*, *letter
  case*, *a different kind of blank* — with the code points of the first
  differing cell.
- **Style maps**: `pty_region_compare_style()` / `ASSERT_REGION_STYLE` check
  attributes and colours as a picture, one legend character per cell.
- **Waiting for an exact picture**: `pty_wait_region()` /
  `ASSERT_WAIT_SCREEN` / `ASSERT_WAIT_REGION` wait until a region shows the
  expected picture — a half-drawn view or the previous view cannot satisfy
  it — and on a timeout fail with the report of the screen at the deadline.
- **Evidence files**: with `PTYTEST_EVIDENCE_DIR` set, every failed screen
  assertion saves its report, the screen as text and the screen with its
  colours, named `file-function-Lline` (`pty_save_evidence()`).
- **Evidence**: `pty_cell_describe()`, `pty_screen_dump_ansi()` (colours,
  replayable with `pty_feed()`), `pty_region_dump_c()` and
  `PTYTEST_SNAPSHOT=1` (the actual screen as a C literal).
- **Background colours**, 256-colour and direct colour, for foreground and
  background, reported in the form the program selected them.
- **A real parser state machine**: a sequence split across two PTY reads used
  to be dropped and its remainder printed as text; now it is assembled.
  Unknown CSI forms (`CSI > 4;1 m`, `CSI ? … h`) no longer print garbage.
- **Much more of xterm**: scrolling regions, insert/delete line and character,
  alternate screen, saved cursor, tab stops, origin mode, insert mode, DEC line
  drawing, REP, wide (CJK) and combining characters, background colour erase.
  The full list — and what is ignored — is in `libptytest(7)`.
- **Terminal answers**: cursor position reports and device attributes are
  answered, so a program that queries its terminal no longer hangs.
- **Keys** follow the program's application-cursor mode, as xterm does; F1–F12,
  Insert, Delete, Shift-Tab and `PTY_KEY_CTRL(c)` added.
- **A failed exec fails `pty_run()`** with the right `errno`, instead of a
  later, misleading timeout.
- `pty_setenv()` (per-child environment), `pty_resize()` (SIGWINCH),
  `pty_signal()`, `pty_wait_until()`, `pty_feed()` (test a renderer without a
  child), `pty_cursor()`, `pty_screen_text()`, `pty_screen_dump()`.
- The coverage-build close grace period is a runtime setting
  (`pty_set_close_grace()`, `PTYTEST_CLOSE_GRACE_MS`) instead of a compile-time
  `ENABLE_GCOV` of the library.
- `ASSERT_SCREEN_NOT_CONTAINS` fails on a blank screen; failed screen
  assertions print the screen.

## Using it

```c
#include "ptytest.h"
#include "pty_assert.h"

int g_tests_run, g_tests_failed;

static void test_menu(void) {
    PtySession *s = pty_open(30, 8);
    const char *argv[] = { "./tiny-menu", NULL };
    ASSERT(pty_run(s, argv) == 0, "start");
    ASSERT_WAIT_FOR(s, "Up/Down", 3000);
    const char *const screen[] = {
        " Tiny menu", "", " > Open", "   Save", "   Quit", "",
        "Up/Down, Enter, q", NULL };
    ASSERT_SCREEN_EQ(s, screen);

    static const PtyStyleKey legend[] = {
        { '.', PTY_ATTR_NONE, 0, 0 }, { 'R', PTY_ATTR_REVERSE, 0, 0 }, { 0 } };
    const char *const style[] = { ".RRRRRRRR", NULL };
    ASSERT_REGION_STYLE(s, PTY_REGION(2, 0, 1, 12), style, legend);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_EXIT_STATUS(s, 0, 3000);
    pty_close(s);
}
```

Linking:

```sh
cc -o my-test my-test.c $(pkg-config --cflags --libs libptytest)
```

```cmake
find_package(ptytest 0.1 REQUIRED)
target_link_libraries(my-test PRIVATE ptytest::ptytest)   # or ptytest::ptytest_static
```

or vendored, with `add_subdirectory(path/to/libptytest)` (builds only the
library). The complete example is `examples/tiny_menu.c` with its test
`tests/pty/test_tiny_menu.c`.

Documentation: `man 7 libptytest` (architecture, how to write a test, the
emulator's supported and unsupported sequences), `man 3 libptytest` (function
overview) and one page per function group.

## Layout

```
include/        ptytest.h, pty_assert.h          (public)
src/            pty_session.c  PTY, child, input, inspection
                pty_compare.c  region/style comparison, explaining reports
                pty_sync.c     reading, waiting
                pty_screen.c   terminal emulator
                pty_internal.h
man/            man3/*.3, man7/libptytest.7
tests/unit/     emulator and inspection API, in-process
tests/pty/      real programs through a real PTY (with pty_fixture.c)
examples/       a small TUI, its PTY test, and a consumer build
docs/           portability.md: what binds the project to Linux
cmake/          package config, pkg-config template, .deb build metadata
```

## Development

```sh
./manage.sh deps        # build and test dependencies (root)
./manage.sh test        # unit tests, ASAN + UBSan
./manage.sh pty         # PTY tests, ASAN + UBSan
./manage.sh valgrind    # all tests under Valgrind, Release build
./manage.sh coverage    # src/ line coverage: unit alone >= 80%, pty tests >= 80%, all >= 90%
./manage.sh package     # build/packages/: exactly this run's .debs, lintian clean
./manage.sh consumer    # build and run the example against the .deb content
./manage.sh licence     # SPDX header in every file, one licence everywhere
./manage.sh check       # all of the above, with a summary
```

One test: `build/_test/tests/test-screen test_utf8` (substring filter, `-l`
lists the tests).

## License

Copyright (c) 2026 Peter Csaszar (Császár Péter).

libptytest is free software, released under the **GNU General Public License,
version 3 only** (`GPL-3.0-only`): the licence for everyone. The copyright holder
also licenses this work, separately, under proprietary terms to his own
proprietary projects, so every source file carries
`SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Proprietary`; the package
you get from here is the GPL one (`GPL-3.0-only`). The licence text is in
`LICENSE`, and `./manage.sh licence` fails if a file does not carry the tag, or
if `LICENSE`, the Debian copyright file or the package's licence field
disagree.

- The library depends on nothing but the C library (and `libutil`), so no
  third-party licence reaches the code.
- The test harness and examples use no external framework.
- The code started as the PTY test harness of `email-cli`, by the same
  author, and was extracted into this project; it contains no code of other
  authors.

Contributions: see `CONTRIBUTING.md`.
