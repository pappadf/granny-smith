# The shared audio output path (src/core/peripherals/audio_out.c): the DC
# blocker that models the machines' AC-coupled output stage, and where it
# applies -- the host stream and the recorder's sink coupled, each with its
# own state, the golden capture raw.  See test.c.
#
# No emulator harness: test.c stubs the platform sink and the object-model
# calls audio_out.c makes; value.c is the real one.

TEST_NAME    := audio_out
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/peripherals/audio_out.c ../../../../src/core/object/value.c
include ../../common.mk
