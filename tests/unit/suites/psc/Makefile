# PSC DMA-engine unit test.
# Links the real av/psc.c against recording stubs and replays the three
# known-good client sequences from Apple's drivers:
# StartPSC/StopPSCRead (SCSI HAL), InitRecv/PrimeXmitRS (MACE), and the
# New Age FDC read/write dance.

TEST_NAME    := psc
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert checkpoint
EXTRA_SRCS   := ../../../../src/core/peripherals/dma_mem.c \
                ../../../../src/machines/av/psc.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/peripherals/irq_controller.c
# -fsanitize=bounds: the sound block is indexed by address bits into a
# fixed latch array, and a lane past its end must trap here, not read the
# next member.
SANITIZE     := -fsanitize=bounds -fno-sanitize-recover=bounds
include ../../common.mk
