#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The C compiler for the vendored IMBE tests (third_party/imbe/tools/test_c.py, `make codec-test`). Those files are
# pinned (tools/sources.py verify-vendored) and build with -Werror; GCC 15 warns on them (-Wsign-compare,
# -Wenum-compare in tests/test_*.c), which -Werror turns into errors. They stay warnings here, still printed, for
# these tests only; everything of ours keeps -Werror.
exec "${VENDORED_CC:-cc}" "$@" -Wno-error
