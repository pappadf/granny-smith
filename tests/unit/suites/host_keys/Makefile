# The one host keyboard path: DOM codes -> ADB raw codes, the held-key set,
# and every key a Lisa has surviving the trip.  See test.c.

TEST_NAME    := host_keys
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/host_keys.c \
                ../../../../src/machines/lisa/lisa_keymap.c
include ../../common.mk
