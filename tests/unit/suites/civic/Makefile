# CIVIC serial-register + sense unit test.
# Links the real av/civic.c against recording stubs and pins the bit-serial
# register codec (one bit per longword, LSB ascending on write / MSB
# descending on read), the five direct-longword registers, the inverted
# and computed slots, the monitor-sense drive/read protocol, the Sebastian
# CLUT quad protocol with auto-increment, and the VBL ack dance.

TEST_NAME    := civic
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := memory checkpoint display_class
EXTRA_SRCS   := ../../../../src/machines/av/civic.c
include ../../common.mk
