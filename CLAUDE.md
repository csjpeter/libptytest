# CLAUDE.md

Guidance for Claude Code working in this repository.

## Policies first

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

`requirements/`, if it exists, is human-authored and never edited by an agent.

## Commands

```bash
./manage.sh test       # unit tests (ASAN + UBSan) — run most often
./manage.sh pty        # PTY tests (ASAN + UBSan)
./manage.sh check      # every gate: unit, pty, valgrind, coverage, package, lintian, consumer
build/_test/tests/test-screen <substring>   # one test
```

## Architecture

- `include/ptytest.h` — the whole public API; `include/pty_assert.h` — macros.
- `src/pty_session.c` — PTY + child lifecycle, input, screen inspection.
- `src/pty_sync.c` — reading the master, every wait (condition + deadline).
- `src/pty_screen.c` — the terminal emulator (byte state machine, cell grid).
- The shared library exports only `PTYTEST_API` symbols (hidden visibility);
  unit tests link the static library to reach `pty_internal.h`.

## Rules specific to this project

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
- Source, comments and documentation are in English.
- C11, no external test framework, no dependencies beyond libc (+ libutil).
