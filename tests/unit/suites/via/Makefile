# VIA control-line unit test.
# Links the real via.c against recording stubs and pins the PCR CA2/CB2 mode
# field: which edge is active in each of the four input modes, and which of
# those modes leave the IFR flag standing across an ORA/ORB access.

TEST_NAME    := via
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := checkpoint assert
EXTRA_SRCS   := ../../../../src/core/peripherals/via.c
include ../../common.mk
