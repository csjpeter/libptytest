# Portability: what binds libptytest to Linux

The target platform is **Linux** (Debian). Nothing here is released for,
tested on or supported on another platform (`policies/platform-scope-model.md`).
This file is a list of what would have to be resolved for another platform
and roughly how much work each item is. It is not a plan.

## In the library

| Dependency | Where | Would need |
|---|---|---|
| `forkpty()` from `<pty.h>`, linked from `libutil` | `src/pty_session.c` (`pty_run()`), `CMakeLists.txt` (`util`), `cmake/libptytest.pc.in` | The header and library differ per OS (`<util.h>` and no extra library on macOS, `<libutil.h>` on FreeBSD); on Windows there is no equivalent short of ConPTY and a different child-start model. Small on a BSD, a rewrite of the session module on Windows. |
| A kernel PTY with `TIOCSWINSZ` and `SIGWINCH` delivered to the child's process group | `src/pty_session.c` (`pty_resize()`, `pty_signal()`) | The same on the BSDs and macOS; on Windows ConPTY's resize call. Small. |
| `poll()` on the master descriptor, non-blocking reads, `CLOCK_MONOTONIC` deadlines | `src/pty_sync.c` | POSIX; present on the BSDs and macOS. On Windows the wait loop is rewritten on top of overlapped I/O. |
| Child-start failure reported through a `FD_CLOEXEC` pipe, `fork`/`exec`, `waitpid`, process groups and `kill()` | `src/pty_session.c` | POSIX; present on the BSDs and macOS. Not on Windows (job objects and `CreateProcess` instead). |
| `newlocale()`/`uselocale()` with the `C.UTF-8` locale, falling back to `en_US.UTF-8`, to get glibc's `wcwidth()` | `src/pty_screen.c` | The locale name and the width tables differ per C library. The emulator must agree with the program under test, so the program under test and the library would have to be measured against the same C library. Small, but the expected widths of the tests change. |
| Hidden symbol visibility by `__attribute__((visibility("default")))` (GCC, Clang) | `include/ptytest.h`, `CMakeLists.txt` | A `__declspec(dllexport)` branch on Windows; nothing on the BSDs and macOS. |
| `_XOPEN_SOURCE 700` feature macro | `src/pty_*.c` | Same on the BSDs; macOS needs `_DARWIN_C_SOURCE` for some of the calls. |

## In the build, test and package

| Dependency | Where | Would need |
|---|---|---|
| The Debian package: CPack DEB, `lintian`, `dpkg`, `apt` | `CMakeLists.txt`, `cmake/deb-post-build.cmake`, `manage.sh package`, `debian/` | None: the build and release host is Linux regardless. |
| ASAN/UBSan, `gcov`/`lcov`, Valgrind | `manage.sh test`, `coverage`, `valgrind` | None: the build and test host is Linux regardless. |
| `apt-get`/`dnf` for the build dependencies | `manage.sh deps` | None: Linux build host. |
| `bash`, `timeout`, GNU `sed`/`grep`/`find` options | `manage.sh`, `tools/*.sh` | None: Linux build host. |

## In the tests

| Dependency | Where | Would need |
|---|---|---|
| The tests drive real programs through the PTY and compare their cells; the helper program and the example use the same terminal calls as above | `tests/pty/`, `examples/` | The same as the library: the PTY session module. |
| The emulator tests assert glibc's character widths | `tests/unit/test_screen.c` | See the `wcwidth` row. |

## Reviewed `#ifdef`s

- `include/ptytest.h`: `__cplusplus` (a C header used from C++) and
  `__GNUC__` (the visibility attribute) are language and compiler guards, not
  platform branches.
- `src/pty_session.c`: the former `__APPLE__` include branch was **removed**:
  it was never built or tested, which makes it a claim without a test
  (`policies/platform-scope-model.md`, "What is deliberately not done").
