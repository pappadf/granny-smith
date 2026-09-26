# RTC / PRAM unit test.
# Links the real rtc.c against recording stubs and pins the write-protect law
# on BOTH addressing windows, the extended-command address decode, and the
# per-variant legacy PRAM group mapping.

TEST_NAME    := rtc
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert checkpoint
EXTRA_SRCS   := ../../../../src/core/peripherals/rtc.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
