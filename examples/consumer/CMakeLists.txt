# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Péter Császár <peter@csaszar.email>
# Builds the example the way another project would: against an installed
# libptytest, found with find_package().  "./manage.sh consumer" points
# CMAKE_PREFIX_PATH at the content of the built .deb packages, so what is
# checked is the package itself, not the build tree.
cmake_minimum_required(VERSION 3.20)
project(ptytest-consumer LANGUAGES C)

find_package(ptytest 0.1 REQUIRED)

add_executable(tiny-menu ../tiny_menu.c)
add_executable(test-tiny-menu ../../tests/pty/test_tiny_menu.c)
target_link_libraries(test-tiny-menu PRIVATE ptytest::ptytest)
