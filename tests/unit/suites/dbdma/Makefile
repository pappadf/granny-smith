# DBDMA engine unit test.
# Links the real tnt/dbdma.c against array-backed memory hooks and a
# scripted device port, and pins: the control register's mask/value
# convention, the synchronous status transitions behind the canonical
# stop/reset/start poll loops, the operation-word commit rule (refetch,
# never cache), data transfers with device stalls and resumption, the
# NOP-branch ring idiom, the STOP/WAKE overwrite idiom, quad commands,
# conditional branch/wait/interrupt selectors, and result write-back
# ORDER (resCount/xferStatus guest-visible before the channel interrupt).

TEST_NAME    := dbdma
TEST_SRCS    := test.c
# No stub_checkpoint.c: the test provides RECORDING checkpoint stream
# functions so the save/restore round-trip is exercised for real.
TEST_HARNESS := none
EXTRA_SRCS   := ../../../../src/core/peripherals/dma_mem.c \
                ../../../../src/machines/tnt/dbdma.c
include ../../common.mk
