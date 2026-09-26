# ADB: the ST-transition filter that used to live in four machine files.  See test.c.

TEST_NAME    := adb
TEST_SRCS    := test.c
# The test supplies its own scheduler, VIA and checkpoint-stream mocks, so the
# only support stubs linked in are the machine object and assert.
TEST_HARNESS := none
STUBS        := machine_object assert
EXTRA_SRCS   := ../../../../src/core/peripherals/adb.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
