# RBV sound-interrupt unit test.
# Links the real rbv.c + asc.c against recording stubs: pins the RvIFR/RvIER
# set/clr-via-bit-7 semantics ($90/$10 writes), the RvSndIRQ (bit 4) source,
# the VIA-spaced register aliases, and the full ASC→RBV interrupt chain
# (FIFO half-empty → asc_set_irq_handler → rbv_set_snd_irq → machine IRQ).

TEST_NAME    := rbv
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert checkpoint
EXTRA_SRCS   := ../../../../src/machines/mdu/rbv.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/peripherals/irq_controller.c \
                ../../../../src/core/peripherals/asc.c
include ../../common.mk
