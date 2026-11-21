# GLUE-family dispatch-table unit test.
# Exercises the data-driven I/O address map + IRQ→IPL routing directly (no
# emulator/ROM/MMU): links the engine and the table files and stubs their device getters.
# Also pins the direct page map behind the PowerPC islands' byte fast paths
# (board_io_direct_*) against the general engine, call for call.

TEST_NAME    := board_io
TEST_SRCS    := test.c
TEST_HARNESS := none
EXTRA_SRCS   := ../../../../src/machines/runtime/board_io.c \
                ../../../../src/machines/runtime/m68k_board_io.c \
                ../../../../src/machines/glue/glue_io.c \
                ../../../../src/machines/mdu/mdu_io.c
include ../../common.mk
