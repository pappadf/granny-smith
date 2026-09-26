# The checkpoint stream reader, tested as an untrusted parser.  See test.c.
#
# Deliberately no harness: the isolated harness links support/stub_checkpoint.c,
# which defines system_read_checkpoint_data_loc, checkpoint_has_error and friends
# -- the very functions under test.  This suite links the real core/checkpoint.c
# and supplies only the handful of symbols it reaches OUTSIDE its own module.

TEST_NAME    := checkpoint
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert
# The object layer is real so checkpoint.c's class descriptor is validated the
# way it is in the emulator; everything past the module boundary (the cmd_*
# handlers, the gs_* platform hooks, get_build_id) is stubbed in test.c, which
# also lets the test drive the build-ID gate.
EXTRA_SRCS   := ../../../../src/core/checkpoint.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
