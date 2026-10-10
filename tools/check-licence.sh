#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Proprietary
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-10
# Checks the source-file headers and that every licence declaration agrees.
#
# Header (policies/source-header-model.md), after an optional shebang line,
# four consecutive comment lines:
#
#   SPDX-License-Identifier: GPL-3.0-only
#   Copyright (c) <year> Peter Csaszar (Császár Péter)
#   Created: <YYYY-MM-DD, a valid date, not later than today>
#   <one line: what the file is>
#
# Applies to every tracked file except LICENSE, .gitignore, Markdown documents
# and man pages (policies/source-header-model.md, "Where it applies").
#
# Declarations: LICENSE is the GNU GPL version 3 text; the Debian copyright
# file in CMakeLists.txt, the RPM licence field and README.md say the same
# licence as the headers (policies/licensing-model.md, "One declaration").
#
# Usage: check-licence.sh [<project-dir>]
#        check-licence.sh --self-test
set -euo pipefail

TAG="GPL-3.0-only OR LicenseRef-Proprietary"
DUAL_SENTENCE="The copyright holder also licenses this work, separately, under proprietary terms."
HOLDER='Peter Csaszar \(Császár Péter\)'
# Declarations in CMakeLists.txt and README.md.
DEB_LICENCE="GPL-3"
RPM_LICENCE="GPL-3.0-only"
README_LICENCE="GPL-3.0-only"

# header_problem <file>: prints why the header of <file> is wrong, if it is.
header_problem() {
    local f="$1" today lines line n=0 created
    local -a h
    today="$(date +%F)"
    mapfile -t lines < <(head -n 6 "$f")
    n=0
    if [ "${#lines[@]}" -gt 0 ] && [ "${lines[0]:0:2}" = "#!" ]; then
        n=1
    fi
    h=("${lines[@]:n:4}")

    local i
    for i in "${!lines[@]}"; do
        if [ "$i" -gt 0 ] && [ "${lines[$i]:0:2}" = "#!" ]; then
            echo "the shebang is on line $((i + 1)), not on line 1"
            return
        fi
    done

    local c='(/\*|#)'
    local e='( \*/)?'
    local re_tag="^${c} SPDX-License-Identifier: ${TAG}${e}\$"
    if ! [[ "${h[0]:-}" =~ $re_tag ]]; then
        echo "line $((n + 1)) is not \"SPDX-License-Identifier: $TAG\""
        return
    fi
    if ! [[ "${h[1]:-}" =~ ^${c}\ Copyright\ \(c\)\ [0-9]{4}\ ${HOLDER}${e}$ ]]; then
        echo "line $((n + 2)) is not \"Copyright (c) <year> Peter Csaszar (Császár Péter)\""
        return
    fi
    if ! [[ "${h[2]:-}" =~ ^${c}\ Created:\ ([0-9]{4}-[0-9]{2}-[0-9]{2})${e}$ ]]; then
        echo "line $((n + 3)) is not \"Created: YYYY-MM-DD\""
        return
    fi
    created="${BASH_REMATCH[2]}"
    if [ "$(date -d "$created" +%F 2> /dev/null)" != "$created" ]; then
        echo "Created: $created is not a valid date"
        return
    fi
    if [[ "$created" > "$today" ]]; then
        echo "Created: $created is later than today ($today)"
        return
    fi
    if ! [[ "${h[3]:-}" =~ ^${c}\ [^[:space:]].*$ ]] \
        || [[ "${h[3]:-}" =~ SPDX-License-Identifier|Copyright|Created: ]]; then
        echo "line $((n + 4)) is not a purpose line"
        return
    fi
    for line in "${h[@]}"; do
        if [[ "$line" == *@* ]]; then
            echo "the header contains an e-mail address"
            return
        fi
    done
}

# check <dir>: prints the problems, returns their count.
check() {
    local dir="$1" problems=0 f why
    local -a files

    if ! git -C "$dir" rev-parse --is-inside-work-tree > /dev/null; then
        echo "not a git work tree: $dir"
        return 1
    fi
    mapfile -t files < <(git -C "$dir" ls-files)

    for f in "${files[@]}"; do
        case "$f" in
            LICENSE|.gitignore|*.md|man/*) continue ;;
        esac
        [ -f "$dir/$f" ] || continue
        why="$(header_problem "$dir/$f")"
        if [ -n "$why" ]; then
            echo "$f: $why"
            problems=$((problems + 1))
        fi
    done

    if ! tr '\n' ' ' < "$dir/LICENSE" 2> /dev/null | tr -s ' ' | grep -qF "$DUAL_SENTENCE"; then
        echo "LICENSE does not carry: $DUAL_SENTENCE"
        problems=$((problems + 1))
    fi
    if ! tr '\n' ' ' < "$dir/CMakeLists.txt" 2> /dev/null | tr -s ' ' | grep -qF "$DUAL_SENTENCE"; then
        echo "CMakeLists.txt: the Debian copyright file does not carry the dual-licence sentence"
        problems=$((problems + 1))
    fi
    if ! grep -qF "Version 3, 29 June 2007" "$dir/LICENSE" 2> /dev/null \
        || ! grep -qF "GNU GENERAL PUBLIC LICENSE" "$dir/LICENSE"; then
        echo "LICENSE is not the GNU GPL version 3 text"
        problems=$((problems + 1))
    fi
    if ! grep -qx "License: ${DEB_LICENCE}" "$dir/CMakeLists.txt" 2> /dev/null; then
        echo "CMakeLists.txt: the Debian copyright file does not say \"License: $DEB_LICENCE\""
        problems=$((problems + 1))
    fi
    if ! grep -qF "CPACK_RPM_PACKAGE_LICENSE \"${RPM_LICENCE}\"" "$dir/CMakeLists.txt" 2> /dev/null; then
        echo "CMakeLists.txt: CPACK_RPM_PACKAGE_LICENSE is not \"$RPM_LICENCE\""
        problems=$((problems + 1))
    fi
    if ! grep -qF "$README_LICENCE" "$dir/README.md" 2> /dev/null; then
        echo "README.md does not name $README_LICENCE"
        problems=$((problems + 1))
    fi

    # Nothing but the licence text itself may say "or later".
    local hit
    while IFS= read -r hit; do
        echo "says \"or later\": $hit"
        problems=$((problems + 1))
    done < <(git -C "$dir" grep -n -i -E '(^|[^[:alnum:]])or[- ]later|any later version' -- . \
        ':!LICENSE' ':!tools/check-licence.sh')
    return "$problems"
}

# A correct tree in $tmp, and mutations of it that the check must find.
good_header() {
    printf '# SPDX-License-Identifier: %s\n# Copyright (c) 2026 Peter Csaszar (Császár Péter)\n# Created: 2026-10-03\n# A file of the self-test.\n' "$TAG"
}

good_cmake() {
    good_header
    printf 'License: %s\n %s\nset(CPACK_RPM_PACKAGE_LICENSE "%s")\n' "$DEB_LICENCE" "$DUAL_SENTENCE" "$RPM_LICENCE"
}

self_test() {
    tmp="$(mktemp -d "/tmp/check-licence-$$-XXXXXX")"
    case "$tmp" in /tmp/check-licence-*) ;; *) echo "ERROR: unsafe path: '$tmp'" >&2; exit 1 ;; esac
    trap 'rm -f "$tmp/LICENSE" "$tmp/CMakeLists.txt" "$tmp/README.md" "$tmp/a.sh"; rm -rf "$tmp/.git"; rmdir "$tmp"' EXIT

    git -C "$tmp" init -q
    printf '%s\nGNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n' "$DUAL_SENTENCE" > "$tmp/LICENSE"
    printf '%s\n' "$README_LICENCE" > "$tmp/README.md"
    good_cmake > "$tmp/CMakeLists.txt"
    { printf '#!/bin/sh\n'; good_header; printf 'exit 0\n'; } > "$tmp/a.sh"
    git -C "$tmp" add -A

    local rc=0
    check "$tmp" > /dev/null || rc=$?
    [ "$rc" -eq 0 ] || { check "$tmp"; echo "self-test: a correct tree was rejected"; return 1; }

    # expect_fail <what> : the tree as it is now must be rejected.
    expect_fail() {
        rc=0
        check "$tmp" > /dev/null || rc=$?
        [ "$rc" -ne 0 ] || { echo "self-test: passed although $1"; return 1; }
    }
    set_script() { { printf '#!/bin/sh\n'; printf '%s\n' "$@"; printf 'exit 0\n'; } > "$tmp/a.sh"; }

    printf '#!/bin/sh\nexit 0\n' > "$tmp/a.sh"
    expect_fail "a file has no header"
    set_script "# SPDX-License-Identifier: GPL-3.0-or-later" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Created: 2026-10-03" "# Purpose."
    expect_fail "the tag says or-later"
    set_script "# SPDX-License-Identifier: GPL-3.0-only" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Created: 2026-10-03" "# Purpose."
    expect_fail "the tag lacks the dual licence"
    set_script "# SPDX-License-Identifier: $TAG" "# Copyright (c) 2026 Somebody Else" "# Created: 2026-10-03" "# Purpose."
    expect_fail "the copyright holder is wrong"
    set_script "# SPDX-License-Identifier: $TAG" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Purpose."
    expect_fail "there is no Created line"
    set_script "# SPDX-License-Identifier: $TAG" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Created: 2026-13-45" "# Purpose."
    expect_fail "the date is not a date"
    set_script "# SPDX-License-Identifier: $TAG" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Created: 2999-01-01" "# Purpose."
    expect_fail "the date is in the future"
    set_script "# SPDX-License-Identifier: $TAG" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Created: 2026-10-03"
    expect_fail "there is no purpose line"
    set_script "# SPDX-License-Identifier: $TAG" "# Copyright (c) 2026 Peter Csaszar (Császár Péter)" "# Created: 2026-10-03" "# Mail me at someone@example.org"
    expect_fail "the header holds an e-mail address"
    { good_header; printf '#!/bin/sh\n'; } > "$tmp/a.sh"
    expect_fail "the shebang is not on line 1 (header first)"
    { printf '#!/bin/sh\n'; good_header; } > "$tmp/a.sh"

    good_cmake | grep -v "$DUAL_SENTENCE" > "$tmp/CMakeLists.txt"
    expect_fail "the Debian copyright file lacks the dual-licence sentence"
    good_cmake | sed 's/^License: .*/License: MIT/' > "$tmp/CMakeLists.txt"
    expect_fail "the Debian licence is wrong"
    good_cmake | sed "s/\"$RPM_LICENCE\"/\"MIT\"/" > "$tmp/CMakeLists.txt"
    expect_fail "the RPM licence is wrong"
    good_cmake > "$tmp/CMakeLists.txt"
    printf 'GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n' > "$tmp/LICENSE"
    expect_fail "LICENSE lacks the dual-licence sentence"
    printf 'MIT\n' > "$tmp/LICENSE"
    expect_fail "LICENSE is not the GPL"
    printf '%s\nGNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n' "$DUAL_SENTENCE" > "$tmp/LICENSE"
    printf '%s, or (at your option) any later version\n' "$README_LICENCE" > "$tmp/README.md"
    git -C "$tmp" add -A
    expect_fail "the README says \"any later version\""
    echo "check-licence self-test: ok"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rc=0
check "${1:-$ROOT}" || rc=$?
if [ "$rc" -eq 0 ]; then
    echo "licence: every header says $TAG and the declarations agree"
else
    echo "licence: $rc problem(s)"
fi
exit "$rc"
