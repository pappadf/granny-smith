# The 8*24 GC's VidComm mode change (display_card_824gc.c gc_vidcomm) against
# geometry that decodes but does not fit its 2 MB DRAM.  test.c includes the
# card source to reach the static handler, links its drawing engine and the
# value layer, and stubs the NuBus, ROM and object plumbing.  Natively under
# ASan + UBSan: the defect was the renderer reading, and the engine writing,
# past the DRAM.

TEST_NAME     := gc824_vidcomm
TEST_SRCS     := test.c
TEST_HARNESS  := none
STUBS         := checkpoint assert
SANITIZE      := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
EXTRA_LDFLAGS := -lpthread
RUN_ENV       := ASAN_OPTIONS=detect_leaks=1
EXTRA_SRCS    := ../../../../src/core/peripherals/nubus/cards/display_card_824gc_qd.c \
                 ../../../../src/core/object/value.c
include ../../common.mk
