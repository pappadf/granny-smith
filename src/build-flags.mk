# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# build-flags.mk — the C dialect, warnings and feature macros every build
# shares: Makefile (wasm), Makefile.headless and tests/unit/common.mk.  One
# copy, so the unit tier compiles against the declarations the product does.

# -Wall -Wextra on every C build, minus one: -Wmissing-field-initializers
# objects to positional initializers that stop before a struct's last
# fields, and the I/O range and display timing tables are written that way
# on purpose (mac030_io_range_t keeps its optional fields last "so
# positional initializers stay valid").  WERROR=1 (set in CI) makes every
# warning an error; it applies to compilation only, never to the link.
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers
WERROR ?= 0
ifeq ($(WERROR),1)
WARN_CFLAGS += -Werror
endif

# The host builds' feature macros (headless and the unit tier; the wasm
# build takes Emscripten's defaults): without _GNU_SOURCE glibc hides
# strptime and the like, and _DARWIN_C_SOURCE does the same on macOS.
FEATURE_CFLAGS := -D_GNU_SOURCE -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L
