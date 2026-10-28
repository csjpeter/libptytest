# CLAUDE.md

Guidance for Claude Code working in this repository.

## Policies first

Index of every policy: `../policies/README.md`. Change flow: `../policies/change-flow-model.md` (a ticket, a branch named exactly after its key, the key in each commit message; a finished ticket goes to `READY` and the owner closes). Never write into `policies/`; file a ticket under the owning key instead.

This project follows the workspace policies in `../policies/` — read them
before changing build, test or packaging behaviour, and re-read them
regularly: they change. They are never edited from here (see
`policies/project-boundaries-model.md`); a disagreement is reported to the
human.

- `manage-sh-model.md` — the `manage.sh` verbs and what they mean
- `testing-model.md` — test kinds, "a test must be able to fail", floors,
  timeouts, no suppressed errors
- `packaging-and-release-model.md` — CPack only, one version source,
  lintian clean, man pages, `umask 022`
- `project-boundaries-model.md` — write only inside this directory; working
  files under `build/`

## Commands

```bash
./manage.sh test       # unit tests (ASAN + UBSan) — run most often
./manage.sh pty        # PTY tests (ASAN + UBSan)
./manage.sh functional # functional tests: public API, real child, real PTY
./manage.sh functional-coverage # every exported function/macro has a row in docs/functional-coverage.md
./manage.sh result-format # self-test of the suite runner and the result file, then build/result/*.result and build/coverage/{unit,external}
./manage.sh check      # every gate: licence, private-names, test-bins, version, tag-build-test, timing-log, unit, pty, valgrind, coverage, result-format, package (+lintian), consumer, package-output
build/_test/tests/unit/bin/test_screen <substring>   # one test
```

`test`, `functional`, `pty`, `valgrind` and `coverage` run the built suites
themselves (`run_suites` in `manage.sh`, no ctest) and write
`build/result/<verb>.result` (policies/manage-sh-model.md); `test -R REGEX`
selects suites. `coverage` leaves its instrumented trees and counters in
`build/coverage/unit` and `build/coverage/external`. `tools/check-result.sh`
holds the self-test.

Tests are declared one per line in `tests/CMakeLists.txt` as
`project_test(<category> <name> SOURCES ...)` and built into
`build/tests/<category>/bin/<name>`; helpers go to `.../helpers/`.
`tools/check-test-bins.sh` fails a build where the two sets differ
(policies/testing-model.md).

## Architecture

- `include/ptytest.h` — the whole public API; `include/pty_assert.h` — macros.
- `src/pty_session.c` — PTY + child lifecycle, input, screen inspection.
- `src/pty_sync.c` — reading the master, every wait (condition + deadline).
- `src/pty_screen.c` — the terminal emulator (byte state machine, cell grid).
- `src/pty_compare.c` — region text/style comparison and the explaining report.
  A new kind of difference gets a note there, a unit test in
  `tests/unit/test_inspect.c`, and a line in `pty_region_compare.3`.
- The shared library exports only `PTYTEST_API` symbols (hidden visibility);
  unit tests link the static library to reach `pty_internal.h`.

## Rules specific to this project

- **Every public function and assertion macro has a row in
  `docs/functional-coverage.md`** naming the functional test that drives it
  (`tests/functional` or `tests/pty`); `tools/check-functional-coverage.sh`
  fails otherwise. A new capability brings its row and test in the same commit.

- **The emulator's limits are the tests' limits.** Any sequence added to or
  dropped from `pty_screen.c` is added to or dropped from the EMULATOR section
  of `man/man7/libptytest.7`, with a unit test in `tests/unit/test_screen.c`
  that fails if it is mis-handled.
- **Every public function has a man page**, and the page matches the header.
  A new function goes into `man/man3/`, `libptytest.3` and the alias list in
  `CMakeLists.txt`.
- The ABI is versioned by `SOVERSION` (= major version). Removing or changing
  an exported function's signature is a major version bump and a package
  rename (`libptytest0` → `libptytest1`).
- **Every source file and script has the four-line header** (SPDX tag,
  copyright, `Created:` date, purpose); `./manage.sh licence` checks it.
- Source, comments and documentation are in English.
- C11, no external test framework, no dependencies beyond libc (+ libutil).
