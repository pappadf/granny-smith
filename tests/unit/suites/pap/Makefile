# The PAP server (appletalk_printer.c) without the interpreter: ATP, NBP and
# the clock are stubbed in test.c, and the platform's capture sink is the
# test's own.  PRINTER_CAPTURE_MAX is small here so the job-too-large path is
# cheap to reach.
TEST_NAME := pap
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/network/appletalk_printer.c \
              ../../../../src/core/network/laserwriter_job.c
EXTRA_CFLAGS := -DPRINTER_CAPTURE_MAX=64
include ../../common.mk
