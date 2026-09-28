# DP83932 SONIC chip-model unit test.
# Links sonic.c against mock guest memory (sonic_set_memory_hooks) so the
# scsi flows are replaced by SONIC descriptor DMA over the mock buffer.
# Apple ROM self-tests pin are exercised deterministically.

TEST_NAME    := sonic
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := shell checkpoint memory debugger peripherals assert
EXTRA_SRCS   := ../../../../src/core/peripherals/dma_mem.c \
                ../../../../src/core/peripherals/sonic.c
include ../../common.mk
