# Functional test coverage of libptytest

<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (c) 2026 Peter Csaszar (Császár Péter) -->
<!-- Created: 2026-10-16 -->
<!-- Purpose: map of every capability of the library to the test that drives it from outside. -->

Policy: `policies/testing-model.md`, "Every capability has a functional test".

libptytest is a library, so it has no CLI subcommands.  Its capabilities are
what it exports: every `PTYTEST_API` function of `include/ptytest.h` and every
assertion macro of `include/pty_assert.h`.  A test counts as functional when it
uses only the public interface of the shared library, a real child process and
a real pseudo-terminal: the suites `tests/functional/` and `tests/pty/`.  The
unit tests (`tests/unit/`) reach the internals and do not count here.

`tools/check-functional-coverage.sh` (run by `./manage.sh functional-coverage`,
part of `./manage.sh check`) lists the exported functions and macros from the
headers and fails when one has no row, when a row names a test function that
does not exist, or when a row is a gap without a ticket.  A new capability is
added with its row and its functional test in the same commit.

The emulated escape sequences (EMULATOR section of `man/man7/libptytest.7`) are
covered as a group by `tests/pty/test_render.c`, one `test_*` per family, each
ending in a cell-exact comparison of a real child's screen.

Row format: `` `symbol` | `file::test_function` ``.  Gaps, if any, read
`` `symbol` | GAP PTY-N ``.

| Capability | Functional test |
|---|---|
| `pty_version` | `tests/functional/test_api.c::test_version` |
| `pty_open` | `tests/pty/test_session.c::test_echo` |
| `pty_setenv` | `tests/pty/test_session.c::test_setenv_override_and_unset` |
| `pty_run` | `tests/pty/test_session.c::test_missing_program_fails_at_run` |
| `pty_close` | `tests/pty/test_session.c::test_close_without_grace_terminates` |
| `pty_set_close_grace` | `tests/pty/test_session.c::test_close_grace_lets_child_finish` |
| `pty_resize` | `tests/pty/test_session.c::test_resize_delivers_sigwinch` |
| `pty_signal` | `tests/pty/test_session.c::test_wait_exit_killed_by_signal` |
| `pty_get_size` | `tests/pty/test_session.c::test_resize_delivers_sigwinch` |
| `pty_send` | `tests/pty/test_session.c::test_large_input_does_not_deadlock` |
| `pty_send_key` | `tests/pty/test_session.c::test_cat_interactive` |
| `pty_send_str` | `tests/pty/test_session.c::test_cat_interactive` |
| `pty_wait_for` | `tests/pty/test_session.c::test_echo` |
| `pty_wait_until` | `tests/pty/test_session.c::test_cat_interactive` |
| `pty_settle` | `tests/pty/test_session.c::test_cat_interactive` |
| `pty_wait_exit` | `tests/pty/test_session.c::test_wait_exit_status` |
| `pty_drain` | `tests/functional/test_api.c::test_drain` |
| `pty_feed` | `tests/pty/test_render.c::test_attributes_and_colours` |
| `pty_cell_text` | `tests/functional/test_api.c::test_text_inspection` |
| `pty_cell_attr` | `tests/pty/test_render.c::test_attributes_and_colours` |
| `pty_cell_fg` | `tests/pty/test_session.c::test_full_screen_frame` |
| `pty_cell_bg` | `tests/pty/test_session.c::test_full_screen_frame` |
| `pty_cursor` | `tests/pty/test_session.c::test_full_screen_frame` |
| `pty_row_text` | `tests/functional/test_api.c::test_text_inspection` |
| `pty_row_contains` | `tests/functional/test_api.c::test_text_inspection` |
| `pty_screen_contains` | `tests/functional/test_api.c::test_text_inspection` |
| `pty_screen_is_blank` | `tests/functional/test_api.c::test_screen_is_blank` |
| `pty_screen_text` | `tests/pty/test_render.c::test_evidence_of_a_real_screen` |
| `pty_region_compare` | `tests/pty/test_render.c::test_report_on_a_real_screen` |
| `pty_screen_compare` | `tests/functional/test_api.c::test_screen_compare` |
| `pty_region_compare_style` | `tests/pty/test_render.c::test_style_report_on_a_real_screen` |
| `pty_region_style_map` | `tests/pty/test_render.c::test_style_report_on_a_real_screen` |
| `pty_cell_describe` | `tests/pty/test_render.c::test_style_report_on_a_real_screen` |
| `pty_region_dump_c` | `tests/pty/test_render.c::test_evidence_of_a_real_screen` |
| `pty_screen_dump_ansi` | `tests/functional/test_api.c::test_dump_ansi_roundtrip` |
| `pty_wait_region` | `tests/pty/test_session.c::test_wait_region_times_out_with_report` |
| `pty_wait_region_style` | `tests/functional/test_api.c::test_wait_region_style` |
| `pty_save_evidence` | `tests/pty/test_render.c::test_evidence_of_a_real_screen` |
| `pty_screen_dump` | `tests/pty/test_session.c::test_wait_region_times_out_with_report` |
| `pty_trace_enable` | `tests/pty/test_session.c::test_trace` |
| `ASSERT` | `tests/pty/test_session.c::test_echo` |
| `ASSERT_ROW_CONTAINS` | `tests/pty/test_session.c::test_echo` |
| `ASSERT_CELL_ATTR` | `tests/functional/test_api.c::test_assert_cell_attr_flag` |
| `ASSERT_CELL_ATTR_EQ` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_CELL_TEXT` | `tests/pty/test_render.c::test_unicode` |
| `ASSERT_CELL_FG` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_CELL_BG` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_CURSOR` | `tests/pty/test_render.c::test_cursor_and_erase` |
| `ASSERT_SCREEN_CONTAINS` | `tests/pty/test_session.c::test_echo` |
| `ASSERT_SCREEN_NOT_CONTAINS` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_REGION_EQ` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_SCREEN_EQ` | `tests/pty/test_render.c::test_cursor_and_erase` |
| `ASSERT_REGION_STYLE` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_WAIT_REGION` | `tests/pty/test_session.c::test_wait_region_times_out_with_report` |
| `ASSERT_WAIT_SCREEN` | `tests/pty/test_session.c::test_full_screen_frame` |
| `ASSERT_WAIT_REGION_STYLE` | `tests/pty/test_session.c::test_wait_region_times_out_with_report` |
| `ASSERT_WAIT_FOR` | `tests/pty/test_session.c::test_echo` |
| `ASSERT_EXIT_STATUS` | `tests/pty/test_session.c::test_wait_exit_status` |
