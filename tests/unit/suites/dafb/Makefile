# DAFB register-level unit test.
# Links the real dafb.c against the isolated stubs: depth matrix (PCBR0),
# Swatch geometry derivation from the ROM's observed mode-set values,
# AC842 CLUT component phase, DP8531 commit, monitor-sense protocol,
# frame-buffer base/stride decode, and interrupt-clear side effects.

TEST_NAME    := dafb
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := shell memory debugger peripherals assert
STUBS        := display_class
EXTRA_SRCS   := ../../../../src/machines/mcu/dafb.c
include ../../common.mk
