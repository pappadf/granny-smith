# Grand Central unit test.
# Links the real tnt/grand_central.c (+ the real tnt/dbdma.c behind the
# island's channel windows) against recording stubs and pins: the MkLinux
# initialisation sequence and events-driven acknowledge (clear mode 0),
# the NanoKernel's $80000000 ifMode1Clear acknowledge and the mode-1
# output-latch semantics the boot debugging established, the
# banked two-aperture NVRAM, BoxID byte/longword composition, and the
# island's little-endian DBDMA register routing.

TEST_NAME    := tnt_gc
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert checkpoint
EXTRA_SRCS   := ../../../../src/core/peripherals/dma_mem.c \
                ../../../../src/machines/tnt/grand_central.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/peripherals/irq_controller.c \
                ../../../../src/machines/tnt/dbdma.c
include ../../common.mk
