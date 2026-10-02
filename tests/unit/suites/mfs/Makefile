# MFS (image_mfs.c) over volumes built by the test.  See test.c.
TEST_NAME := mfs
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/storage/image_mfs.c \
              ../../../../src/core/storage/macroman.c
USE_SOURCE_LAYER := 1
include ../../common.mk
