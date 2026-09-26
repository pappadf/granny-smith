# SCC checkpoint round-trip.  See test.c for what this caught.

TEST_NAME    := scc_checkpoint
TEST_SRCS    := test.c
TEST_HARNESS := none
# The test supplies a recording checkpoint stream and its own link stubs;
# from support/ it takes only the machine-object and assert stubs.
STUBS        := machine_object assert
EXTRA_SRCS   := ../../../../src/core/peripherals/scc.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
