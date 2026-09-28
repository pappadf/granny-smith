# The ADB keyboard queue's overflow policy.
#
# Links the real adb.c and drains the queue the way a guest does -- Talk
# Register 0 through the same dispatch the IOP path uses -- so the policy is
# exercised rather than re-implemented.  test.c stubs the object layer and
# the rest of the machine itself; adb.c needs the job seam since the
# execution-model rework, so that is linked for real.
#
# -Wno-unused-parameter: the unit harness replaces LOG() with a macro that
# discards its arguments, so parameters a function uses only in a log line
# look unused in THIS build alone.

TEST_NAME    := adb_kbdqueue
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert checkpoint
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/peripherals/adb.c
EXTRA_CFLAGS := -Wno-unused-parameter
include ../../common.mk
