# The Apple 6-and-2 GCR codec (src/core/storage/gcr.c) that the MOOF image
# reader / writer, the IWM / SWIM track codec and SWIM3's DMA stream share:
# the codeword tables, the sector nibblizer and its checksum chain, the
# address field, steady-state framing, and weak bits (the zero runs of a
# captured track and the noise a drive reads there).  Pure functions; the
# callers' own suites (floppy, moof) pin the bytes each lays down.

TEST_NAME    := gcr
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/storage/gcr.c
include ../../common.mk
