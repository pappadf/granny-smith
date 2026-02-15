TEST_NAME := disasm
TEST_SRCS := test.c
TEST_HARNESS := cpu
# Disassembler additionally needs debug_mac.c for A-trap names, plus data files
EXTRA_SRCS := ../../../../src/core/debug/debug_mac.c \
              ../../../../src/core/debug/mac_globals_data.c \
              ../../../../src/core/debug/mac_traps_data.c
include ../../common.mk
