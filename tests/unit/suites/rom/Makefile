# ROM content identity unit test: per-kind ids and self-checks, the ROM
# table, and the Lisa/XL interleave.  Links the real rom.c + rom_table.c
# against real memory.c/mmu.c/object/value/meta, with stub system/cpu.

TEST_NAME    := rom
TEST_SRCS    := test.c stub_lisa_system.c
TEST_HARNESS := isolated
# stub_lisa_system.c stands in for stub_system.c and the real memory.c for
# stub_memory.c; stub_peripherals.c is not needed.
OMIT_STUBS   := system memory peripherals
STUBS        := cpu
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
              ../../../../src/core/memory/rom.c \
                ../../../../src/core/memory/rom_table.c \
                ../../../../src/core/memory/memory.c \
                ../../../../src/core/memory/mmu.c \
                ../../../../src/core/memory/mmu040.c \
                ../../../../src/core/machine_config.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c
include ../../common.mk
