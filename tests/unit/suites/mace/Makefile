# MACE Ethernet core unit test.
# Links the real core/peripherals/mace.c (the Curio Am79C940 the AV, TNT and
# PDM machines share) against recording hooks and drives it the way Apple's
# AV driver (Mace.a) does: reset values and the chip ID, the IAC/PADR/LADRF
# protocol, the self-clearing bits, Curio's one-deep transmit status, the
# three loopback modes with the 79-byte self-test frame, the address filter
# and its LADRF hash, the counters, and a checkpoint round trip.

TEST_NAME    := mace
TEST_SRCS    := test.c
TEST_HARNESS := none
EXTRA_SRCS   := $(wildcard ../../../../src/codec/lib/*.c) \
                ../../../../src/core/peripherals/mace.c
include ../../common.mk
