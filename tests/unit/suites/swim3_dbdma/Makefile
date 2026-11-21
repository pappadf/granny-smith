# SWIM3-over-DBDMA port unit test.
# Links the shared machines/runtime/swim3_dbdma.c (the byte ring and DBDMA
# device port Grand Central and Heathrow both feed the floppy through)
# against the real core/peripherals/dbdma.c and array-backed memory, with the
# SWIM3 chip reduced to its backend (swim3_bind is stubbed in the test).
# Pins: a write's leftover OUTPUT bytes are dropped when the driver stops
# the channel (the MacTest Pro seek-test bug), the read path, the underrun
# answer, and IRQ forwarding.

TEST_NAME    := swim3_dbdma
TEST_SRCS    := test.c
TEST_HARNESS := none
EXTRA_SRCS   := ../../../../src/core/peripherals/dma_mem.c \
                ../../../../src/core/peripherals/dbdma.c \
                ../../../../src/machines/runtime/swim3_dbdma.c
include ../../common.mk
