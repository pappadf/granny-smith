# DSP3210 disassembler unit test.
# The 100 encoding vectors of the validated reference disassembler
# (IM worked examples, ROM-verified words, the illegal-opcode
# list) against src/core/cpu/dsp3210/dsp3210_disasm.c.  Dependency-free
# C99 — no stubs, and it compiles against its own directory only.

TEST_NAME     := dsp3210_disasm
TEST_SRCS     := test.c
TEST_HARNESS  := none
INCLUDE_FLAGS  = -I$(EMU_ROOT)/core/cpu/dsp3210
EXTRA_SRCS    := ../../../../src/core/cpu/dsp3210/dsp3210_disasm.c
include ../../common.mk
