# Lisa/XL ROM interleave + identification unit test (Step 1 of the Lisa-XL work)
# Links the real rom.c against real memory.c/mmu.c/object/value/meta,
# with stub system/cpu, to exercise interleaving and signature identification.

TEST_NAME    := rom
TEST_SRCS    := test.c stub_lisa_system.c
TEST_HARNESS := isolated
# stub_lisa_system.c stands in for stub_system.c and the real memory.c for
# stub_memory.c; stub_peripherals.c is not needed.
OMIT_STUBS   := system memory peripherals
STUBS        := cpu
EXTRA_SRCS   := ../../../../src/core/memory/rom.c \
                ../../../../src/core/memory/memory.c \
                ../../../../src/core/memory/mmu.c \
                ../../../../src/core/memory/mmu040.c \
                ../../../../src/core/machine_config.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c
include ../../common.mk
