# common.h -- the header-only helpers every subsystem shares (byte order,
# four-character codes, array length, FNV-1a, ...).
#
# Header-only, so there is nothing to link.  The values pinned here are the
# published ones: FNV-1a results reach files (checkpoint block tags, the AFP
# remote cache names), so the helpers must be the standard algorithm.
TEST_NAME := common
TEST_SRCS := test.c
TEST_HARNESS := isolated
include ../../common.mk
