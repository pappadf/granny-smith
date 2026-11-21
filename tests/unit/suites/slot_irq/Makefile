# /SLOTIRQ aggregate unit test.
# Links the real via.c and the GLUE/MCU /SLOTIRQ aggregate (runtime/slot_irq.c)
# against recording stubs: the OR on VIA2 CA1, and the mask re-derived on a
# restore from the VIA2 pins.

TEST_NAME    := slot_irq
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := checkpoint assert
EXTRA_SRCS   := ../../../../src/core/peripherals/via.c \
                ../../../../src/machines/runtime/slot_irq.c
include ../../common.mk
