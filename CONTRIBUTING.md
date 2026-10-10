# Contributing to libptytest

libptytest is released under the GNU General Public License, version 3 only
(`GPL-3.0-only`).

## Before you start

- Open an issue (or a ticket in the project's tracker) and describe the
  defect or the change you want. Small, single-purpose changes are welcome.
- Read `CLAUDE.md` and `man 7 libptytest`: they hold the rules of the project
  (every public function has a man page, the emulator's limits are the tests'
  limits, the ABI is versioned by `SOVERSION`).

## Licence of contributions

Every source file and script begins with the four-line header of
`policies/source-header-model.md`: the SPDX licence tag, the copyright line,
`Created: YYYY-MM-DD` and one line saying what the file is (after the shebang
on a script). `./manage.sh licence` fails on a file without it.

The project's copyright is held entirely by its author, so that the code can
keep one licensing position. **A contribution is accepted only when you state,
in the pull request, that you assign the copyright of it to the project's
author, or license it to the author under terms that allow relicensing**
(for example an unrestricted licence grant). Without that statement a
contribution cannot be merged. Do not include code you did not write or whose
licence is not GPL-3.0-only compatible.

## Checks

```sh
./manage.sh check     # every gate, including licence, lintian and the coverage floors
```

A change that is not covered by a test which can fail is not finished.
