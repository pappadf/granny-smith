# SCC guest- and host-reachable inputs that used to abort.  See test.c.

TEST_NAME    := scc_bad_input
TEST_SRCS    := test.c
# The test supplies its own checkpoint stubs; the support stubs linked in are
# stub_machine_object and stub_assert.
TEST_HARNESS := none
STUBS        := machine_object assert
EXTRA_SRCS   := ../../../../src/core/peripherals/scc.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
