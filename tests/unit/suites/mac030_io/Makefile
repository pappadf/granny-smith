# GLUE-family dispatch-table unit test.
# Exercises the data-driven I/O address map + IRQ→IPL routing directly (no
# emulator/ROM/MMU): links only the table file and stubs its device getters.

TEST_NAME    := mac030_io
TEST_SRCS    := test.c
TEST_HARNESS := none
EXTRA_SRCS   := ../../../../src/machines/mac030/mac030_glue_io.c \
                ../../../../src/machines/mdu/mdu_io.c
include ../../common.mk
