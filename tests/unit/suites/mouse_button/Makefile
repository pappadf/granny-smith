# The quadrature mouse's button hold.  See test.c.

TEST_NAME    := mouse_button
TEST_SRCS    := test.c
TEST_HARNESS := none
# The test supplies its own link stubs (scheduler, VIA, SCC, checkpoint I/O);
# from support/ it takes only the machine-object and assert stubs.
STUBS        := machine_object assert
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/peripherals/mouse.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
