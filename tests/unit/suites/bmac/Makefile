# BMAC Ethernet cell unit test.
# Links the real core/peripherals/bmac.c and dbdma.c against array-backed
# guest memory and drives them the way the drivers do: the serial EEPROM
# read protocol (Apple's and Open Firmware's station-address source), the
# register semantics the init sequences depend on (TXRST self-clearing,
# STATUS read-to-clear, no PHY on MIFCSR, the XCVRIF link bit), a transmit
# program waiting on S5, receive descriptors ended early at a frame's end
# with S6 set and the status trailer after the FCS, the address and hash
# filters, the drivers' self-addressed warm-up frame, and loopback.

TEST_NAME    := bmac
TEST_SRCS    := test.c
TEST_HARNESS := none
EXTRA_SRCS   := ../../../../src/core/crc32.c \
                ../../../../src/core/peripherals/dma_mem.c \
                ../../../../src/core/peripherals/dbdma.c \
                ../../../../src/core/peripherals/bmac.c
include ../../common.mk
